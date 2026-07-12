#!/usr/bin/env python3
"""Strict end-to-end acceptance capture over the master's raw WebSocket.

The probe deliberately has no third-party dependencies. It configures one GEO
slave for native-rate, SD-backed capture, performs a directed VER acquisition,
survives the master's intentional WiFi/WS outage, and streams the resulting
signed int24 samples to disk in little-endian order.

Example (10 minutes at the canonical native ADC rate)::

    python ws_capture_test.py --seconds 600 --fs 2604 \
        --output acceptance_10min.i24le

The companion ``.json`` file records the exact sample count, protocol evidence,
SHA-256, timing, reconnects, cleanup result, and any failure reason.
"""

from __future__ import annotations

import argparse
import base64
from collections import deque
from datetime import datetime, timezone
import hashlib
import io
import json
import math
import os
from pathlib import Path
import socket
import struct
import sys
from tempfile import TemporaryDirectory
import time
from typing import BinaryIO, Callable, Iterable


DEFAULT_HOST = "192.168.4.1"
DEFAULT_PORT = 80
DEFAULT_PATH = "/ws?takeover=1"
DEFAULT_FS_HZ = 2604.0
SAMPLES_PER_BATCH = 30
DEFAULT_BATCHES = 34
MAX_BATCHES = 60_000
MASTER_NODE_ID = 0xFF
GEO_HW_CLASS = 0

PKT_HEADER = 0x56
PKT_LEN = 6
PTYPE_DATA = 0x00
PTYPE_HEARTBEAT = 0x01
PTYPE_ACK = 0x07
PTYPE_STATUS = 0xFD
PTYPE_READY = 0xFE

CMD_ARM = 0xA2
CMD_STOP = 0xA4
CMD_STATUS = 0xA5
CMD_SET_RECLEN_HAMMER = 0xAD
CMD_SET_RECLEN = 0xAE
SUBCMD_VER = 0xB2
SUBCMD_DECIMATION = 0xBB
SUBCMD_SD_CAPTURE = 0xBE

MASTER_STATES = [
    "IDLE", "ARMING", "ARMED", "RUNNING", "STOPPING", "DUMPING",
    "PRESTART", "SCOPE_MULTI",
]
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MAX_WS_FRAME_BYTES = 16 * 1024 * 1024
DUMP_SILENCE_RECONNECT_S = 20.0
CONFIG_MAX_ATTEMPTS = 2
CONFIG_ACK_TIMEOUT_S = 2.0
CONFIG_RETRY_POLL_S = 0.25


class ProbeError(RuntimeError):
    """Acceptance failure with a concise user-facing reason."""


class ProtocolError(ProbeError):
    """Malformed HTTP, WebSocket, or six-byte master packet."""


class PhaseTimeout(ProbeError):
    """A required phase did not produce its evidence before its deadline."""


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def hex2(value: int) -> str:
    return f"{value & 0xFF:02X}"


def b64key() -> str:
    return base64.b64encode(os.urandom(16)).decode("ascii")


def resolve_batches(batches: int, seconds: float | None, fs_hz: float) -> int:
    """Resolve CLI duration to whole 30-sample batches, enforcing the limit."""

    if not math.isfinite(fs_hz) or fs_hz <= 0:
        raise ValueError("--fs must be a finite value > 0")
    if seconds is not None:
        if not math.isfinite(seconds) or seconds <= 0:
            raise ValueError("--seconds must be a finite value > 0")
        batches = math.ceil(seconds * fs_hz / SAMPLES_PER_BATCH)
    if batches < 1 or batches > MAX_BATCHES:
        raise ValueError(f"batches must be in 1..{MAX_BATCHES}")
    return int(batches)


def dump_silence_reconnect_due(silence_since: float | None, now: float) -> bool:
    """Return whether an active dump has been silent long enough to reconnect."""

    return (
        silence_since is not None
        and now - silence_since >= DUMP_SILENCE_RECONNECT_S
    )


def metadata_path_for(output_path: Path) -> Path:
    if output_path.suffix.lower() == ".json":
        return output_path.with_name(output_path.stem + ".metadata.json")
    if output_path.suffix:
        return output_path.with_suffix(".json")
    return output_path.with_name(output_path.name + ".json")


def open_capture_output(
    output_path: Path,
    metadata_path: Path,
    *,
    force: bool,
    unlink_metadata: Callable[[Path], None] | None = None,
) -> BinaryIO:
    """Invalidate stale metadata before creating or truncating the binary."""

    if force:
        if unlink_metadata is None:
            metadata_path.unlink(missing_ok=True)
        else:
            unlink_metadata(metadata_path)
    return output_path.open("wb")


def build_client_frame(
    payload: bytes,
    opcode: int = 0x1,
    mask_key: bytes | None = None,
) -> bytes:
    """Build one FIN client frame, including RFC 6455 extended lengths."""

    if mask_key is None:
        mask_key = os.urandom(4)
    if len(mask_key) != 4:
        raise ValueError("WebSocket mask must contain exactly four bytes")
    size = len(payload)
    first = 0x80 | (opcode & 0x0F)
    if size <= 125:
        header = bytes((first, 0x80 | size))
    elif size <= 0xFFFF:
        header = bytes((first, 0x80 | 126)) + struct.pack(">H", size)
    else:
        header = bytes((first, 0x80 | 127)) + struct.pack(">Q", size)
    masked = bytes(byte ^ mask_key[i & 3] for i, byte in enumerate(payload))
    return header + mask_key + masked


def send_text(sock: socket.socket, obj: object) -> int:
    """Send compact JSON, correctly framing payloads both below and above 125 B."""

    payload = json.dumps(obj, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    sock.sendall(build_client_frame(payload, opcode=0x1))
    return len(payload)


def send_control(sock: socket.socket, opcode: int, payload: bytes = b"") -> None:
    if len(payload) > 125:
        raise ValueError("WebSocket control payload exceeds 125 bytes")
    sock.sendall(build_client_frame(payload, opcode=opcode))


def handshake(sock: socket.socket, host: str, port: int, path: str, key: str) -> bytearray:
    host_header = host if port == 80 else f"{host}:{port}"
    request = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host_header}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n"
    )
    sock.sendall(request.encode("ascii"))
    response = bytearray()
    while b"\r\n\r\n" not in response:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("closed during WebSocket handshake")
        response.extend(chunk)
        if len(response) > 65_536:
            raise ProtocolError("WebSocket handshake headers exceed 64 KiB")

    split = response.index(b"\r\n\r\n") + 4
    raw_headers = bytes(response[:split]).decode("iso-8859-1")
    lines = raw_headers.split("\r\n")
    if not lines or " 101 " not in lines[0]:
        raise ProtocolError(f"WebSocket handshake rejected: {lines[0] if lines else 'empty response'}")
    headers: dict[str, str] = {}
    for line in lines[1:]:
        if ":" in line:
            name, value = line.split(":", 1)
            headers[name.strip().lower()] = value.strip()
    expected_accept = base64.b64encode(
        hashlib.sha1((key + WS_GUID).encode("ascii")).digest()
    ).decode("ascii")
    if headers.get("sec-websocket-accept") != expected_accept:
        raise ProtocolError("invalid Sec-WebSocket-Accept in handshake")
    return bytearray(response[split:])


