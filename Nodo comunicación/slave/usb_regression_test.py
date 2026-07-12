#!/usr/bin/env python3
"""Regresion USB estricta para el banco GEO (ESP esclavo + PSoC + SD).

El firmware de ``platformio.ini`` debe estar compilado con
``SLAVE_USB_CMD_ENABLE=1`` y ``SLAVE_LAB_TOOLS_ENABLE=1``. El runner no pulsa
DTR ni RTS: crea el objeto pyserial cerrado, fija ambas lineas en ``False`` y
recién entonces abre el puerto para no resetear el ESP/PSoC accidentalmente.

Uso en el banco (COM12 es el valor por defecto)::

    python usb_regression_test.py
    python usb_regression_test.py --port COM12 --verbose

Prueba offline de parsers y criterios, sin importar pyserial ni abrir un COM::

    python usb_regression_test.py --self-test

La corrida real valida preflight, ACKs PSoC explicitos 0xBB/0xB7/0xBE (un
reintento maximo, informado como ``retry_used=yes``, por el F6 conocido),
capturas RAM RAW con decimacion 1 y 3, captura SD de 100 lotes con lecturas de
los extremos y rechazo fuera de rango, la frontera RAM/SD 512/513, y captura
FIR con decimacion 2. El
``finally`` siempre intenta restaurar ``stop; sdcap 0; stream 0; decim 1;
clear`` y exige un probe/status final limpio. Cualquier fallo produce exit 1.
"""

from __future__ import annotations

import argparse
import re
import sys
import time
from dataclasses import dataclass
from typing import Callable, Optional


BAUD = 115_200
NATIVE_FS_HZ = 2_604
SAMPLES_PER_BATCH = 30
RAM_BATCH_LIMIT = 512
BOUNDARY_BATCHES = RAM_BATCH_LIMIT + 1
ESP_STATE_WAIT_ARM = 0
ESP_STATE_STOPPED = 4

CMD_SELECT_STREAM = 0xB7
CMD_SET_DECIMATION = 0xBB
CMD_SD_CAPTURE = 0xBE

ACK_RE = re.compile(
    r"PSoC\s+cfg\s+ack(?P<unsolicited>\s+unsolicited)?\s+"
    r"sub=0x(?P<sub>[0-9A-Fa-f]{2})\s+val=(?P<val>\d+)"
    r"(?:\s+expected=(?P<expected>\d+)\s+ok=(?P<ok>[01]))?",
    re.IGNORECASE,
)
PROBE_OK_RE = re.compile(r"\[USB\]\s+probe\s+psoc=1\b", re.IGNORECASE)
PSOC_IDLE_RE = re.compile(r"\bpstate=0/IDLE\b", re.IGNORECASE)
SD_STATUS_RE = re.compile(
    r"\[PSoC\]\s+SD\s+status=0x(?P<status>[0-9A-Fa-f]{2})\s+"
    r"present=(?P<present>[01])\b",
    re.IGNORECASE,
)
SDREAD_RE = re.compile(
    r"\[USB\]\s+SDREAD\s+seq=(?P<seq>\d+)\s+"
    r"raw\[0\]=(?P<raw0>-?\d+)\s+raw\[29\]=(?P<raw29>-?\d+)\s+"
    r"sumRaw=(?P<sum>-?\d+)\s+dig\[0\]=(?P<dig0>-?\d+)\s+"
    r"dig\[29\]=(?P<dig29>-?\d+)\s+flags\[0\]=0x(?P<flags>[0-9A-Fa-f]{2})",
    re.IGNORECASE,
)
SDREAD_OOR_RE = re.compile(
    r"\[USB\]\s+sdread\s+ignored:\s+seq=(?P<seq>\d+)\s+"
    r"fuera\s+de\s+rango\s+\(fill=(?P<fill>\d+)\)",
    re.IGNORECASE,
)

FATAL_PATTERNS = (
    ("timeout de ACK de configuracion", re.compile(r"PSoC\s+cfg\s+ack\s+timeout", re.I)),
    ("ACK de configuracion negativo", re.compile(r"PSoC\s+cfg\s+ack.*\bok=0\b", re.I)),
    ("timeout de lectura SD", re.compile(r"PSoC\s+SD_READ\s+timeout", re.I)),
    ("NACK de lectura SD", re.compile(r"PSoC\s+SD_READ\s+NACK", re.I)),
    ("evento SD_ERROR", re.compile(r"\bSD_ERROR\b", re.I)),
    ("dump parcial", re.compile(r"\bpartial\s+dump\b", re.I)),
)


