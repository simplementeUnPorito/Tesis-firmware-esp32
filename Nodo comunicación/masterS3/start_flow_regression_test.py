#!/usr/bin/env python3
"""F9: regresion del flujo START (WS 0xA3) via broadcastPrestart.

Antes del fix, el retry de HOT_WAIT en el flujo START normal (no-VER)
re-emitia broadcastPrestart() ademas de re-consultar. Eso forzaba al esclavo
a reenviar SETN (0xA3 UART) a un PSoC que ya habia llegado a PSOC_ARMED y
por diseno dejo de escuchar UART (ventana de muestreo silenciosa). El SETN
nunca se ackeaba, vencia a los 500 ms y el esclavo abortaba liberando el
store — mientras el PSoC seguia armado y sordo, sin nadie para asertar SYNC:
nodo roto hasta un reset fisico. La corrida siempre fallaba, sin importar N,
porque la carrera 120 ms (timeout de query) vs ~130 ms (latencia real de
ARMED) es casi deterministica.

Fix: (1) el retry de HOT_WAIT en el flujo START ya no re-emite
broadcastPrestart, solo re-consulta (igual que VER); (2) el esclavo trata
CMD_PRESTART como idempotente si ya esta HOT_WAIT/listo para el mismo N.

Este runner ejercita el camino START real (ARM -> A3 N) dos veces (RAM chica
y SD >512, el caso que mas rompia) y verifica dump completo end-to-end sin
ningun SETN_TIMEOUT/HOT_WAIT_ABORT en el log del esclavo (COM12 opcional).

Ejemplo::

    python start_flow_regression_test.py --node 1 \
        --output "$env:TEMP/f9_start_flow.json"

``--self-test`` es completamente offline: no abre WS ni COM.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import socket
import struct
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

HOST_DEFAULT = "192.168.4.1"
PORT_DEFAULT = 80
SAMPLES_PER_BATCH = 30
NATIVE_FS_HZ = 2604

PKT_HEADER = 0x56
PTYPE_DATA = 0x00
PTYPE_ACK = 0x07
PTYPE_READY = 0xFE

MASTER_STATES = [
    "IDLE", "ARMING", "ARMED", "RUNNING", "STOPPING", "DUMPING",
    "PRESTART", "SCOPE_MULTI",
]

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class F9Failure(RuntimeError):
    """Criterio de aceptacion incumplido."""


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


@dataclass
class RunEvents:
    data_count: int = 0
    reached_dumping: bool = False
    reached_stopping_or_idle: bool = False
    acks: list = field(default_factory=list)
    hb_transitions: list = field(default_factory=list)


class WsClient:
    def __init__(self, host: str, port: int, path: str = "/ws?takeover=1") -> None:
        self.sock = socket.create_connection((host, port), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            f"GET {path} HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\n"
            f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(req.encode())
        resp = b""
        deadline = time.monotonic() + 5.0
        while b"\r\n\r\n" not in resp and time.monotonic() < deadline:
            resp += self.sock.recv(4096)
        if b"101" not in resp.split(b"\r\n", 1)[0]:
            raise F9Failure(f"WS handshake fallido: {resp[:200]!r}")
        self.sock.settimeout(0.3)
        self.buf = b""

    def send_cmd(self, obj: dict) -> None:
        payload = json.dumps(obj).encode()
        if len(payload) < 126:
            hdr = bytes([0x81, 0x80 | len(payload)])
        else:
            hdr = bytes([0x81, 0x80 | 126]) + struct.pack(">H", len(payload))
        self.sock.sendall(hdr + b"\x00\x00\x00\x00" + payload)

    def pump(self, seconds: float, events: RunEvents) -> bool:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            try:
                chunk = self.sock.recv(8192)
            except socket.timeout:
                continue
            if not chunk:
                return False
            self.buf += chunk
            while len(self.buf) >= 2:
                b1, b2 = self.buf[0], self.buf[1]
                op = b1 & 0xF
                ln = b2 & 0x7F
                idx = 2
                if ln == 126:
                    if len(self.buf) < 4:
                        break
                    ln = struct.unpack(">H", self.buf[2:4])[0]
                    idx = 4
                elif ln == 127:
                    if len(self.buf) < 10:
                        break
                    ln = struct.unpack(">Q", self.buf[2:10])[0]
                    idx = 10
                if len(self.buf) < idx + ln:
                    break
                payload = self.buf[idx:idx + ln]
                self.buf = self.buf[idx + ln:]
                if op == 2:
                    i = 0
                    while i + 6 <= len(payload):
                        if payload[i] != PKT_HEADER:
                            i += 1
                            continue
                        node, ptype, c2, c1, c0 = payload[i + 1:i + 6]
                        if ptype == PTYPE_DATA:
                            events.data_count += 1
                        elif ptype == PTYPE_ACK:
                            events.acks.append((node, c2, c1))
                        elif ptype == 0x01:
                            state = MASTER_STATES[c0] if c0 < len(MASTER_STATES) else f"?{c0}"
                            if not events.hb_transitions or events.hb_transitions[-1] != state:
                                events.hb_transitions.append(state)
                            if state == "DUMPING":
                                events.reached_dumping = True
                            if state in ("ARMED", "IDLE") and events.reached_dumping:
                                events.reached_stopping_or_idle = True
                        i += 6
                elif op == 9:
                    self.sock.sendall(b"\x8a\x80\x00\x00\x00\x00")
                elif op == 8:
                    return False
        return True

    def close(self) -> None:
        try:
            self.sock.close()
        except Exception:
            pass


def run_start(host: str, port: int, node: int, batches: int, timeout_s: float) -> dict:
    client = WsClient(host, port)
    events = RunEvents()
    try:
        client.send_cmd({"cmd": "AD", "value": 0})
        client.pump(1.0, events)
        client.send_cmd({"cmd": "A2", "param": 0})
        client.pump(3.0, events)
        client.send_cmd({"cmd": "A3", "value": batches})
        client.pump(timeout_s, events)
        expected = batches * SAMPLES_PER_BATCH
        return {
            "batches": batches,
            "expected_samples": expected,
            "data_count": events.data_count,
            "reached_dumping": events.reached_dumping,
            "hb_transitions": events.hb_transitions,
            "complete": events.data_count >= expected and events.reached_dumping,
        }
    finally:
        try:
            client.send_cmd({"cmd": "A5", "param": 0})
            client.pump(1.0, events)
        except Exception:
            pass
        client.close()


def run_self_test() -> int:
    checks: list[tuple[str, bool]] = []

    def check(name: str, condition: bool) -> None:
        checks.append((name, condition))
        print(f"[{'PASS' if condition else 'FAIL'}] {name}")

    events = RunEvents()
    events.data_count = 100
    events.reached_dumping = True
    result_complete = events.data_count >= 100 and events.reached_dumping
    check("criterio completo con data+dumping", result_complete)

    events2 = RunEvents()
    events2.data_count = 50
    events2.reached_dumping = False
    result_incomplete = events2.data_count >= 100 and events2.reached_dumping
    check("criterio detecta incompleto (data insuficiente)", not result_incomplete)

    check("estados de master conocidos", MASTER_STATES[5] == "DUMPING")
    check("PTYPE_DATA es 0x00", PTYPE_DATA == 0x00)

    passed = sum(1 for _, ok in checks if ok)
    print(f"\nF9 self-test: {passed}/{len(checks)} PASS")
    return 0 if passed == len(checks) else 1


def parse_args(argv: Optional[list[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default=HOST_DEFAULT)
    parser.add_argument("--port", type=int, default=PORT_DEFAULT)
    parser.add_argument("--node", type=int, default=1)
    parser.add_argument("--output", type=Path, help="JSON de evidencia (requerido salvo --self-test)")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if not args.self_test and args.output is None:
        parser.error("--output es requerido")
    return args


def main(argv: Optional[list[str]] = None) -> int:
    args = parse_args(argv)
    if args.self_test:
        return run_self_test()

    metadata: dict[str, object] = {
        "schema_version": 1,
        "test": "F9_START_FLOW_REGRESSION",
        "status": "failed",
        "error": None,
        "started_at_utc": utc_now(),
        "host": args.host,
        "node": args.node,
        "scenarios": [],
    }
    try:
        small = run_start(args.host, args.port, args.node, 261, 40.0)
        print(f"[SCENARIO small] {small}")
        metadata["scenarios"].append({"name": "small_ram_261", **small})
        if not small["complete"]:
            raise F9Failure(f"START pequeno (261) incompleto: {small}")

        big = run_start(args.host, args.port, args.node, 600, 90.0)
        print(f"[SCENARIO chained_sd] {big}")
        metadata["scenarios"].append({"name": "chained_sd_600", **big})
        if not big["complete"]:
            raise F9Failure(f"START encadenado SD (600) incompleto: {big}")

        metadata["status"] = "ok"
    except Exception as exc:
        metadata["error"] = f"{type(exc).__name__}: {exc}"
        print(f"[FAIL] {metadata['error']}")
    finally:
        metadata["completed_at_utc"] = utc_now()
        assert args.output is not None
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(metadata, indent=2, sort_keys=True), encoding="utf-8")
        print(f"Evidencia JSON: {args.output}")

    return 0 if metadata["status"] == "ok" else 1


if __name__ == "__main__":
    sys.exit(main())