def recv_exact(sock: socket.socket, size: int, buffer: bytearray) -> bytes:
    while len(buffer) < size:
        chunk = sock.recv(max(4096, size - len(buffer)))
        if not chunk:
            raise ConnectionError("closed mid-frame")
        buffer.extend(chunk)
    result = bytes(buffer[:size])
    del buffer[:size]
    return result


def read_frame(sock: socket.socket, buffer: bytearray) -> tuple[bool, int, bytes]:
    header = recv_exact(sock, 2, buffer)
    fin = bool(header[0] & 0x80)
    if header[0] & 0x70:
        raise ProtocolError("WebSocket RSV bits are unexpectedly set")
    opcode = header[0] & 0x0F
    masked = bool(header[1] & 0x80)
    size = header[1] & 0x7F
    if size == 126:
        size = struct.unpack(">H", recv_exact(sock, 2, buffer))[0]
    elif size == 127:
        size = struct.unpack(">Q", recv_exact(sock, 8, buffer))[0]
    if size > MAX_WS_FRAME_BYTES:
        raise ProtocolError(f"WebSocket frame too large: {size} bytes")
    if opcode >= 0x8 and (not fin or size > 125):
        raise ProtocolError("invalid fragmented/oversized WebSocket control frame")
    mask = recv_exact(sock, 4, buffer) if masked else b""
    payload = recv_exact(sock, size, buffer)
    if masked:
        payload = bytes(byte ^ mask[i & 3] for i, byte in enumerate(payload))
    return fin, opcode, payload


def iter_packets(payload: bytes) -> Iterable[bytes]:
    """Parse a grouped master WS payload without silently resynchronizing it."""

    if len(payload) % PKT_LEN:
        raise ProtocolError(
            f"binary WS payload length {len(payload)} is not a multiple of {PKT_LEN}"
        )
    for offset in range(0, len(payload), PKT_LEN):
        packet = payload[offset:offset + PKT_LEN]
        if packet[0] != PKT_HEADER:
            raise ProtocolError(
                f"bad packet header 0x{packet[0]:02X} at binary payload offset {offset}"
            )
        yield packet


def int24_le_from_packet(packet: bytes) -> bytes:
    """Convert wire b2:b1:b0 two's-complement int24 to little-endian bytes."""

    if len(packet) != PKT_LEN or packet[0] != PKT_HEADER or packet[2] != PTYPE_DATA:
        raise ProtocolError("int24 conversion received a non-DATA packet")
    return bytes((packet[5], packet[4], packet[3]))


def describe_packet(packet: bytes) -> str:
    node, ptype, b2, b1, b0 = packet[1:]
    if ptype == PTYPE_ACK:
        return f"ACK node={node} cmd=0x{b2:02X} val={b1}"
    if ptype == PTYPE_READY:
        return f"READY n={b2}"
    if ptype == PTYPE_HEARTBEAT:
        state = MASTER_STATES[b0] if b0 < len(MASTER_STATES) else f"?{b0}"
        return f"HEARTBEAT node={node} state={state}"
    if ptype == PTYPE_STATUS:
        return f"STATUS node={node} sub=0x{b2:02X} b1={b1} b0={b0}"
    return f"node={node} type=0x{ptype:02X} b2:b1:b0={b2:02X}:{b1:02X}:{b0:02X}"


