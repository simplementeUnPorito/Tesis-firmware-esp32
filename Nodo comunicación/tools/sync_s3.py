#!/usr/bin/env python3
"""Sincroniza el firmware actual WROOM con los proyectos ESP32-S3.

Conserva platformio.ini, README y herramientas de cada port. Regenera solo
src/ y, para el maestro, data/; despues aplica el delta mecanico S3.
"""

from __future__ import annotations

import shutil
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


MASTER_BOARD = r'''#pragma once
/* Delta de placa ESP32-S3 N16R8. Este archivo se fuerza con platformio.ini. */
#if !defined(ARDUINO_ESP32S3_DEV)
#error "masterS3 requiere ESP32-S3; usar master para ESP32 clasico"
#endif

#define SYNC_OUT_PIN 7
'''


SLAVE_BOARD = r'''#pragma once
/* Delta de placa ESP32-S3 N16R8. Este archivo se fuerza con platformio.ini. */
#if !defined(ARDUINO_ESP32S3_DEV)
#error "slaveS3 requiere ESP32-S3; usar slave para ESP32 clasico"
#endif

/* Enlace PSoC y sincronismo. */
#define PSOC_I2C_ENABLE 1
#define PSOC_I2C_ADDR 0x42
#define PSOC_I2C_SDA 8
#define PSOC_I2C_SCL 9
#define PSOC_UART_BAUD 115200
#define PSOC_UART_RX 18
#define PSOC_UART_TX 17
#define PSOC_RESET_REQUEST_PIN 16
#define SYNC_TO_PSOC_PIN 7
#define SOC_OUT_PIN 1
#define SOC_PERIOD_NS 500000ULL
#define PSOC_CLK_OUT_ENABLE 1
#define PSOC_CLK_OUT_PIN 2

/* OLED y botones. */
#define LOCAL_UI_ENABLE 1
#define LOCAL_OLED_SCK_PIN 12
#define LOCAL_OLED_MOSI_PIN 11
#define LOCAL_OLED_CS_PIN 10
#define LOCAL_OLED_DC_PIN 5
#define LOCAL_OLED_RESET_PIN 6
#define LOCAL_OLED_SPI_HZ 8000000UL
#define LOCAL_BTN_UP_PIN 38
#define LOCAL_BTN_DOWN_PIN 39
#define LOCAL_BTN_OK_PIN 40
#define LOCAL_BTN_BACK_PIN 41
#define LOCAL_BUTTON_USE_INTERNAL_PULLUPS 1
#define LOCAL_CAPTURE_BATCHES 10

/* SD local opcional del modo live: bus separado del OLED. */
#define LIVE_SD_SPI_HOST FSPI
#define LIVE_SD_SCK 14
#define LIVE_SD_MISO 21
#define LIVE_SD_MOSI 13
#define LIVE_SD_CS 42
#define LIVE_SD_HZ 1000000
#define SD_SPI_HOST LIVE_SD_SPI_HOST
#define SDDIAG_SCK LIVE_SD_SCK
#define SDDIAG_MISO LIVE_SD_MISO
#define SDDIAG_MOSI LIVE_SD_MOSI
#define SDDIAG_CS LIVE_SD_CS

/* 2048 x 32 x 90 B = 5,90 MiB; deja margen en los 8 MiB de PSRAM. */
#define LIVE_MAX_PAGES 2048u
#define LIVE_SD_RT_RING 512u
'''


# Propios del port S3: el build de pioarduino (IDF 5) los necesita en src/.
KEEP_IN_TARGET = ("idf_component.yml",)


