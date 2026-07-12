#!/usr/bin/env python3
"""E17: reset fisico del PSoC (ToggleReset KitProg) + auto-cal + recovery SD.

Valida la secuencia operativa de campo tras un power-cycle del PSoC:

1. estado previo sano y sesion SD COMPLETE montada (``sdinfo`` 0x31);
2. ``ToggleReset`` real por KitProg/ppcli con el ESP esclavo vivo;
3. espera de auto-calibracion con polling de ``probe`` no invasivo
   (trampa operativa: configurar antes de la auto-cal produce NACKs);
4. post-reset: la sesion COMPLETE de GEOLAST.BIN sigue recuperable,
   los ACK de configuracion vuelven a responder y una captura RAM y una
   captura SD cortas (con ``sdread``) pasan de punta a punta.

Requiere ppcli.exe (Cypress Programmer) y el firmware de banco del esclavo.

Ejemplo::

    python psoc_reset_recovery_test.py --port COM12 \
        --output "$env:TEMP/e17_reset_recovery.json"

``--self-test`` es completamente offline: no abre COM ni ejecuta ppcli.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Optional

from usb_regression_test import (
    BAUD,
    CMD_SD_CAPTURE,
    CMD_SELECT_STREAM,
    CMD_SET_DECIMATION,
    NATIVE_FS_HZ,
    PROBE_OK_RE,
    PSOC_IDLE_RE,
    CheckRecorder,
    RegressionFailure,
    SerialSession,
    UsbRegression,
    parse_status,
)
from sd_max_regression_test import atomic_json, parse_sd_status


PPCLI_DIR_DEFAULT = r"C:\Program Files (x86)\Cypress\Programmer"
KITPROG_RE = re.compile(r"^<(KitProg[^\r\n]*)$", re.M)
PPCLI_OK_RE = re.compile(r"^0 OK$", re.M)

SD_COMPLETE_MOUNTED = 0x31   # present + FAT + sesion COMPLETE
SD_CAPTURE_BIT = 0x40
# Al boot con SD presente y FAT montado el PSoC habilita g_sd_cap_en=1 por
# diseno (main.c sd_session_recover: default field-safe tras power-cycle),
# asi que post-reset el bit 0x40 debe venir ENCENDIDO.
SD_BOOT_RECOVERED = SD_COMPLETE_MOUNTED | SD_CAPTURE_BIT  # 0x71

AUTOCAL_DEADLINE_S = 150.0
AUTOCAL_POLL_S = 5.0
SMOKE_BATCHES = 4
SD_SMOKE_BATCHES = 5


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def ppcli_reset_script(kitprog: str, ppcli_dir: str) -> list[str]:
    return [
        f'OpenPort "{kitprog}" "{ppcli_dir}\\"',
        "SetProtocol 8",
        "SetProtocolClock 152",
        "SetProtocolConnector 1",
        "ToggleReset 0 100",
        "ClosePort",
        "quit",
    ]


def ppcli_run(lines: list[str], ppcli_dir: str) -> str:
    handle, path = tempfile.mkstemp(suffix=".cli", text=True)
    try:
        with os.fdopen(handle, "w", encoding="ascii", newline="\r\n") as stream:
            stream.write("\n".join(lines) + "\n")
        completed = subprocess.run(
            [os.path.join(ppcli_dir, "ppcli.exe"), f"--runfile {path.replace(os.sep, '/')}"],
            cwd=ppcli_dir,
            capture_output=True,
            text=True,
            timeout=60.0,
        )
        return completed.stdout + completed.stderr
    finally:
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass


def discover_kitprog(ppcli_dir: str) -> Optional[str]:
    output = ppcli_run(["GetPorts", "quit"], ppcli_dir)
    match = KITPROG_RE.search(output)
    return match.group(1).strip() if match else None


def reset_ok(output: str) -> bool:
    return bool(PPCLI_OK_RE.search(output)) and "ToggleReset" in output and "error" not in output.lower()


class E17Runner:
    def __init__(
        self,
        reg: UsbRegression,
        checks: CheckRecorder,
        metadata: dict[str, object],
        ppcli_dir: str,
        kitprog: Optional[str],
        skip_reset: bool,
    ) -> None:
        self.reg = reg
        self.checks = checks
        self.metadata = metadata
        self.ppcli_dir = ppcli_dir
        self.kitprog = kitprog
        self.skip_reset = skip_reset

    def sdinfo_status(self, label: str, timeout_s: float = 4.0) -> int:
        text = self.reg.command(
            "sdinfo", timeout_s,
            predicate=lambda data: parse_sd_status(data) is not None,
        )
        status = parse_sd_status(text)
        self.checks.require(f"{label}: sdinfo responde", status is not None)
        assert status is not None
        return status

    def pre_reset(self) -> None:
        print("\n=== E17 ESTADO PREVIO ===")
        self.reg.preflight()
        status = self.sdinfo_status("pre-reset")
        self.checks.require(
            "pre-reset sesion COMPLETE montada y capture off",
            status & SD_COMPLETE_MOUNTED == SD_COMPLETE_MOUNTED and not status & SD_CAPTURE_BIT,
            f"status=0x{status:02X}",
        )
        self.metadata["pre_reset"] = {"sd_status": status}

    def toggle_reset(self) -> None:
        print("\n=== E17 TOGGLERESET POR KITPROG ===")
        if self.skip_reset:
            self.checks.add("ToggleReset ejecutado", True, "OMITIDO por --skip-reset")
            self.metadata["reset"] = {"skipped": True}
            return
        kitprog = self.kitprog or discover_kitprog(self.ppcli_dir)
        self.checks.require("KitProg detectado", kitprog is not None, kitprog or "GetPorts sin KitProg")
        assert kitprog is not None
        output = ppcli_run(ppcli_reset_script(kitprog, self.ppcli_dir), self.ppcli_dir)
        self.checks.require("ToggleReset ejecutado", reset_ok(output), output[-300:])
        self.metadata["reset"] = {"skipped": False, "kitprog": kitprog, "ppcli_output": output}

    def wait_autocal(self) -> None:
        print("\n=== E17 ESPERA DE AUTO-CAL (probe no invasivo) ===")
        started = time.monotonic()
        boot_log = ""
        healthy_at: Optional[float] = None
        while time.monotonic() - started < AUTOCAL_DEADLINE_S:
            boot_log += self.reg.session.collect(AUTOCAL_POLL_S, quiet_s=0.10).text
            probe = self.reg.session.command(
                "probe", 2.5,
                predicate=lambda data: bool(PROBE_OK_RE.search(data)),
            )
            boot_log += probe.text
            status = parse_status(
                self.reg.session.command(
                    "status", 1.5,
                    predicate=lambda data: parse_status(data) is not None,
                ).text
            )
            elapsed = time.monotonic() - started
            if (
                PROBE_OK_RE.search(probe.text)
                and PSOC_IDLE_RE.search(probe.text)
                and status is not None
                and status.fs_hz == NATIVE_FS_HZ
            ):
                healthy_at = elapsed
                break
            print(f"[WAIT] auto-cal en curso ({elapsed:.0f}/{AUTOCAL_DEADLINE_S:.0f} s)")
        self.checks.require(
            "PSoC sano tras reset (psoc=1, IDLE, fs=2604)",
            healthy_at is not None,
            f"elapsed={healthy_at if healthy_at is not None else AUTOCAL_DEADLINE_S:.1f}s",
        )
        self.metadata["autocal"] = {
            "healthy_after_s": round(healthy_at, 1) if healthy_at is not None else None,
            "boot_log_tail": boot_log[-4000:],
        }

    def post_reset(self) -> None:
        print("\n=== E17 RECOVERY POST-RESET ===")
        status = self.sdinfo_status("post-reset", timeout_s=6.0)
        self.checks.require(
            "post-reset GEOLAST COMPLETE recuperado y capture re-armado (boot default)",
            status & SD_BOOT_RECOVERED == SD_BOOT_RECOVERED,
            f"status=0x{status:02X}",
        )
        self.metadata["post_reset"] = {"sd_status": status}

        # Los ACK de configuracion deben volver a responder tras la auto-cal.
        self.reg.require_config("decim 1", CMD_SET_DECIMATION, 1, "ACK 0xBB post-reset")
        self.reg.require_config("stream 0", CMD_SELECT_STREAM, 0, "ACK 0xB7 post-reset")
        self.reg.require_config("sdcap 0", CMD_SD_CAPTURE, 0, "ACK 0xBE SD=off post-reset")

        self.reg.command("clear", 1.0)
        self.reg.wait_capture(
            name=f"RAM {SMOKE_BATCHES} lotes post-reset",
            batches=SMOKE_BATCHES,
            expected_fs_hz=NATIVE_FS_HZ,
            require_uart_batches=True,
            timeout_s=15.0,
        )

        # Captura SD corta nueva: valida el camino SD completo post-reset.
        # Sobrescribe GEOLAST.BIN (la evidencia E15 vive en su JSON).
        print("\n=== E17 CAPTURA SD CORTA + SDREAD POST-RESET ===")
        self.reg.command("clear", 1.0)
        self.reg.require_config("sdcap 1", CMD_SD_CAPTURE, 1, "ACK 0xBE SD=on post-reset")
        self.reg.wait_capture(
            name=f"SD {SD_SMOKE_BATCHES} lotes post-reset",
            batches=SD_SMOKE_BATCHES,
            expected_fs_hz=NATIVE_FS_HZ,
            require_uart_batches=False,
            timeout_s=20.0,
        )
        self.reg.require_sdread(0, "sdread seq=0 post-reset")
        self.reg.require_sdread(SD_SMOKE_BATCHES - 1, f"sdread seq={SD_SMOKE_BATCHES - 1} post-reset")
        self.reg.require_sdread_oor(
            SD_SMOKE_BATCHES, SD_SMOKE_BATCHES,
            f"sdread seq={SD_SMOKE_BATCHES} rechazado OOR post-reset",
        )

    def run(self) -> None:
        self.pre_reset()
        self.toggle_reset()
        self.wait_autocal()
        self.post_reset()


def run_self_test() -> int:
    checks: list[tuple[str, bool]] = []

    def check(name: str, condition: bool) -> None:
        checks.append((name, condition))
        print(f"[{'PASS' if condition else 'FAIL'}] {name}")

    script = ppcli_reset_script("KitProg (CMSIS-DAP/236111)", PPCLI_DIR_DEFAULT)
    check("script ppcli: OpenPort con puerto", script[0].startswith('OpenPort "KitProg'))
    check("script ppcli: ToggleReset 0 100", "ToggleReset 0 100" in script)
    check("script ppcli: cierra y sale", script[-2:] == ["ClosePort", "quit"])

    ports_output = "GetPorts\n<KitProg (CMSIS-DAP/236111)\n0 OK\nquit\n<OK\n<OK\n"
    match = KITPROG_RE.search(ports_output)
    check("parser GetPorts", bool(match and match.group(1) == "KitProg (CMSIS-DAP/236111)"))

    reset_output = "OpenPort ...\n0 OK\nToggleReset 0 100\n0 OK\nClosePort\n0 OK\n"
    check("veredicto reset OK", reset_ok(reset_output))
    check("veredicto reset con error", not reset_ok(reset_output + "Error: port busy\n"))
    check("veredicto reset sin ToggleReset", not reset_ok("0 OK\n"))

    check(
        "criterio pre-reset (COMPLETE + capture off)",
        (0x37 & SD_COMPLETE_MOUNTED == SD_COMPLETE_MOUNTED and not 0x37 & SD_CAPTURE_BIT)
        and not (0x11 & SD_COMPLETE_MOUNTED == SD_COMPLETE_MOUNTED),
    )
    check(
        "criterio post-boot (COMPLETE + capture re-armado)",
        (0x77 & SD_BOOT_RECOVERED == SD_BOOT_RECOVERED)
        and not (0x37 & SD_BOOT_RECOVERED == SD_BOOT_RECOVERED)
        and not (0x5F & SD_BOOT_RECOVERED == SD_BOOT_RECOVERED),
    )
    check(
        "parser sdinfo reutilizado",
        parse_sd_status("[PSoC] SD status=0x31 present=1 type=1 selftest=0\n") == 0x31,
    )

    passed = sum(1 for _, ok in checks if ok)
    print(f"\nE17 self-test: {passed}/{len(checks)} PASS")
    return 0 if passed == len(checks) else 1


def parse_args(argv: Optional[list[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", default="COM12", help="Puerto USB del esclavo (default: COM12)")
    parser.add_argument("--output", type=Path, help="JSON de evidencia (requerido salvo --self-test)")
    parser.add_argument("--ppcli-dir", default=PPCLI_DIR_DEFAULT, help="Carpeta de ppcli.exe")
    parser.add_argument("--kitprog", help="Nombre exacto del KitProg (default: GetPorts)")
    parser.add_argument("--skip-reset", action="store_true",
                        help="No ejecuta ToggleReset (solo verifica recovery ya hecho)")
    parser.add_argument("--verbose", action="store_true", help="Mostrar cada respuesta serial")
    parser.add_argument("--self-test", action="store_true", help="Pruebas offline; no abre COM ni ppcli")
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
        "test": "E17_PSOC_RESET_RECOVERY",
        "status": "failed",
        "error": None,
        "started_at_utc": utc_now(),
        "port": args.port,
        "baud": BAUD,
        "dtr": False,
        "rts": False,
        "autocal_deadline_s": AUTOCAL_DEADLINE_S,
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
        E17Runner(reg, checks, metadata, args.ppcli_dir, args.kitprog, args.skip_reset).run()
        primary_ok = True
    except KeyboardInterrupt:
        metadata["error"] = "KeyboardInterrupt"
        print("\n[FAIL] Corrida interrumpida")
    except RegressionFailure as exc:
        metadata["error"] = f"{type(exc).__name__}: {exc}"
    except Exception as exc:
        metadata["error"] = f"{type(exc).__name__}: {exc}"
        print(f"\n[FAIL] {metadata['error']}")
    finally:
        if reg is not None:
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
