#!/usr/bin/env python3
"""E16: transiciones dirigidas de rango ADC (0xBA) y decimacion (0xBB) por USB.

Recorre las 4 configs de rango con transiciones que no pasan por el default,
factores de decimacion con divisor exacto de 2604, un combo cruzado
rango+decimacion, y rechazos locales de valores invalidos. Cada transicion se
confirma con ACK y una mini-captura RAM de 4 lotes verificando Fs efectiva y
UART sin lotes malos. Requiere el firmware de banco (SLAVE_USB_CMD_ENABLE +
SLAVE_LAB_TOOLS_ENABLE).

Ejemplo::

    python adc_decim_transitions_test.py --port COM12 \
        --output "$env:TEMP/e16_adc_decim_transitions.json"

``--self-test`` es completamente offline: no importa pyserial ni abre un COM.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import sys
import time
from pathlib import Path
from typing import Optional

from usb_regression_test import (
    BAUD,
    CMD_SD_CAPTURE,
    CMD_SELECT_STREAM,
    CMD_SET_DECIMATION,
    NATIVE_FS_HZ,
    CheckRecorder,
    RegressionFailure,
    SerialSession,
    UsbRegression,
    capture_status_errors,
    find_config_ack,
    has_ack_for_subcommand,
)
from sd_max_regression_test import atomic_json


CMD_ADC_CONFIG = 0xBA
DEFAULT_RANGE = 1  # ADC_CF_2V5, default de boot del PSoC (psoc_adc.c)
RANGE_LABELS = {1: "2V5", 2: "0V512", 3: "1V024", 4: "0V625"}

# Desde el default 1 cubre 1->2, 2->4, 4->3 y 3->1 (vuelve al default sin
# repetir transiciones triviales default->X->default).
RANGE_PATH = (2, 4, 3, 1)

# Divisores exactos de 2604 para que la Fs reportada por USB_STATUS sea
# entera sin ambiguedad de redondeo: 1->2, 2->6, 6->3, 3->4 y 4->1.
DECIM_PATH = (2, 6, 3, 4, 1)

# Combo cruzado: rango no-default + decimacion no trivial a la vez.
CROSS_RANGE = 4
CROSS_DECIM = 3

INVALID_COMMANDS = (
    ("range 0", CMD_ADC_CONFIG, 0),
    ("range 5", CMD_ADC_CONFIG, 5),
    ("decim 0", CMD_SET_DECIMATION, 0),
    ("decim 101", CMD_SET_DECIMATION, 101),
)

CAPTURE_BATCHES = 4


class E16Failure(RuntimeError):
    """Criterio de aceptacion incumplido."""


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def range_transitions() -> list[tuple[int, int]]:
    path = (DEFAULT_RANGE,) + RANGE_PATH
    return list(zip(path[:-1], path[1:]))


def decim_transitions() -> list[tuple[int, int]]:
    path = (1,) + DECIM_PATH
    return list(zip(path[:-1], path[1:]))


class E16Runner:
    def __init__(self, reg: UsbRegression, checks: CheckRecorder, metadata: dict[str, object]) -> None:
        self.reg = reg
        self.checks = checks
        self.metadata = metadata
        self.steps: list[dict[str, object]] = []
        self.metadata["steps"] = self.steps

    def capture(self, name: str, decim: int) -> None:
        # Igual que wait_capture() pero con 2 intentos dirigidos: inmediatamente
        # despues de un cambio de config el `cap` puede quedar ignorado en
        # silencio (misma clase transitoria que F6); el segundo intento tras un
        # stop corto lo resuelve y queda registrado en la evidencia.
        expected_fs = NATIVE_FS_HZ // decim
        timeout_s = max(15.0, CAPTURE_BATCHES * 30 * decim / NATIVE_FS_HZ * 2.0 + 8.0)
        errors: list[str] = ["sin intento"]
        for attempt in (1, 2):
            self.reg.command("clear", 1.0)
            baseline = self.reg.read_status()
            self.reg.command(f"cap {CAPTURE_BATCHES}", 2.0)
            deadline = time.monotonic() + timeout_s
            last = None
            while time.monotonic() < deadline:
                last = self.reg.read_status()
                if (last.fill, last.target) == (CAPTURE_BATCHES, CAPTURE_BATCHES):
                    break
                time.sleep(0.20)
            if last is None:
                errors = ["sin status posterior a cap"]
            else:
                errors = capture_status_errors(
                    last, CAPTURE_BATCHES, expected_fs, baseline.b_ok, True,
                )
            if not errors:
                self.checks.require(
                    name, True,
                    f"attempts={attempt} retry_used={'yes' if attempt == 2 else 'no'} "
                    f"fill={last.fill}/{last.target} fs={last.fs_hz} bBad={last.b_bad}",
                )
                self.reg.command("clear", 1.0)
                return
            if attempt == 1:
                print(f"[RETRY] {name} - intento 1: {'; '.join(errors)}")
                self.reg.command("stop", 1.0)
                time.sleep(0.5)
        self.checks.require(name, False, "; ".join(errors))

    def baseline(self) -> None:
        print("\n=== E16 BASELINE r1 d1 ===")
        self.reg.require_config("sdcap 0", CMD_SD_CAPTURE, 0, "ACK 0xBE SD=off (E16)")
        self.reg.require_config("stream 0", CMD_SELECT_STREAM, 0, "ACK 0xB7 stream=RAW (E16)")
        self.reg.require_config("decim 1", CMD_SET_DECIMATION, 1, "ACK 0xBB decim=1 (baseline)")
        self.reg.require_config(
            f"range {DEFAULT_RANGE}", CMD_ADC_CONFIG, DEFAULT_RANGE,
            f"ACK 0xBA range={DEFAULT_RANGE} (baseline)",
        )
        time.sleep(0.5)
        self.capture("baseline RAM 4 lotes r1 d1", 1)
        self.steps.append({"step": "baseline", "range": DEFAULT_RANGE, "decim": 1})

    def range_sweep(self) -> None:
        print("\n=== E16 TRANSICIONES DE RANGO (decim 1) ===")
        for prev, cfg in range_transitions():
            label = f"r{prev}->r{cfg} ({RANGE_LABELS[cfg]})"
            self.reg.require_config(
                f"range {cfg}", CMD_ADC_CONFIG, cfg, f"ACK 0xBA transicion {label}",
            )
            time.sleep(0.5)
            self.capture(f"RAM 4 lotes tras {label}", 1)
            self.steps.append({"step": "range", "from": prev, "to": cfg})

    def decim_sweep(self) -> None:
        print("\n=== E16 TRANSICIONES DE DECIMACION (rango 1) ===")
        for prev, factor in decim_transitions():
            label = f"d{prev}->d{factor} (fs={NATIVE_FS_HZ // factor})"
            self.reg.require_config(
                f"decim {factor}", CMD_SET_DECIMATION, factor, f"ACK 0xBB transicion {label}",
            )
            self.capture(f"RAM 4 lotes tras {label}", factor)
            self.steps.append({"step": "decim", "from": prev, "to": factor})

    def cross_combo(self) -> None:
        print(f"\n=== E16 COMBO r{CROSS_RANGE} + d{CROSS_DECIM} ===")
        self.reg.require_config(
            f"range {CROSS_RANGE}", CMD_ADC_CONFIG, CROSS_RANGE,
            f"ACK 0xBA combo range={CROSS_RANGE}",
        )
        self.reg.require_config(
            f"decim {CROSS_DECIM}", CMD_SET_DECIMATION, CROSS_DECIM,
            f"ACK 0xBB combo decim={CROSS_DECIM}",
        )
        time.sleep(0.5)
        self.capture(
            f"RAM 4 lotes combo r{CROSS_RANGE} d{CROSS_DECIM}", CROSS_DECIM,
        )
        self.steps.append({"step": "cross", "range": CROSS_RANGE, "decim": CROSS_DECIM})

    def invalid_values(self) -> None:
        print("\n=== E16 RECHAZOS LOCALES (sin ACK del PSoC) ===")
        for command, sub_cmd, value in INVALID_COMMANDS:
            # Directo sobre la sesion: el rechazo es local del ESP y no debe
            # generar trafico UART; los patrones fatales no aplican aca.
            result = self.reg.session.command(command, 1.5)
            ack = find_config_ack(result.text, sub_cmd, value)
            any_ack = has_ack_for_subcommand(result.text, sub_cmd)
            self.checks.require(
                f"'{command}' rechazado localmente",
                ack is None and not any_ack and not result.truncated,
                f"sub=0x{sub_cmd:02X} val={value}",
            )
            self.steps.append({"step": "invalid", "command": command})
        # El banco debe seguir sano despues de los rechazos.
        self.capture("RAM 4 lotes post-rechazos (config previa intacta)", CROSS_DECIM)

    def restore(self) -> None:
        print("\n=== E16 RESTAURACION r1 d1 ===")
        self.reg.require_config(
            f"range {DEFAULT_RANGE}", CMD_ADC_CONFIG, DEFAULT_RANGE,
            f"ACK 0xBA restore range={DEFAULT_RANGE}",
        )
        self.reg.require_config("decim 1", CMD_SET_DECIMATION, 1, "ACK 0xBB restore decim=1")
        time.sleep(0.5)
        self.capture("RAM 4 lotes post-restore r1 d1", 1)
        self.steps.append({"step": "restore", "range": DEFAULT_RANGE, "decim": 1})

    def run(self) -> None:
        self.reg.preflight()
        self.baseline()
        self.range_sweep()
        self.decim_sweep()
        self.cross_combo()
        self.invalid_values()
        self.restore()


def run_self_test() -> int:
    checks: list[tuple[str, bool]] = []

    def check(name: str, condition: bool) -> None:
        checks.append((name, condition))
        print(f"[{'PASS' if condition else 'FAIL'}] {name}")

    transitions = range_transitions()
    check("rango: cubre las 4 configs", {cfg for _, cfg in transitions} | {DEFAULT_RANGE} == {1, 2, 3, 4})
    check("rango: termina en default", transitions[-1][1] == DEFAULT_RANGE)
    check("rango: sin transiciones triviales", all(prev != cfg for prev, cfg in transitions))
    check(
        "rango: encadenado consistente",
        all(transitions[i][1] == transitions[i + 1][0] for i in range(len(transitions) - 1)),
    )

    decims = decim_transitions()
    check("decim: divisores exactos de 2604", all(NATIVE_FS_HZ % factor == 0 for _, factor in decims))
    check("decim: termina en 1", decims[-1][1] == 1)
    check("decim: sin transiciones triviales", all(prev != factor for prev, factor in decims))
    check("decim: fs esperadas", [NATIVE_FS_HZ // f for _, f in decims] == [1302, 434, 868, 651, 2604])

    check("combo: divisor exacto", NATIVE_FS_HZ % CROSS_DECIM == 0 and CROSS_RANGE in RANGE_LABELS)

    ack_line = "[SLAVE 1] PSoC cfg ack sub=0xBA val=4 expected=4 ok=1\n"
    check("parser ACK 0xBA", find_config_ack(ack_line, CMD_ADC_CONFIG, 4) is not None)
    check("parser ACK 0xBA val distinto", find_config_ack(ack_line, CMD_ADC_CONFIG, 2) is None)
    rejected = "[USB] unknown command 'range 5' (use: help)\n"
    check("rechazo sin ACK detectable", not has_ack_for_subcommand(rejected, CMD_ADC_CONFIG))
    negative = "[SLAVE 1] PSoC cfg ack sub=0xBA val=0 expected=5 ok=0\n"
    check("ACK negativo no cuenta como valido", find_config_ack(negative, CMD_ADC_CONFIG, 5) is None)

    passed = sum(1 for _, ok in checks if ok)
    print(f"\nE16 self-test: {passed}/{len(checks)} PASS")
    return 0 if passed == len(checks) else 1


def parse_args(argv: Optional[list[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", default="COM12", help="Puerto USB del esclavo (default: COM12)")
    parser.add_argument("--output", type=Path, help="JSON de evidencia (requerido salvo --self-test)")
    parser.add_argument("--verbose", action="store_true", help="Mostrar cada respuesta serial")
    parser.add_argument("--self-test", action="store_true", help="Pruebas offline; no abre COM")
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
        "test": "E16_ADC_DECIM_TRANSITIONS",
        "status": "failed",
        "error": None,
        "started_at_utc": utc_now(),
        "port": args.port,
        "baud": BAUD,
        "dtr": False,
        "rts": False,
        "range_path": list(RANGE_PATH),
        "decim_path": list(DECIM_PATH),
        "cross": {"range": CROSS_RANGE, "decim": CROSS_DECIM},
    }
    checks = CheckRecorder()
    session: Optional[SerialSession] = None
    reg: Optional[UsbRegression] = None
    primary_ok = False
    json_ok = False
    try:
        print(f"Abriendo {args.port} a {BAUD} baud (DTR=False, RTS=False)...")
        session = SerialSession.open(args.port)
        reg = UsbRegression(session, checks, args.verbose)
        E16Runner(reg, checks, metadata).run()
        primary_ok = True
    except KeyboardInterrupt:
        metadata["error"] = "KeyboardInterrupt"
        print("\n[FAIL] Corrida interrumpida")
    except (RegressionFailure, E16Failure) as exc:
        metadata["error"] = f"{type(exc).__name__}: {exc}"
    except Exception as exc:
        metadata["error"] = f"{type(exc).__name__}: {exc}"
        print(f"\n[FAIL] {metadata['error']}")
    finally:
        if reg is not None:
            # El cleanup heredado no conoce 0xBA: reponer el rango default
            # primero, incluso si la corrida murio a mitad del barrido.
            try:
                result = reg.session.command(
                    f"range {DEFAULT_RANGE}", 2.0,
                    predicate=lambda data: has_ack_for_subcommand(data, CMD_ADC_CONFIG),
                )
                checks.add(
                    "cleanup ACK range default",
                    find_config_ack(result.text, CMD_ADC_CONFIG, DEFAULT_RANGE) is not None,
                    f"sub=0x{CMD_ADC_CONFIG:02X} val={DEFAULT_RANGE}",
                )
            except Exception as exc:
                checks.add("cleanup ACK range default", False, repr(exc))
            try:
                reg.cleanup()
            except Exception as exc:
                checks.add("cleanup ejecutado", False, repr(exc))
        if session is not None:
            session.close()
        all_pass = checks.summary()
        metadata["completed_at_utc"] = utc_now()
        metadata["checks"] = [
            {"name": name, "passed": passed, "detail": detail}
            for name, passed, detail in checks.results
        ]
        if primary_ok and all_pass:
            metadata["status"] = "ok"
            metadata["error"] = None
        elif metadata["error"] is None:
            metadata["error"] = "checks con fallos"
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
