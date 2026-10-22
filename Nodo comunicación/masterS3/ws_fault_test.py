#!/usr/bin/env python3
"""Persistent E12/E13 WebSocket fault-injection runner.

This module deliberately reuses :mod:`ws_capture_test` for every protocol,
capture, reconnect, ACK, and cleanup operation.  It only adds deterministic
fault triggers and verdicts:

``reconnect``
    Drop the local WS exactly once after ``--drop-after-samples`` DATA samples
    during the dump.  A PASS requires an exact capture plus exactly two WS
    connections and one counted disconnect.

``stop-running``
    Start a sufficiently long SD capture, observe a fresh RUNNING heartbeat,
    send STOP over that same WS, clear the configured lengths, require a fresh
    ARMED/IDLE heartbeat, then run an exact recovery smoke capture.

``stop-dump``
    Observe DUMPING and at least one DATA sample, then perform the same abort
    and recovery sequence.  The first binary is intentionally partial and is
    reported as ``expected_abort``; only the subsequent smoke must be exact.

There is intentionally no overwrite/force option.  Runtime results are always
written atomically to the JSON companion of ``--output`` and process exit codes
are 0=PASS, 1=runtime/test FAIL, 2=invalid arguments or unsafe output paths.
"""

from __future__ import annotations

import argparse
from collections import deque
import hashlib
import io
import json
import math
import os
from pathlib import Path
import sys
import time
from typing import BinaryIO, Callable

import ws_capture_test as capture


MODE_RECONNECT = "reconnect"
MODE_STOP_RUNNING = "stop-running"
MODE_STOP_DUMP = "stop-dump"
STOP_MODES = (MODE_STOP_RUNNING, MODE_STOP_DUMP)
DEFAULT_BATCHES = 600
DEFAULT_SMOKE_BATCHES = 4
DEFAULT_DROP_AFTER_SAMPLES = 300
MIN_STOP_CAPTURE_SECONDS = 5.0
STATE_IDLE = 0
STATE_ARMED = 2
STATE_RUNNING = 3
STATE_DUMPING = 5


def normalize_mode(value: str) -> str:
    """Accept the checklist spelling (STOP_RUNNING) and CLI-style aliases."""

    normalized = value.strip().lower().replace("_", "-")
    if normalized not in (MODE_RECONNECT, MODE_STOP_RUNNING, MODE_STOP_DUMP):
        raise argparse.ArgumentTypeError(
            "mode must be reconnect, STOP_RUNNING, or STOP_DUMP"
        )
    return normalized


class FaultCaptureRunner(capture.CaptureRunner):
    """CaptureRunner with observation and one local reconnect injection hook."""

    def __init__(
        self,
        args: argparse.Namespace,
        n_batches: int,
        output_stream: BinaryIO,
        *,
        drop_after_samples: int | None = None,
    ) -> None:
        super().__init__(args, n_batches, output_stream)
        self.drop_after_samples = drop_after_samples
        self.drop_injected = False
        self.drop_injected_at_sample: int | None = None
        self.drop_injected_at_elapsed_s: float | None = None
        self.state_events: deque[tuple[float, int]] = deque(maxlen=256)

    def _process_packet(self, packet: bytes) -> None:
        now = time.monotonic()
        node, packet_type, _b2, _b1, state = packet[1:]
        super()._process_packet(packet)

        if packet_type == capture.PTYPE_HEARTBEAT and node == 0:
            self.state_events.append((now, state))

        if (
            packet_type == capture.PTYPE_DATA
            and node == self.args.node
            and self.capture_accepting
            and self.drop_after_samples is not None
            and not self.drop_injected
            and self.samples_written >= self.drop_after_samples
        ):
            self.drop_injected = True
            self.drop_injected_at_sample = self.samples_written
            self.drop_injected_at_elapsed_s = round(self.elapsed(), 3)
            self._drop_socket(
                f"E12 inyeccion local tras muestra {self.samples_written}"
            )

    def fresh_states(self, since: float) -> list[int]:
        return [state for at, state in self.state_events if at >= since]