class CaptureRunner:
    def __init__(
        self,
        args: argparse.Namespace,
        n_batches: int,
        output_stream: BinaryIO,
    ) -> None:
        self.args = args
        self.n_batches = n_batches
        self.expected_samples = n_batches * SAMPLES_PER_BATCH
        self.output_stream = output_stream
        self.sha256 = hashlib.sha256()

        self.started_monotonic = time.monotonic()
        self.started_utc = utc_now()
        self.sock: socket.socket | None = None
        self.ws_buffer = bytearray()
        self.connections = 0
        self.disconnects = 0
        self.connect_failures = 0
        self.last_disconnect_at: float | None = None
        self.last_connect_failure_log = 0.0
        self.fragment_opcode: int | None = None
        self.fragment_payload = bytearray()

        self.ack_events: deque[tuple[float, int, int, int]] = deque(maxlen=512)
        self.ready_events: deque[tuple[float, int]] = deque(maxlen=64)
        self.master_state: int | None = None
        self.master_state_history: list[dict[str, object]] = []
        self.packet_count = 0
        self.text_count = 0

        self.hello_accept_after: float | None = None
        self.hello: dict[str, int] = {}
        self.hello_times: dict[str, float] = {}

        self.capture_accepting = False
        self.ver_sent_at = 0.0
        self.target_data_packets_seen = 0
        self.samples_written = 0
        self.extra_samples = 0
        self.other_node_samples = 0
        self.stale_target_samples = 0
        self.stale_other_samples = 0
        self.first_data_at: float | None = None
        self.last_data_at: float | None = None
        self.dump_silence_since: float | None = None
        self.next_progress_sample = max(1, self.expected_samples // 100)
        self.progress_step = max(1, self.expected_samples // 100)
        self.last_progress_at = self.started_monotonic

        self.phase_seconds: dict[str, float] = {}
        self.config_retry_count = 0
        self.config_attempts: list[dict[str, object]] = []
        self.initial_cleanup_ok = False
        self.final_cleanup_ok = False
        self.cleanup_errors: list[str] = []

    def elapsed(self) -> float:
        return time.monotonic() - self.started_monotonic

    def log(self, message: str) -> None:
        print(f"[{self.elapsed():8.2f}s] {message}", flush=True)

    def _drop_socket(self, reason: str, *, count_disconnect: bool = True) -> None:
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None
            self.ws_buffer.clear()
            self.fragment_opcode = None
            self.fragment_payload.clear()
            if count_disconnect:
                self.disconnects += 1
                self.last_disconnect_at = time.monotonic()
                self.log(f"WS desconectado: {reason}")

    def _reconnect_if_dump_silent(self, now: float) -> bool:
        """Drop a connected half-open socket after the dump silence limit."""

        if (
            self.sock is None
            or self.samples_written >= self.expected_samples
            or not dump_silence_reconnect_due(self.dump_silence_since, now)
        ):
            return False
        # Dar a la nueva conexión una ventana completa. No adulterar
        # last_data_at: debe seguir representando DATA real.
        self.dump_silence_since = now
        self._drop_socket(
            f"sin DATA >={DUMP_SILENCE_RECONNECT_S:g}s durante dump; "
            "forzando reconexion"
        )
        return True

    def close(self) -> None:
        self._drop_socket("cierre local", count_disconnect=False)

    def _authenticate(self, deadline: float) -> None:
        if not self.args.token:
            return
        assert self.sock is not None
        send_text(self.sock, {"cmd": "AUTH", "token": self.args.token})
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            self.sock.settimeout(max(0.05, min(1.0, remaining)))
            fin, opcode, payload = read_frame(self.sock, self.ws_buffer)
            if opcode == 0x9:
                send_control(self.sock, 0xA, payload)
                continue
            if opcode == 0x8:
                raise ConnectionError("server closed during authentication")
            if opcode != 0x1 or not fin:
                continue
            try:
                message = json.loads(payload.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                continue
            if message.get("type") == "auth" and message.get("ok") is True:
                self.log("WS autenticado")
                return
            if message.get("type") == "auth" and message.get("ok") is False:
                raise ProbeError("WS authentication rejected")
        raise PhaseTimeout("WS authentication timed out")

    def _connect_once(self, deadline: float) -> None:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise PhaseTimeout("connection deadline expired")
        timeout = max(0.2, min(self.args.connect_timeout, remaining))
        sock = socket.create_connection((self.args.host, self.args.port), timeout=timeout)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.settimeout(timeout)
        key = b64key()
        buffer = handshake(sock, self.args.host, self.args.port, self.args.path, key)
        self.sock = sock
        self.ws_buffer = buffer
        self.connections += 1
        gap = ""
        if self.last_disconnect_at is not None:
            gap = f", corte {time.monotonic() - self.last_disconnect_at:.2f}s"
        self.log(f"WS conectado #{self.connections}{gap}")
        try:
            self._authenticate(min(deadline, time.monotonic() + self.args.phase_timeout))
        except Exception:
            self._drop_socket("fallo de autenticacion")
            raise

    def ensure_connected(self, deadline: float) -> None:
        while self.sock is None:
            now = time.monotonic()
            if now >= deadline:
                raise PhaseTimeout("no se pudo reconectar al WebSocket antes del deadline")
            try:
                self._connect_once(deadline)
                return
            except ProbeError:
                raise
            except (ConnectionError, OSError, socket.timeout) as exc:
                self.connect_failures += 1
                now = time.monotonic()
                if now - self.last_connect_failure_log >= 10.0:
                    self.last_connect_failure_log = now
                    self.log(f"WS aun no disponible ({exc}); reintentos={self.connect_failures}")
                time.sleep(min(self.args.reconnect_delay, max(0.0, deadline - now)))

    def _handle_text(self, payload: bytes) -> None:
        self.text_count += 1
        try:
            message = json.loads(payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            self.log(f"TEXT no JSON: {payload[:120]!r}")
            return
        if message.get("type") == "auth" and message.get("required") and not self.args.token:
            raise ProbeError("el maestro requiere autenticacion; usa --token")
        if message.get("type") == "auth" and message.get("ok") is False:
            raise ProbeError("WS authentication rejected")
        if message.get("type") == "busy":
            raise ProbeError(
                f"WebSocket ocupado: {message.get('reason', 'sin detalle')}"
            )
        if message.get("error"):
            raise ProbeError(f"error reportado por WS: {message['error']}")

    def _handle_message(self, opcode: int, payload: bytes) -> None:
        if opcode == 0x2:
            for packet in iter_packets(payload):
                self._process_packet(packet)
        elif opcode == 0x1:
            self._handle_text(payload)
        else:
            raise ProtocolError(f"unsupported WebSocket data opcode 0x{opcode:X}")

    def _handle_frame(self, fin: bool, opcode: int, payload: bytes) -> None:
        if opcode == 0x9:
            assert self.sock is not None
            send_control(self.sock, 0xA, payload)
            return
        if opcode == 0xA:
            return
        if opcode == 0x8:
            close_code = None
            close_reason = ""
            if len(payload) >= 2:
                close_code = struct.unpack(">H", payload[:2])[0]
                close_reason = payload[2:].decode("utf-8", errors="replace")
            detail = f" code={close_code}" if close_code is not None else ""
            if close_reason:
                detail += f" reason={close_reason}"
            if close_code in (1008, 4401):
                raise ProbeError(f"WebSocket rechazado:{detail}")
            raise ConnectionError(f"server sent CLOSE frame{detail}")
        if opcode == 0x0:
            if self.fragment_opcode is None:
                raise ProtocolError("unexpected WebSocket continuation frame")
            self.fragment_payload.extend(payload)
            if len(self.fragment_payload) > MAX_WS_FRAME_BYTES:
                raise ProtocolError("fragmented WebSocket message exceeds limit")
            if fin:
                original_opcode = self.fragment_opcode
                complete = bytes(self.fragment_payload)
                self.fragment_opcode = None
                self.fragment_payload.clear()
                self._handle_message(original_opcode, complete)
            return
        if opcode not in (0x1, 0x2):
            raise ProtocolError(f"unknown WebSocket opcode 0x{opcode:X}")
        if self.fragment_opcode is not None:
            raise ProtocolError("new WebSocket data frame while fragmented message is open")
        if fin:
            self._handle_message(opcode, payload)
        else:
            self.fragment_opcode = opcode
            self.fragment_payload = bytearray(payload)

    def poll_once(self, deadline: float) -> None:
        self.ensure_connected(deadline)
        assert self.sock is not None
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return
        self.sock.settimeout(max(0.05, min(1.0, remaining)))
        try:
            fin, opcode, payload = read_frame(self.sock, self.ws_buffer)
            self._handle_frame(fin, opcode, payload)
        except socket.timeout:
            return
        except (ConnectionError, BrokenPipeError, ConnectionResetError, OSError) as exc:
            self._drop_socket(str(exc))

    def _poll_until(self, deadline: float) -> None:
        """Drain frames until a short deadline, including unrelated traffic."""

        while time.monotonic() < deadline:
            self.poll_once(deadline)

    def _record_hello(self, name: str, value: int, now: float) -> None:
        previous = self.hello.get(name)
        self.hello[name] = value
        self.hello_times[name] = now
        if previous != value:
            self.log(f"HELLO fresco S{self.args.node}: {name}={value}")

    def _process_packet(self, packet: bytes) -> None:
        self.packet_count += 1
        now = time.monotonic()
        node, ptype, b2, b1, b0 = packet[1:]

        if ptype == PTYPE_DATA:
            if node == self.args.node:
                if not self.capture_accepting:
                    self.stale_target_samples += 1
                    return
                self.target_data_packets_seen += 1
                if self.target_data_packets_seen > self.expected_samples:
                    self.extra_samples += 1
                    raise ProbeError(
                        f"muestra extra del nodo {node}: esperadas {self.expected_samples}, "
                        f"recibidas al menos {self.target_data_packets_seen}"
                    )
                sample_bytes = int24_le_from_packet(packet)
                written = self.output_stream.write(sample_bytes)
                if written != 3:
                    raise OSError(f"short output write: {written}/3 bytes")
                self.sha256.update(sample_bytes)
                self.samples_written += 1
                if self.first_data_at is None:
                    self.first_data_at = now
                    self.log("comenzo el dump DATA del nodo objetivo")
                self.last_data_at = now
                self.dump_silence_since = now
                self._report_progress(now)
            else:
                if self.capture_accepting:
                    self.other_node_samples += 1
                    raise ProbeError(
                        f"DATA inesperado del nodo {node} durante captura dirigida a S{self.args.node}"
                    )
                self.stale_other_samples += 1
            return

        if ptype == PTYPE_ACK:
            self.ack_events.append((now, node, b2, b1))
            if b2 in (
                CMD_ARM, CMD_STOP, CMD_SET_RECLEN_HAMMER, CMD_SET_RECLEN,
                SUBCMD_VER, SUBCMD_DECIMATION, SUBCMD_SD_CAPTURE,
            ):
                self.log(describe_packet(packet))
            if (
                self.capture_accepting
                and node == self.args.node
                and b2 == SUBCMD_VER
                and now >= self.ver_sent_at
            ):
                if b1 == 0:
                    raise ProbeError("VER fue rechazado por el esclavo (ACK B2=0)")
                if b1 == 2 and self.dump_silence_since is None:
                    # B2=2 confirma que el maestro entró a dump. El watchdog
                    # debe correr desde este instante aunque todavía no haya
                    # llegado el primer DATA por este socket.
                    self.dump_silence_since = now
            return

        if ptype == PTYPE_READY:
            self.ready_events.append((now, b2))
            self.log(describe_packet(packet))
            return

        if ptype == PTYPE_HEARTBEAT and node == 0:
            if self.master_state != b0:
                self.master_state = b0
                state_name = MASTER_STATES[b0] if b0 < len(MASTER_STATES) else f"?{b0}"
                self.master_state_history.append(
                    {"elapsed_s": round(self.elapsed(), 3), "state": b0, "name": state_name}
                )
                self.log(f"master state={state_name}")
            return

        if (
            ptype == PTYPE_STATUS
            and node == self.args.node
            and self.hello_accept_after is not None
            and now >= self.hello_accept_after
        ):
            if b2 == 0x01:
                self._record_hello("psoc_ok", b1, now)
                self._record_hello("legacy_fs_hz", b0 * 100, now)
            elif b2 == 0x05:
                self._record_hello("fs_exact_hz", (b1 << 8) | b0, now)
            elif b2 == 0x06:
                self._record_hello("hw_class", b1, now)
            elif b2 == 0x07:
                self._record_hello("sd_present", b1, now)

    def _report_progress(self, now: float) -> None:
        due_count = self.samples_written >= self.next_progress_sample
        due_time = now - self.last_progress_at >= self.args.progress_interval
        if not due_count and not due_time and self.samples_written != self.expected_samples:
            return
        while self.next_progress_sample <= self.samples_written:
            self.next_progress_sample += self.progress_step
        self.last_progress_at = now
        elapsed_dump = max(1e-3, now - (self.first_data_at or now))
        rate = self.samples_written / elapsed_dump
        remaining = self.expected_samples - self.samples_written
        eta = remaining / rate if rate > 0 else math.inf
        percent = self.samples_written * 100.0 / self.expected_samples
        eta_text = f"{eta:.1f}s" if math.isfinite(eta) else "--"
        self.output_stream.flush()
        self.log(
            f"progreso {self.samples_written}/{self.expected_samples} "
            f"({percent:6.2f}%), {rate:,.0f} muestras/s, ETA {eta_text}"
        )

    def _send(self, command: dict[str, object], deadline: float, label: str) -> float:
        self.ensure_connected(deadline)
        assert self.sock is not None
        sent_at = time.monotonic()
        try:
            send_text(self.sock, command)
        except OSError as exc:
            self._drop_socket(f"fallo enviando {label}: {exc}")
            raise ProbeError(f"entrega incierta de {label}; no se reenvia para evitar duplicados") from exc
        self.log(f">>> {label}: {json.dumps(command, separators=(',', ':'))}")
        return sent_at

    def _fresh_ack_events(self, node: int, command: int, since: float) -> list[tuple[float, int]]:
        return [
            (at, value)
            for at, event_node, event_cmd, value in self.ack_events
            if at >= since and event_node == node and event_cmd == command
        ]

    def wait_ack(
        self,
        node: int,
        command: int,
        expected_value: int,
        since: float,
        timeout: float,
        label: str,
    ) -> int:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            events = self._fresh_ack_events(node, command, since)
            if events:
                value = events[0][1]
                if value != expected_value:
                    raise ProbeError(
                        f"{label}: ACK 0x{command:02X}={value}, esperado {expected_value}"
                    )
                return value
            self.poll_once(deadline)
        raise PhaseTimeout(f"{label}: falta ACK 0x{command:02X}={expected_value}")

    def _cleanup_command(
        self,
        command: dict[str, object],
        ack_command: int,
        label: str,
        deadline: float,
    ) -> None:
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            attempt_deadline = min(deadline, time.monotonic() + self.args.phase_timeout)
            try:
                sent_at = self._send(command, attempt_deadline, label)
                self.wait_ack(
                    MASTER_NODE_ID,
                    ack_command,
                    0,
                    sent_at,
                    max(0.1, attempt_deadline - time.monotonic()),
                    label,
                )
                return
            except (PhaseTimeout, ProbeError, OSError) as exc:
                last_error = exc
                if time.monotonic() >= deadline:
                    break
                self._drop_socket(f"reintento cleanup {label}")
                time.sleep(min(self.args.reconnect_delay, max(0.0, deadline - time.monotonic())))
        raise ProbeError(f"{label} no confirmado: {last_error}")

    def cleanup(self, label: str) -> None:
        self.capture_accepting = False
        started = time.monotonic()
        deadline = started + self.args.phase_timeout
        self.log(f"fase {label}: STOP + AE=0 + AD=0")
        self._cleanup_command(
            {"cmd": hex2(CMD_STOP), "param": 0}, CMD_STOP, f"{label}/STOP", deadline
        )
        self._cleanup_command(
            {"cmd": hex2(CMD_SET_RECLEN), "value": 0},
            CMD_SET_RECLEN,
            f"{label}/AE=0",
            deadline,
        )
        # AD is a 16-bit command in web_relay.h, just like AE.  Use ``value``
        # explicitly so cleanup keeps working if the legacy ``param`` fallback
        # is removed later.
        self._cleanup_command(
            {"cmd": hex2(CMD_SET_RECLEN_HAMMER), "value": 0},
            CMD_SET_RECLEN_HAMMER,
            f"{label}/AD=0",
            deadline,
        )
        settle_deadline = min(deadline, time.monotonic() + self.args.cleanup_settle)
        while time.monotonic() < settle_deadline:
            self.poll_once(settle_deadline)
        self.phase_seconds[label] = round(time.monotonic() - started, 3)

    def _configure_directed_subcommand(
        self,
        subcommand: int,
        label: str,
        deadline: float,
    ) -> None:
        command = {
            "cmd": "BD",
            "node": self.args.node,
            "sub": hex2(subcommand),
            "param": 1,
        }
        for attempt in range(1, CONFIG_MAX_ATTEMPTS + 1):
            attempt_label = f"{label} intento {attempt}/{CONFIG_MAX_ATTEMPTS}"
            sent_at = self._send(command, deadline, attempt_label)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise PhaseTimeout(f"{label}: deadline agotado tras el envio")
            try:
                self.wait_ack(
                    self.args.node,
                    subcommand,
                    1,
                    sent_at,
                    min(CONFIG_ACK_TIMEOUT_S, remaining),
                    attempt_label,
                )
            except (PhaseTimeout, ProbeError) as exc:
                self.config_attempts.append(
                    {
                        "node": self.args.node,
                        "subcommand": f"0x{subcommand:02X}",
                        "attempt": attempt,
                        "status": "error",
                        "error": f"{type(exc).__name__}: {exc}",
                    }
                )
                if attempt == CONFIG_MAX_ATTEMPTS:
                    self.log(f"{label}: fallo persistente tras {attempt} intentos: {exc}")
                    raise
                self.config_retry_count += 1
                self.log(
                    f"{label}: intento {attempt} no confirmado ({exc}); "
                    f"reintentando una vez"
                )
                poll_deadline = min(
                    deadline,
                    time.monotonic() + CONFIG_RETRY_POLL_S,
                )
                if time.monotonic() < poll_deadline:
                    # Drena un ACK tardio del intento anterior. El siguiente
                    # wait_ack usa el nuevo sent_at y no puede aceptarlo.
                    self._poll_until(poll_deadline)
                continue
            self.config_attempts.append(
                {
                    "node": self.args.node,
                    "subcommand": f"0x{subcommand:02X}",
                    "attempt": attempt,
                    "status": "ok",
                }
            )
            return

    def configure_slave(self) -> None:
        started = time.monotonic()
        deadline = started + self.args.phase_timeout
        self._configure_directed_subcommand(
            SUBCMD_DECIMATION,
            f"S{self.args.node} decim BB=1",
            deadline,
        )
        self._configure_directed_subcommand(
            SUBCMD_SD_CAPTURE,
            f"S{self.args.node} SD BE=1",
            deadline,
        )
        self.phase_seconds["configure"] = round(time.monotonic() - started, 3)

    def wait_fresh_geo_hello(self) -> None:
        started = time.monotonic()
        deadline = started + self.args.phase_timeout
        self.hello.clear()
        self.hello_times.clear()
        self.hello_accept_after = time.monotonic() + self.args.hello_fresh_delay
        self._send(
            {"cmd": hex2(CMD_STATUS), "param": 0},
            deadline,
            "STATUS para HELLO fresco",
        )
        required = {"psoc_ok", "fs_exact_hz", "hw_class", "sd_present"}
        while time.monotonic() < deadline and not required.issubset(self.hello):
            self.poll_once(deadline)
        missing = sorted(required.difference(self.hello))
        if missing:
            raise PhaseTimeout(f"HELLO fresco incompleto; faltan {', '.join(missing)}")
        if self.hello["psoc_ok"] != 1:
            raise ProbeError(f"HELLO: PSoC no disponible (psoc_ok={self.hello['psoc_ok']})")
        if self.hello["hw_class"] != GEO_HW_CLASS:
            raise ProbeError(
                f"HELLO: S{self.args.node} no es GEO (hw_class={self.hello['hw_class']})"
            )
        if self.hello["sd_present"] != 1:
            raise ProbeError("HELLO: GEO no reporta SD presente")
        expected_fs = int(round(self.args.fs))
        if self.hello["fs_exact_hz"] != expected_fs:
            raise ProbeError(
                f"HELLO: Fs exacta {self.hello['fs_exact_hz']} Hz, esperada {expected_fs} Hz"
            )
        self.log(
            f"HELLO aceptado: GEO, SD presente, PSoC OK, Fs={self.hello['fs_exact_hz']} Hz"
        )
        self.phase_seconds["fresh_hello"] = round(time.monotonic() - started, 3)

    def arm(self) -> None:
        started = time.monotonic()
        deadline = started + self.args.phase_timeout
        sent = self._send(
            {"cmd": hex2(CMD_ARM), "param": 1}, deadline, "ARM n=1"
        )
        self.wait_ack(
            MASTER_NODE_ID,
            CMD_ARM,
            0,
            sent,
            max(0.1, deadline - time.monotonic()),
            "ARM",
        )
        while time.monotonic() < deadline:
            ready = [count for at, count in self.ready_events if at >= sent]
            if ready:
                if ready[-1] < 1:
                    raise ProbeError(f"ARM produjo READY={ready[-1]}, esperado >=1")
                self.phase_seconds["arm"] = round(time.monotonic() - started, 3)
                return
            self.poll_once(deadline)
        raise PhaseTimeout("ARM: no llego READY fresco")

    def set_record_length(self) -> None:
        started = time.monotonic()
        deadline = started + self.args.phase_timeout
        sent = self._send(
            {"cmd": hex2(CMD_SET_RECLEN), "value": self.n_batches},
            deadline,
            f"SET_RECLEN={self.n_batches}",
        )
        self.wait_ack(
            MASTER_NODE_ID,
            CMD_SET_RECLEN,
            self.n_batches & 0xFF,
            sent,
            max(0.1, deadline - time.monotonic()),
            "SET_RECLEN",
        )
        self.phase_seconds["set_reclen"] = round(time.monotonic() - started, 3)

    def _ver_statuses(self) -> set[int]:
        return {
            value
            for at, node, command, value in self.ack_events
            if at >= self.ver_sent_at and node == self.args.node and command == SUBCMD_VER
        }

    def capture(self) -> None:
        started = time.monotonic()
        capture_seconds = self.expected_samples / self.args.fs
        capture_timeout = self.args.capture_timeout
        if capture_timeout <= 0:
            capture_timeout = capture_seconds + max(180.0, capture_seconds * 0.35)
        dump_timeout = self.args.dump_timeout
        if dump_timeout <= 0:
            # El stop-and-wait PSoC-SD -> UART -> ESP-NOW -> WS medido en el
            # GEO real entrega ~950 muestras/s. Presupuestar 600 muestras/s
            # más 180 s evita que una captura válida de 10 min expire por un
            # supuesto optimista de laboratorio.
            dump_timeout = max(300.0, self.expected_samples / 600.0 + 180.0)

        self.capture_accepting = True
        command_deadline = time.monotonic() + self.args.phase_timeout
        self.ver_sent_at = self._send(
            {
                "cmd": "BD",
                "node": self.args.node,
                "sub": hex2(SUBCMD_VER),
                "param": 1,
            },
            command_deadline,
            f"VER S{self.args.node} n={self.n_batches}",
        )
        capture_deadline = self.ver_sent_at + capture_timeout
        dump_deadline: float | None = None
        next_wait_log = time.monotonic() + 30.0
        self.log(
            f"captura esperada {capture_seconds:.2f}s; timeout captura={capture_timeout:.1f}s, "
            f"timeout dump={dump_timeout:.1f}s"
        )

        while True:
            statuses = self._ver_statuses()
            if 0 in statuses:
                raise ProbeError("VER rechazo/aborto reportado por ACK B2=0")
            if (2 in statuses or self.first_data_at is not None) and dump_deadline is None:
                dump_deadline = time.monotonic() + dump_timeout
                self.log("fase dump activa")
            if self.samples_written == self.expected_samples and {1, 2}.issubset(statuses):
                break

            now = time.monotonic()
            active_deadline = dump_deadline if dump_deadline is not None else capture_deadline
            if now >= active_deadline:
                missing_status = sorted({1, 2}.difference(statuses))
                raise PhaseTimeout(
                    "VER/captura timeout: "
                    f"muestras={self.samples_written}/{self.expected_samples}, "
                    f"ACK_B2={sorted(statuses)}, faltan={missing_status}"
                )
            if now >= next_wait_log and self.first_data_at is None:
                next_wait_log = now + 30.0
                self.log(
                    f"esperando fin de captura/reconexion; ACK_B2={sorted(statuses)}, "
                    f"faltan ~{max(0.0, capture_deadline - now):.0f}s de deadline"
                )
            self.poll_once(active_deadline)
            # Watchdog de silencio (F5, 2026-07-12): un socket medio-muerto
            # (WiFi drop sin FIN) hace que poll_once devuelva timeout limpio
            # para siempre y el tool esperaba TODO el dump_timeout (46 min a
            # 52080 lotes) sin reconectar, mientras el maestro pausaba el dump
            # por falta de cliente. El reloj arranca con ACK B2=2 (o con el
            # primer DATA, si llega antes), de modo que también cubre el hueco
            # half-open entre entrar a dump y recibir la primera muestra.
            watchdog_now = time.monotonic()
            self._reconnect_if_dump_silent(watchdog_now)

        self.log(
            f"conteo exacto alcanzado ({self.samples_written}); drenando "
            f"{self.args.extra_grace:.1f}s para detectar extras"
        )
        grace_deadline = time.monotonic() + self.args.extra_grace
        while time.monotonic() < grace_deadline:
            self.poll_once(grace_deadline)
        self.capture_accepting = False

        statuses = self._ver_statuses()
        if statuses.intersection({0}) or not {1, 2}.issubset(statuses):
            raise ProbeError(f"secuencia VER incompleta: ACK_B2={sorted(statuses)}")
        if self.samples_written != self.expected_samples:
            raise ProbeError(
                f"conteo final {self.samples_written}, esperado {self.expected_samples}"
            )
        if self.extra_samples or self.other_node_samples:
            raise ProbeError(
                f"datos inesperados: extras={self.extra_samples}, otros={self.other_node_samples}"
            )
        self.output_stream.flush()
        self.phase_seconds["capture_and_dump"] = round(time.monotonic() - started, 3)

    def execute(self) -> None:
        self.cleanup("initial_cleanup")
        self.initial_cleanup_ok = True
        self.configure_slave()
        self.wait_fresh_geo_hello()
        self.arm()
        self.set_record_length()
        self.capture()

    def metadata(self, *, success: bool, error: str | None, output_path: Path) -> dict[str, object]:
        now = time.monotonic()
        ver_statuses = sorted(self._ver_statuses()) if self.ver_sent_at else []
        return {
            "schema_version": 1,
            "status": "ok" if success else "error",
            "error": error,
            "started_at_utc": self.started_utc,
            "completed_at_utc": utc_now(),
            "elapsed_s": round(now - self.started_monotonic, 3),
            "host": self.args.host,
            "port": self.args.port,
            "path": self.args.path,
            "node": self.args.node,
            "fs_hz": self.args.fs,
            "requested_seconds": self.args.seconds,
            "batches": self.n_batches,
            "samples_per_batch": SAMPLES_PER_BATCH,
            "expected_samples": self.expected_samples,
            "target_data_packets_seen": self.target_data_packets_seen,
            "samples_written": self.samples_written,
            "extra_samples": self.extra_samples,
            "other_node_samples": self.other_node_samples,
            "stale_target_samples_before_capture": self.stale_target_samples,
            "stale_other_samples_before_capture": self.stale_other_samples,
            "bytes_written": self.samples_written * 3,
            "sample_format": "signed_int24_le_twos_complement",
            "output_file": str(output_path.resolve()),
            "sha256": self.sha256.hexdigest(),
            "decimation_factor": 1,
            "sd_capture_enabled": True,
            "hello": dict(self.hello),
            "ver_ack_values": ver_statuses,
            "master_state_history": self.master_state_history,
            "connections": self.connections,
            "disconnects": self.disconnects,
            "connect_failures": self.connect_failures,
            "ws_packets_received": self.packet_count,
            "ws_text_messages_received": self.text_count,
            "initial_cleanup_ok": self.initial_cleanup_ok,
            "final_cleanup_ok": self.final_cleanup_ok,
            "cleanup_errors": self.cleanup_errors,
            "config_retry_count": self.config_retry_count,
            "config_attempts": self.config_attempts,
            "phase_seconds": self.phase_seconds,
        }


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Strict GEO WS capture: BB=1, BE=1, fresh GEO+SD HELLO, "
            "ARM/AE/VER, exact streaming int24-le output"
        )
    )
    parser.add_argument("-n", "--batches", type=int, default=DEFAULT_BATCHES)
    parser.add_argument(
        "--seconds",
        type=float,
        default=None,
        help="capture duration converted with ceil(seconds*fs/30)",
    )
    parser.add_argument(
        "--fs",
        type=float,
        default=DEFAULT_FS_HZ,
        help=f"expected exact Fs and duration conversion rate (default: {DEFAULT_FS_HZ:g})",
    )
    parser.add_argument("--output", type=Path, help="streaming signed-int24-le output file")
    parser.add_argument("--force", action="store_true", help="overwrite output/metadata files")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--path", default=DEFAULT_PATH)
    parser.add_argument("--node", type=int, default=1, help="target GEO node (default: 1)")
    parser.add_argument(
        "--token",
        default=os.environ.get("GEO_WS_TOKEN", ""),
        help="optional WS auth token (or GEO_WS_TOKEN environment variable)",
    )
    parser.add_argument("--phase-timeout", type=float, default=30.0)
    parser.add_argument(
        "--capture-timeout",
        type=float,
        default=0.0,
        help="seconds; 0 derives nominal duration plus a generous margin",
    )
    parser.add_argument(
        "--dump-timeout",
        type=float,
        default=0.0,
        help="seconds; 0 derives a streaming-size-based timeout",
    )
    parser.add_argument("--connect-timeout", type=float, default=5.0)
    parser.add_argument("--reconnect-delay", type=float, default=1.0)
    parser.add_argument("--hello-fresh-delay", type=float, default=1.0)
    parser.add_argument("--cleanup-settle", type=float, default=1.0)
    parser.add_argument("--extra-grace", type=float, default=3.0)
    parser.add_argument("--progress-interval", type=float, default=5.0)
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run offline protocol/counting unit checks and exit",
    )
    return parser.parse_args(argv)


def validate_args(args: argparse.Namespace) -> tuple[int, Path, Path]:
    n_batches = resolve_batches(args.batches, args.seconds, args.fs)
    if args.output is None:
        raise ValueError("--output is required unless --self-test is used")
    if args.node < 1 or args.node > 254:
        raise ValueError("--node must be in 1..254")
    if args.port < 1 or args.port > 65_535:
        raise ValueError("--port must be in 1..65535")
    for name in (
        "phase_timeout", "connect_timeout", "reconnect_delay", "hello_fresh_delay",
        "cleanup_settle", "extra_grace", "progress_interval",
    ):
        value = getattr(args, name)
        if not math.isfinite(value) or value <= 0:
            raise ValueError(f"--{name.replace('_', '-')} must be finite and > 0")
    for name in ("capture_timeout", "dump_timeout"):
        value = getattr(args, name)
        if not math.isfinite(value) or value < 0:
            raise ValueError(f"--{name.replace('_', '-')} must be finite and >= 0")

    output_path = args.output.expanduser()
    metadata_path = metadata_path_for(output_path)
    if output_path.resolve() == metadata_path.resolve():
        raise ValueError("binary output and metadata paths collide")
    if not args.force:
        existing = [str(path) for path in (output_path, metadata_path) if path.exists()]
        if existing:
            raise ValueError("refusing to overwrite existing file(s): " + ", ".join(existing))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    metadata_path.parent.mkdir(parents=True, exist_ok=True)
    return n_batches, output_path, metadata_path


def _decode_test_client_frame(frame: bytes) -> tuple[int, bytes]:
    opcode = frame[0] & 0x0F
    size_code = frame[1] & 0x7F
    offset = 2
    if size_code == 126:
        size = struct.unpack(">H", frame[offset:offset + 2])[0]
        offset += 2
    elif size_code == 127:
        size = struct.unpack(">Q", frame[offset:offset + 8])[0]
        offset += 8
    else:
        size = size_code
    mask = frame[offset:offset + 4]
    offset += 4
    payload = bytes(byte ^ mask[i & 3] for i, byte in enumerate(frame[offset:]))
    if len(payload) != size:
        raise AssertionError(f"decoded test payload {len(payload)} != header {size}")
    return opcode, payload


def run_self_tests() -> None:
    assert resolve_batches(1, 600.0, 2604.0) == 52_080
    assert resolve_batches(60_000, None, 2604.0) == 60_000
    try:
        resolve_batches(60_001, None, 2604.0)
    except ValueError:
        pass
    else:
        raise AssertionError("60001 batches must be rejected")

    deterministic_mask = b"\x01\x02\x03\x04"
    for size in (0, 125, 126, 65_535, 65_536):
        payload = bytes((i & 0xFF for i in range(size)))
        opcode, decoded = _decode_test_client_frame(
            build_client_frame(payload, opcode=0x1, mask_key=deterministic_mask)
        )
        assert opcode == 0x1 and decoded == payload

    class FakeSocket:
        def __init__(self) -> None:
            self.sent = bytearray()

        def sendall(self, data: bytes) -> None:
            self.sent.extend(data)

    fake = FakeSocket()
    payload_len = send_text(fake, {"cmd": "TEST", "padding": "x" * 200})  # type: ignore[arg-type]
    assert payload_len > 125
    opcode, decoded = _decode_test_client_frame(bytes(fake.sent))
    assert opcode == 0x1 and json.loads(decoded)["padding"] == "x" * 200

    p_pos = bytes((PKT_HEADER, 1, PTYPE_DATA, 0x7F, 0xFF, 0xFF))
    p_neg = bytes((PKT_HEADER, 1, PTYPE_DATA, 0x80, 0x00, 0x00))
    p_m1 = bytes((PKT_HEADER, 1, PTYPE_DATA, 0xFF, 0xFF, 0xFF))
    assert int24_le_from_packet(p_pos) == b"\xFF\xFF\x7F"
    assert int24_le_from_packet(p_neg) == b"\x00\x00\x80"
    assert int24_le_from_packet(p_m1) == b"\xFF\xFF\xFF"
    assert list(iter_packets(p_pos + p_neg)) == [p_pos, p_neg]
    try:
        list(iter_packets(p_pos + b"\x00"))
    except ProtocolError:
        pass
    else:
        raise AssertionError("misaligned grouped payload must fail")
    assert metadata_path_for(Path("capture.i24le")) == Path("capture.json")
    assert metadata_path_for(Path("capture")) == Path("capture.json")

    with TemporaryDirectory() as temporary_directory:
        root = Path(temporary_directory)
        output_path = root / "capture.i24le"
        metadata_path = metadata_path_for(output_path)
        output_path.write_bytes(b"stale-binary-payload")
        metadata_path.write_text('{"success":true}\n', encoding="utf-8")
        with open_capture_output(
            output_path,
            metadata_path,
            force=True,
        ) as output_stream:
            assert not metadata_path.exists()
            assert output_path.stat().st_size == 0
            output_stream.write(b"new")
        assert output_path.read_bytes() == b"new"

        original_binary = b"must-remain-untouched"
        original_metadata = '{"success":false}\n'
        output_path.write_bytes(original_binary)
        metadata_path.write_text(original_metadata, encoding="utf-8")
        unlink_calls: list[Path] = []

        def reject_metadata_unlink(path: Path) -> None:
            unlink_calls.append(path)
            raise PermissionError("simulated metadata unlink failure")

        try:
            open_capture_output(
                output_path,
                metadata_path,
                force=True,
                unlink_metadata=reject_metadata_unlink,
            )
        except PermissionError:
            pass
        else:
            raise AssertionError("metadata unlink failure must abort before output open")
        assert unlink_calls == [metadata_path]
        assert output_path.read_bytes() == original_binary
        assert metadata_path.read_text(encoding="utf-8") == original_metadata

    def fake_config_runner(
        outcomes: list[int | Exception],
    ) -> tuple[
        CaptureRunner,
        list[dict[str, object]],
        list[float],
        list[float],
    ]:
        runner = CaptureRunner(argparse.Namespace(node=1), 1, io.BytesIO())
        runner.log = lambda _message: None  # type: ignore[method-assign]
        send_calls: list[dict[str, object]] = []
        wait_since: list[float] = []
        poll_deadlines: list[float] = []
        pending = deque(outcomes)

        def fake_send(command: dict[str, object], _deadline: float, label: str) -> float:
            send_calls.append({"command": dict(command), "label": label})
            return 100.0 + len(send_calls)

        def fake_wait_ack(
            node: int,
            command: int,
            expected_value: int,
            since: float,
            _timeout: float,
            _label: str,
        ) -> int:
            assert node == 1 and command == SUBCMD_DECIMATION and expected_value == 1
            wait_since.append(since)
            outcome = pending.popleft()
            if isinstance(outcome, Exception):
                raise outcome
            return outcome

        runner._send = fake_send  # type: ignore[method-assign]
        runner.wait_ack = fake_wait_ack  # type: ignore[method-assign]
        runner._poll_until = poll_deadlines.append  # type: ignore[method-assign]
        return runner, send_calls, wait_since, poll_deadlines

    # Regresion F6: ACK=0 transitorio se drena y cada intento acepta solamente
    # ACK posteriores a su propio envio.
    retry_runner, retry_sends, retry_since, retry_polls = fake_config_runner(
        [ProbeError("ACK BB=0 transitorio"), 1]
    )
    retry_runner._configure_directed_subcommand(
        SUBCMD_DECIMATION,
        "test BB=1",
        time.monotonic() + 10.0,
    )
    assert len(retry_sends) == 2 and retry_since == [101.0, 102.0]
    assert len(retry_polls) == 1 and retry_runner.config_retry_count == 1
    assert [item["status"] for item in retry_runner.config_attempts] == ["error", "ok"]

    persistent_timeout = PhaseTimeout("segundo ACK ausente")
    fail_runner, fail_sends, fail_since, fail_polls = fake_config_runner(
        [ProbeError("primer ACK BB=0"), persistent_timeout]
    )
    try:
        fail_runner._configure_directed_subcommand(
            SUBCMD_DECIMATION,
            "test BB=1 persistente",
            time.monotonic() + 10.0,
        )
    except PhaseTimeout as exc:
        assert exc is persistent_timeout
    else:
        raise AssertionError("dos fallos de configuracion deben propagarse")
    assert len(fail_sends) == 2 and fail_since == [101.0, 102.0]
    assert len(fail_polls) == 1 and fail_runner.config_retry_count == 1
    assert [item["status"] for item in fail_runner.config_attempts] == ["error", "error"]

    ok_runner, ok_sends, ok_since, ok_polls = fake_config_runner([1])
    ok_runner._configure_directed_subcommand(
        SUBCMD_DECIMATION,
        "test BB=1 directo",
        time.monotonic() + 10.0,
    )
    assert len(ok_sends) == 1 and ok_since == [101.0]
    assert not ok_polls and ok_runner.config_retry_count == 0
    assert [item["status"] for item in ok_runner.config_attempts] == ["ok"]

    # Regresión F5: B2=2 activa el reloj aun antes del primer DATA. Este es
    # exactamente el estado que antes quedaba esperando todo el dump_timeout.
    watchdog_runner = CaptureRunner(argparse.Namespace(node=1), 1, io.BytesIO())
    watchdog_runner.log = lambda _message: None  # type: ignore[method-assign]
    watchdog_runner.capture_accepting = True
    watchdog_runner.ver_sent_at = 0.0
    watchdog_runner._process_packet(
        bytes((PKT_HEADER, 1, PTYPE_ACK, SUBCMD_VER, 2, 0))
    )
    assert watchdog_runner.first_data_at is None
    assert watchdog_runner.last_data_at is None
    silence_since = watchdog_runner.dump_silence_since
    assert silence_since is not None

    class HalfOpenSocket:
        def __init__(self) -> None:
            self.closed = False

        def close(self) -> None:
            self.closed = True

    half_open = HalfOpenSocket()
    watchdog_runner.sock = half_open  # type: ignore[assignment]
    assert not watchdog_runner._reconnect_if_dump_silent(
        silence_since + DUMP_SILENCE_RECONNECT_S - 0.001
    )
    assert watchdog_runner.sock is half_open and not half_open.closed
    reconnect_at = silence_since + DUMP_SILENCE_RECONNECT_S
    assert watchdog_runner._reconnect_if_dump_silent(reconnect_at)
    assert watchdog_runner.sock is None and half_open.closed
    assert watchdog_runner.disconnects == 1
    assert watchdog_runner.dump_silence_since == reconnect_at
    assert watchdog_runner.first_data_at is None
    assert watchdog_runner.last_data_at is None

    print(
        "self-test OK: 52080 batches, max=60000, WS lengths, grouped packets, "
        "int24-le, force metadata ordering, bounded config retry, "
        "pre-DATA dump watchdog"
    )


def write_metadata(path: Path, metadata: dict[str, object]) -> None:
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(metadata, stream, indent=2, sort_keys=True, ensure_ascii=False)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(path)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.self_test:
        run_self_tests()
        return 0
    try:
        n_batches, output_path, metadata_path = validate_args(args)
    except ValueError as exc:
        print(f"argument error: {exc}", file=sys.stderr)
        return 2

    runner: CaptureRunner | None = None
    success = False
    error: str | None = None
    cleanup_error: str | None = None
    try:
        with open_capture_output(
            output_path,
            metadata_path,
            force=args.force,
        ) as output_stream:
            runner = CaptureRunner(args, n_batches, output_stream)
            runner.log(
                f"inicio: node={args.node}, batches={n_batches}, "
                f"muestras={runner.expected_samples}, output={output_path}"
            )
            try:
                runner.execute()
                success = True
            except KeyboardInterrupt:
                error = "interrumpido por el usuario"
            except Exception as exc:  # metadata must survive every runtime failure
                error = f"{type(exc).__name__}: {exc}"
            finally:
                try:
                    runner.cleanup("final_cleanup")
                    runner.final_cleanup_ok = True
                except Exception as exc:
                    cleanup_error = f"{type(exc).__name__}: {exc}"
                    runner.cleanup_errors.append(cleanup_error)
                    runner.log(f"ERROR cleanup final: {cleanup_error}")
                runner.close()
                output_stream.flush()
                os.fsync(output_stream.fileno())
                if cleanup_error:
                    success = False
                    error = f"{error}; cleanup: {cleanup_error}" if error else f"cleanup: {cleanup_error}"
                if runner.samples_written != runner.expected_samples:
                    success = False
                    if error is None:
                        error = (
                            f"conteo incompleto {runner.samples_written}/"
                            f"{runner.expected_samples}"
                        )

        assert runner is not None
        actual_size = output_path.stat().st_size
        expected_size = runner.samples_written * 3
        if actual_size != expected_size:
            success = False
            size_error = f"tamano binario {actual_size}, esperado {expected_size}"
            error = f"{error}; {size_error}" if error else size_error
        metadata = runner.metadata(success=success, error=error, output_path=output_path)
        metadata["actual_file_bytes"] = actual_size
        write_metadata(metadata_path, metadata)
        if success:
            runner.log(
                f"PASS: {runner.samples_written} muestras exactas, "
                f"SHA256={runner.sha256.hexdigest()}"
            )
            runner.log(f"metadata={metadata_path}")
            return 0
        runner.log(f"FAIL: {error}")
        runner.log(
            f"parcial={runner.samples_written}/{runner.expected_samples}, "
            f"SHA256={runner.sha256.hexdigest()}, metadata={metadata_path}"
        )
        return 1
    except Exception as exc:
        print(f"fatal before metadata completion: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
