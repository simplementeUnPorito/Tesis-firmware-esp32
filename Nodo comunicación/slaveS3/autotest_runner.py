#!/usr/bin/env python3
"""Runner de PC para el autotest de placa del nodo esclavo (firmware `slaveTest`).

Abre el USB del esclavo, dispara la corrida automatica, parsea el checklist y la
linea ``#JSON``, y emite un veredicto reproducible mas un JSON de evidencia con
el mismo formato que los runners E15/E16/E17 del banco.

Por que existe: el checklist por serial es para leer con los ojos. Para que una
placa quede aprobada de forma reproducible hace falta un gate que ademas
verifique que la corrida fue COMPLETA. Una placa que muere al tercer item
imprime tres PASS y ningun FAIL; sin este runner eso parece un exito.

Ejemplo::

    python autotest_runner.py --port COM8 \\
        --output artifacts/autotest_placa_2026-09-01.json

``--self-test`` es completamente offline: no importa pyserial ni abre un COM.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

BAUD = 115200

# Items que TIENEN que haber corrido para que el veredicto valga. Si falta
# alguno la corrida quedo trunca, y una corrida trunca sin FAIL no es un PASS.
# B1/B2/B3 son los tres cables; C6 es la SD con el ruteo nuevo de SPIp; D2 es la
# matriz de transferencia y D8 la calibracion.
# Estos corren siempre, haya o no PSoC. Si falta alguno, la corrida se corto.
REQUIRED_ALWAYS = ("A1", "A2", "A3", "A4", "B1", "B4")

# Estos dependen del PSoC. Si el PSoC no contesta, el firmware emite los
# marcadores `C*` y `D*` y la corrida es PARCIAL A PROPOSITO, no truncada: no
# tiene sentido exigirlos.
REQUIRED_WITH_PSOC = ("B2", "B3", "C1", "C4", "C6", "D1", "D2", "D8")

# Marcadores que el firmware emite cuando saltea un grupo entero.
SKIP_MARKERS = ("C*", "D*")

VERDICTS = ("PASS", "FAIL", "WARN", "SKIP", "INFO")

# `[A3] Pull-ups de botones .......... FAIL  detalle`
ITEM_RE = re.compile(
    r"^\[([A-Za-z0-9_.*]{1,8})\]\s+(.*?)[\s.]*\s(" + "|".join(VERDICTS) + r")(?:\s\s(.*))?$"
)
JSON_RE = re.compile(r"^#JSON\s+(\{.*\})\s*$")
SUMMARY_RE = re.compile(r"^RESUMEN\s+.*VEREDICTO:\s+(APTO|NO APTO)\s*$")
END_RE = re.compile(r"^=+$")


class AutotestFailure(RuntimeError):
    pass


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


# --------------------------------------------------------------------------
# Parseo
# --------------------------------------------------------------------------
def parse_report(text: str) -> dict:
    """Convierte la salida cruda del esclavo en una estructura verificable.

    No decide nada: solo extrae. El veredicto lo da `evaluate`.
    """
    items: list[dict] = []
    payload: Optional[dict] = None
    summary_verdict: Optional[str] = None

    for raw in text.splitlines():
        line = raw.rstrip("\r")

        m = JSON_RE.match(line)
        if m:
            try:
                payload = json.loads(m.group(1))
            except json.JSONDecodeError as exc:
                raise AutotestFailure(f"linea #JSON invalida: {exc}") from exc
            continue

        m = SUMMARY_RE.match(line)
        if m:
            summary_verdict = m.group(1)
            continue

        m = ITEM_RE.match(line)
        if m:
            items.append(
                {
                    "code": m.group(1),
                    "name": m.group(2).strip(" ."),
                    "verdict": m.group(3),
                    "detail": (m.group(4) or "").strip(),
                }
            )

    return {"items": items, "json": payload, "summary_verdict": summary_verdict}


def counts_from_items(items: list[dict]) -> dict:
    out = {v: 0 for v in VERDICTS}
    for it in items:
        out[it["verdict"]] += 1
    return out


# --------------------------------------------------------------------------
# Gate
# --------------------------------------------------------------------------
def evaluate(parsed: dict) -> dict:
    """Aplica los invariantes y devuelve el veredicto del runner."""
    problems: list[str] = []
    items = parsed["items"]
    payload = parsed["json"]

    if payload is None:
        problems.append("no llego la linea #JSON: corrida incompleta o puerto perdido")
    if parsed["summary_verdict"] is None:
        problems.append("no llego la linea RESUMEN")
    if not items:
        problems.append("no se parseo ningun item del checklist")

    counts = counts_from_items(items)
    codes = {it["code"] for it in items}

    # Corrida parcial deliberada: el firmware avisa con `C*` / `D*` que salteo
    # esos grupos porque el PSoC no contesta. Eso no es una corrida truncada.
    partial = any(m in codes for m in SKIP_MARKERS)

    requeridos = list(REQUIRED_ALWAYS)
    if not partial:
        requeridos += list(REQUIRED_WITH_PSOC)

    faltantes = [c for c in requeridos if c not in codes]
    if faltantes:
        problems.append(f"items obligatorios ausentes: {', '.join(faltantes)}")

    fails = sorted(it["code"] for it in items if it["verdict"] == "FAIL")
    warns = sorted(it["code"] for it in items if it["verdict"] == "WARN")

    # El JSON del firmware y el checklist parseado tienen que coincidir. Si no,
    # se perdieron lineas por el camino y ningun conteo es confiable.
    if payload is not None:
        if payload.get("pass") != counts["PASS"]:
            problems.append(
                f"PASS del #JSON ({payload.get('pass')}) != items parseados ({counts['PASS']})"
            )
        if sorted(payload.get("fail", [])) != fails:
            problems.append(
                f"lista FAIL del #JSON {sorted(payload.get('fail', []))} != parseada {fails}"
            )
        if payload.get("skip") != counts["SKIP"]:
            problems.append(
                f"SKIP del #JSON ({payload.get('skip')}) != items parseados ({counts['SKIP']})"
            )
        json_verdict = payload.get("verdict")
        expected = "FAIL" if fails else "PASS"
        if json_verdict != expected:
            problems.append(f"verdict del #JSON '{json_verdict}' no coincide con la lista FAIL")

    if parsed["summary_verdict"] is not None:
        esperado = "NO APTO" if fails else "APTO"
        if parsed["summary_verdict"] != esperado:
            problems.append(
                f"RESUMEN dice '{parsed['summary_verdict']}' pero la lista FAIL dice '{esperado}'"
            )

    ok = (not problems) and (not fails)
    if partial:
        cobertura = "parcial_sin_psoc"
    elif problems:
        cobertura = "incompleta"
    else:
        cobertura = "completa"

    return {
        "ok": ok,
        "partial": partial,
        "coverage": cobertura,
        "board_verdict": (
            "NO APTO" if (fails or problems)
            else ("APTO (parcial: sin PSoC)" if partial else "APTO")
        ),
        "counts": counts,
        "fails": fails,
        "warns": warns,
        "missing_required": faltantes,
        "problems": problems,
        "item_count": len(items),
    }


# --------------------------------------------------------------------------
# Serie
# --------------------------------------------------------------------------
def collect_report(port: str, timeout_s: float, trigger: bool, verbose: bool) -> str:
    """Lee del USB hasta juntar una corrida entera.

    DTR y RTS en False de forma obligatoria: con los valores por defecto de
    pyserial el ESP32 se resetea al abrir el puerto (regla del banco).
    """
    import serial  # import diferido: --self-test no necesita pyserial

    with serial.Serial(
        port=port, baudrate=BAUD, timeout=0.2, dsrdtr=False, rtscts=False
    ) as ser:
        ser.dtr = False
        ser.rts = False
        time.sleep(0.3)
        ser.reset_input_buffer()

        if trigger:
            ser.write(b"\r\nrun\r\n")
            ser.flush()

        # OJO: NO usar ser.readline(). Con timeout, pyserial devuelve lo que
        # tenga acumulado aunque no haya llegado el fin de linea, asi que una
        # linea puede salir partida en dos trozos. Ninguno de los dos matchea
        # el parser y el item se pierde EN SILENCIO: el runner reportaria
        # "corrida trunca" sobre una corrida perfecta. Se acumula en un buffer
        # y se corta por salto de linea a mano.
        lines: list[str] = []
        pending = ""
        seen_json = False
        done = False
        t0 = time.time()

        while not done and (time.time() - t0) < timeout_s:
            n = ser.in_waiting or 1
            data = ser.read(n).decode("utf-8", errors="replace")
            if not data:
                if seen_json:
                    break
                continue
            if verbose:
                sys.stdout.write(data)
                sys.stdout.flush()

            pending += data
            while "\n" in pending:
                line, pending = pending.split("\n", 1)
                line = line.rstrip("\r")
                lines.append(line)
                if JSON_RE.match(line):
                    seen_json = True
                elif seen_json and END_RE.match(line.strip()):
                    pending = ""
                    done = True
                    break

        if pending:
            lines.append(pending.rstrip("\r"))
        return "\n".join(lines) + "\n"


# --------------------------------------------------------------------------
# Self-test offline
# --------------------------------------------------------------------------
GOOD_REPORT = """=========== AUTOTEST NODO ESCLAVO ===========
ESP  slaveTest Sep  1 2026 10:00:00   MAC 24:6F:28:11:22:33   NODE_ID=2
PSoC AcondAnalogTest (ver DIAG BOOT)
HW   oled=auto btn=auto geo=auto sd=auto psoc=auto
[A1] Arranque ESP32 ....................... PASS  POWERON, heap 218 KB
[A2] OLED SSD1306 por SPI ................. PASS  responde en SPI
[A3] Pull-ups de botones .................. PASS  4/4 en alto estable
[A4] ESP-NOW (init + peer + TX) ........... PASS  esp_now_send=0
[A5] Radio / maestro visible .............. INFO  GeoNetwork en canal 1
[A6] GPIO25 (ex PSOC_UART_RX) ............. SKIP  sin conectar por diseno
[B1] Subida I2C PSoC->ESP (0x42) .......... PASS  4200 B/1.5s
[B2] Bajada UART ESP->PSoC ................ PASS  STATUS respondido en 41 ms
[B3] Linea SYNC GPIO27 -> P0[4] ........... PASS  20 flancos en 10 ciclos
[C1] Identidad del PSoC ................... PASS  clase=GEO, Fs=2604 Hz
[C4] Camino digital E2E (rampa cruda) ..... PASS  8/8 lotes, 99% crecientes
[C6] SD FatFs (ruteo SPIp nuevo) .......... PASS  SDHC, FAT montado
[D1] Reposo de los taps analogicos ........ PASS  ch0=12mV ch1=-4mV
[D2] Matriz DC IDAC->etapa ................ PASS  4/4 barridos
[D8] Auto-calibracion ..................... PASS  IDAC 142/118/97/163
[B4] Integridad de trama .................. PASS  bBad=0 badLen=0