@dataclass(frozen=True)
class Status:
    """Ultima linea de diagnostico ``USB_STATUS`` parseada."""

    b_ok: int
    b_bad: int
    esp_state: int
    fill: int
    target: int
    fs_hz: int


@dataclass(frozen=True)
class ReadResult:
    text: str
    truncated: bool
    byte_count: int


class RegressionFailure(RuntimeError):
    """Fallo de criterio ya registrado y mostrado al operador."""


def parse_status(text: str) -> Optional[Status]:
    """Devuelve el ultimo status completo; ignora lineas periodicas ajenas."""

    for line in reversed(text.splitlines()):
        if "USB_STATUS" not in line:
            continue
        fields: dict[str, int] = {}
        for name, key in (("bOK", "b_ok"), ("bBad", "b_bad"), ("state", "esp_state"), ("fs", "fs_hz")):
            match = re.search(rf"\b{re.escape(name)}=(\d+)\b", line)
            if match:
                fields[key] = int(match.group(1))
        fill_match = re.search(r"\bfill=(\d+)/(\d+)\b", line)
        if fill_match and len(fields) == 4:
            return Status(
                b_ok=fields["b_ok"],
                b_bad=fields["b_bad"],
                esp_state=fields["esp_state"],
                fill=int(fill_match.group(1)),
                target=int(fill_match.group(2)),
                fs_hz=fields["fs_hz"],
            )
    return None


def find_config_ack(text: str, sub_cmd: int, value: int) -> Optional[re.Match[str]]:
    """Busca un ACK PSoC valido, tanto correlacionado como ``unsolicited``."""

    for match in ACK_RE.finditer(text):
        if int(match.group("sub"), 16) != sub_cmd or int(match.group("val")) != value:
            continue
        expected = match.group("expected")
        ok = match.group("ok")
        if expected is None and match.group("unsolicited"):
            return match
        if expected is not None and int(expected) == value and ok == "1":
            return match
    return None


def has_ack_for_subcommand(text: str, sub_cmd: int) -> bool:
    return any(int(match.group("sub"), 16) == sub_cmd for match in ACK_RE.finditer(text))


def fatal_errors(text: str) -> list[str]:
    return [label for label, pattern in FATAL_PATTERNS if pattern.search(text)]


def find_sdread(text: str, seq: int) -> Optional[re.Match[str]]:
    return next(
        (match for match in SDREAD_RE.finditer(text) if int(match.group("seq")) == seq),
        None,
    )


def valid_sdread_oor(text: str, seq: int, expected_fill: int) -> bool:
    match = SDREAD_OOR_RE.search(text)
    return bool(
        match
        and int(match.group("seq")) == seq
        and int(match.group("fill")) == expected_fill
        and find_sdread(text, seq) is None
    )


def capture_status_errors(
    status: Status,
    batches: int,
    expected_fs_hz: int,
    baseline_b_ok: int,
    require_uart_batches: bool,
) -> list[str]:
    errors: list[str] = []
    if (status.fill, status.target) != (batches, batches):
        errors.append(f"fill={status.fill}/{status.target}, esperado {batches}/{batches}")
    if status.esp_state != ESP_STATE_STOPPED:
        errors.append(f"estado ESP={status.esp_state}, esperado STOPPED/{ESP_STATE_STOPPED}")
    if status.fs_hz != expected_fs_hz:
        errors.append(f"fs={status.fs_hz}, esperado {expected_fs_hz}")
    if status.b_bad != 0:
        errors.append(f"bBad={status.b_bad}, esperado 0")
    if require_uart_batches and status.b_ok - baseline_b_ok < batches:
        errors.append(
            f"delta bOK={status.b_ok - baseline_b_ok}, esperado al menos {batches}"
        )
    return errors


def final_state_errors(probe_text: str, status: Optional[Status]) -> list[str]:
    errors: list[str] = []
    if not PROBE_OK_RE.search(probe_text):
        errors.append("probe no reporto psoc=1")
    if not PSOC_IDLE_RE.search(probe_text):
        errors.append("probe no reporto pstate=0/IDLE")
    if status is None:
        errors.append("status USB ausente o incompleto")
        return errors
    if status.fs_hz != NATIVE_FS_HZ:
        errors.append(f"fs={status.fs_hz}, esperado {NATIVE_FS_HZ}")
    if status.b_bad != 0:
        errors.append(f"bBad={status.b_bad}, esperado 0")
    if (status.fill, status.target) != (0, 0):
        errors.append(f"fill={status.fill}/{status.target}, esperado 0/0")
    if status.esp_state not in (ESP_STATE_WAIT_ARM, ESP_STATE_STOPPED):
        errors.append(
            f"estado ESP={status.esp_state}, esperado fuera de captura "
            f"({ESP_STATE_WAIT_ARM}/{ESP_STATE_STOPPED})"
        )
    return errors