def should_issue_stop(mode: str, runner: FaultCaptureRunner, since: float) -> bool:
    """Pure trigger predicate kept separate for offline regression tests."""

    fresh_states = runner.fresh_states(since)
    if mode == MODE_STOP_RUNNING:
        return STATE_RUNNING in fresh_states and 1 in runner._ver_statuses()
    if mode == MODE_STOP_DUMP:
        return (
            STATE_DUMPING in fresh_states
            and 2 in runner._ver_statuses()
            and runner.samples_written >= 1
        )
    raise ValueError(f"unsupported STOP mode: {mode}")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def append_error(existing: str | None, new_error: str | None) -> str | None:
    if not new_error:
        return existing
    return f"{existing}; {new_error}" if existing else new_error


def safe_finalize_runner(
    runner: capture.CaptureRunner,
    stream: BinaryIO,
    label: str,
) -> str | None:
    """Always attempt protocol cleanup, local close, flush, and durable output."""

    error: str | None = None
    try:
        runner.cleanup(label)
        runner.final_cleanup_ok = True
    except (Exception, KeyboardInterrupt) as exc:
        detail = f"{type(exc).__name__}: {exc}"
        runner.cleanup_errors.append(detail)
        runner.log(f"ERROR {label}: {detail}")
        error = f"cleanup: {detail}"
    finally:
        runner.close()
        try:
            stream.flush()
            os.fsync(stream.fileno())
        except (AttributeError, OSError) as exc:
            detail = f"{type(exc).__name__}: {exc}"
            error = append_error(error, f"output flush: {detail}")
    return error


def runner_summary(
    runner: capture.CaptureRunner,
    output_path: Path,
    *,
    outcome: str,
    runtime_error: str | None,
) -> dict[str, object]:
    actual_bytes = output_path.stat().st_size
    actual_sha256 = sha256_file(output_path)
    completed = outcome == "complete" and runtime_error is None
    summary = runner.metadata(
        success=completed,
        error=runtime_error,
        output_path=output_path,
    )
    summary.update(
        {
            "status": "expected_abort" if outcome == "expected_abort" and runtime_error is None else summary["status"],
            "outcome": outcome if runtime_error is None else "failed",
            "expected_outcome": outcome,
            "actual_file_bytes": actual_bytes,
            "actual_file_sha256": actual_sha256,
            "sha256_matches_file": runner.sha256.hexdigest() == actual_sha256,
        }
    )
    if isinstance(runner, FaultCaptureRunner):
        summary["drop_injected"] = runner.drop_injected
        summary["drop_injected_at_sample"] = runner.drop_injected_at_sample
        summary["drop_injected_at_elapsed_s"] = runner.drop_injected_at_elapsed_s
    return summary


def exact_capture_failures(summary: dict[str, object]) -> list[str]:
    failures: list[str] = []
    expected = int(summary["expected_samples"])
    samples = int(summary["samples_written"])
    actual_bytes = int(summary["actual_file_bytes"])
    if samples != expected:
        failures.append(f"samples_written={samples}, expected={expected}")
    if int(summary["target_data_packets_seen"]) != expected:
        failures.append("target DATA packet count is not exact")
    if actual_bytes != expected * 3:
        failures.append(f"actual_file_bytes={actual_bytes}, expected={expected * 3}")
    if int(summary["bytes_written"]) != actual_bytes:
        failures.append("metadata bytes_written differs from file size")
    if int(summary["extra_samples"]) != 0:
        failures.append("extra samples observed")
    if int(summary["other_node_samples"]) != 0:
        failures.append("other-node samples observed")
    if summary["sha256_matches_file"] is not True:
        failures.append("stream SHA-256 differs from file SHA-256")
    if summary["initial_cleanup_ok"] is not True:
        failures.append("initial cleanup was not confirmed")
    if summary["final_cleanup_ok"] is not True:
        failures.append("final cleanup was not confirmed")
    if summary["cleanup_errors"] != []:
        failures.append("cleanup_errors is not empty")
    return failures


def reconnect_failures(summary: dict[str, object], drop_after: int) -> list[str]:
    failures = exact_capture_failures(summary)
    if summary.get("drop_injected") is not True:
        failures.append("the requested local WS drop was not injected")
    injected_at = summary.get("drop_injected_at_sample")
    if not isinstance(injected_at, int) or injected_at < drop_after:
        failures.append("drop injection occurred before its sample threshold")
    if int(summary["connections"]) != 2:
        failures.append(f"connections={summary['connections']}, expected=2")
    if int(summary["disconnects"]) != 1:
        failures.append(f"disconnects={summary['disconnects']}, expected=1")
    return failures


