"""Verifica que lo que IMPRIME selftest_report.cpp sea exactamente lo que
PARSEA autotest_runner.py.

Los dos se escribieron por separado y nunca se conectaron. Si el ancho de la
columna, el relleno de puntos o el separador cambian, el runner deja de
reconocer items y reporta "corrida trunca" sobre una corrida perfecta.

Se porta a Python el formateo del C y se alimenta al parser real del runner.
"""
import importlib.util
import sys
from pathlib import Path

RUNNER = Path(
    r"C:\Github\Tesis\src\firmware\esp32\Nodo comunicación\slave\autotest_runner.py"
)

spec = importlib.util.spec_from_file_location("autotest_runner", RUNNER)
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

# ---- Transcripcion de formatItemLabel() y stReportItem() de selftest_report.cpp
LABEL_W = 44          # kItemLabelWidth
CODE_SZ = 8           # kStoredCodeSize


def format_item_label(code, name):
    code = code or ""
    code_len = min(len(code), CODE_SZ - 1)
    prefix_len = code_len + 3
    name_cap = LABEL_W - prefix_len - 2

    label = "[%s] %s" % (code[:code_len], (name or "")[:name_cap])
    pos = min(len(label), LABEL_W - 2)
    label = label[:pos]
    label += " "
    pos += 1
    while pos < LABEL_W - 1:
        label += "."
        pos += 1
    label += " "
    return label


def report_item(code, name, verdict, detail=""):
    label = format_item_label(code, name)
    if detail:
        return "%s%s  %s" % (label, verdict, detail)
    return "%s%s" % (label, verdict)


# ---- Corrida sintetica que ejercita los casos raros de formato --------------
ITEMS = [
    ("A1", "Arranque ESP32", "PASS", "POWERON, heap 218 KB, flash 4 MB"),
    ("A2", "OLED SSD1306 por SPI", "INFO",
     "patron dibujado. El SPI no tiene lectura: confirmalo con la vista"),
    ("A3", "Pull-ups de botones", "FAIL",
     "OK(36) en bajo -> falta R 10k a 3V3, pulsador en corto, o sin soldar"),
    ("A4", "ESP-NOW (init + peer + TX)", "PASS", "esp_now_send=0, canal 1"),
    ("A5", "Radio / maestro visible", "INFO", "3 redes, GeoNetwork no visible"),
    ("A6", "GPIO25 (ex PSOC_UART_RX)", "SKIP", "sin conectar por diseno"),
    ("B1", "Subida I2C PSoC->ESP (0x42)", "PASS", "4200 B/1.5s, 2 pings"),
    ("B2", "Bajada UART ESP->PSoC + vuelta por I2C", "PASS", "STATUS respondido en 41 ms"),
    ("B3", "Linea SYNC GPIO27 -> P0[4]", "PASS", "20 flancos en 10 ciclos (ambos flancos)"),
    ("C1", "Identidad del PSoC", "PASS", "clase=GEO, Fs=2604 Hz, 4 etapas, 5 canales AMux"),
    ("C2", "Sin trampas de IRQ / HardFault", "PASS", "IRQ inesperadas=0, hardfaults=0"),
    ("C3", "EEPROM de calibracion (CRC-16)", "WARN", "0/9 slots: placa sin calibrar todavia"),
    ("C4", "Camino digital E2E (rampa cruda)", "PASS", "8/8 lotes, 240 muestras, 99% crecientes"),
    ("C5", "Filtro FIR de hardware (DFB)", "PASS", "RMS FIR/crudo = 0.31 (12.4 vs 40.1)"),
    ("C6", "SD FatFs (ruteo SPIp nuevo)", "PASS", "0x37 tipo=2 FAT montado, escritura y lectura OK"),
    ("C7", "Pulsador del PSoC en reposo", "PASS", "nivel 1 estable en 5 lecturas"),
    ("D1", "Reposo de los taps analogicos", "PASS", "ch0=12mV ch1=-4mV ch2=3mV ch3=-1mV"),
    ("D1b", "AMuxCapacitor (100 nF a Vss)", "INFO", "2 mV (cerca de Vss es lo esperado)"),
    ("D2", "Matriz DC IDAC->etapa", "PASS",
     "4/4 barridos, triangular superior OK, aislamiento < 10%"),
    ("D2.0", "  pendientes uV/codigo", "INFO", "3721.4 12.1 8.0 4.2"),
    ("D2.1", "  pendientes uV/codigo", "INFO", "1.1 3698.2 15.3 9.7"),
    ("D2.2", "  pendientes uV/codigo", "INFO", "0.4 2.2 3702.9 3611.0"),
    ("D2.3", "  pendientes uV/codigo", "INFO", "0.2 0.9 1.8 3745.1"),
    ("D3", "Asentamiento del ultimo tap", "PASS", "deriva 5ms->500ms = 812 uV"),
    ("D4", "Ganancia PGAout 1x vs 4x", "PASS", "cociente 3.97 (nominal 4.00, tol 25%)"),
    ("D6.0", "Piso de ruido del tap", "PASS", "media 12000 uV, RMS 41 uV, pp 210 uV, 50Hz 18 uV"),
    ("D6.1", "Piso de ruido del tap", "PASS", "media -4000 uV, RMS 38 uV, pp 190 uV, 50Hz 15 uV"),
    ("D6.2", "Piso de ruido del tap", "WARN", "media 3000 uV, RMS 52 uV, pp 300 uV, 50Hz 140000 uV"),
    ("D6.3", "Piso de ruido del tap", "PASS", "media -1000 uV, RMS 44 uV, pp 220 uV, 50Hz 22 uV"),
    ("D6b", "Carga de la entrada (indicador de geofono)", "INFO",
     "ch0: RMS 41 uV, 50 Hz 18 uV. Con geofono baja"),
    ("D8", "Auto-calibracion", "PASS", "IDAC 142/118/97/163, 0 etapa(s) al riel, 17 s"),
    ("D5", "Coherencia de rangos del ADC", "PASS", "dispersion 0.8% sobre 148000 uV"),
    ("B4", "Integridad de trama en toda la corrida", "PASS", "bBad=0 badLen=0 (drop=58 informativo)"),
]