class CheckRecorder:
    def __init__(self) -> None:
        self.results: list[tuple[str, bool, str]] = []

    def add(self, name: str, passed: bool, detail: str = "") -> bool:
        clean_detail = " ".join(detail.split())
        self.results.append((name, passed, clean_detail))
        suffix = f" - {clean_detail}" if clean_detail else ""
        print(f"[{'PASS' if passed else 'FAIL'}] {name}{suffix}")
        return passed

    def require(self, name: str, condition: bool, detail: str = "") -> None:
        if not self.add(name, condition, detail):
            raise RegressionFailure(name)

    def summary(self) -> bool:
        failed = [(name, detail) for name, passed, detail in self.results if not passed]
        passed = len(self.results) - len(failed)
        print(f"\nUSB GEO: {passed}/{len(self.results)} checks PASS")
        if failed:
            print("Fallos:")
            for name, detail in failed:
                print(f"  - {name}: {detail or 'sin detalle'}")
        return not failed


class SerialSession:
    """Wrapper acotado: ninguna lectura puede extenderse por chatter infinito."""

    def __init__(self, serial_port: object) -> None:
        self.serial = serial_port

    @classmethod
    def open(cls, port: str) -> "SerialSession":
        # Import diferido: --self-test es realmente offline y no requiere pyserial.
        import serial  # type: ignore[import-not-found]

        serial_port = serial.Serial()
        serial_port.port = port
        serial_port.baudrate = BAUD
        serial_port.timeout = 0
        serial_port.write_timeout = 2.0
        serial_port.dtr = False
        serial_port.rts = False
        serial_port.open()
        return cls(serial_port)

    def collect(
        self,
        timeout_s: float,
        *,
        quiet_s: float = 0.20,
        max_bytes: int = 131_072,
        predicate: Optional[Callable[[str], bool]] = None,
    ) -> ReadResult:
        deadline = time.monotonic() + timeout_s
        last_data_at: Optional[float] = None
        matched = predicate is None
        buffer = bytearray()
        truncated = False

        while time.monotonic() < deadline:
            waiting = int(getattr(self.serial, "in_waiting"))
            if waiting:
                remaining = max_bytes - len(buffer)
                if remaining <= 0:
                    truncated = True
                    break
                chunk = self.serial.read(min(waiting, remaining))
                buffer.extend(chunk)
                last_data_at = time.monotonic()
                text = buffer.decode("utf-8", errors="replace")
                if predicate is not None and predicate(text):
                    matched = True
                if len(buffer) >= max_bytes:
                    truncated = True
                    break
            else:
                now = time.monotonic()
                if matched and last_data_at is not None and now - last_data_at >= quiet_s:
                    break
                time.sleep(0.01)

        if truncated:
            # El limite es tambien un limite de memoria atrasada: purgar evita que
            # una calibracion verbosa contamine todos los comandos posteriores.
            self.serial.reset_input_buffer()
        return ReadResult(
            text=buffer.decode("utf-8", errors="replace"),
            truncated=truncated,
            byte_count=len(buffer),
        )

    def command(
        self,
        command: str,
        timeout_s: float = 2.0,
        *,
        predicate: Optional[Callable[[str], bool]] = None,
        max_bytes: int = 131_072,
    ) -> ReadResult:
        self.serial.write((command + "\n").encode("ascii"))
        self.serial.flush()
        return self.collect(
            timeout_s,
            predicate=predicate,
            max_bytes=max_bytes,
        )

    def close(self) -> None:
        if getattr(self.serial, "is_open", False):
            self.serial.close()