def interrupted_attempt_failures(
    mode: str,
    summary: dict[str, object],
    evidence: dict[str, object],
) -> list[str]:
    failures: list[str] = []
    samples = int(summary["samples_written"])
    expected = int(summary["expected_samples"])
    if summary.get("outcome") != "expected_abort":
        failures.append("interrupted attempt is not labelled expected_abort")
    if samples >= expected:
        failures.append("interrupted attempt unexpectedly reached full sample count")
    if int(summary["actual_file_bytes"]) != samples * 3:
        failures.append("partial binary size does not equal samples_written*3")
    if summary["sha256_matches_file"] is not True:
        failures.append("partial stream SHA-256 differs from file SHA-256")
    if evidence.get("stop_same_socket") is not True:
        failures.append("STOP was not sent on the same WS that observed the trigger")
    if evidence.get("stop_ack") != 0:
        failures.append("STOP ACK A4=0 was not observed")
    if evidence.get("return_state") not in (STATE_IDLE, STATE_ARMED):
        failures.append("no fresh IDLE/ARMED return state after STOP")
    if int(evidence.get("connections_at_stop", -1)) != 1:
        failures.append("STOP path did not use the initial WS connection")
    if int(evidence.get("disconnects_at_stop", -1)) != 0:
        failures.append("a disconnect occurred before STOP")
    if summary["initial_cleanup_ok"] is not True:
        failures.append("initial cleanup was not confirmed")
    if summary["final_cleanup_ok"] is not True or summary["cleanup_errors"] != []:
        failures.append("final cleanup was not clean")

    samples_at_stop = int(evidence.get("samples_at_stop", -1))
    trigger_state = evidence.get("trigger_state")
    if mode == MODE_STOP_RUNNING:
        if trigger_state != STATE_RUNNING:
            failures.append("STOP_RUNNING did not trigger from RUNNING")
        if samples_at_stop != 0:
            failures.append("STOP_RUNNING saw DATA before STOP")
    elif mode == MODE_STOP_DUMP:
        if trigger_state != STATE_DUMPING:
            failures.append("STOP_DUMP did not trigger from DUMPING")
        if samples_at_stop < 1:
            failures.append("STOP_DUMP did not observe DATA before STOP")
    else:
        failures.append(f"unknown interruption mode {mode}")
    return failures


def wait_until(
    runner: FaultCaptureRunner,
    deadline: float,
    predicate: Callable[[], bool],
    timeout_message: str,
    *,
    reject_ver_abort: bool = True,
) -> None:
    while time.monotonic() < deadline:
        if predicate():
            return
        if reject_ver_abort and 0 in runner._ver_statuses():
            raise capture.ProbeError("VER reported reject/abort before fault trigger")
        runner.poll_once(deadline)
    raise capture.PhaseTimeout(timeout_message)


def prepare_capture(runner: FaultCaptureRunner) -> None:
    runner.cleanup("initial_cleanup")
    runner.initial_cleanup_ok = True
    runner.configure_slave()
    runner.wait_fresh_geo_hello()
    runner.arm()
    runner.set_record_length()


def clear_lengths_and_wait_return(
    runner: FaultCaptureRunner,
    stop_sent_at: float,
) -> int:
    """Abort the intentionally partial test without leaving DUMPING behind."""

    deadline = time.monotonic() + runner.args.phase_timeout
    sent = runner._send(
        {"cmd": capture.hex2(capture.CMD_SET_RECLEN), "value": 0},
        deadline,
        "abort/AE=0",
    )
    runner.wait_ack(
        capture.MASTER_NODE_ID,
        capture.CMD_SET_RECLEN,
        0,
        sent,
        max(0.1, deadline - time.monotonic()),
        "abort/AE=0",
    )
    sent = runner._send(
        {"cmd": capture.hex2(capture.CMD_SET_RECLEN_HAMMER), "value": 0},
        deadline,
        "abort/AD=0",
    )
    runner.wait_ack(
        capture.MASTER_NODE_ID,
        capture.CMD_SET_RECLEN_HAMMER,
        0,
        sent,
        max(0.1, deadline - time.monotonic()),
        "abort/AD=0",
    )

    def returned() -> bool:
        return any(
            state in (STATE_IDLE, STATE_ARMED)
            for state in runner.fresh_states(stop_sent_at)
        )

    wait_until(
        runner,
        deadline,
        returned,
        "STOP acknowledged but master did not return to fresh IDLE/ARMED",
        reject_ver_abort=False,
    )
    return next(
        state
        for state in reversed(runner.fresh_states(stop_sent_at))
        if state in (STATE_IDLE, STATE_ARMED)
    )