def build_report():
    lines = []
    lines.append("=========== AUTOTEST NODO ESCLAVO ===========")
    lines.append("ESP  slaveTest Sep  1 2026 11:00:00   MAC 24:6F:28:11:22:33   NODE_ID=2")
    lines.append("PSoC AcondAnalogTest (ver DIAG BOOT)")
    lines.append("HW   oled=auto btn=auto geo=auto sd=auto psoc=auto")
    lines.append("--- grupo A: ESP32 solo ---")
    for code, name, verdict, detail in ITEMS:
        lines.append(report_item(code, name, verdict, detail))

    n = {v: 0 for v in ("PASS", "FAIL", "WARN", "SKIP", "INFO")}
    fails, warns = [], []
    for code, _, v, _ in ITEMS:
        n[v] += 1
        if v == "FAIL":
            fails.append(code)
        elif v == "WARN":
            warns.append(code)

    lines.append("")
    lines.append(
        "RESUMEN  %d PASS - %d FAIL - %d WARN - %d SKIP - %d INFO     VEREDICTO: %s"
        % (n["PASS"], n["FAIL"], n["WARN"], n["SKIP"], n["INFO"],
           "NO APTO" if fails else "APTO")
    )
    if fails:
        lines.append("FAIL: " + ", ".join(fails))
    if warns:
        lines.append("WARN: " + ", ".join(warns))
    lines.append(
        '#JSON {"verdict":"%s","pass":%d,"fail":[%s],"warn":[%s],"skip":%d,"info":%d}'
        % ("FAIL" if fails else "PASS", n["PASS"],
           ",".join('"%s"' % c for c in fails),
           ",".join('"%s"' % c for c in warns),
           n["SKIP"], n["INFO"])
    )
    lines.append("=============================================")
    return "\n".join(lines) + "\n", n, fails, warns


def main():
    texto, n, fails, warns = build_report()

    print("=== Muestra de la salida tal como la imprimiria el firmware ===\n")
    for ln in texto.splitlines()[:10]:
        print("   " + ln)
    print("   ...\n")

    parsed = runner.parse_report(texto)
    result = runner.evaluate(parsed)

    checks = []

    def check(nombre, cond, extra=""):
        checks.append((nombre, cond))
        print("[%s] %s%s" % ("PASS" if cond else "FAIL", nombre,
                             ("  -> " + extra) if (extra and not cond) else ""))

    check("el runner parsea TODOS los items", result["item_count"] == len(ITEMS),
          "parseo %d de %d" % (result["item_count"], len(ITEMS)))

    codes_esperados = [c for c, _, _, _ in ITEMS]
    codes_parseados = [it["code"] for it in parsed["items"]]
    faltan = [c for c in codes_esperados if c not in codes_parseados]
    check("ningun codigo se pierde", not faltan, "faltan: %s" % faltan)

    for v in ("PASS", "FAIL", "WARN", "SKIP", "INFO"):
        check("conteo de %s coincide (%d)" % (v, n[v]), result["counts"][v] == n[v],
              "runner dice %d" % result["counts"][v])

    check("lista FAIL coincide", result["fails"] == sorted(fails))
    check("lista WARN coincide", result["warns"] == sorted(warns))
    check("sin problemas de consistencia", result["problems"] == [],
          str(result["problems"]))
    check("cobertura completa", result["coverage"] == "completa", result["coverage"])
    check("veredicto NO APTO por el FAIL de A3", result["board_verdict"] == "NO APTO")

    # El detalle tiene que sobrevivir intacto: es lo que se lee para diagnosticar.
    d = {it["code"]: it["detail"] for it in parsed["items"]}
    check("el detalle de A3 llega entero",
          d.get("A3", "").startswith("OK(36) en bajo"), repr(d.get("A3")))
    check("el detalle de D2.2 llega entero",
          d.get("D2.2") == "0.4 2.2 3702.9 3611.0", repr(d.get("D2.2")))
    check("los nombres no se contaminan con puntos",
          all(not it["name"].endswith(".") for it in parsed["items"]))

    # Nombre largo: se trunca pero el item se sigue reconociendo.
    largo = report_item("D9", "Un nombre deliberadamente larguisimo que no entra "
                              "en la columna de 44 caracteres", "FAIL", "detalle")
    p2 = runner.parse_report(largo + "\n")
    check("item con nombre larguisimo se reconoce igual",
          len(p2["items"]) == 1 and p2["items"][0]["code"] == "D9"
          and p2["items"][0]["verdict"] == "FAIL", repr(largo))

    # Item sin detalle.
    sin_det = report_item("Z1", "Item sin detalle", "SKIP", "")
    p3 = runner.parse_report(sin_det + "\n")
    check("item sin detalle se reconoce", len(p3["items"]) == 1
          and p3["items"][0]["verdict"] == "SKIP", repr(sin_det))

    ok = sum(1 for _, c in checks if c)
    print("\ntest_formato: %d/%d PASS" % (ok, len(checks)))
    return 0 if ok == len(checks) else 1


if __name__ == "__main__":
    raise SystemExit(main())
