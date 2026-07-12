#!/usr/bin/env python3
"""E15: regresion USB del limite SD de 60000 lotes, sin dump por radio.

El runner usa ``pre`` y ``sync`` por separado, no transmite durante el
muestreo y valida la sesion persistida con ``sdinfo``/``sdread``. Requiere el
firmware de banco con ``SLAVE_USB_CMD_ENABLE`` y ``SLAVE_LAB_TOOLS_ENABLE``.

Ejemplo::

    python sd_max_regression_test.py --port COM12 --batches 60000 \
        --output "$env:TEMP/e15_sd_max_60000.json"

``--self-test`` es completamente offline: no importa pyserial ni abre un COM.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import sys
import time
from dataclasses import dataclass
from typing import Callable, Optional

from usb_regression_test import (
    BAUD,
    CMD_SD_CAPTURE,
    CMD_SELECT_STREAM,
    CMD_SET_DECIMATION,
    NATIVE_FS_HZ,
    PROBE_OK_RE,
    PSOC_IDLE_RE,
    SDREAD_OOR_RE,
    SDREAD_RE,
    SD_STATUS_RE,
    ReadResult,
    SerialSession,
    Status,
    find_config_ack,
    has_ack_for_subcommand,
    parse_status,
)


MAX_BATCHES = 60_000
SAMPLES_PER_BATCH = 30
SD_TEST_CMD = 0xBD
SD_REQUIRED_READY = 0x11       # present + FAT mounted
SD_REQUIRED_CAPTURE = 0x51     # present + FAT mounted + capture enabled
SD_REQUIRED_RECOVERED = 0x71   # idem + COMPLETE session recovered
CAPTURE_DEADLINE_S = 760.0
SDREAD_ATTEMPTS = 3
SDREAD_ATTEMPT_S = 2.0

STORE_SD_RE = re.compile(r"\[SLAVE\]\s+store\s+PSoC-SD\s+n=(?P<n>\d+)", re.I)
SETN_RE = re.compile(r"\[PSoC\]\s+SETN\s+ack\s+val=(?P<value>\d+)", re.I)
ARMED_RE = re.compile(r"\[PSoC\]\s+ARMED\b.*\bfill=(?P<fill>\d+)/(?P<n>\d+)", re.I)
SYNC_RE = re.compile(
    r"\[USB\]\s+sync\s+start\s+n=(?P<n>\d+)\s+sync=(?P<before>[01])->(?P<after>[01])",
    re.I,
)
SD_SESSION_RE = re.compile(r"\[PSoC\]\s+SD_SESSION\s+val=0x(?P<value>[0-9A-Fa-f]{2})", re.I)
SD_COMPLETE_RE = re.compile(
    r"\[SLAVE\]\s+START\s+SD\s+complete=(?P<complete>[01])\s+"
    r"err=(?P<err>\d+)\s+fill=(?P<fill>\d+)/(?P<n>\d+)",
    re.I,
)
SDREAD_TIMEOUT_RE = re.compile(r"PSoC\s+SD_READ\s+timeout\s+seq=(?P<seq>\d+)\s+usb=1", re.I)
SDREAD_NACK_RE = re.compile(r"PSoC\s+SD_READ\s+NACK", re.I)

FORBIDDEN = (
    ("SD_ERROR", re.compile(r"\bSD_ERROR\b", re.I)),
    ("SYNC_STALL", re.compile(r"(?:CAPTURE|START|VIEW)_SYNC_STALL", re.I)),
    ("SETN_TIMEOUT", re.compile(r"SETN(?:\s+ack)?\s+timeout|SETN_TIMEOUT", re.I)),
    ("HOT_WAIT_ABORT", re.compile(r"HOT_WAIT.*abort", re.I)),
    ("PARTIAL_DUMP", re.compile(r"partial\s+dump", re.I)),
)


class E15Failure(RuntimeError):
    """Criterio de aceptacion incumplido."""


@dataclass(frozen=True)
class Diag:
    b_bad: int
    drops: int
    bad_len: int
    state: int
    fill: int
    target: int
    fs_hz: int


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def timing_for(batches: int) -> dict[str, float | int]:
    nominal_s = batches * SAMPLES_PER_BATCH / NATIVE_FS_HZ
    nominal_ms_ceil = (
        batches * SAMPLES_PER_BATCH * 1000 + NATIVE_FS_HZ - 1
    ) // NATIVE_FS_HZ
    expected_capture_ms = nominal_ms_ceil + 2500
    return {
        "nominal_sampling_s": nominal_s,
        "expected_capture_ms": expected_capture_ms,
        "capwait_timeout_ms": expected_capture_ms + 2000,
        "sd_stall_fallback_ms": expected_capture_ms + 15000 + batches // 2,
        "external_deadline_s": CAPTURE_DEADLINE_S,
    }


def parse_sd_status(text: str) -> Optional[int]:
    matches = list(SD_STATUS_RE.finditer(text))
    return int(matches[-1].group("status"), 16) if matches else None


def parse_diag(text: str) -> Optional[Diag]:
    for line in reversed(text.splitlines()):
        if "USB_STATUS" not in line:
            continue
        values: dict[str, int] = {}
        for token, key in (
            ("bBad", "b_bad"),
            ("drop", "drops"),
            ("badLen", "bad_len"),
            ("state", "state"),
            ("fs", "fs_hz"),
        ):
            match = re.search(rf"\b{token}=(\d+)\b", line)
            if match:
                values[key] = int(match.group(1))
        fill = re.search(r"\bfill=(\d+)/(\d+)\b", line)
        if fill and len(values) == 5:
            return Diag(
                values["b_bad"], values["drops"], values["bad_len"],
                values["state"], int(fill.group(1)), int(fill.group(2)),
                values["fs_hz"],
            )
    return None


def forbidden_events(text: str) -> list[str]:
    return [name for name, pattern in FORBIDDEN if pattern.search(text)]


def uart_integrity_errors(baseline: Diag, diag: Diag) -> list[str]:
    """Solo bBad/badLen son corrupcion real de frames. ``drops`` cuenta bytes
    de texto UART del PSoC descartados por el framer mientras busca 0xAB
    (psoc_uart.cpp ``_syncDrops``): crece de forma benigna con cada linea de
    log del PSoC, incluso en idle, y no debe bloquear el veredicto."""

    errors: list[str] = []
    if (diag.b_bad, diag.bad_len) != (baseline.b_bad, baseline.bad_len):
        errors.append(
            f"UART delta bBad/badLen={diag.b_bad - baseline.b_bad}/"
            f"{diag.bad_len - baseline.bad_len}"
        )
    return errors


def capture_errors(text: str, batches: int) -> list[str]:
    errors = forbidden_events(text)
    sessions = list(SD_SESSION_RE.finditer(text))
    complete = list(SD_COMPLETE_RE.finditer(text))
    if not sessions or int(sessions[-1].group("value"), 16) != 1:
        errors.append("falta SD_SESSION val=1")
    if not complete:
        errors.append("falta START SD complete")
    else:
        event = complete[-1]
        got = tuple(int(event.group(name)) for name in ("complete", "err", "fill", "n"))
        if got != (1, 0, batches, batches):
            errors.append(
                f"START SD complete={got[0]} err={got[1]} fill={got[2]}/{got[3]}"
            )
    return errors


def atomic_json(path: Path, payload: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    try:
        with temporary.open("w", encoding="utf-8", newline="\n") as stream:
            json.dump(payload, stream, indent=2, sort_keys=True, ensure_ascii=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


class E15Runner:
    def __init__(
        self,
        session: SerialSession,
        batches: int,
        metadata: dict[str, object],
        verbose: bool,
    ) -> None:
        self.session = session
        self.batches = batches
        self.metadata = metadata
        self.verbose = verbose
        self.started = time.monotonic()
        self.phase = "opened"
        self.checks: list[dict[str, object]] = []
        self.lines: list[dict[str, object]] = []
        self.baseline: Optional[Diag] = None
        self.capture_complete = False

    def _record(self, result: ReadResult) -> str:
        for line in result.text.splitlines():
            if line.strip():
                self.lines.append({"elapsed_s": round(time.monotonic() - self.started, 3), "line": line})
                if self.verbose:
                    print(f"  {line}")
        if result.truncated:
            self.require("respuesta serial sin truncar", False, f"{result.byte_count} bytes")
        return result.text

    def check(self, name: str, passed: bool, detail: str = "") -> bool:
        item = {"name": name, "passed": bool(passed), "detail": " ".join(detail.split())}
        self.checks.append(item)
        print(f"[{'PASS' if passed else 'FAIL'}] {name}" + (f" - {item['detail']}" if detail else ""))
        return passed

    def require(self, name: str, passed: bool, detail: str = "") -> None:
        if not self.check(name, passed, detail):
            raise E15Failure(name)

    def command(
        self,
        command: str,
        timeout_s: float,
        predicate: Optional[Callable[[str], bool]] = None,
    ) -> str:
        print(f"> {command}")
        return self._record(self.session.command(command, timeout_s, predicate=predicate))

    def status(self) -> tuple[Status, Diag, str]:
        text = self.command("status", 1.5, lambda value: parse_status(value) is not None)
        status = parse_status(text)
        diag = parse_diag(text)
        self.require("status parseable", status is not None and diag is not None)
        assert status is not None and diag is not None
        return status, diag, text

    def config(self, command: str, sub_cmd: int, value: int, label: str) -> str:
        combined = ""
        for attempt in (1, 2):
            text = self.command(
                command,
                2.5,
                lambda data: has_ack_for_subcommand(data, sub_cmd),
            )
            combined += text
            if find_config_ack(text, sub_cmd, value) is not None:
                self.require(label, True, f"attempts={attempt} sub=0x{sub_cmd:02X} val={value}")
                return combined
            if attempt == 1:
                time.sleep(0.25)
        self.require(label, False, f"ACK sub=0x{sub_cmd:02X} val={value} ausente")
        raise AssertionError("unreachable")

    def preflight(self) -> None:
        print("\n=== E15 PREFLIGHT ===")
        self._record(self.session.collect(2.0, quiet_s=0.10, max_bytes=32_768))
        probe = self.command(
            "probe", 2.5, lambda text: bool(PROBE_OK_RE.search(text)),
        )
        self.require("probe psoc=1", bool(PROBE_OK_RE.search(probe)))

        self.command("stop", 1.5, lambda text: "[USB] stop" in text)
        self.command("clear", 1.5, lambda text: "[USB] clear" in text)
        time.sleep(0.25)
        self.phase = "idle"
        status, diag, _ = self.status()
        self.require("estado inicial limpio", status.esp_state == 0 and (status.fill, status.target) == (0, 0),
                     f"state={status.esp_state} fill={status.fill}/{status.target}")
        self.require("Fs nativa", status.fs_hz == NATIVE_FS_HZ, f"fs={status.fs_hz}")
        self.require("UART inicial sin lotes malos", diag.b_bad == 0,
                     f"bBad={diag.b_bad} drop={diag.drops} badLen={diag.bad_len}")
        self.baseline = diag

        self.config("decim 1", CMD_SET_DECIMATION, 1, "decim=1 confirmado")
        self.config("stream 0", CMD_SELECT_STREAM, 0, "stream RAW confirmado")
        sdinfo = self.command("sdinfo", 4.0, lambda text: parse_sd_status(text) is not None)
        sd_status = parse_sd_status(sdinfo)
        self.require("SD presente y FAT montado", sd_status is not None and sd_status & SD_REQUIRED_READY == SD_REQUIRED_READY,
                     f"status=0x{(sd_status or 0):02X}")
        self.config("sdtest", SD_TEST_CMD, 1, "self-test FatFs confirmado")
        sdcap = self.config("sdcap 1", CMD_SD_CAPTURE, 1, "captura SD habilitada")
        status_after_be = parse_sd_status(sdcap)
        if status_after_be is None:
            more = self._record(self.session.collect(1.0, predicate=lambda text: parse_sd_status(text) is not None))
            sdcap += more
            status_after_be = parse_sd_status(sdcap)
        self.require(
            "status SD listo para 60000",
            status_after_be is not None and status_after_be & SD_REQUIRED_CAPTURE == SD_REQUIRED_CAPTURE,
            f"status=0x{(status_after_be or 0):02X}",
        )
        self.metadata["preflight"] = {
            "baseline_diag": diag.__dict__,
            "sd_status_before": sd_status,
            "sd_status_capture": status_after_be,
        }
        self.phase = "configured"

    def arm_and_sync(self) -> float:
        print("\n=== PRE 60000 -> ARMED -> SYNC ===")
        pre_started = time.monotonic()
        # Desde este punto un fallo puede dejar al PSoC UART-silencioso en
        # ARMED; no fingir que clear/stop alcanzan para recuperarlo.
        self.phase = "pre_sent"
        pre = self.command(
            f"pre {self.batches}", 2.5,
            lambda text: STORE_SD_RE.search(text) is not None and SETN_RE.search(text) is not None,
        )
        store = STORE_SD_RE.search(pre)
        setn = SETN_RE.search(pre)
        self.require("store SD sin clamp", bool(store and int(store.group("n")) == self.batches), pre[-200:])
        self.require("SETN max aceptado", bool(setn and int(setn.group("value")) == 255),
                     f"val={(setn.group('value') if setn else 'missing')}")
        self.require("PSoC IDLE al aceptar SETN", bool(PSOC_IDLE_RE.search(pre)))
        armed_text = pre
        while not ARMED_RE.search(armed_text) and time.monotonic() - pre_started < 5.0:
            armed_text += self._record(self.session.collect(0.5, quiet_s=0.05))
            blocked = forbidden_events(armed_text)
            if blocked:
                self.require("HOT_WAIT sin fallos", False, ", ".join(blocked))
        armed = ARMED_RE.search(armed_text)
        self.require("PSoC ARMED para 60000", bool(armed and int(armed.group("n")) == self.batches),
                     f"elapsed={time.monotonic() - pre_started:.3f}s")
        self.phase = "armed"

        sync_sent = time.monotonic()
        sync_text = self.command("sync", 2.0, lambda text: SYNC_RE.search(text) is not None)
        sync = SYNC_RE.search(sync_text)
        valid = bool(
            sync
            and int(sync.group("n")) == self.batches
            and (int(sync.group("before")), int(sync.group("after"))) == (0, 1)
        )
        self.require("SYNC 0->1 para 60000", valid, sync_text[-200:])
        self.phase = "sampling"
        self.metadata["capture"] = {
            "sync_elapsed_s": round(sync_sent - self.started, 3),
            "setn_ack_value": 255,
        }
        return sync_sent

    def wait_capture(self, sync_sent: float) -> None:
        print("\n=== SAMPLING SILENCIOSO (no se transmiten comandos) ===")
        deadline = sync_sent + CAPTURE_DEADLINE_S
        text = ""
        next_progress = sync_sent + 60.0
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            text += self._record(self.session.collect(min(1.0, remaining), quiet_s=0.05))
            blocked = forbidden_events(text)
            if blocked:
                self.require("captura sin eventos prohibidos", False, ", ".join(blocked))
            if not capture_errors(text, self.batches):
                break
            now = time.monotonic()
            if now >= next_progress:
                print(f"[WAIT] {now - sync_sent:.0f}/{CAPTURE_DEADLINE_S:.0f} s")
                next_progress += 60.0

        errors = capture_errors(text, self.batches)
        elapsed = time.monotonic() - sync_sent
        self.require("sesion SD 60000 completa", not errors, "; ".join(errors) or f"elapsed={elapsed:.3f}s")
        self.capture_complete = True
        self.phase = "captured"
        capture = self.metadata["capture"]
        assert isinstance(capture, dict)
        capture.update({
            "elapsed_to_complete_s": round(elapsed, 3),
            "sd_session": 1,
            "fill": self.batches,
            "target": self.batches,
            "forbidden_events": forbidden_events(text),
        })
        self._record(self.session.collect(0.30, quiet_s=0.05))

    def read_batch(self, seq: int) -> dict[str, object]:
        attempts: list[dict[str, object]] = []
        for attempt in range(1, SDREAD_ATTEMPTS + 1):
            text = self.command(
                f"sdread {seq}", SDREAD_ATTEMPT_S,
                lambda data: (
                    any(int(match.group("seq")) == seq for match in SDREAD_RE.finditer(data))
                    or bool(SDREAD_NACK_RE.search(data) or re.search(r"\bSD_ERROR\b", data, re.I))
                ),
            )
            timeouts = sum(1 for match in SDREAD_TIMEOUT_RE.finditer(text) if int(match.group("seq")) == seq)
            attempts.append({"attempt": attempt, "timeouts": timeouts})
            if SDREAD_NACK_RE.search(text) or re.search(r"\bSD_ERROR\b", text, re.I):
                self.require(f"sdread {seq} sin NACK/SD_ERROR", False, text[-200:])
            match = next((item for item in SDREAD_RE.finditer(text) if int(item.group("seq")) == seq), None)
            if match is not None:
                summary = {key: int(value, 16) if key == "flags" else int(value)
                           for key, value in match.groupdict().items()}
                self.require(f"sdread extremo {seq}", True, f"attempts={attempt} timeouts={sum(a['timeouts'] for a in attempts)}")
                return {"seq": seq, "attempts": attempts, "summary": summary}
        self.require(f"sdread extremo {seq}", False, f"sin respuesta tras {SDREAD_ATTEMPTS} intentos")
        raise AssertionError("unreachable")

    def validate_persisted(self) -> None:
        print("\n=== RECOVERY SD + EXTREMOS ===")
        sdinfo = self.command("sdinfo", 5.0, lambda text: parse_sd_status(text) is not None)
        sd_status = parse_sd_status(sdinfo)
        self.require(
            "sesion COMPLETE recuperable",
            sd_status is not None and sd_status & SD_REQUIRED_RECOVERED == SD_REQUIRED_RECOVERED,
            f"status=0x{(sd_status or 0):02X}",
        )
        reads = [self.read_batch(0), self.read_batch(self.batches - 1)]
        oor_text = self.command(
            f"sdread {self.batches}", 2.0,
            lambda text: SDREAD_OOR_RE.search(text) is not None,
        )
        oor = SDREAD_OOR_RE.search(oor_text)
        valid_oor = bool(
            oor
            and int(oor.group("seq")) == self.batches
            and int(oor.group("fill")) == self.batches
            and not any(int(item.group("seq")) == self.batches for item in SDREAD_RE.finditer(oor_text))
            and not re.search(r"\bSD_ERROR\b|SD_READ\s+NACK", oor_text, re.I)
        )
        self.require("sdread 60000 rechazado localmente", valid_oor, oor_text[-200:])
        self.metadata["persistence"] = {
            "sd_status_recovered": sd_status,
            "expected_blocks": (self.batches + 4) // 5,
            "expected_file_bytes": (1 + (self.batches + 4) // 5) * 512,
            "canonical_samples": self.batches * SAMPLES_PER_BATCH,
            "canonical_data_bytes": self.batches * SAMPLES_PER_BATCH * 3,
            "reads": reads,
            "oor_seq": self.batches,
        }
        self.phase = "validated"

    def cleanup(self) -> bool:
        """Limpia bookkeeping sólo cuando el PSoC ya terminó la sesión."""

        cleanup: dict[str, object] = {"attempted": False, "manual_reset_required": False}
        self.metadata["cleanup"] = cleanup
        if self.phase in ("armed", "sampling", "pre_sent") and not self.capture_complete:
            cleanup["manual_reset_required"] = True
            cleanup["reason"] = "PSoC puede seguir ARMED/SAMPLING; usar ToggleReset KitProg"
            print("[WARN] No se envía cleanup a una captura dudosa; se requiere ToggleReset KitProg.")
            return False
        if self.phase == "opened":
            return False

        cleanup["attempted"] = True
        errors: list[str] = []

        # 0xBE sólo cambia la política de la próxima captura; no borra ni
        # modifica GEOLAST.BIN. Restaurarlo antes de clear permite comprobar
        # además que la sesión COMPLETE sigue montada.
        try:
            restore = self.config("sdcap 0", CMD_SD_CAPTURE, 0, "cleanup captura SD deshabilitada")
            restored_status = parse_sd_status(restore)
            if restored_status is None:
                restore += self._record(
                    self.session.collect(1.0, predicate=lambda text: parse_sd_status(text) is not None)
                )
                restored_status = parse_sd_status(restore)
            restored = bool(
                restored_status is not None
                and restored_status & 0x31 == 0x31
                and not restored_status & 0x40
            )
            self.check(
                "cleanup GEOLAST preservado con SD capture=off",
                restored,
                f"status=0x{(restored_status or 0):02X}",
            )
            if not restored:
                errors.append("status SD no confirma COMPLETE con capture=off")
            cleanup.update({
                "sdcap_restored": restored,
                "sdcap_ack": 0,
                "sd_status_after_restore": restored_status,
            })
        except Exception as exc:
            cleanup["sdcap_restored"] = False
            errors.append(f"restore sdcap 0: {type(exc).__name__}: {exc}")

        try:
            clear = self.command("clear", 1.5, lambda text: "[USB] clear" in text)
            if "[USB] clear" not in clear:
                errors.append("clear sin confirmacion")
        except Exception as exc:
            errors.append(f"clear: {type(exc).__name__}: {exc}")

        time.sleep(0.25)
        try:
            probe = self.command(
                "probe", 2.5, lambda text: bool(PROBE_OK_RE.search(text)),
            )
            if not PROBE_OK_RE.search(probe):
                errors.append("probe final no reporta psoc=1")
        except Exception as exc:
            errors.append(f"probe final: {type(exc).__name__}: {exc}")

        try:
            status, diag, _ = self.status()
            baseline = self.baseline
            if status.esp_state != 0 or (status.fill, status.target) != (0, 0):
                errors.append(f"state={status.esp_state} fill={status.fill}/{status.target}")
            if status.fs_hz != NATIVE_FS_HZ:
                errors.append(f"fs={status.fs_hz}")
            if baseline:
                errors.extend(uart_integrity_errors(baseline, diag))
                cleanup["sync_drops_delta"] = diag.drops - baseline.drops
        except Exception as exc:
            errors.append(f"status final: {type(exc).__name__}: {exc}")
            diag = None

        cleanup.update({
            "probe_ok": not any("probe final" in error for error in errors),
            "final_diag": diag.__dict__ if diag is not None else None,
            "errors": errors,
        })
        self.check("cleanup final limpio", not errors, "; ".join(errors) or "SD off state=0 fill=0/0 fs=2604")
        if not errors:
            self.phase = "clean"
        return not errors

    def run(self) -> None:
        self.preflight()
        sync_sent = self.arm_and_sync()
        self.wait_capture(sync_sent)
        self.validate_persisted()

    def finish_metadata(self) -> None:
        self.metadata["checks"] = self.checks
        self.metadata["serial_log"] = self.lines
        self.metadata["last_phase"] = self.phase


def run_self_test() -> int:
    checks: list[tuple[str, bool]] = []

    def check(name: str, condition: bool) -> None:
        checks.append((name, condition))
        print(f"[{'PASS' if condition else 'FAIL'}] {name}")

    timing = timing_for(MAX_BATCHES)
    check("nominal 60000", abs(float(timing["nominal_sampling_s"]) - 691.2442396313364) < 1e-9)
    check("expectedCaptureMs", timing["expected_capture_ms"] == 693_745)
    check("capwait timeout", timing["capwait_timeout_ms"] == 695_745)
    check("fallback SD", timing["sd_stall_fallback_ms"] == 738_745)

    status_text = "[PSoC] SD status=0x73 present=1 type=1 selftest=0\n"
    check("status SD recuperado", parse_sd_status(status_text) == 0x73)
    restored_status = 0x33
    check(
        "status cleanup preserva COMPLETE y apaga capture",
        restored_status & 0x31 == 0x31 and not restored_status & 0x40,
    )
    diag_text = (
        "[SLAVE 1] USB_STATUS bOK=4 bBad=0 uartBytes=99 mark=1 drop=0 badLen=0 "
        "ping=1 diag=2 state=0 fill=0/0 fs=2604\n"
    )
    check("diag final", parse_diag(diag_text) == Diag(0, 0, 0, 0, 0, 0, 2604))

    baseline = Diag(0, 15_197, 0, 0, 0, 0, 2604)
    benign = Diag(0, 15_215, 0, 0, 0, 0, 2604)
    corrupt = Diag(1, 15_215, 0, 0, 0, 0, 2604)
    check("drops benignos no bloquean", not uart_integrity_errors(baseline, benign))
    check("bBad si bloquea", bool(uart_integrity_errors(baseline, corrupt)))

    good_capture = (
        "[PSoC] SD_SESSION val=0x01\n"
        "[SLAVE] START SD complete=1 err=0 fill=60000/60000\n"
    )
    check("criterio captura PASS", not capture_errors(good_capture, MAX_BATCHES))
    check("criterio detecta fill corto", bool(capture_errors(good_capture.replace("60000/60000", "59999/60000"), MAX_BATCHES)))
    check("criterio detecta SD_ERROR", "SD_ERROR" in capture_errors(good_capture + "[PSoC] SD_ERROR val=0x02\n", MAX_BATCHES))
    check("criterio detecta stall", "SYNC_STALL" in forbidden_events("[SLAVE] START_SYNC_STALL elapsed=1"))

    read = (
        "[USB] SDREAD seq=59999 raw[0]=-1 raw[29]=2 sumRaw=3 "
        "dig[0]=-4 dig[29]=5 flags[0]=0xA0\n"
    )
    check("parser sdread ultimo", any(int(match.group("seq")) == 59999 for match in SDREAD_RE.finditer(read)))
    oor = "[USB] sdread ignored: seq=60000 fuera de rango (fill=60000)\n"
    match = SDREAD_OOR_RE.search(oor)
    check("parser OOR exacto", bool(match and match.group("seq") == match.group("fill") == "60000"))
    check("timeout USB permitido aparte", bool(SDREAD_TIMEOUT_RE.search("PSoC SD_READ timeout seq=0 usb=1")))

    passed = sum(1 for _, ok in checks if ok)
    print(f"\nE15 self-test: {passed}/{len(checks)} PASS")
    return 0 if passed == len(checks) else 1


def parse_args(argv: Optional[list[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", default="COM12", help="Puerto USB del esclavo (default: COM12)")
    parser.add_argument("--batches", type=int, default=MAX_BATCHES, help="Debe ser exactamente 60000")
    parser.add_argument("--output", type=Path, help="JSON de evidencia (requerido salvo --self-test)")
    parser.add_argument("--verbose", action="store_true", help="Mostrar cada linea serial")
    parser.add_argument("--self-test", action="store_true", help="Pruebas offline; no abre COM")
    args = parser.parse_args(argv)
    if not args.self_test and args.output is None:
        parser.error("--output es requerido")
    if args.batches != MAX_BATCHES:
        parser.error("E15 exige --batches 60000")
    return args


def main(argv: Optional[list[str]] = None) -> int:
    args = parse_args(argv)
    if args.self_test:
        return run_self_test()

    metadata: dict[str, object] = {
        "schema_version": 1,
        "test": "E15_SD_MAX_60000",
        "status": "failed",
        "error": None,
        "started_at_utc": utc_now(),
        "port": args.port,
        "baud": BAUD,
        "dtr": False,
        "rts": False,
        "batches": args.batches,
        "timing": timing_for(args.batches),
    }
    session: Optional[SerialSession] = None
    runner: Optional[E15Runner] = None
    primary_ok = False
    cleanup_ok = False
    json_ok = False
    try:
        print(f"Abriendo {args.port} a {BAUD} baud (DTR=False, RTS=False)...")
        session = SerialSession.open(args.port)
        runner = E15Runner(session, args.batches, metadata, args.verbose)
        runner.run()
        primary_ok = True
    except KeyboardInterrupt:
        metadata["error"] = "KeyboardInterrupt"
        print("\n[FAIL] Corrida interrumpida")
    except Exception as exc:
        metadata["error"] = f"{type(exc).__name__}: {exc}"
        print(f"\n[FAIL] {metadata['error']}")
    finally:
        if runner is not None:
            cleanup_ok = runner.cleanup()
            runner.finish_metadata()
        if session is not None:
            session.close()
        metadata["completed_at_utc"] = utc_now()
        metadata["cleanup_ok"] = cleanup_ok
        if primary_ok and cleanup_ok:
            metadata["status"] = "ok"
            metadata["error"] = None
        elif primary_ok and metadata["error"] is None:
            metadata["error"] = "cleanup final fallido"
        try:
            assert args.output is not None
            atomic_json(args.output, metadata)
            print(f"Evidencia JSON: {args.output}")
            json_ok = True
        except Exception as exc:
            print(f"[FAIL] No se pudo escribir JSON atomico: {type(exc).__name__}: {exc}")

    return 0 if metadata["status"] == "ok" and json_ok else 1


if __name__ == "__main__":
    sys.exit(main())