def execute_interrupted_attempt(
    runner: FaultCaptureRunner,
    mode: str,
) -> dict[str, object]:
    prepare_capture(runner)
    runner.capture_accepting = True
    command_deadline = time.monotonic() + runner.args.phase_timeout
    runner.ver_sent_at = runner._send(
        {
            "cmd": "BD",
            "node": runner.args.node,
            "sub": capture.hex2(capture.SUBCMD_VER),
            "param": 1,
        },
        command_deadline,
        f"{mode} VER S{runner.args.node} n={runner.n_batches}",
    )
    capture_seconds = runner.expected_samples / runner.args.fs
    if mode == MODE_STOP_RUNNING:
        trigger_timeout = runner.args.phase_timeout
    else:
        trigger_timeout = runner.args.capture_timeout
        if trigger_timeout <= 0:
            trigger_timeout = capture_seconds + max(180.0, capture_seconds * 0.35)
    trigger_deadline = runner.ver_sent_at + trigger_timeout
    wait_until(
        runner,
        trigger_deadline,
        lambda: should_issue_stop(mode, runner, runner.ver_sent_at),
        f"{mode}: trigger not observed before deadline",
    )

    trigger_state = STATE_RUNNING if mode == MODE_STOP_RUNNING else STATE_DUMPING
    samples_at_stop = runner.samples_written
    socket_before_stop = runner.sock
    connections_at_stop = runner.connections
    disconnects_at_stop = runner.disconnects
    stop_deadline = time.monotonic() + runner.args.phase_timeout
    stop_sent_at = runner._send(
        {"cmd": capture.hex2(capture.CMD_STOP), "param": 0},
        stop_deadline,
        mode.upper(),
    )
    stop_same_socket = socket_before_stop is not None and runner.sock is socket_before_stop
    # DATA already inside the frame that established the trigger is retained;
    # anything arriving after the STOP send belongs to the aborted tail.
    runner.capture_accepting = False
    stop_ack = runner.wait_ack(
        capture.MASTER_NODE_ID,
        capture.CMD_STOP,
        0,
        stop_sent_at,
        max(0.1, stop_deadline - time.monotonic()),
        mode.upper(),
    )
    return_state = clear_lengths_and_wait_return(runner, stop_sent_at)
    runner.output_stream.flush()
    runner.phase_seconds[mode] = round(time.monotonic() - runner.ver_sent_at, 3)
    return {
        "trigger_state": trigger_state,
        "trigger_state_name": capture.MASTER_STATES[trigger_state],
        "samples_at_stop": samples_at_stop,
        "stop_sent_at_elapsed_s": round(stop_sent_at - runner.started_monotonic, 3),
        "stop_same_socket": stop_same_socket,
        "stop_ack": stop_ack,
        "connections_at_stop": connections_at_stop,
        "disconnects_at_stop": disconnects_at_stop,
        "return_state": return_state,
        "return_state_name": capture.MASTER_STATES[return_state],
        "attempt_expected_abort": True,
    }


def execute_runner_to_file(
    args: argparse.Namespace,
    n_batches: int,
    output_path: Path,
    action: Callable[[FaultCaptureRunner], dict[str, object] | None],
    *,
    drop_after_samples: int | None = None,
    outcome: str,
) -> tuple[dict[str, object], dict[str, object], str | None]:
    runner: FaultCaptureRunner | None = None
    evidence: dict[str, object] = {}
    error: str | None = None
    with output_path.open("xb") as stream:
        runner = FaultCaptureRunner(
            args,
            n_batches,
            stream,
            drop_after_samples=drop_after_samples,
        )
        runner.log(
            f"fault test: mode={args.mode}, node={args.node}, batches={n_batches}, "
            f"output={output_path}"
        )
        try:
            result = action(runner)
            if result:
                evidence.update(result)
        except KeyboardInterrupt:
            error = "interrupted by user"
        except Exception as exc:
            error = f"{type(exc).__name__}: {exc}"
        finally:
            cleanup_error = safe_finalize_runner(runner, stream, "final_cleanup")
            error = append_error(error, cleanup_error)
    assert runner is not None
    summary = runner_summary(
        runner,
        output_path,
        outcome=outcome,
        runtime_error=error,
    )
    return summary, evidence, error