def sync_tree(source: Path, target: Path) -> None:
    kept = {}
    if target.exists():
        for name in KEEP_IN_TARGET:
            if (target / name).exists():
                kept[name] = (target / name).read_bytes()
        shutil.rmtree(target)
    shutil.copytree(source, target)
    for name, data in kept.items():
        (target / name).write_bytes(data)


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: se esperaba 1 coincidencia, hay {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")


def replace_all(path: Path, old: str, new: str, expected: int) -> None:
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{path}: se esperaban {expected} coincidencias, hay {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")


def patch_master() -> None:
    src = ROOT / "masterS3" / "src"
    (src / "board_pins_s3.h").write_text(MASTER_BOARD, encoding="utf-8", newline="\n")
    replace_once(
        src / "main.cpp",
        "#define SYNC_OUT_PIN 25",
        "#ifndef SYNC_OUT_PIN\n#define SYNC_OUT_PIN 25\n#endif",
    )


def patch_slave() -> None:
    src = ROOT / "slaveS3" / "src"
    (src / "board_pins_s3.h").write_text(SLAVE_BOARD, encoding="utf-8", newline="\n")

    live = src / "live_capture.cpp"
    replace_once(
        live,
        "constexpr uint32_t MAX_PAGES       = 48;    /* 1536 lotes: mas de lo que entra sin PSRAM */",
        "constexpr uint32_t MAX_PAGES       = LIVE_MAX_PAGES;",
    )
    replace_once(
        live,
        "constexpr uint32_t SD_RT_RING      = 160;",
        "constexpr uint32_t SD_RT_RING      = LIVE_SD_RT_RING;",
    )

    main = src / "main.cpp"
    old_scan = '''static GpioScanSlot g_rx_scan[] = {
    {4, 0, 0}, {5, 0, 0}, {12, 0, 0}, {13, 0, 0}, {14, 0, 0},
    {18, 0, 0}, {19, 0, 0}, {21, 0, 0}, {22, 0, 0},
    {25, 0, 0}, {27, 0, 0}, {32, 0, 0}, {33, 0, 0},
};

static void IRAM_ATTR onRxScan0()  { g_rx_scan[0].edges++;  g_rx_scan[0].level  = (uint8_t)digitalRead(g_rx_scan[0].pin); }
static void IRAM_ATTR onRxScan1()  { g_rx_scan[1].edges++;  g_rx_scan[1].level  = (uint8_t)digitalRead(g_rx_scan[1].pin); }
static void IRAM_ATTR onRxScan2()  { g_rx_scan[2].edges++;  g_rx_scan[2].level  = (uint8_t)digitalRead(g_rx_scan[2].pin); }
static void IRAM_ATTR onRxScan3()  { g_rx_scan[3].edges++;  g_rx_scan[3].level  = (uint8_t)digitalRead(g_rx_scan[3].pin); }
static void IRAM_ATTR onRxScan4()  { g_rx_scan[4].edges++;  g_rx_scan[4].level  = (uint8_t)digitalRead(g_rx_scan[4].pin); }
static void IRAM_ATTR onRxScan5()  { g_rx_scan[5].edges++;  g_rx_scan[5].level  = (uint8_t)digitalRead(g_rx_scan[5].pin); }
static void IRAM_ATTR onRxScan6()  { g_rx_scan[6].edges++;  g_rx_scan[6].level  = (uint8_t)digitalRead(g_rx_scan[6].pin); }
static void IRAM_ATTR onRxScan7()  { g_rx_scan[7].edges++;  g_rx_scan[7].level  = (uint8_t)digitalRead(g_rx_scan[7].pin); }
static void IRAM_ATTR onRxScan8()  { g_rx_scan[8].edges++;  g_rx_scan[8].level  = (uint8_t)digitalRead(g_rx_scan[8].pin); }
static void IRAM_ATTR onRxScan9()  { g_rx_scan[9].edges++;  g_rx_scan[9].level  = (uint8_t)digitalRead(g_rx_scan[9].pin); }
static void IRAM_ATTR onRxScan10() { g_rx_scan[10].edges++; g_rx_scan[10].level = (uint8_t)digitalRead(g_rx_scan[10].pin); }
static void IRAM_ATTR onRxScan11() { g_rx_scan[11].edges++; g_rx_scan[11].level = (uint8_t)digitalRead(g_rx_scan[11].pin); }
static void IRAM_ATTR onRxScan12() { g_rx_scan[12].edges++; g_rx_scan[12].level = (uint8_t)digitalRead(g_rx_scan[12].pin); }

static void (* const g_rx_scan_isr[])(void) = {
    onRxScan0, onRxScan1, onRxScan2, onRxScan3, onRxScan4,
    onRxScan5, onRxScan6, onRxScan7, onRxScan8, onRxScan9,
    onRxScan10, onRxScan11, onRxScan12,
};'''
    new_scan = '''/* Solo GPIO libres: fuera memoria, USB, consola, arranque y cableado. */
static GpioScanSlot g_rx_scan[] = {
    {1, 0, 0}, {2, 0, 0}, {4, 0, 0}, {47, 0, 0}, {48, 0, 0},
};

static void IRAM_ATTR onRxScan0() { g_rx_scan[0].edges++; g_rx_scan[0].level = (uint8_t)digitalRead(g_rx_scan[0].pin); }
static void IRAM_ATTR onRxScan1() { g_rx_scan[1].edges++; g_rx_scan[1].level = (uint8_t)digitalRead(g_rx_scan[1].pin); }
static void IRAM_ATTR onRxScan2() { g_rx_scan[2].edges++; g_rx_scan[2].level = (uint8_t)digitalRead(g_rx_scan[2].pin); }
static void IRAM_ATTR onRxScan3() { g_rx_scan[3].edges++; g_rx_scan[3].level = (uint8_t)digitalRead(g_rx_scan[3].pin); }
static void IRAM_ATTR onRxScan4() { g_rx_scan[4].edges++; g_rx_scan[4].level = (uint8_t)digitalRead(g_rx_scan[4].pin); }

static void (* const g_rx_scan_isr[])(void) = {
    onRxScan0, onRxScan1, onRxScan2, onRxScan3, onRxScan4,
};
static_assert(sizeof(g_rx_scan_isr) / sizeof(g_rx_scan_isr[0]) ==
              sizeof(g_rx_scan) / sizeof(g_rx_scan[0]), "un ISR por pin");'''
    replace_once(main, old_scan, new_scan)
    replace_once(
        main,
        "    if (pin == (uint8_t)PSOC_UART_RX ||",
        "    if (pin == (uint8_t)PSOC_I2C_SDA ||\n"
        "        pin == (uint8_t)PSOC_I2C_SCL ||\n"
        "        pin == (uint8_t)PSOC_RESET_REQUEST_PIN ||\n"
        "        pin == (uint8_t)PSOC_UART_RX ||",
    )

    replace_once(
        main,
        '''    if (g_state == SAMPLING && cmd != CMD_STOP && cmd != CMD_ARM &&
        cmd != CMD_STREAM_ACK) {''',
        '''    /* La rampa ESP es una prueba de radio: admitir DEBUG=0 para que
     * s3_link_test.py pueda detenerla sin un STOP global. */
    const bool stopDebug = g_debug_mode &&
        ((cmd == CMD_DEBUG && len == (int)sizeof(MsgDebug) &&
          ((const MsgDebug *)data)->enable == 0) ||
         (cmd == CMD_DEBUG_NODE && len == (int)sizeof(MsgDebugNode) &&
          ((const MsgDebugNode *)data)->node_id == NODE_ID &&
          ((const MsgDebugNode *)data)->enable == 0));
    if (g_state == SAMPLING && cmd != CMD_STOP && cmd != CMD_ARM &&
        cmd != CMD_STREAM_ACK && !stopDebug) {''',
    )
    replace_once(
        main,
        '    SLAVE_LOG_PRINTF("[SLAVE %d] boot\\n", NODE_ID);',
        '''    SLAVE_LOG_PRINTF("[slaveS3 %d] boot chip=%s flash=%u psram=%u\\n", NODE_ID,
                     ESP.getChipModel(), ESP.getFlashChipSize(), ESP.getPsramSize());''',
    )

    start = main.read_text(encoding="utf-8")
    log_start = start.index("static void logPsocRxScan()")
    log_end = start.index("\n}\n#endif", log_start) + 2
    new_log = '''static void logPsocRxScan()
{
    noInterrupts();
    for (size_t i = 0; i < (sizeof(g_rx_scan) / sizeof(g_rx_scan[0])); i++) {
        SLAVE_LOG_PRINTF("[SLAVE] RX_SCAN p%u=%lu/%u\\n",
                         (unsigned)g_rx_scan[i].pin,
                         (unsigned long)g_rx_scan[i].edges,
                         (unsigned)g_rx_scan[i].level);
    }
    interrupts();
}'''
    main.write_text(start[:log_start] + new_log + start[log_end:], encoding="utf-8", newline="\n")

    replace_once(main, "static SPIClass spiHspi(HSPI);", "static SPIClass spiHspi(LIVE_SD_SPI_HOST);")
    replace_once(main, "#define SDDIAG_CS   25", "/* SDDIAG_* vienen de board_pins_s3.h. */")
    replace_once(main, "#define SDDIAG_MISO 32\n", "")
    replace_once(main, "#define SDDIAG_SCK  14", "/* Pines SD definidos por la placa. */")
    replace_once(main, "#define SDDIAG_MOSI 13\n", "")
    replace_all(
        main,
        "spiHspi.begin(14, SDDIAG_MISO, 13, -1);",
        "spiHspi.begin(SDDIAG_SCK, SDDIAG_MISO, SDDIAG_MOSI, -1);",
        8,
    )
    replace_all(
        main,
        "spiHspi.begin(14, 32, 13, 25);",
        "spiHspi.begin(LIVE_SD_SCK, LIVE_SD_MISO, LIVE_SD_MOSI, LIVE_SD_CS);",
        3,
    )
    replace_once(main, "SD.begin(25, spiHspi, 4000000, \"/sd\", 5, true)",
                 "SD.begin(LIVE_SD_CS, spiHspi, 4000000, \"/sd\", 5, true)")
    replace_all(main, "SD.begin(25, spiHspi, 4000000)",
                "SD.begin(LIVE_SD_CS, spiHspi, 4000000)", 2)

def main() -> None:
    sync_tree(ROOT / "master" / "src", ROOT / "masterS3" / "src")
    sync_tree(ROOT / "master" / "data", ROOT / "masterS3" / "data")
    sync_tree(ROOT / "slave" / "src", ROOT / "slaveS3" / "src")
    patch_master()
    patch_slave()
    print("S3 sincronizado: masterS3/src+data y slaveS3/src")


if __name__ == "__main__":
    main()