class UsbRegression:
    def __init__(self, session: SerialSession, checks: CheckRecorder, verbose: bool) -> None:
        self.session = session
        self.checks = checks
        self.verbose = verbose

    def _show(self, command: str, result: ReadResult, force: bool = False) -> None:
        if not (self.verbose or force):
            return
        print(f"\n--- {command} ({result.byte_count} bytes) ---")
        print(result.text.rstrip() or "<sin respuesta>")

    def command(
        self,
        command: str,
        timeout_s: float = 2.0,
        *,
        predicate: Optional[Callable[[str], bool]] = None,
    ) -> str:
        result = self.session.command(command, timeout_s, predicate=predicate)
        errors = fatal_errors(result.text)
        if result.truncated:
            errors.append(f"respuesta truncada al limite de {result.byte_count} bytes")
        if errors:
            self._show(command, result, force=True)
            self.checks.require(f"{command}: respuesta valida", False, "; ".join(errors))
        self._show(command, result)
        return result.text

    def require_config(self, command: str, sub_cmd: int, value: int, label: str) -> None:
        def ack_or_failure(text: str) -> bool:
            return (
                has_ack_for_subcommand(text, sub_cmd)
                or bool(re.search(rf"cfg\s+ack\s+timeout\s+sub=0x{sub_cmd:02X}\b", text, re.I))
            )

        attempts: list[ReadResult] = []
        for attempt in (1, 2):
            result = self.session.command(command, 2.0, predicate=ack_or_failure)
            attempts.append(result)
            errors = fatal_errors(result.text)
            non_config_errors = [
                error
                for error in errors
                if error not in ("timeout de ACK de configuracion", "ACK de configuracion negativo")
            ]
            ack = find_config_ack(result.text, sub_cmd, value)
            if ack is not None and not result.truncated and not errors:
                self._show(command, result)
                self.checks.require(
                    label,
                    True,
                    f"sub=0x{sub_cmd:02X} val={value} attempts={attempt} "
                    f"retry_used={'yes' if attempt == 2 else 'no'}",
                )
                return

            self._show(command, result, force=bool(non_config_errors or result.truncated))
            if non_config_errors or result.truncated:
                break
            if attempt == 1:
                reason = "; ".join(errors) or "ACK correcto ausente"
                print(f"[RETRY] {label} - intento 1: {reason}")
                time.sleep(0.25)

        details: list[str] = []
        for index, result in enumerate(attempts, start=1):
            errors = fatal_errors(result.text)
            reason = ", ".join(errors) or "ACK correcto ausente"
            if result.truncated:
                reason += f", truncado en {result.byte_count} bytes"
            details.append(f"intento {index}: {reason}")
        self.checks.require(
            label,
            False,
            f"esperado sub=0x{sub_cmd:02X} val={value}; " + "; ".join(details),
        )

    def read_status(self) -> Status:
        text = self.command("status", 1.5, predicate=lambda data: parse_status(data) is not None)
        status = parse_status(text)
        if status is None:
            self.checks.require("status USB parseable", False, "USB_STATUS ausente o incompleto")
        assert status is not None
        return status

    def preflight(self) -> None:
        print("\n=== PREFLIGHT ===")
        initial = self.session.collect(0.50, quiet_s=0.10, max_bytes=32_768)
        if self.verbose and initial.text:
            self._show("drain inicial", initial)
        self.checks.require(
            "drain inicial acotado",
            not initial.truncated,
            f"{initial.byte_count} bytes",
        )

        probe = self.command(
            "probe",
            2.5,
            predicate=lambda data: bool(PROBE_OK_RE.search(data) and PSOC_IDLE_RE.search(data)),
        )
        self.checks.require("preflight probe psoc=1", bool(PROBE_OK_RE.search(probe)))
        self.checks.require("preflight PSoC IDLE", bool(PSOC_IDLE_RE.search(probe)))

        status = self.read_status()
        self.checks.add("preflight status USB parseable", True)
        self.checks.require(
            "preflight Fs nativa",
            status.fs_hz == NATIVE_FS_HZ,
            f"fs={status.fs_hz}",
        )
        self.checks.require("preflight UART sin lotes malos", status.b_bad == 0, f"bBad={status.b_bad}")
        self.checks.require(
            "preflight ESP fuera de captura",
            status.esp_state in (ESP_STATE_WAIT_ARM, ESP_STATE_STOPPED),
            f"state={status.esp_state}",
        )

        sdinfo = self.command(
            "sdinfo",
            3.0,
            predicate=lambda data: SD_STATUS_RE.search(data) is not None,
        )
        sd_match = SD_STATUS_RE.search(sdinfo)
        status_byte = int(sd_match.group("status"), 16) if sd_match else 0
        sd_ready = bool(
            sd_match
            and sd_match.group("present") == "1"
            and status_byte & 0x01
            and status_byte & 0x10
        )
        self.checks.require(
            "preflight SD presente y FAT montado",
            sd_ready,
            f"status=0x{status_byte:02X}",
        )

        # Normaliza cualquier store viejo antes de tomar baselines bOK/fill.
        self.command("stop", 1.0)
        self.command("clear", 1.0)

    def wait_capture(
        self,
        *,
        name: str,
        batches: int,
        expected_batches: Optional[int] = None,
        expected_fs_hz: int,
        require_uart_batches: bool,
        timeout_s: float,
    ) -> Status:
        stored_batches = batches if expected_batches is None else expected_batches
        baseline = self.read_status()
        self.command(f"cap {batches}", 2.0)
        deadline = time.monotonic() + timeout_s
        last_status: Optional[Status] = None
        while time.monotonic() < deadline:
            last_status = self.read_status()
            if (last_status.fill, last_status.target) == (stored_batches, stored_batches):
                break
            time.sleep(0.20)

        if last_status is None:
            self.checks.require(name, False, "sin status posterior a cap")
            raise AssertionError("unreachable")

        errors = capture_status_errors(
            last_status,
            stored_batches,
            expected_fs_hz,
            baseline.b_ok,
            require_uart_batches,
        )
        detail = (
            f"requested={batches} expected={stored_batches} "
            f"fill={last_status.fill}/{last_status.target} fs={last_status.fs_hz} "
            f"bBad={last_status.b_bad} delta_bOK={last_status.b_ok - baseline.b_ok}"
        )
        if errors:
            detail += "; " + "; ".join(errors)
        self.checks.require(name, not errors, detail)
        return last_status

    def require_sdread(self, seq: int, label: str) -> None:
        text = self.command(
            f"sdread {seq}",
            3.0,
            predicate=lambda data: find_sdread(data, seq) is not None,
        )
        self.checks.require(
            label,
            find_sdread(text, seq) is not None,
            "resumen de 30 muestras esperado",
        )

    def require_sdread_oor(self, seq: int, expected_fill: int, label: str) -> None:
        text = self.command(
            f"sdread {seq}",
            2.0,
            predicate=lambda data: SDREAD_OOR_RE.search(data) is not None,
        )
        self.checks.require(
            label,
            valid_sdread_oor(text, seq, expected_fill),
            f"seq={seq} fill esperado={expected_fill}",
        )

    def ram_raw(self, factor: int) -> None:
        expected_fs = NATIVE_FS_HZ // factor
        print(f"\n=== RAM RAW decim {factor} ===")
        self.command("clear", 1.0)
        self.require_config(
            f"decim {factor}",
            CMD_SET_DECIMATION,
            factor,
            f"ACK 0xBB decim={factor} (RAM RAW)",
        )
        self.require_config("stream 0", CMD_SELECT_STREAM, 0, "ACK 0xB7 stream=RAW")
        self.require_config("sdcap 0", CMD_SD_CAPTURE, 0, "ACK 0xBE SD=off (RAM)")
        duration = 60 * SAMPLES_PER_BATCH / expected_fs
        self.wait_capture(
            name=f"RAM RAW 60 lotes d{factor}",
            batches=60,
            expected_fs_hz=expected_fs,
            require_uart_batches=True,
            timeout_s=max(15.0, duration * 2.0 + 8.0),
        )

    def sd_capture(self) -> None:
        print("\n=== SD RAW 100 lotes + sdread ===")
        self.command("clear", 1.0)
        self.require_config("decim 1", CMD_SET_DECIMATION, 1, "ACK 0xBB decim=1 (SD)")
        self.require_config("stream 0", CMD_SELECT_STREAM, 0, "ACK 0xB7 stream=RAW (SD)")
        self.require_config("sdcap 1", CMD_SD_CAPTURE, 1, "ACK 0xBE SD=on")
        self.wait_capture(
            name="SD RAW 100 lotes",
            batches=100,
            expected_fs_hz=NATIVE_FS_HZ,
            require_uart_batches=False,
            timeout_s=20.0,
        )

        for seq in (0, 99):
            self.require_sdread(seq, f"sdread extremo seq={seq}")
        self.require_sdread_oor(100, 100, "sdread seq=100 rechazado OOR")

    def boundary_512_513(self) -> None:
        print("\n=== FRONTERA RAM/SD 512/513 ===")
        self.command("clear", 1.0)
        self.require_config("decim 1", CMD_SET_DECIMATION, 1, "ACK 0xBB decim=1 (frontera)")
        self.require_config("stream 0", CMD_SELECT_STREAM, 0, "ACK 0xB7 stream=RAW (frontera)")
        self.require_config("sdcap 0", CMD_SD_CAPTURE, 0, "ACK 0xBE SD=off (frontera)")
        self.wait_capture(
            name="RAM cap 513 clamped estrictamente a 512",
            batches=BOUNDARY_BATCHES,
            expected_batches=RAM_BATCH_LIMIT,
            expected_fs_hz=NATIVE_FS_HZ,
            require_uart_batches=True,
            timeout_s=20.0,
        )

        self.command("clear", 1.0)
        self.require_config("sdcap 1", CMD_SD_CAPTURE, 1, "ACK 0xBE SD=on (frontera)")
        self.wait_capture(
            name="SD cap 513 conservada y completa",
            batches=BOUNDARY_BATCHES,
            expected_batches=BOUNDARY_BATCHES,
            expected_fs_hz=NATIVE_FS_HZ,
            require_uart_batches=False,
            timeout_s=20.0,
        )
        self.require_sdread(512, "SD cap 513 permite sdread seq=512")
        self.require_sdread_oor(513, 513, "SD cap 513 rechaza seq=513 OOR")

    def fir_decim2(self) -> None:
        print("\n=== FIR decim 2 ===")
        self.require_config("sdcap 0", CMD_SD_CAPTURE, 0, "ACK 0xBE SD=off (FIR)")
        self.command("clear", 1.0)
        self.require_config("stream 1", CMD_SELECT_STREAM, 1, "ACK 0xB7 stream=FIR")
        self.require_config("decim 2", CMD_SET_DECIMATION, 2, "ACK 0xBB decim=2 (FIR)")
        self.wait_capture(
            name="FIR 30 lotes d2",
            batches=30,
            expected_fs_hz=NATIVE_FS_HZ // 2,
            require_uart_batches=True,
            timeout_s=15.0,
        )

    def run(self) -> None:
        self.preflight()
        self.ram_raw(1)
        self.ram_raw(3)
        self.sd_capture()
        self.boundary_512_513()
        self.fir_decim2()

    def cleanup(self) -> None:
        """Best effort completo: nunca abandona los pasos restantes por un fallo."""

        print("\n=== CLEANUP OBLIGATORIO ===")

        def simple(command: str, marker: str) -> None:
            try:
                result = self.session.command(command, 1.5)
                errors = fatal_errors(result.text)
                passed = not result.truncated and marker.lower() in result.text.lower() and not errors
                self._show(command, result, force=not passed)
                detail = "; ".join(errors) if errors else marker
                self.checks.add(f"cleanup {command}", passed, detail)
            except Exception as exc:  # continuar con los otros restores
                self.checks.add(f"cleanup {command}", False, repr(exc))

        def config(command: str, sub_cmd: int, value: int) -> None:
            try:
                last_result: Optional[ReadResult] = None
                last_errors: list[str] = []
                for attempt in (1, 2):
                    result = self.session.command(
                        command,
                        2.0,
                        predicate=lambda data: (
                            has_ack_for_subcommand(data, sub_cmd)
                            or bool(
                                re.search(
                                    rf"cfg\s+ack\s+timeout\s+sub=0x{sub_cmd:02X}\b",
                                    data,
                                    re.I,
                                )
                            )
                        ),
                    )
                    last_result = result
                    last_errors = fatal_errors(result.text)
                    non_config_errors = [
                        error
                        for error in last_errors
                        if error
                        not in (
                            "timeout de ACK de configuracion",
                            "ACK de configuracion negativo",
                        )
                    ]
                    passed = (
                        not result.truncated
                        and not last_errors
                        and find_config_ack(result.text, sub_cmd, value) is not None
                    )
                    if passed:
                        self._show(command, result)
                        self.checks.add(
                            f"cleanup ACK {command}",
                            True,
                            f"sub=0x{sub_cmd:02X} val={value} attempts={attempt} "
                            f"retry_used={'yes' if attempt == 2 else 'no'}",
                        )
                        return
                    self._show(command, result, force=bool(non_config_errors or result.truncated))
                    if non_config_errors or result.truncated:
                        break
                    if attempt == 1:
                        time.sleep(0.25)

                assert last_result is not None
                detail = "; ".join(last_errors) or f"ACK sub=0x{sub_cmd:02X} val={value} ausente"
                if last_result.truncated:
                    detail += f"; truncado en {last_result.byte_count} bytes"
                self._show(command, last_result, force=True)
                self.checks.add(f"cleanup ACK {command}", False, detail)
            except Exception as exc:  # continuar con los otros restores
                self.checks.add(f"cleanup ACK {command}", False, repr(exc))

        simple("stop", "[USB] stop")
        config("sdcap 0", CMD_SD_CAPTURE, 0)
        config("stream 0", CMD_SELECT_STREAM, 0)
        config("decim 1", CMD_SET_DECIMATION, 1)
        simple("clear", "[USB] clear")

        probe_text = ""
        final_status: Optional[Status] = None
        try:
            probe_result = self.session.command(
                "probe",
                2.5,
                predicate=lambda data: bool(PROBE_OK_RE.search(data) and PSOC_IDLE_RE.search(data)),
            )
            probe_text = probe_result.text
            self._show("probe final", probe_result, force=probe_result.truncated)
            if probe_result.truncated:
                self.checks.add("cleanup probe sin truncar", False, f"{probe_result.byte_count} bytes")
        except Exception as exc:
            self.checks.add("cleanup probe final", False, repr(exc))

        try:
            status_result = self.session.command(
                "status",
                1.5,
                predicate=lambda data: parse_status(data) is not None,
            )
            final_status = parse_status(status_result.text)
            self._show("status final", status_result, force=status_result.truncated)
            if status_result.truncated:
                self.checks.add("cleanup status sin truncar", False, f"{status_result.byte_count} bytes")
        except Exception as exc:
            self.checks.add("cleanup status final", False, repr(exc))

        errors = final_state_errors(probe_text, final_status)
        detail = "; ".join(errors)
        if not errors and final_status is not None:
            detail = (
                f"psoc=1 pstate=IDLE fs={final_status.fs_hz} "
                f"fill={final_status.fill}/{final_status.target} bBad={final_status.b_bad}"
            )
        self.checks.add("estado final limpio", not errors, detail)