def smoke_args(args: argparse.Namespace) -> argparse.Namespace:
    values = vars(args).copy()
    values.update(
        {
            "mode": "recovery-smoke",
            "batches": args.smoke_batches,
            "seconds": None,
            "output": None,
        }
    )
    return argparse.Namespace(**values)


def derive_smoke_path(output_path: Path) -> Path:
    if output_path.suffix:
        return output_path.with_name(output_path.stem + ".smoke" + output_path.suffix)
    return output_path.with_name(output_path.name + ".smoke.i24le")


def recovery_wait_seconds(
    mode: str,
    batches: int,
    fs_hz: float,
    requested_seconds: float,
) -> float:
    """Return the settle needed before a post-STOP recovery smoke.

    STOP_RUNNING returns the master to ARMED but does not abort an SD capture
    already running inside the PSoC.  Wait the full nominal window plus a
    close margin before sending new configuration.  STOP_DUMP runs after the
    PSoC has already closed the SD session and only needs a short settle.
    """

    if requested_seconds > 0:
        return requested_seconds
    if mode == MODE_STOP_RUNNING:
        return batches * capture.SAMPLES_PER_BATCH / fs_hz + 5.0
    if mode == MODE_STOP_DUMP:
        return 1.0
    return 0.0


def validate_cli(
    args: argparse.Namespace,
) -> tuple[int, Path, Path, Path | None]:
    if args.output is None:
        raise ValueError("--output is required unless --self-test is used")
    n_batches = capture.resolve_batches(args.batches, None, args.fs)
    capture.resolve_batches(args.smoke_batches, None, args.fs)
    if args.node < 1 or args.node > 254:
        raise ValueError("--node must be in 1..254")
    if args.port < 1 or args.port > 65_535:
        raise ValueError("--port must be in 1..65535")
    for name in (
        "fs",
        "phase_timeout",
        "connect_timeout",
        "reconnect_delay",
        "hello_fresh_delay",
        "cleanup_settle",
        "extra_grace",
        "progress_interval",
    ):
        value = getattr(args, name)
        if not math.isfinite(value) or value <= 0:
            raise ValueError(f"--{name.replace('_', '-')} must be finite and > 0")
    for name in ("capture_timeout", "dump_timeout"):
        value = getattr(args, name)
        if not math.isfinite(value) or value < 0:
            raise ValueError(f"--{name.replace('_', '-')} must be finite and >= 0")
    if not math.isfinite(args.recovery_settle) or args.recovery_settle < 0:
        raise ValueError("--recovery-settle must be finite and >= 0")

    expected_samples = n_batches * capture.SAMPLES_PER_BATCH
    if args.mode == MODE_RECONNECT:
        if args.drop_after_samples < 1 or args.drop_after_samples >= expected_samples:
            raise ValueError(
                f"--drop-after-samples must be in 1..{expected_samples - 1}"
            )
    else:
        nominal_seconds = expected_samples / args.fs
        if nominal_seconds < MIN_STOP_CAPTURE_SECONDS:
            raise ValueError(
                f"STOP modes require at least {MIN_STOP_CAPTURE_SECONDS:g}s nominal "
                f"capture (got {nominal_seconds:.3f}s)"
            )

    output_path = args.output.expanduser()
    metadata_path = capture.metadata_path_for(output_path)
    smoke_path = derive_smoke_path(output_path) if args.mode in STOP_MODES else None
    paths = [output_path, metadata_path]
    if smoke_path is not None:
        paths.append(smoke_path)
    resolved = [path.resolve() for path in paths]
    if len(set(resolved)) != len(resolved):
        raise ValueError("output, metadata, and smoke paths must be distinct")
    existing = [str(path) for path in paths if path.exists()]
    if existing:
        raise ValueError("refusing to overwrite existing file(s): " + ", ".join(existing))
    for path in paths:
        path.parent.mkdir(parents=True, exist_ok=True)
    return n_batches, output_path, metadata_path, smoke_path


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="E12/E13 WS reconnect and STOP recovery acceptance runner"
    )
    parser.add_argument(
        "--mode",
        type=normalize_mode,
        choices=(MODE_RECONNECT, MODE_STOP_RUNNING, MODE_STOP_DUMP),
        default=MODE_RECONNECT,
    )
    parser.add_argument("--output", type=Path, help="primary signed-int24-le output")
    parser.add_argument("--host", default=capture.DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=capture.DEFAULT_PORT, help="WebSocket TCP port")
    parser.add_argument("--path", default=capture.DEFAULT_PATH)
    parser.add_argument("--node", type=int, default=1)
    parser.add_argument("--batches", type=int, default=DEFAULT_BATCHES)
    parser.add_argument("--fs", type=float, default=capture.DEFAULT_FS_HZ)
    parser.add_argument("--drop-after-samples", type=int, default=DEFAULT_DROP_AFTER_SAMPLES)
    parser.add_argument("--smoke-batches", type=int, default=DEFAULT_SMOKE_BATCHES)
    parser.add_argument(
        "--token",
        default=os.environ.get("GEO_WS_TOKEN", ""),
        help="optional WS auth token (or GEO_WS_TOKEN)",
    )
    parser.add_argument("--phase-timeout", type=float, default=30.0)
    parser.add_argument("--capture-timeout", type=float, default=0.0)
    parser.add_argument("--dump-timeout", type=float, default=0.0)
    parser.add_argument("--connect-timeout", type=float, default=5.0)
    parser.add_argument("--reconnect-delay", type=float, default=1.0)
    parser.add_argument("--hello-fresh-delay", type=float, default=1.0)
    parser.add_argument("--cleanup-settle", type=float, default=1.0)
    parser.add_argument("--extra-grace", type=float, default=3.0)
    parser.add_argument("--progress-interval", type=float, default=5.0)
    parser.add_argument(
        "--recovery-settle",
        type=float,
        default=0.0,
        help=(
            "seconds before the post-STOP smoke; 0 derives full nominal capture +5s "
            "for STOP_RUNNING and 1s for STOP_DUMP"
        ),
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run offline trigger/injection/verdict checks; never opens network/serial",
    )
    args = parser.parse_args(argv)
    # CaptureRunner metadata expects this duration-shaped attribute.
    args.seconds = None
    return args