RESUMEN  14 PASS - 0 FAIL - 0 WARN - 1 SKIP - 1 INFO     VEREDICTO: APTO
#JSON {"verdict":"PASS","pass":14,"fail":[],"warn":[],"skip":1,"info":1}
=============================================
"""

BAD_REPORT = GOOD_REPORT.replace(
    "[A3] Pull-ups de botones .................. PASS  4/4 en alto estable",
    "[A3] Pull-ups de botones .................. FAIL  BTN_OK(36) flotante",
).replace(
    "RESUMEN  14 PASS - 0 FAIL - 0 WARN - 1 SKIP - 1 INFO     VEREDICTO: APTO",
    "RESUMEN  13 PASS - 1 FAIL - 0 WARN - 1 SKIP - 1 INFO     VEREDICTO: NO APTO",
).replace(
    '#JSON {"verdict":"PASS","pass":14,"fail":[],"warn":[],"skip":1,"info":1}',
    '#JSON {"verdict":"FAIL","pass":13,"fail":["A3"],"warn":[],"skip":1,"info":1}',
)

# Corrida que muere despues de B1: sin FAIL, pero incompleta.
TRUNCATED_REPORT = "\n".join(GOOD_REPORT.splitlines()[:9]) + "\n"


NO_PSOC_REPORT = """=========== AUTOTEST NODO ESCLAVO ===========
ESP  slaveTest Sep  1 2026 10:00:00   MAC 24:6F:28:11:22:33   NODE_ID=2
PSoC AcondAnalogTest (ver DIAG BOOT)
HW   oled=auto btn=auto geo=auto sd=auto psoc=auto
[A1] Arranque ESP32 ....................... PASS  POWERON, heap 218 KB
[A2] OLED SSD1306 por SPI ................. INFO  patron dibujado
[A3] Pull-ups de botones .................. PASS  4/4 en alto estable
[A4] ESP-NOW (init + peer + TX) ........... PASS  esp_now_send=0
[A5] Radio / maestro visible .............. INFO  3 redes
[A6] GPIO25 (ex PSOC_UART_RX) ............. SKIP  sin conectar por diseno
[B1] Subida I2C PSoC->ESP (0x42) .......... WARN  silencio total en 1.5 s
[B2] Bajada UART ESP->PSoC ................ SKIP  sin enlace de subida
[B3] Linea SYNC GPIO27 -> P0[4] ........... SKIP  sin enlace de subida
[C*] Tests del PSoC ....................... SKIP  sin enlace con el PSoC
[D*] Tests analogicos ..................... SKIP  sin enlace con el PSoC
[B4] Integridad de trama .................. SKIP  no llego ningun byte