def run_self_test() -> int:
    """Fixtures offline de todos los parsers y criterios de aceptacion."""

    checks = CheckRecorder()
    good_probe = """
[PSoC] boot hw=0/GEO pstate=0/IDLE
[USB] probe psoc=1
[SLAVE 2] USB_PROBE bOK=160 bBad=0 uartBytes=1234 state=4 fill=0/0 fs=2604
"""
    good_status_text = """
[SLAVE 2] STATUS bOK=159 bBad=9 state=3 fill=2/60 fs=868
[SLAVE 2] USB_STATUS bOK=160 bBad=0 uartBytes=456 state=4 fill=60/60 fs=868
"""
    final_status_text = (
        "[SLAVE 2] USB_STATUS bOK=190 bBad=0 uartBytes=999 mark=1 "
        "state=0 fill=0/0 fs=2604\n"
    )
    ack_fixture = """
[SLAVE] PSoC cfg ack sub=0xBB val=3 expected=3 ok=1
[SLAVE] PSoC cfg ack unsolicited sub=0xB7 val=1
[SLAVE] PSoC cfg ack unsolicited sub=0xBE val=0
"""
    sdread_fixture = (
        "[USB] SDREAD seq=99 raw[0]=-12 raw[29]=44 sumRaw=88 "
        "dig[0]=-123 dig[29]=456 flags[0]=0xA0\n"
    )
    oor_fixture = "[USB] sdread ignored: seq=100 fuera de rango (fill=100)\n"
    boundary_sdread_fixture = (
        "[USB] SDREAD seq=512 raw[0]=-1 raw[29]=1 sumRaw=0 "
        "dig[0]=-2 dig[29]=2 flags[0]=0x80\n"
    )
    boundary_oor_fixture = "[USB] sdread ignored: seq=513 fuera de rango (fill=513)\n"

    status = parse_status(good_status_text)
    checks.add("self-test parse ultimo USB_STATUS", status == Status(160, 0, 4, 60, 60, 868))
    checks.add("self-test status incompleto rechazado", parse_status("USB_STATUS bOK=1") is None)
    checks.add("self-test ACK 0xBB correlacionado", find_config_ack(ack_fixture, 0xBB, 3) is not None)
    checks.add("self-test ACK 0xB7 unsolicited", find_config_ack(ack_fixture, 0xB7, 1) is not None)
    checks.add("self-test ACK 0xBE unsolicited", find_config_ack(ack_fixture, 0xBE, 0) is not None)
    checks.add(
        "self-test ACK negativo rechazado",
        find_config_ack("PSoC cfg ack sub=0xBB val=0 expected=2 ok=0", 0xBB, 2) is None,
    )
    checks.add("self-test SDREAD resumen", SDREAD_RE.search(sdread_fixture) is not None)
    checks.add("self-test SDREAD OOR", SDREAD_OOR_RE.search(oor_fixture) is not None)
    checks.add(
        "self-test frontera RAM acepta clamp 512",
        not capture_status_errors(
            Status(612, 0, ESP_STATE_STOPPED, 512, 512, NATIVE_FS_HZ),
            RAM_BATCH_LIMIT,
            NATIVE_FS_HZ,
            100,
            True,
        ),
    )
    checks.add(
        "self-test frontera RAM rechaza 513 sin clamp",
        bool(
            capture_status_errors(
                Status(613, 0, ESP_STATE_STOPPED, 513, 513, NATIVE_FS_HZ),
                RAM_BATCH_LIMIT,
                NATIVE_FS_HZ,
                100,
                True,
            )
        ),
    )
    checks.add(
        "self-test frontera SD conserva 513",
        not capture_status_errors(
            Status(100, 0, ESP_STATE_STOPPED, 513, 513, NATIVE_FS_HZ),
            BOUNDARY_BATCHES,
            NATIVE_FS_HZ,
            100,
            False,
        ),
    )
    checks.add(
        "self-test frontera SD detecta clamp indebido",
        bool(
            capture_status_errors(
                Status(100, 0, ESP_STATE_STOPPED, 512, 512, NATIVE_FS_HZ),
                BOUNDARY_BATCHES,
                NATIVE_FS_HZ,
                100,
                False,
            )
        ),
    )
    checks.add(
        "self-test frontera SDREAD seq=512",
        find_sdread(boundary_sdread_fixture, 512) is not None,
    )
    checks.add(
        "self-test frontera OOR seq=513 fill=513",
        valid_sdread_oor(boundary_oor_fixture, 513, 513),
    )
    checks.add(
        "self-test OOR no acepta payload seq=513",
        not valid_sdread_oor(boundary_oor_fixture + boundary_sdread_fixture.replace("512", "513"), 513, 513),
    )
    checks.add(
        "self-test captura valida",
        status is not None and not capture_status_errors(status, 60, 868, 100, True),
    )
    checks.add(
        "self-test captura detecta bBad/Fs",
        bool(capture_status_errors(Status(160, 1, 4, 60, 60, 867), 60, 868, 100, True)),
    )
    final_status = parse_status(final_status_text)
    checks.add("self-test estado final valido", not final_state_errors(good_probe, final_status))
    checks.add(
        "self-test estado final exige IDLE",
        bool(final_state_errors(good_probe.replace("pstate=0/IDLE", "pstate=2/ARMED"), final_status)),
    )
    checks.add(
        "self-test timeout=0 no es falso positivo",
        not fatal_errors("[LAB] done=1 timeout=0 state=4 fill=30/30"),
    )
    checks.add(
        "self-test timeout SD detectado",
        "timeout de lectura SD" in fatal_errors("[SLAVE] PSoC SD_READ timeout seq=9 usb=1"),
    )

    return 0 if checks.summary() else 1