def _test_args(**overrides: object) -> argparse.Namespace:
    values: dict[str, object] = {
        "mode": MODE_RECONNECT,
        "host": "offline.invalid",
        "port": 80,
        "path": "/ws?takeover=1",
        "node": 1,
        "batches": 1,
        "seconds": None,
        "fs": 2604.0,
        "token": "",
        "phase_timeout": 1.0,
        "capture_timeout": 0.0,
        "dump_timeout": 0.0,
        "connect_timeout": 1.0,
        "reconnect_delay": 0.01,
        "hello_fresh_delay": 0.01,
        "cleanup_settle": 0.01,
        "extra_grace": 0.01,
        "progress_interval": 1.0,
        "recovery_settle": 0.0,
    }
    values.update(overrides)
    return argparse.Namespace(**values)


def run_self_tests() -> None:
    assert normalize_mode("STOP_RUNNING") == MODE_STOP_RUNNING
    assert normalize_mode("stop_dump") == MODE_STOP_DUMP
    assert normalize_mode("reconnect") == MODE_RECONNECT

    class FakeSocket:
        def __init__(self) -> None:
            self.closed = False

        def close(self) -> None:
            self.closed = True

    runner = FaultCaptureRunner(
        _test_args(),
        1,
        io.BytesIO(),
        drop_after_samples=3,
    )
    runner.log = lambda _message: None  # type: ignore[method-assign]
    runner.capture_accepting = True
    fake_socket = FakeSocket()
    runner.sock = fake_socket  # type: ignore[assignment]
    runner.connections = 1
    packet = bytes((capture.PKT_HEADER, 1, capture.PTYPE_DATA, 0, 0, 1))
    runner._process_packet(packet)
    runner._process_packet(packet)
    assert not runner.drop_injected and runner.sock is fake_socket
    runner._process_packet(packet)
    assert runner.drop_injected and runner.drop_injected_at_sample == 3
    assert runner.sock is None and fake_socket.closed and runner.disconnects == 1

    running = FaultCaptureRunner(_test_args(), 1, io.BytesIO())
    running.log = lambda _message: None  # type: ignore[method-assign]
    running.ver_sent_at = time.monotonic() - 1.0
    running.ack_events.append((time.monotonic(), 1, capture.SUBCMD_VER, 1))
    running.state_events.append((time.monotonic(), STATE_RUNNING))
    assert should_issue_stop(MODE_STOP_RUNNING, running, running.ver_sent_at)
    assert not should_issue_stop(MODE_STOP_DUMP, running, running.ver_sent_at)

    dumping = FaultCaptureRunner(_test_args(), 1, io.BytesIO())
    dumping.log = lambda _message: None  # type: ignore[method-assign]
    dumping.capture_accepting = True
    dumping.ver_sent_at = time.monotonic() - 1.0
    dumping.ack_events.append((time.monotonic(), 1, capture.SUBCMD_VER, 2))
    dumping.state_events.append((time.monotonic(), STATE_DUMPING))
    assert not should_issue_stop(MODE_STOP_DUMP, dumping, dumping.ver_sent_at)
    dumping._process_packet(packet)
    assert should_issue_stop(MODE_STOP_DUMP, dumping, dumping.ver_sent_at)

    exact: dict[str, object] = {
        "expected_samples": 30,
        "samples_written": 30,
        "target_data_packets_seen": 30,
        "actual_file_bytes": 90,
        "bytes_written": 90,
        "extra_samples": 0,
        "other_node_samples": 0,
        "sha256_matches_file": True,
        "initial_cleanup_ok": True,
        "final_cleanup_ok": True,
        "cleanup_errors": [],
        "drop_injected": True,
        "drop_injected_at_sample": 3,
        "connections": 2,
        "disconnects": 1,
    }
    assert reconnect_failures(exact, 3) == []
    broken = dict(exact)
    broken["connections"] = 3
    broken["actual_file_bytes"] = 87
    failures = reconnect_failures(broken, 3)
    assert any("connections=3" in item for item in failures)
    assert any("actual_file_bytes" in item for item in failures)

    interrupted = dict(exact)
    interrupted.update(
        {
            "outcome": "expected_abort",
            "expected_samples": 300,
            "samples_written": 30,
            "actual_file_bytes": 90,
            "connections": 1,
            "disconnects": 0,
        }
    )
    evidence: dict[str, object] = {
        "stop_same_socket": True,
        "stop_ack": 0,
        "return_state": STATE_ARMED,
        "connections_at_stop": 1,
        "disconnects_at_stop": 0,
        "samples_at_stop": 30,
        "trigger_state": STATE_DUMPING,
    }
    assert interrupted_attempt_failures(MODE_STOP_DUMP, interrupted, evidence) == []
    evidence["return_state"] = STATE_DUMPING
    assert interrupted_attempt_failures(MODE_STOP_DUMP, interrupted, evidence)

    assert derive_smoke_path(Path("capture.i24le")) == Path("capture.smoke.i24le")
    assert capture.metadata_path_for(Path("capture.i24le")) == Path("capture.json")
    assert recovery_wait_seconds(MODE_STOP_RUNNING, 600, 2604.0, 0.0) > 11.9
    assert recovery_wait_seconds(MODE_STOP_DUMP, 600, 2604.0, 0.0) == 1.0
    assert recovery_wait_seconds(MODE_STOP_RUNNING, 600, 2604.0, 2.5) == 2.5
    print(
        "self-test OK: one-shot drop injection, RUNNING/DUMP triggers, "
        "strict reconnect/interruption verdicts, output derivation"
    )


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.self_test:
        run_self_tests()
        return 0
    try:
        n_batches, output_path, metadata_path, smoke_path = validate_cli(args)
    except ValueError as exc:
        print(f"argument error: {exc}", file=sys.stderr)
        return 2

    started_utc = capture.utc_now()
    started_monotonic = time.monotonic()
    result: dict[str, object] = {
        "schema_version": 1,
        "test": "E12_E13_ws_fault_injection",
        "mode": args.mode,
        "status": "error",
        "error": None,
        "started_at_utc": started_utc,
        "host": args.host,
        "port": args.port,
        "path": args.path,
        "node": args.node,
        "batches": n_batches,
        "smoke_batches": args.smoke_batches if args.mode in STOP_MODES else None,
        "output_file": str(output_path.resolve()),
        "metadata_file": str(metadata_path.resolve()),
        "smoke_output_file": str(smoke_path.resolve()) if smoke_path else None,
        "attempt": None,
        "fault_evidence": {},
        "recovery_smoke": None,
        "criteria_failures": [],
    }
    errors: list[str] = []
    criteria: list[str] = []

    try:
        if args.mode == MODE_RECONNECT:
            attempt, evidence, runtime_error = execute_runner_to_file(
                args,
                n_batches,
                output_path,
                lambda runner: (runner.execute(), None)[1],
                drop_after_samples=args.drop_after_samples,
                outcome="complete",
            )
            result["attempt"] = attempt
            result["fault_evidence"] = {
                "drop_after_samples": args.drop_after_samples,
                "drop_injected": attempt.get("drop_injected"),
                "drop_injected_at_sample": attempt.get("drop_injected_at_sample"),
                **evidence,
            }
            if runtime_error:
                errors.append(runtime_error)
            criteria.extend(reconnect_failures(attempt, args.drop_after_samples))
        else:
            assert smoke_path is not None
            attempt, evidence, runtime_error = execute_runner_to_file(
                args,
                n_batches,
                output_path,
                lambda runner: execute_interrupted_attempt(runner, args.mode),
                outcome="expected_abort",
            )
            result["attempt"] = attempt
            result["fault_evidence"] = evidence
            if runtime_error:
                errors.append(runtime_error)
            criteria.extend(interrupted_attempt_failures(args.mode, attempt, evidence))

            # Recovery smoke is only meaningful after the STOP path itself met
            # its runtime/cleanup obligations.  All failure paths were already
            # cleaned in execute_runner_to_file.
            if not errors and not criteria:
                wait_seconds = recovery_wait_seconds(
                    args.mode,
                    n_batches,
                    args.fs,
                    args.recovery_settle,
                )
                result["fault_evidence"]["recovery_wait_s"] = round(wait_seconds, 3)
                if wait_seconds > 0:
                    print(
                        f"esperando {wait_seconds:.3f}s antes del smoke de recuperacion "
                        f"({args.mode})",
                        flush=True,
                    )
                    time.sleep(wait_seconds)
                recovery_args = smoke_args(args)
                smoke, _smoke_evidence, smoke_error = execute_runner_to_file(
                    recovery_args,
                    args.smoke_batches,
                    smoke_path,
                    lambda runner: (runner.execute(), None)[1],
                    outcome="complete",
                )
                result["recovery_smoke"] = smoke
                if smoke_error:
                    errors.append(f"recovery smoke: {smoke_error}")
                criteria.extend(
                    f"recovery smoke: {failure}"
                    for failure in exact_capture_failures(smoke)
                )
            else:
                criteria.append("recovery smoke not run because interrupted attempt failed")
    except Exception as exc:
        # Includes local file creation/stat failures; protocol cleanup is owned
        # by execute_runner_to_file and has already been attempted if needed.
        errors.append(f"{type(exc).__name__}: {exc}")

    result["criteria_failures"] = criteria
    all_failures = errors + criteria
    result["status"] = "ok" if not all_failures else "error"
    result["error"] = "; ".join(all_failures) if all_failures else None
    result["completed_at_utc"] = capture.utc_now()
    result["elapsed_s"] = round(time.monotonic() - started_monotonic, 3)
    try:
        capture.write_metadata(metadata_path, result)
    except Exception as exc:
        print(
            f"fatal writing metadata {metadata_path}: {type(exc).__name__}: {exc}",
            file=sys.stderr,
        )
        return 1

    if result["status"] == "ok":
        print(f"PASS {args.mode}: metadata={metadata_path}", flush=True)
        return 0
    print(f"FAIL {args.mode}: {result['error']}", flush=True)
    print(f"metadata={metadata_path}", flush=True)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