RESUMEN  3 PASS - 0 FAIL - 1 WARN - 6 SKIP - 2 INFO     VEREDICTO: APTO
#JSON {"verdict":"PASS","pass":3,"fail":[],"warn":["B1"],"skip":6,"info":2}
=============================================
"""


class _FakeSerial:
    """Puerto serie de mentira que entrega los datos en trozos arbitrarios.

    Reproduce lo que hace pyserial con timeout: devolver lo que haya llegado,
    aunque la linea este partida al medio.
    """

    def __init__(self, texto: str, trozo: int):
        self._data = texto.encode()
        self._i = 0
        self._trozo = trozo
        self.dtr = True
        self.rts = True

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False

    @property
    def in_waiting(self) -> int:
        return min(self._trozo, len(self._data) - self._i)

    def read(self, n: int) -> bytes:
        n = min(n, self._trozo, len(self._data) - self._i)
        out = self._data[self._i:self._i + n]
        self._i += n
        return out

    def reset_input_buffer(self):
        pass

    def write(self, b):
        pass

    def flush(self):
        pass


def _self_test_lectura_fragmentada(check) -> None:
    """Verifica collect_report contra un puerto que parte las lineas.

    Es el caso que rompia la version anterior, que usaba ser.readline(): una
    linea partida no matchea el parser y el item se perdia en silencio, con lo
    que una corrida perfecta se reportaba como trunca.
    """
    import sys as _sys
    import types as _types

    esperado = len([l for l in GOOD_REPORT.splitlines() if ITEM_RE.match(l)])
    previo = _sys.modules.get("serial")
    try:
        for trozo in (1, 3, 13, 64, 4096):
            mod = _types.ModuleType("serial")
            mod.Serial = lambda *a, _t=trozo, **k: _FakeSerial(GOOD_REPORT, _t)
            _sys.modules["serial"] = mod
            texto = collect_report("COM_FALSO", 30.0, False, False)
            res = evaluate(parse_report(texto))
            check(
                "lectura en trozos de %d B: %d items, sin problemas" % (trozo, esperado),
                res["item_count"] == esperado and res["problems"] == [],
            )
    finally:
        if previo is not None:
            _sys.modules["serial"] = previo
        else:
            _sys.modules.pop("serial", None)


def run_self_test() -> int:
    checks: list[tuple[str, bool]] = []

    def check(name: str, cond: bool) -> None:
        checks.append((name, cond))
        print(f"[{'PASS' if cond else 'FAIL'}] {name}")

    good = evaluate(parse_report(GOOD_REPORT))
    check("placa sana: ok", good["ok"])
    check("placa sana: veredicto APTO", good["board_verdict"] == "APTO")
    check("placa sana: 16 items parseados", good["item_count"] == 16)
    check("placa sana: sin problemas de consistencia", good["problems"] == [])
    check("placa sana: conteo PASS = 14", good["counts"]["PASS"] == 14)

    bad = evaluate(parse_report(BAD_REPORT))
    check("placa con falla: no ok", not bad["ok"])
    check("placa con falla: lista FAIL = [A3]", bad["fails"] == ["A3"])
    check("placa con falla: veredicto NO APTO", bad["board_verdict"] == "NO APTO")
    check("placa con falla: sin falsos problemas", bad["problems"] == [])

    trunc = evaluate(parse_report(TRUNCATED_REPORT))
    check("corrida trunca: no ok pese a no tener FAIL", not trunc["ok"] and not trunc["fails"])
    check("corrida trunca: detecta items faltantes", bool(trunc["missing_required"]))
    check("corrida trunca: detecta falta de #JSON",
          any("#JSON" in p for p in trunc["problems"]))

    # Un #JSON que no coincide con el checklist significa lineas perdidas.
    mismatch = GOOD_REPORT.replace('"pass":14', '"pass":99')
    mm = evaluate(parse_report(mismatch))
    check("desajuste #JSON vs checklist: detectado",
          not mm["ok"] and any("PASS del #JSON" in p for p in mm["problems"]))

    # Un firmware que dijera APTO teniendo un FAIL tiene que ser rechazado.
    lying = BAD_REPORT.replace("VEREDICTO: NO APTO", "VEREDICTO: APTO")
    ly = evaluate(parse_report(lying))
    check("RESUMEN mentiroso: detectado",
          any("RESUMEN dice" in p for p in ly["problems"]))

    # El parser tiene que tolerar codigos con punto (D2.0, D6.3).
    dotted = parse_report("[D6.3] Piso de ruido del tap .............. WARN  50Hz alto\n")
    check("parser: codigos con punto", dotted["items"] and dotted["items"][0]["code"] == "D6.3")

    detail = parse_report(GOOD_REPORT)["items"][0]["detail"]
    check("parser: conserva el detalle", detail.startswith("POWERON"))

    nop = evaluate(parse_report(NO_PSOC_REPORT))
    check("sin PSoC: reconocido como parcial", nop["partial"])
    check("sin PSoC: cobertura parcial_sin_psoc", nop["coverage"] == "parcial_sin_psoc")
    check("sin PSoC: no exige items del PSoC", nop["missing_required"] == [])
    check("sin PSoC: sin problemas de consistencia", nop["problems"] == [])
    check("sin PSoC: no reprueba la placa", not nop["fails"])
    check("placa sana: cobertura completa", good["coverage"] == "completa")
    check("corrida trunca: cobertura incompleta", trunc["coverage"] == "incompleta")

    _self_test_lectura_fragmentada(check)

    passed = sum(1 for _, ok in checks if ok)
    print(f"\nautotest_runner self-test: {passed}/{len(checks)} PASS")
    return 0 if passed == len(checks) else 1


# --------------------------------------------------------------------------
def parse_args(argv: Optional[list[str]] = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--port", default="COM8",
               help="Puerto USB del esclavo, el CP210x del ESP32 (default: COM8). "
                    "NO es el COM del KitProg, que es el del PSoC.")
    p.add_argument("--output", type=Path, help="JSON de evidencia (requerido salvo --self-test)")
    p.add_argument("--timeout", type=float, default=900.0,
                   help="Segundos de espera de la corrida (default: 900). El grupo D "
                        "puede tardar minutos y D8 tiene tope propio de 180 s.")
    p.add_argument("--no-trigger", action="store_true",
                   help="No mandar 'run': engancharse a la corrida que arranca sola al boot")
    p.add_argument("--verbose", action="store_true", help="Eco de la salida cruda")
    p.add_argument("--self-test", action="store_true", help="Pruebas offline; no abre COM")
    p.add_argument("--from-file", type=Path,
                   help="Evaluar una captura ya guardada en vez de abrir el COM")
    args = p.parse_args(argv)
    if not args.self_test and args.output is None:
        p.error("--output es requerido")
    return args


def main(argv: Optional[list[str]] = None) -> int:
    args = parse_args(argv)
    if args.self_test:
        return run_self_test()

    metadata: dict[str, object] = {
        "schema_version": 1,
        "test": "AUTOTEST_PLACA_NODO_ESCLAVO",
        "status": "failed",
        "error": None,
        "started_at_utc": utc_now(),
        "port": None if args.from_file else args.port,
        "baud": BAUD,
        "dtr": False,
        "rts": False,
        "required_always": list(REQUIRED_ALWAYS),
        "required_with_psoc": list(REQUIRED_WITH_PSOC),
    }

    try:
        if args.from_file:
            raw = args.from_file.read_text(encoding="utf-8", errors="replace")
        else:
            print(f"Abriendo {args.port} a {BAUD} baud (DTR=False, RTS=False)...")
            raw = collect_report(args.port, args.timeout, not args.no_trigger, args.verbose)

        parsed = parse_report(raw)
        result = evaluate(parsed)

        metadata.update(
            {
                "finished_at_utc": utc_now(),
                "raw_lines": len(raw.splitlines()),
                "items": parsed["items"],
                "firmware_json": parsed["json"],
                "summary_verdict": parsed["summary_verdict"],
                "counts": result["counts"],
                "fails": result["fails"],
                "warns": result["warns"],
                "missing_required": result["missing_required"],
                "problems": result["problems"],
                "board_verdict": result["board_verdict"],
                "coverage": result["coverage"],
                "status": "ok" if result["ok"] else "failed",
            }
        )

        print()
        print(f"Items parseados : {result['item_count']}")
        print(f"Conteo          : {result['counts']}")
        if result["fails"]:
            print(f"FAIL            : {', '.join(result['fails'])}")
        if result["warns"]:
            print(f"WARN            : {', '.join(result['warns'])}")
        for p_ in result["problems"]:
            print(f"PROBLEMA        : {p_}")
        print(f"COBERTURA       : {result['coverage']}")
        print(f"VEREDICTO PLACA : {result['board_verdict']}")

        # 0 = todo bien y cobertura completa
        # 1 = hay FAIL o la corrida quedo trunca
        # 3 = sin FAIL pero cobertura parcial por PSoC ausente (a proposito)
        if not result["ok"]:
            return 1
        return 3 if result["partial"] else 0

    except Exception as exc:  # noqa: BLE001 - se registra en la evidencia
        metadata["error"] = f"{type(exc).__name__}: {exc}"
        print(f"ERROR: {metadata['error']}", file=sys.stderr)
        return 2
    finally:
        if args.output is not None:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(
                json.dumps(metadata, indent=2, ensure_ascii=False), encoding="utf-8"
            )
            print(f"Evidencia: {args.output}")


if __name__ == "__main__":
    raise SystemExit(main())