def parse_args(argv: Optional[list[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", default="COM12", help="Puerto USB del esclavo GEO (default: COM12)")
    parser.add_argument("--verbose", action="store_true", help="Imprimir respuestas seriales completas")
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Ejecutar fixtures offline; no importa pyserial ni abre el puerto",
    )
    return parser.parse_args(argv)


def main(argv: Optional[list[str]] = None) -> int:
    args = parse_args(argv)
    if args.self_test:
        return run_self_test()

    checks = CheckRecorder()
    session: Optional[SerialSession] = None
    runner: Optional[UsbRegression] = None
    try:
        print(f"Abriendo {args.port} a {BAUD} baud (DTR=False, RTS=False)...")
        session = SerialSession.open(args.port)
        runner = UsbRegression(session, checks, args.verbose)
        runner.run()
    except RegressionFailure as exc:
        print(f"\nCorrida principal abortada en: {exc}")
    except KeyboardInterrupt:
        checks.add("corrida interrumpida", False, "KeyboardInterrupt")
    except Exception as exc:
        checks.add("excepcion no esperada", False, f"{type(exc).__name__}: {exc}")
    finally:
        if runner is not None:
            try:
                runner.cleanup()
            except Exception as exc:
                checks.add("excepcion durante cleanup", False, f"{type(exc).__name__}: {exc}")
        if session is not None:
            session.close()

    return 0 if checks.summary() else 1


if __name__ == "__main__":
    sys.exit(main())
