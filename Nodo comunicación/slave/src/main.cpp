/*
 * ESP Esclavo — Geofono node
 *
 * Lee muestras del PSoC via UART (raw) y las envía al maestro via ESP-NOW.
 *
 * Configurar hardware en platformio.ini:
 *   NODE_ID, pines UART al PSoC, pines SYNC, marcadores de osciloscopio
 *   y MAC del maestro.
 *
 * Estados:
 *   WAIT_ARM  → espera CMD_ARM del maestro
 *   ARMED     → envia CMD_ARM_ACK, espera PRESTART
 *   HOT_WAIT  → PRESTART recibido; no hace nada salvo esperar START/consulta
 *   SAMPLING  → lee PSoC y envía batches
 *   STOPPED   → vuelve a WAIT_ARM al recibir CMD_STOP
 */

#include <Arduino.h>
#include <esp_wifi.h>
#include "psoc_uart.h"
#include "espnow_transport.h"
#include "sync_protocol.h"
#include "debug_log.h"
#include "../../scope_measurement.h"

/* ── Configuración ────────────────────────────────────────────────────────── */
#ifndef NODE_ID
  #define NODE_ID 1
#endif
#ifndef SAMPLE_PULSE_PIN
  #define SAMPLE_PULSE_PIN -1   /* Deshabilitado: no conmutar GPIO/LED durante muestreo. */
#endif
#ifndef SAMPLE_PULSE_IDLE
  #if defined(ESP8266)
    #define SAMPLE_PULSE_IDLE HIGH
  #else
    #define SAMPLE_PULSE_IDLE LOW
  #endif
#endif
#ifndef SAMPLE_PULSE_US
  #define SAMPLE_PULSE_US 2
#endif
#ifndef DEBUG_HARDWARE
  #define DEBUG_HARDWARE 0
#endif
#ifndef DEBUG_HW_START_PIN
  #define DEBUG_HW_START_PIN -1
#endif
#ifndef DEBUG_HW_START_IDLE
  #define DEBUG_HW_START_IDLE LOW
#endif
#ifndef DEBUG_HW_START_US
  #define DEBUG_HW_START_US SCOPE_START_PULSE_US
#endif
#ifndef PSOC_CAPTURE_MAX_BATCHES
  #define PSOC_CAPTURE_MAX_BATCHES 512
#endif
/* Logging (humano + máquina) en debug_log.h, gateado por DBG_ENABLE.
 * Estos defines limpian el monitor USB: por defecto queda solo lo accionable
 * para diagnosticar calibracion/captura. */
#ifndef PSOC_DIAG_VERBOSE
  #define PSOC_DIAG_VERBOSE 0
#endif
#ifndef PSOC_CAL_LOG_POINTS
  #define PSOC_CAL_LOG_POINTS 1
#endif
#ifndef PSOC_CAL_LOG_CLAMPED_POINTS
  #define PSOC_CAL_LOG_CLAMPED_POINTS 0
#endif
#ifndef PSOC_CAL_LOG_PROGRESS
  #define PSOC_CAL_LOG_PROGRESS 0
#endif
#ifndef SLAVE_LOG_HELLO_TX
  #define SLAVE_LOG_HELLO_TX 0
#endif
#ifndef SLAVE_LOG_STATUS_PERIODIC
  #define SLAVE_LOG_STATUS_PERIODIC 0
#endif
#ifndef SLAVE_LOG_VIEW_UART
  #define SLAVE_LOG_VIEW_UART 0
#endif
#ifndef SLAVE_USB_CMD_ENABLE
  #define SLAVE_USB_CMD_ENABLE 1
#endif
#ifndef PSOC_AUTO_CAL_ON_READY
  #define PSOC_AUTO_CAL_ON_READY 1
#endif
#ifndef PSOC_AUTO_CAL_DELAY_MS
  #define PSOC_AUTO_CAL_DELAY_MS 500u
#endif
#ifndef PSOC_AUTO_CAL_RETRY_MS
  #define PSOC_AUTO_CAL_RETRY_MS 3000u
#endif
#ifndef PSOC_NOMINAL_SAMPLE_RATE_HZ
  #define PSOC_NOMINAL_SAMPLE_RATE_HZ 3000u
#endif
#ifndef PSOC_EFFECTIVE_SAMPLE_RATE_HZ
  #define PSOC_EFFECTIVE_SAMPLE_RATE_HZ 2929u
#endif

/* MAC del ESP maestro. El valor real debe venir de platformio.ini. */
#ifndef MASTER_MAC0
  #define MASTER_MAC0 0xFF
#endif
#ifndef MASTER_MAC1
  #define MASTER_MAC1 0xFF
#endif
#ifndef MASTER_MAC2
  #define MASTER_MAC2 0xFF
#endif
#ifndef MASTER_MAC3
  #define MASTER_MAC3 0xFF
#endif
#ifndef MASTER_MAC4
  #define MASTER_MAC4 0xFF
#endif
#ifndef MASTER_MAC5
  #define MASTER_MAC5 0xFF
#endif
static const uint8_t MASTER_MAC[6] = {
    MASTER_MAC0, MASTER_MAC1, MASTER_MAC2,
    MASTER_MAC3, MASTER_MAC4, MASTER_MAC5
};

/* Pines GPIO hardware sync. platformio.ini es la fuente de verdad; estos
 * defaults solo evitan romper builds viejos que no definan las macros. */
#ifndef SYNC_IN_PIN
  #if defined(ESP8266)
    #define SYNC_IN_PIN 5
  #else
    #define SYNC_IN_PIN 26
  #endif
#endif
#ifndef SYNC_TO_PSOC_PIN
  #if defined(ESP8266)
    #define SYNC_TO_PSOC_PIN 16
  #else
    #define SYNC_TO_PSOC_PIN 27
  #endif
#endif

/* ── Estado ──────────────────────────────────────────────────────────────── */
enum SlaveState { WAIT_ARM, ARMED, HOT_WAIT, SAMPLING, STOPPED };
static volatile SlaveState g_state       = WAIT_ARM;
static bool                g_psocConnected = false;  /* resultado de probe() en setup */
static volatile uint64_t   g_t_start_us  = 0;
static volatile bool       g_debug_mode  = false;
static          uint32_t   g_debug_count = 0;
static volatile uint32_t   g_debug_last_us = 0;
/* g_debugHwActive / g_debugHwFallUs viven en scope_pulse.h */
static          uint8_t    g_pga_code    = 0;
static          uint8_t    g_pgavdac     = 0;
static          uint8_t    g_vdac_byte   = 128;
static volatile bool       g_cfg_waiting = false;
static          uint8_t    g_cfg_sub_cmd = 0;
static          uint8_t    g_cfg_param   = 0;
static          uint32_t   g_cfg_start_ms = 0;
static          uint32_t   g_cfg_timeout_ms = 0;
static          uint32_t   g_cal_progress_ack_ms = 0; /* ultimo "calibrando..." (ok=2) enviado al maestro */
static          bool       g_auto_cal_requested = false;
static          uint32_t   g_auto_cal_due_ms = 0;

/* ── LED blink para identificación (CMD_BLINK_LED) ──────────────────────── */
#ifndef BLINK_LED_PIN
  #ifdef LED_BUILTIN
    #define BLINK_LED_PIN LED_BUILTIN
  #else
    #define BLINK_LED_PIN 2
  #endif
#endif
#define BLINK_TIMES       10u     /* flashes totales (on+off = 1 flash) */
#define BLINK_INTERVAL_MS 150u
#ifndef BLINK_LED_ACTIVE_LOW
#define BLINK_LED_ACTIVE_LOW 1u
#endif
#define BLINK_LED_ON_LEVEL  ((BLINK_LED_ACTIVE_LOW) ? LOW : HIGH)
#define BLINK_LED_OFF_LEVEL ((BLINK_LED_ACTIVE_LOW) ? HIGH : LOW)
static uint8_t   g_blink_count    = 0;
static uint32_t  g_blink_last_ms  = 0;

/* ── Store-and-forward ───────────────────────────────────────────────────── */
static uint16_t    g_rec_n_batches = 0;          /* 0 = modo streaming clásico */
static SampleBytes *g_store_buf    = nullptr;    /* n_batches × 30 SampleBytes */
static uint32_t   *g_store_ts_us   = nullptr;    /* timestamp relativo por batch */
static uint16_t    g_store_fill    = 0;

/* ── Modo "Ver" (disparo único, store primero y dump después) ────────────── */
static volatile uint16_t g_view_remaining    = 0; /* legado live deshabilitado */
static volatile bool     g_view_store_active = false;

#define PSOC_CFG_ACK_TIMEOUT_MS 750u
#define PSOC_CAL_ACK_TIMEOUT_MS 450000u
#define PSOC_ADC_SNAPSHOT_ACK_TIMEOUT_MS 5000u
#define PSOC_CAL_PROGRESS_ACK_PERIOD_MS 3000u /* "sigue calibrando" (ok=2) hacia el maestro */
#define PSOC_CFG_READY_STALE_MS 5000u
#ifndef PSOC_VIEW_START_FALLBACK_EXTRA_MS
  #define PSOC_VIEW_START_FALLBACK_EXTRA_MS 350u
#endif

static uint32_t g_sampling_start_ms = 0;
static uint32_t g_sampling_start_psoc_bytes = 0;
static uint32_t g_sampling_start_batches_ok = 0;
static bool     g_view_start_fallback_sent = false;

static volatile uint32_t g_psoc_rx_edges = 0;
static volatile uint8_t  g_psoc_rx_last_level = 0;

#ifndef PSOC_RX_SCAN_ENABLE
  #define PSOC_RX_SCAN_ENABLE 0
#endif

#if PSOC_RX_SCAN_ENABLE
struct GpioScanSlot {
    uint8_t pin;
    volatile uint32_t edges;
    volatile uint8_t level;
};

static GpioScanSlot g_rx_scan[] = {
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
};

static bool rxScanIsReservedPin(uint8_t pin)
{
    return pin == (uint8_t)PSOC_UART_RX ||
           pin == (uint8_t)PSOC_UART_TX ||
           pin == (uint8_t)SYNC_TO_PSOC_PIN ||
           pin == (uint8_t)SYNC_IN_PIN ||
           pin == (uint8_t)DEBUG_HW_START_PIN;
}

static void beginPsocRxScan()
{
    for (size_t i = 0; i < (sizeof(g_rx_scan) / sizeof(g_rx_scan[0])); i++) {
        const uint8_t pin = g_rx_scan[i].pin;
        if (rxScanIsReservedPin(pin)) {
            continue;
        }
        pinMode(pin, INPUT_PULLUP);
        g_rx_scan[i].level = (uint8_t)digitalRead(pin);
        attachInterrupt(digitalPinToInterrupt(pin), g_rx_scan_isr[i], CHANGE);
    }
    SLAVE_LOG_PRINTLN("[SLAVE] RX_SCAN enabled");
}

static void logPsocRxScan()
{
    uint32_t edges[sizeof(g_rx_scan) / sizeof(g_rx_scan[0])];
    uint8_t levels[sizeof(g_rx_scan) / sizeof(g_rx_scan[0])];

    noInterrupts();
    for (size_t i = 0; i < (sizeof(g_rx_scan) / sizeof(g_rx_scan[0])); i++) {
        edges[i] = g_rx_scan[i].edges;
        levels[i] = g_rx_scan[i].level;
    }
    interrupts();

    SLAVE_LOG_PRINTF(
        "[SLAVE] RX_SCAN p4=%lu/%u p5=%lu/%u p12=%lu/%u p13=%lu/%u p14=%lu/%u "
        "p18=%lu/%u p19=%lu/%u p21=%lu/%u p22=%lu/%u p25=%lu/%u p27=%lu/%u p32=%lu/%u p33=%lu/%u\n",
        (unsigned long)edges[0], (unsigned)levels[0],
        (unsigned long)edges[1], (unsigned)levels[1],
        (unsigned long)edges[2], (unsigned)levels[2],
        (unsigned long)edges[3], (unsigned)levels[3],
        (unsigned long)edges[4], (unsigned)levels[4],
        (unsigned long)edges[5], (unsigned)levels[5],
        (unsigned long)edges[6], (unsigned)levels[6],
        (unsigned long)edges[7], (unsigned)levels[7],
        (unsigned long)edges[8], (unsigned)levels[8],
        (unsigned long)edges[9], (unsigned)levels[9],
        (unsigned long)edges[10], (unsigned)levels[10],
        (unsigned long)edges[11], (unsigned)levels[11],
        (unsigned long)edges[12], (unsigned)levels[12]);
}
#endif

/* ── Objetos ─────────────────────────────────────────────────────────────── */
static PsocUART       psoc;
static EspNowTransport transport;

/* Helpers de pulsos GPIO de osciloscopio (módulo aparte). */
#include "scope_pulse.h"

static void sendCfgAck(uint8_t sub_cmd, uint8_t ok);

static void IRAM_ATTR onPsocRxEdge()
{
    g_psoc_rx_edges++;
    g_psoc_rx_last_level = (uint8_t)digitalRead(PSOC_UART_RX);
}

static void logPsocUartDiag(const char *tag)
{
    const unsigned long bytes = (unsigned long)psoc.bytesRx();
    const unsigned long mark  = (unsigned long)psoc.markersRx();
    const unsigned long drop  = (unsigned long)psoc.syncDrops();
    const unsigned long badLn = (unsigned long)psoc.badLen();
    const unsigned long ping  = (unsigned long)psoc.pingsRx();
    const unsigned long diag  = (unsigned long)psoc.diagEventsRx();
    const int rxLevel = digitalRead(PSOC_UART_RX);
    uint32_t rxEdges;
    uint8_t rxEdgeLevel;

    noInterrupts();
    rxEdges = g_psoc_rx_edges;
    rxEdgeLevel = g_psoc_rx_last_level;
    interrupts();

    if (psoc.hasLastByte()) {
        SLAVE_LOG_PRINTF(
            "[SLAVE %d] %s bOK=%lu bBad=%lu uartBytes=%lu mark=%lu drop=%lu badLen=%lu ping=%lu diag=%lu rx=%d edge=%lu/%u last=0x%02X age=%lu txOK=%lu txFail=%lu state=%d fill=%u/%u\n",
            NODE_ID, tag,
            (unsigned long)psoc.batchesOK(), (unsigned long)psoc.batchesBad(),
            bytes, mark, drop, badLn, ping, diag, rxLevel,
            (unsigned long)rxEdges, (unsigned)rxEdgeLevel,
            psoc.lastByte(), (unsigned long)psoc.lastByteAgeMs(),
            (unsigned long)transport.sentOK(), (unsigned long)transport.sentFail(),
            (int)g_state, (unsigned)g_store_fill, (unsigned)g_rec_n_batches);
    } else {
        SLAVE_LOG_PRINTF(
            "[SLAVE %d] %s bOK=%lu bBad=%lu uartBytes=%lu mark=%lu drop=%lu badLen=%lu ping=%lu diag=%lu rx=%d edge=%lu/%u last=none txOK=%lu txFail=%lu state=%d fill=%u/%u\n",
            NODE_ID, tag,
            (unsigned long)psoc.batchesOK(), (unsigned long)psoc.batchesBad(),
            bytes, mark, drop, badLn, ping, diag, rxLevel,
            (unsigned long)rxEdges, (unsigned)rxEdgeLevel,
            (unsigned long)transport.sentOK(), (unsigned long)transport.sentFail(),
            (int)g_state, (unsigned)g_store_fill, (unsigned)g_rec_n_batches);
    }
}

static const char *psocDiagName(uint8_t event)
{
    switch (event) {
        case PSOC_EVT_BOOT:           return "BOOT";
        case PSOC_EVT_ANALOG_READY:   return "ANALOG_READY";
        case PSOC_EVT_CAL_START:      return "CAL_START";
        case PSOC_EVT_CAL_DONE:       return "CAL_DONE";
        case PSOC_EVT_CAL_BUSY:       return "CAL_BUSY";
        case PSOC_EVT_CAL_STAGE_DAC:  return "CAL_STAGE_DAC";
        case PSOC_EVT_CAL_STAGE_MEAS: return "CAL_STAGE_MEAS";
        case PSOC_EVT_CAL_STAGE_BEGIN:return "CAL_STAGE_BEGIN";
        case PSOC_EVT_CAL_STAGE_OK:   return "CAL_STAGE_OK";
        case PSOC_EVT_CAL_VERIFY_BEGIN:return "CAL_VERIFY_BEGIN";
        case PSOC_EVT_CAL_VERIFY_OK:  return "CAL_VERIFY_OK";
        case PSOC_EVT_CAL_AMUX_IN:    return "CAL_AMUX_IN";
        case PSOC_EVT_CAL_PROGRESS:   return "CAL_PROGRESS";
        case PSOC_EVT_CAL_WATCHDOG:   return "CAL_WATCHDOG";
        case PSOC_EVT_CAL_LP_BAD:     return "CAL_LP_BAD";
        case PSOC_EVT_CAL_STAGE_MEAS32: return "CAL_STAGE_MEAS32";
        case PSOC_EVT_SERVO_STAGE:    return "SERVO_STAGE";
        case PSOC_EVT_SERVO_STEP:     return "SERVO_STEP";
        case PSOC_EVT_WAIT_ESP:       return "WAIT_ESP";
        case PSOC_EVT_ESP_SEEN:       return "ESP_SEEN";
        case PSOC_EVT_CAL_LOOP:       return "CAL_LOOP";
        case PSOC_EVT_CAL_STAGE_SAT:  return "CAL_STAGE_SAT";
        case PSOC_EVT_CAL_STAGE_SAT_ALL: return "CAL_STAGE_SAT_ALL";
        case PSOC_EVT_CAL_REALCHECK_BEGIN: return "CAL_REALCHECK_BEGIN";
        case PSOC_EVT_CAL_REALCHECK_DAC: return "CAL_REALCHECK_DAC";
        case PSOC_EVT_CAL_REALCHECK_MEAS32: return "CAL_REALCHECK_MEAS32";
        case PSOC_EVT_CAL_REALCHECK_NUDGE: return "CAL_REALCHECK_NUDGE";
        case PSOC_EVT_CAL_REALCHECK_OK: return "CAL_REALCHECK_OK";
        case PSOC_EVT_CAL_STAGE_TARGET32: return "CAL_STAGE_TARGET32";
        case PSOC_EVT_CAL_SWEEP_DAC: return "CAL_SWEEP_DAC";
        case PSOC_EVT_CAL_SWEEP_MEAS32: return "CAL_SWEEP_MEAS32";
        case PSOC_EVT_ADC_SNAPSHOT_BEGIN: return "ADC_SNAPSHOT_BEGIN";
        case PSOC_EVT_ADC_RAW32: return "ADC_RAW32";
        case PSOC_EVT_ADC_FILT32: return "ADC_FILT32";
        case PSOC_EVT_RX_CMD:         return "RX_CMD";
        case PSOC_EVT_SETN:           return "SETN";
        case PSOC_EVT_ARMED:          return "ARMED";
        case PSOC_EVT_SYNC_RISE:      return "SYNC_RISE";
        case PSOC_EVT_SYNC_FALL:      return "SYNC_FALL";
        case PSOC_EVT_SAMPLING_START: return "SAMPLING_START";
        case PSOC_EVT_CAPTURE_DONE:   return "CAPTURE_DONE";
        case PSOC_EVT_DUMP_START:     return "DUMP_START";
        case PSOC_EVT_DUMP_DONE:      return "DUMP_DONE";
        case PSOC_EVT_START_NOW:      return "START_NOW";
        case PSOC_EVT_DEBUG_MODE:     return "DEBUG_MODE";
        case PSOC_EVT_STATUS_REQ:     return "STATUS_REQ";
        case PSOC_EVT_BUTTON:         return "BUTTON";
        case PSOC_EVT_BUTTON_IGNORED: return "BUTTON_IGNORED";
        case PSOC_EVT_CAPTURE_CLAMPED:return "CAPTURE_CLAMPED";
        case PSOC_EVT_CAL_PI_GAIN32:  return "CAL_PI_GAIN32";
        case PSOC_EVT_CAL_PI_DEADBAND:return "CAL_PI_DEADBAND";
        case PSOC_EVT_CAL_PI_ERROR32: return "CAL_PI_ERROR32";
        case PSOC_EVT_CAL_PI_BUCKET32:return "CAL_PI_BUCKET32";
        case PSOC_EVT_CAL_PI_STABLE:  return "CAL_PI_STABLE";
        case PSOC_EVT_CAL_AMUX_CAP:   return "CAL_AMUX_CAP";
        default:                      return "UNKNOWN";
    }
}

static const char *psocStateName(uint8_t state)
{
    switch (state) {
        case 0:  return "IDLE";
        case 1:  return "ARMED";
        case 2:  return "SAMPLING";
        case 3:  return "CALIBRATING";
        default: return "UNKNOWN";
    }
}

static uint8_t g_psoc_hw_class = 0xFFu;

static const char *psocHwName(uint8_t hwClass)
{
    switch (hwClass) {
        case 0:  return "GEO";
        case 1:  return "HAMMER";
        default: return "UNKNOWN";
    }
}

static const char *psocCalStageName(uint8_t stage)
{
    if (g_psoc_hw_class == 1u) {
        switch (stage) {
            case 0:  return "HAMMER_PGA";
            case 1:  return "HAMMER_LP";
            default: return "UNKNOWN";
        }
    } else {
        switch (stage) {
            case 0:  return "GEO_PGA";
            case 1:  return "GEO_BP";
            case 2:  return "GEO_ADDER";
            case 3:  return "GEO_LP";
            default: return "UNKNOWN";
        }
    }
}

static uint32_t absCounts32(int32_t value)
{
    return (value < 0) ? (uint32_t)(-value) : (uint32_t)value;
}

static int32_t psocCalCompareCounts(int32_t raw)
{
    return (g_psoc_hw_class == 1u) ? (int32_t)absCounts32(raw) : raw;
}

static int32_t adcCountsToMv(int32_t counts)
{
    const int64_t scaled = (int64_t)counts * 1000LL;
    if (scaled >= 0) {
        return (int32_t)((scaled + 26214LL) / 52429LL);
    }
    return (int32_t)((scaled - 26214LL) / 52429LL);
}

static void onPsocDiag(const PsocDiagEvent &event)
{
    static uint8_t calStage = 0xFF;
    static uint8_t calDac = 0;
    static int16_t calMeas = 0;
    static bool calMeasHigh = false;
    static uint32_t calMeasRaw = 0;
    static int32_t calMeasRawSigned = 0;
    static uint8_t calMeasRawByte = 0;
    static uint32_t calTargetRaw = 0;
    static int32_t calTargetSigned = 0;
    static uint8_t calTargetRawByte = 0;
    static bool calTargetValid = false;
    static uint32_t calSweepRaw = 0;
    static uint8_t calSweepRawByte = 0;
    static uint32_t adcSnapshotRaw = 0;
    static uint8_t adcSnapshotRawByte = 0;
    static uint32_t calPiRaw = 0;
    static uint8_t calPiRawByte = 0;
    static uint8_t calPiRawEvent = 0;
    static uint16_t calPointIndex = 0;
    g_psocConnected = true;

    if (g_state == SAMPLING && event.event != PSOC_EVT_DUMP_DONE) {
        return;
    }

    if (event.event == PSOC_EVT_DUMP_DONE && g_view_store_active &&
        g_state == SAMPLING && g_store_fill < g_rec_n_batches) {
        digitalWrite(SYNC_TO_PSOC_PIN, LOW);
        g_view_store_active = false;
        g_state = STOPPED;
        sendCfgAck(CMD_VIEW, 0);
        SLAVE_LOG_PRINTF("[SLAVE] VIEW partial dump fill=%u/%u -> FAIL\n",
                         (unsigned)g_store_fill, (unsigned)g_rec_n_batches);
        LOGM("VIEW_PARTIAL", "fill=%u,n=%u,bOK=%lu,bBad=%lu,drop=%lu,bytes=%lu",
             (unsigned)g_store_fill, (unsigned)g_rec_n_batches,
             (unsigned long)psoc.batchesOK(),
             (unsigned long)psoc.batchesBad(),
             (unsigned long)psoc.syncDrops(),
             (unsigned long)psoc.bytesRx());
    }

    if (event.event == PSOC_EVT_BOOT) {
        g_psoc_hw_class = event.value;
        SLAVE_LOG_PRINTF("[PSoC] boot hw=%u/%s pstate=%u/%s\n",
                         event.value, psocHwName(event.value),
                         event.psoc_state, psocStateName(event.psoc_state));
        LOGM("PSOC_BOOT", "hw=%u,hwName=%s,pstate=%u,pstateName=%s",
             event.value, psocHwName(event.value),
             event.psoc_state, psocStateName(event.psoc_state));
    }

    if (event.event == PSOC_EVT_CAL_START) {
        calStage = 0;
        calDac = 0;
        calMeas = 0;
        calMeasHigh = false;
        calMeasRaw = 0;
        calMeasRawSigned = 0;
        calMeasRawByte = 0;
        calTargetRaw = 0;
        calTargetSigned = 0;
        calTargetRawByte = 0;
        calTargetValid = false;
        adcSnapshotRaw = 0;
        adcSnapshotRawByte = 0;
        calPiRaw = 0;
        calPiRawByte = 0;
        calPiRawEvent = 0;
        calPointIndex = 0;
        g_cal_progress_ack_ms = millis();
        sendCfgAck(PSOC_CMD_CALIBRATE, 2);
        SLAVE_LOG_PRINTF("[CAL] start\n");
        LOGM("CAL_START", "pstate=%u", event.psoc_state);
    } else if (event.event == PSOC_EVT_CAL_DONE) {
        if (!(g_cfg_waiting && g_cfg_sub_cmd == PSOC_CMD_CALIBRATE)) {
            sendCfgAck(PSOC_CMD_CALIBRATE, event.value ? 1 : 0);
        }
        SLAVE_LOG_PRINTF("[CAL] done ok=%u\n", event.value);
        LOGM("CAL_DONE", "ok=%u,pstate=%u", event.value, event.psoc_state);
    } else if (event.event == PSOC_EVT_CAL_BUSY) {
        SLAVE_LOG_PRINTF("[CAL] busy\n");
        LOGM("CAL_BUSY", "pstate=%u", event.psoc_state);
    } else if (event.event == PSOC_EVT_CAL_STAGE_BEGIN) {
        calStage = event.value;
        calDac = 0;
        calMeas = 0;
        calMeasHigh = false;
        calMeasRaw = 0;
        calMeasRawSigned = 0;
        calMeasRawByte = 0;
        calTargetRaw = 0;
        calTargetSigned = 0;
        calTargetRawByte = 0;
        calTargetValid = false;
        adcSnapshotRaw = 0;
        adcSnapshotRawByte = 0;
        calPiRaw = 0;
        calPiRawByte = 0;
        calPiRawEvent = 0;
        calPointIndex = 0;
        g_cal_progress_ack_ms = millis();
        sendCfgAck(PSOC_CMD_CALIBRATE, (uint8_t)(3u + (event.value & 0x03u)));
        SLAVE_LOG_PRINTF("[CAL] begin stage=%u/%s\n",
                         event.value, psocCalStageName(event.value));
        LOGM("CAL_BEGIN", "stage=%u,name=%s", event.value,
             psocCalStageName(event.value));
    } else if (event.event == PSOC_EVT_SERVO_STAGE) {
        calStage = event.value;
        calDac = 0;
        calMeas = 0;
        calMeasHigh = false;
        calMeasRaw = 0;
        calMeasRawSigned = 0;
        calMeasRawByte = 0;
        calPointIndex = 0;
        SLAVE_LOG_PRINTF("[CAL] servo stage=%u/%s\n",
                         event.value, psocCalStageName(event.value));
        LOGM("SERVO_STAGE", "stage=%u,name=%s", event.value,
             psocCalStageName(event.value));
    } else if (event.event == PSOC_EVT_CAL_VERIFY_BEGIN) {
        calStage = event.value;
        calDac = 0;
        calMeas = 0;
        calMeasHigh = false;
        calMeasRaw = 0;
        calMeasRawSigned = 0;
        calMeasRawByte = 0;
        calPointIndex = 0;
        g_cal_progress_ack_ms = millis();
        sendCfgAck(PSOC_CMD_CALIBRATE, (uint8_t)(7u + (event.value & 0x03u)));
        SLAVE_LOG_PRINTF("[CAL] verify begin stage=%u/%s\n",
                         event.value, psocCalStageName(event.value));
        LOGM("CAL_VERIFY_BEGIN", "stage=%u,name=%s", event.value,
             psocCalStageName(event.value));
    } else if (event.event == PSOC_EVT_CAL_REALCHECK_BEGIN) {
        calStage = event.value;
        calDac = 0;
        calMeas = 0;
        calMeasHigh = false;
        calMeasRaw = 0;
        calMeasRawSigned = 0;
        calMeasRawByte = 0;
        calPointIndex = 0;
        g_cal_progress_ack_ms = millis();
        sendCfgAck(PSOC_CMD_CALIBRATE, (uint8_t)(11u + (event.value & 0x03u)));
        SLAVE_LOG_PRINTF("[CAL] realcheck begin stage=%u/%s\n",
                         event.value, psocCalStageName(event.value));
        LOGM("CAL_REALCHECK_BEGIN", "stage=%u,name=%s", event.value,
             psocCalStageName(event.value));
    } else if (event.event == PSOC_EVT_CAL_STAGE_DAC) {
        calDac = event.value;
    } else if (event.event == PSOC_EVT_CAL_SWEEP_DAC) {
        calDac = event.value;
        calSweepRaw = 0;
        calSweepRawByte = 0;
    } else if (event.event == PSOC_EVT_ADC_SNAPSHOT_BEGIN) {
        calStage = event.value;
        calDac = 0;
        calMeasRaw = 0;
        calMeasRawByte = 0;
        calTargetRaw = 0;
        calTargetRawByte = 0;
        calTargetValid = false;
        calSweepRaw = 0;
        calSweepRawByte = 0;
        adcSnapshotRaw = 0;
        adcSnapshotRawByte = 0;
        calPointIndex = 0;
        SLAVE_LOG_PRINTF("[ADC] snapshot begin stage=%u/%s\n",
                         calStage, psocCalStageName(calStage));
        LOGM("ADC_SNAPSHOT_BEGIN", "stage=%u,name=%s",
             calStage, psocCalStageName(calStage));
    } else if (event.event == PSOC_EVT_CAL_STAGE_TARGET32) {
        calTargetRaw = (calTargetRaw << 8) | event.value;
        calTargetRawByte++;
        if (calTargetRawByte >= 4) {
            calTargetRawByte = 0;
            calTargetSigned = (int32_t)calTargetRaw;
            calTargetRaw = 0;
            calTargetValid = true;
            SLAVE_LOG_PRINTF("[CAL] target stage=%u/%s counts=%ld mV=%ld\n",
                             calStage, psocCalStageName(calStage),
                             (long)calTargetSigned,
                             (long)adcCountsToMv(calTargetSigned));
            LOGM("CAL_TARGET", "stage=%u,name=%s,target=%ld,target_mV=%ld",
                 calStage, psocCalStageName(calStage),
                 (long)calTargetSigned, (long)adcCountsToMv(calTargetSigned));
        }
    } else if (event.event == PSOC_EVT_CAL_SWEEP_MEAS32) {
        calSweepRaw = (calSweepRaw << 8) | event.value;
        calSweepRawByte++;
        if (calSweepRawByte >= 4) {
            int32_t sweepSigned;
            uint32_t absRaw;
            int32_t cmpRaw;
            int32_t errRaw;
            calSweepRawByte = 0;
            sweepSigned = (int32_t)calSweepRaw;
            calSweepRaw = 0;
            absRaw = absCounts32(sweepSigned);
            cmpRaw = psocCalCompareCounts(sweepSigned);
            errRaw = calTargetValid ? (calTargetSigned - cmpRaw) : 0;
            SLAVE_LOG_PRINTF("[CAL] sweep stage=%u/%s dac=%u raw=%ld raw_mV=%ld abs=%lu cmp=%ld cmp_mV=%ld target=%ld target_mV=%ld err=%ld err_mV=%ld\n",
                             calStage, psocCalStageName(calStage), calDac,
                             (long)sweepSigned, (long)adcCountsToMv(sweepSigned),
                             (unsigned long)absRaw,
                             (long)cmpRaw, (long)adcCountsToMv(cmpRaw),
                             calTargetValid ? (long)calTargetSigned : 0L,
                             calTargetValid ? (long)adcCountsToMv(calTargetSigned) : 0L,
                             (long)errRaw, (long)adcCountsToMv(errRaw));
            LOGM("CAL_SWEEP", "stage=%u,name=%s,dac=%u,measRaw=%ld,raw_mV=%ld,abs=%lu,cmp=%ld,cmp_mV=%ld,target=%ld,target_mV=%ld,err=%ld,err_mV=%ld",
                 calStage, psocCalStageName(calStage), calDac,
                 (long)sweepSigned, (long)adcCountsToMv(sweepSigned),
                 (unsigned long)absRaw, (long)cmpRaw, (long)adcCountsToMv(cmpRaw),
                 calTargetValid ? (long)calTargetSigned : 0L,
                 calTargetValid ? (long)adcCountsToMv(calTargetSigned) : 0L,
                 (long)errRaw, (long)adcCountsToMv(errRaw));
        }
    } else if (event.event == PSOC_EVT_ADC_RAW32) {
        adcSnapshotRaw = (adcSnapshotRaw << 8) | event.value;
        adcSnapshotRawByte++;
        if (adcSnapshotRawByte >= 4) {
            int32_t adcSigned;
            uint32_t absRaw;
            int32_t cmpRaw;
            int32_t errRaw;
            adcSnapshotRawByte = 0;
            adcSigned = (int32_t)adcSnapshotRaw;
            adcSnapshotRaw = 0;
            absRaw = absCounts32(adcSigned);
            cmpRaw = psocCalCompareCounts(adcSigned);
            errRaw = calTargetValid ? (calTargetSigned - cmpRaw) : 0;
            SLAVE_LOG_PRINTF("[ADC] snapshot stage=%u/%s dac=%u raw=%ld raw_mV=%ld abs=%lu cmp=%ld cmp_mV=%ld target=%ld target_mV=%ld err=%ld err_mV=%ld\n",
                             calStage, psocCalStageName(calStage), calDac,
                             (long)adcSigned, (long)adcCountsToMv(adcSigned),
                             (unsigned long)absRaw, (long)cmpRaw,
                             (long)adcCountsToMv(cmpRaw),
                             calTargetValid ? (long)calTargetSigned : 0L,
                             calTargetValid ? (long)adcCountsToMv(calTargetSigned) : 0L,
                             (long)errRaw, (long)adcCountsToMv(errRaw));
            LOGM("ADC_SNAPSHOT", "stage=%u,name=%s,dac=%u,raw=%ld,raw_mV=%ld,abs=%lu,cmp=%ld,cmp_mV=%ld,target=%ld,target_mV=%ld,err=%ld,err_mV=%ld",
                 calStage, psocCalStageName(calStage), calDac,
                 (long)adcSigned, (long)adcCountsToMv(adcSigned),
                 (unsigned long)absRaw, (long)cmpRaw,
                 (long)adcCountsToMv(cmpRaw),
                 calTargetValid ? (long)calTargetSigned : 0L,
                 calTargetValid ? (long)adcCountsToMv(calTargetSigned) : 0L,
                 (long)errRaw, (long)adcCountsToMv(errRaw));
        }
    } else if (event.event == PSOC_EVT_CAL_PI_GAIN32 ||
               event.event == PSOC_EVT_CAL_PI_ERROR32 ||
               event.event == PSOC_EVT_CAL_PI_BUCKET32) {
        if (calPiRawByte == 0 || calPiRawEvent != event.event) {
            calPiRaw = 0;
            calPiRawByte = 0;
            calPiRawEvent = event.event;
        }
        calPiRaw = (calPiRaw << 8) | event.value;
        calPiRawByte++;
        if (calPiRawByte >= 4) {
            int32_t piSigned = (int32_t)calPiRaw;
            const char *kind =
                (event.event == PSOC_EVT_CAL_PI_GAIN32) ? "gain_x1000" :
                (event.event == PSOC_EVT_CAL_PI_ERROR32) ? "error_dac" :
                                                           "bucket";
            calPiRaw = 0;
            calPiRawByte = 0;
            calPiRawEvent = 0;
            SLAVE_LOG_PRINTF("[CAL] pi stage=%u/%s %s=%ld\n",
                             calStage, psocCalStageName(calStage),
                             kind, (long)piSigned);
            LOGM("CAL_PI", "stage=%u,name=%s,%s=%ld",
                 calStage, psocCalStageName(calStage),
                 kind, (long)piSigned);
        }
    } else if (event.event == PSOC_EVT_CAL_PI_DEADBAND) {
        SLAVE_LOG_PRINTF("[CAL] pi stage=%u/%s deadband_dac=%u\n",
                         calStage, psocCalStageName(calStage), event.value);
        LOGM("CAL_PI", "stage=%u,name=%s,deadband_dac=%u",
             calStage, psocCalStageName(calStage), event.value);
    } else if (event.event == PSOC_EVT_CAL_PI_STABLE) {
        SLAVE_LOG_PRINTF("[CAL] pi stage=%u/%s stable=%u\n",
                         calStage, psocCalStageName(calStage), event.value);
        LOGM("CAL_PI", "stage=%u,name=%s,stable=%u",
             calStage, psocCalStageName(calStage), event.value);
    } else if (event.event == PSOC_EVT_CAL_STAGE_MEAS) {
        if (!calMeasHigh) {
            calMeas = (int16_t)((uint16_t)event.value << 8);
            calMeasHigh = true;
        } else {
            calMeas = (int16_t)((uint16_t)calMeas | event.value);
            calMeasHigh = false;
#if PSOC_CAL_LOG_CLAMPED_POINTS
            SLAVE_LOG_PRINTF("[CAL] point16 stage=%u/%s dac=%u meas=%d\n",
                             calStage, psocCalStageName(calStage),
                             calDac, (int)calMeas);
            LOGM("CAL_POINT16", "stage=%u,name=%s,dac=%u,meas=%d",
                 calStage, psocCalStageName(calStage), calDac, (int)calMeas);
#endif
        }
    } else if (event.event == PSOC_EVT_CAL_STAGE_MEAS32) {
        calMeasRaw = (calMeasRaw << 8) | event.value;
        calMeasRawByte++;
        if (calMeasRawByte >= 4) {
            uint32_t absRaw;
            int32_t cmpRaw;
            int32_t errRaw;
            calMeasRawByte = 0;
            calMeasRawSigned = (int32_t)calMeasRaw;
            calMeasRaw = 0;
            absRaw = absCounts32(calMeasRawSigned);
            cmpRaw = psocCalCompareCounts(calMeasRawSigned);
            errRaw = calTargetValid ? (calTargetSigned - cmpRaw) : 0;
#if PSOC_CAL_LOG_POINTS
            SLAVE_LOG_PRINTF("[CAL] point stage=%u/%s i=%u dac=%u raw=%ld raw_mV=%ld abs=%lu cmp=%ld cmp_mV=%ld target=%ld target_mV=%ld err=%ld err_mV=%ld\n",
                             calStage, psocCalStageName(calStage),
                             (unsigned)calPointIndex, calDac,
                             (long)calMeasRawSigned, (long)adcCountsToMv(calMeasRawSigned),
                             (unsigned long)absRaw,
                             (long)cmpRaw, (long)adcCountsToMv(cmpRaw),
                             calTargetValid ? (long)calTargetSigned : 0L,
                             calTargetValid ? (long)adcCountsToMv(calTargetSigned) : 0L,
                             (long)errRaw, (long)adcCountsToMv(errRaw));
            LOGM("CAL_POINT32", "stage=%u,name=%s,i=%u,dac=%u,measRaw=%ld,raw_mV=%ld,abs=%lu,cmp=%ld,cmp_mV=%ld,target=%ld,target_mV=%ld,err=%ld,err_mV=%ld",
                 calStage, psocCalStageName(calStage),
                 (unsigned)calPointIndex, calDac, (long)calMeasRawSigned,
                 (long)adcCountsToMv(calMeasRawSigned),
                 (unsigned long)absRaw, (long)cmpRaw, (long)adcCountsToMv(cmpRaw),
                 calTargetValid ? (long)calTargetSigned : 0L,
                 calTargetValid ? (long)adcCountsToMv(calTargetSigned) : 0L,
                 (long)errRaw, (long)adcCountsToMv(errRaw));
#endif
            calPointIndex++;
        }
    } else if (event.event == PSOC_EVT_CAL_REALCHECK_DAC) {
        calDac = event.value;
        calMeasRaw = 0;
        calMeasRawByte = 0;
    } else if (event.event == PSOC_EVT_CAL_REALCHECK_MEAS32) {
        calMeasRaw = (calMeasRaw << 8) | event.value;
        calMeasRawByte++;
        if (calMeasRawByte >= 4) {
            uint32_t absRaw;
            calMeasRawByte = 0;
            calMeasRawSigned = (int32_t)calMeasRaw;
            absRaw = absCounts32(calMeasRawSigned);
#if PSOC_CAL_LOG_POINTS
            SLAVE_LOG_PRINTF("[CAL] realcheck point stage=%u/%s i=%u dac=%u raw=%ld abs=%lu\n",
                             calStage, psocCalStageName(calStage),
                             (unsigned)calPointIndex, calDac,
                             (long)calMeasRawSigned, (unsigned long)absRaw);
            LOGM("CAL_REALCHECK_POINT32", "stage=%u,name=%s,i=%u,dac=%u,measRaw=%ld,abs=%lu",
                 calStage, psocCalStageName(calStage),
                 (unsigned)calPointIndex, calDac, (long)calMeasRawSigned,
                 (unsigned long)absRaw);
#endif
            calPointIndex++;
        }
    } else if (event.event == PSOC_EVT_CAL_STAGE_OK) {
        uint32_t absRaw = absCounts32(calMeasRawSigned);
        int32_t cmpRaw = psocCalCompareCounts(calMeasRawSigned);
        int32_t errRaw = calTargetValid ? (calTargetSigned - cmpRaw) : 0;
        SLAVE_LOG_PRINTF("[CAL] result stage=%u/%s ok=%u dac=%u raw=%ld raw_mV=%ld abs=%lu cmp=%ld cmp_mV=%ld target=%ld target_mV=%ld err=%ld err_mV=%ld meas16=%d\n",
                         calStage, psocCalStageName(calStage), event.value,
                         calDac, (long)calMeasRawSigned,
                         (long)adcCountsToMv(calMeasRawSigned),
                         (unsigned long)absRaw, (long)cmpRaw,
                         (long)adcCountsToMv(cmpRaw),
                         calTargetValid ? (long)calTargetSigned : 0L,
                         calTargetValid ? (long)adcCountsToMv(calTargetSigned) : 0L,
                         (long)errRaw, (long)adcCountsToMv(errRaw), (int)calMeas);
        LOGM("CAL_STAGE", "stage=%u,name=%s,ok=%u,dac=%u,measRaw=%ld,raw_mV=%ld,abs=%lu,cmp=%ld,cmp_mV=%ld,target=%ld,target_mV=%ld,err=%ld,err_mV=%ld,meas16=%d",
             calStage, psocCalStageName(calStage), event.value,
             calDac, (long)calMeasRawSigned,
             (long)adcCountsToMv(calMeasRawSigned),
             (unsigned long)absRaw,
             (long)cmpRaw, (long)adcCountsToMv(cmpRaw),
             calTargetValid ? (long)calTargetSigned : 0L,
             calTargetValid ? (long)adcCountsToMv(calTargetSigned) : 0L,
             (long)errRaw, (long)adcCountsToMv(errRaw),
             (int)calMeas);
    } else if (event.event == PSOC_EVT_SERVO_STEP) {
        SLAVE_LOG_PRINTF("[CAL] servo step stage=%u/%s dac=%u\n",
                         calStage, psocCalStageName(calStage), event.value);
        LOGM("SERVO_STEP", "stage=%u,name=%s,dac=%u",
             calStage, psocCalStageName(calStage), event.value);
    } else if (event.event == PSOC_EVT_CAL_LOOP) {
        SLAVE_LOG_PRINTF("[CAL] loop stage=%u/%s dac=%u -> best candidate\n",
                         calStage, psocCalStageName(calStage), event.value);
        LOGM("CAL_LOOP", "stage=%u,name=%s,dac=%u",
             calStage, psocCalStageName(calStage), event.value);
    } else if (event.event == PSOC_EVT_CAL_STAGE_SAT) {
        if (event.value) {
            SLAVE_LOG_PRINTF("[CAL] saturation stage=%u/%s dac=%u\n",
                             calStage, psocCalStageName(calStage), calDac);
            LOGM("CAL_SAT", "stage=%u,name=%s,dac=%u",
                 calStage, psocCalStageName(calStage), calDac);
        }
    } else if (event.event == PSOC_EVT_CAL_STAGE_SAT_ALL) {
        SLAVE_LOG_PRINTF("[CAL] all candidates saturated stage=%u/%s\n",
                         event.value, psocCalStageName(event.value));
        LOGM("CAL_SAT_ALL", "stage=%u,name=%s", event.value,
             psocCalStageName(event.value));
    } else if (event.event == PSOC_EVT_CAL_REALCHECK_NUDGE) {
        int8_t nudge = (int8_t)event.value;
        SLAVE_LOG_PRINTF("[CAL] realcheck nudge stage=%u/%s dac=%u step=%d\n",
                         calStage, psocCalStageName(calStage), calDac,
                         (int)nudge);
        LOGM("CAL_REALCHECK_NUDGE", "stage=%u,name=%s,dac=%u,step=%d",
             calStage, psocCalStageName(calStage), calDac, (int)nudge);
    } else if (event.event == PSOC_EVT_CAL_REALCHECK_OK) {
        uint32_t absRaw = absCounts32(calMeasRawSigned);
        SLAVE_LOG_PRINTF("[CAL] realcheck result stage=%u/%s ok=%u dac=%u raw=%ld abs=%lu\n",
                         calStage, psocCalStageName(calStage), event.value,
                         calDac, (long)calMeasRawSigned,
                         (unsigned long)absRaw);
        LOGM("CAL_REALCHECK", "stage=%u,name=%s,ok=%u,dac=%u,measRaw=%ld,abs=%lu",
             calStage, psocCalStageName(calStage), event.value,
             calDac, (long)calMeasRawSigned, (unsigned long)absRaw);
    } else if (event.event == PSOC_EVT_CAL_VERIFY_OK) {
        uint32_t absRaw = absCounts32(calMeasRawSigned);
        SLAVE_LOG_PRINTF("[CAL] verify stage=%u/%s ok=%u dac=%u raw=%ld abs=%lu meas16=%d\n",
                         calStage, psocCalStageName(calStage), event.value,
                         calDac, (long)calMeasRawSigned,
                         (unsigned long)absRaw, (int)calMeas);
        LOGM("CAL_VERIFY", "stage=%u,name=%s,ok=%u,dac=%u,measRaw=%ld,abs=%lu,meas16=%d",
             calStage, psocCalStageName(calStage), event.value,
             calDac, (long)calMeasRawSigned, (unsigned long)absRaw,
             (int)calMeas);
    } else if (event.event == PSOC_EVT_CAL_AMUX_IN) {
        SLAVE_LOG_PRINTF("[CAL] AMux signal channel=%u\n", event.value);
        LOGM("CAL_AMUX_IN", "channel=%u", event.value);
    } else if (event.event == PSOC_EVT_CAL_AMUX_CAP) {
        SLAVE_LOG_PRINTF("[CAL] AMux cap channel=%u\n", event.value);
        LOGM("CAL_AMUX_CAP", "channel=%u", event.value);
    } else if (event.event == PSOC_EVT_CAL_PROGRESS) {
        if (!(g_cfg_waiting && g_cfg_sub_cmd == PSOC_CMD_CALIBRATE)) {
            uint32_t now = millis();
            if ((uint32_t)(now - g_cal_progress_ack_ms) >=
                PSOC_CAL_PROGRESS_ACK_PERIOD_MS) {
                g_cal_progress_ack_ms = now;
                sendCfgAck(PSOC_CMD_CALIBRATE, 2);
            }
        }
#if PSOC_CAL_LOG_PROGRESS
        SLAVE_LOG_PRINTF("[CAL] progress stage=%u/%s\n",
                         event.value, psocCalStageName(event.value));
        LOGM("CAL_PROGRESS", "stage=%u,name=%s", event.value,
             psocCalStageName(event.value));
#endif
        /* Para calibracion manual, servicePsocConfigAck() emite el heartbeat
         * mientras g_cfg_waiting este activo. Para boot-cal autonoma no hay
         * pending ACK, asi que este evento mantiene vivo el estado del maestro. */
    } else if (event.event == PSOC_EVT_CAL_WATCHDOG) {
        SLAVE_LOG_PRINTF("[CAL] WATCHDOG timeout stage=%u/%s -> aborted\n",
                         event.value, psocCalStageName(event.value));
        LOGM("CAL_WATCHDOG", "stage=%u,name=%s", event.value,
             psocCalStageName(event.value));
    } else if (event.event == PSOC_EVT_CAL_LP_BAD) {
        SLAVE_LOG_PRINTF("[CAL] CRITICAL stage=%u/%s LP out of range\n",
                         event.value, psocCalStageName(event.value));
        LOGM("CAL_LP_BAD", "stage=%u,name=%s", event.value,
             psocCalStageName(event.value));
    } else if (event.event == PSOC_EVT_BUTTON) {
        SLAVE_LOG_PRINTF("[BUTTON] pressed pstate=%u/%s\n",
                         event.value, psocStateName(event.value));
        LOGM("BUTTON", "pstate=%u,pstateName=%s",
             event.value, psocStateName(event.value));
    } else if (event.event == PSOC_EVT_BUTTON_IGNORED) {
        SLAVE_LOG_PRINTF("[BUTTON] ignored pstate=%u/%s\n",
                         event.value, psocStateName(event.value));
        LOGM("BUTTON_IGNORED", "pstate=%u,pstateName=%s",
             event.value, psocStateName(event.value));
    } else if (event.event == PSOC_EVT_CAPTURE_CLAMPED) {
        SLAVE_LOG_PRINTF("[CAPTURE] clamped requested_sat=%u max=%u\n",
                         event.value, PSOC_CAPTURE_MAX_BATCHES);
        LOGM("CAPTURE_CLAMPED", "requested_sat=%u,max=%u",
             event.value, PSOC_CAPTURE_MAX_BATCHES);
    }

#if PSOC_DIAG_VERBOSE
    const char *eventName = psocDiagName(event.event);
    const char *stateName = psocStateName(event.psoc_state);
    SLAVE_LOG_PRINTF("[SLAVE] PSOC_EVT %s event=0x%02X val=%u pstate=%u/%s estate=%d bytes=%lu bOK=%lu fill=%u/%u\n",
                     eventName, event.event, event.value,
                     event.psoc_state, stateName, (int)g_state,
                     (unsigned long)psoc.bytesRx(),
                     (unsigned long)psoc.batchesOK(),
                     (unsigned)g_store_fill, (unsigned)g_rec_n_batches);
    LOGM("PSOC_EVT", "event=0x%02X,name=%s,val=%u,pstate=%u,pstateName=%s,estate=%d,bytes=%lu,bOK=%lu,fill=%u/%u",
         event.event, eventName, event.value,
         event.psoc_state, stateName, (int)g_state,
         (unsigned long)psoc.bytesRx(),
         (unsigned long)psoc.batchesOK(),
         (unsigned)g_store_fill, (unsigned)g_rec_n_batches);
#endif
}

/* ── ISR GPIO hardware sync (IRAM para máxima velocidad) ────────────────── */
void IRAM_ATTR onSyncEdge()
{
    int level = digitalRead(SYNC_IN_PIN);
    if (level == HIGH && g_state == HOT_WAIT) {
        digitalWrite(SYNC_TO_PSOC_PIN, HIGH);
        uint32_t nowUs = (uint32_t)micros();
        g_t_start_us    = (uint64_t)nowUs;
        g_store_fill    = 0;
        g_debug_count   = 0;
        g_debug_last_us = nowUs;
        g_state = SAMPLING;
    } else if (level == LOW && g_state == SAMPLING) {
        digitalWrite(SYNC_TO_PSOC_PIN, LOW);
        g_state = STOPPED;
    } else if (g_state != SAMPLING) {
        digitalWrite(SYNC_TO_PSOC_PIN, LOW);
    }
}

/* ── Forward declarations ────────────────────────────────────────────────── */
static void allocStore(uint16_t n_batches);
static void handleReqBatch(const MsgReqBatch *msg);

/* ── Callbacks ESP-NOW ───────────────────────────────────────────────────── */

static void sendCfgAck(uint8_t sub_cmd, uint8_t ok)
{
    MsgCfgAck ack = { CMD_CFG_ACK, NODE_ID, sub_cmd, ok };
    espnowSend(MASTER_MAC, (const uint8_t *)&ack, sizeof(ack));
}

static bool isGainCode(uint8_t code)
{
    return code <= 8u;
}

static void applyConfirmedConfig(uint8_t sub_cmd, uint8_t value)
{
    switch (sub_cmd) {
        case 0xA6:
            g_pga_code = value;
            break;
        case 0xA9:
            g_pgavdac = value;
            break;
        case 0xAA:
            g_vdac_byte = value;
            break;
        default:
            break;
    }
}

static uint8_t expectedPsocAckValue(uint8_t sub_cmd, uint8_t param)
{
    /* SAVE_EEPROM se envia con parametro 0, pero el PSoC responde 1 si grabo bien. */
    return (sub_cmd == PSOC_CMD_SAVE_EEPROM) ? 1u : param;
}

static void waitForPsocConfigAck(uint8_t sub_cmd, uint8_t param)
{
    g_cfg_sub_cmd = sub_cmd;
    g_cfg_param = param;
    g_cfg_start_ms = millis();
    g_cfg_timeout_ms = (sub_cmd == PSOC_CMD_CALIBRATE)
                     ? PSOC_CAL_ACK_TIMEOUT_MS
                     : ((sub_cmd == PSOC_CMD_ADC_SNAPSHOT)
                        ? PSOC_ADC_SNAPSHOT_ACK_TIMEOUT_MS
                        : PSOC_CFG_ACK_TIMEOUT_MS);
    g_cfg_waiting = true;
    if (sub_cmd == PSOC_CMD_CALIBRATE) {
        /* Que el primer "calibrando..." salga ya en el proximo loop(),
         * sin esperar PSOC_CAL_PROGRESS_ACK_PERIOD_MS. */
        g_cal_progress_ack_ms = g_cfg_start_ms - PSOC_CAL_PROGRESS_ACK_PERIOD_MS;
    }
}

static bool ensurePsocReadyForConfig()
{
    psoc.poll();
    if (g_psocConnected && psoc.hasLastByte() &&
        psoc.lastByteAgeMs() <= PSOC_CFG_READY_STALE_MS) {
        return true;
    }

    const bool ready = psoc.probe(250);
    g_psocConnected = ready;
    if (!ready) {
        SLAVE_LOG_PRINTF("[SLAVE] PSoC cfg probe failed age=%lu bytes=%lu\n",
                         (unsigned long)psoc.lastByteAgeMs(),
                         (unsigned long)psoc.bytesRx());
        LOGM("CFG_PSOC_MISS", "age=%lu,bytes=%lu",
             (unsigned long)psoc.lastByteAgeMs(),
             (unsigned long)psoc.bytesRx());
    }
    return ready;
}

static void servicePsocConfigAck()
{
    if (g_state == SAMPLING) {
        return;   /* Nunca emitir ACK/timeout por radio durante la captura. */
    }

    uint8_t ackCmd = 0;
    uint8_t ackVal = 0;
    while (psoc.takeConfigAck(ackCmd, ackVal)) {
        g_psocConnected = true;
        if (g_cfg_waiting && ackCmd == g_cfg_sub_cmd) {
            const uint8_t expected = expectedPsocAckValue(g_cfg_sub_cmd, g_cfg_param);
            const uint8_t ok = (ackVal == expected) ? 1u : 0u;
            if (ok) {
                applyConfirmedConfig(ackCmd, ackVal);
            }
            sendCfgAck(g_cfg_sub_cmd, ok);
            SLAVE_LOG_PRINTF("[SLAVE] PSoC cfg ack sub=0x%02X val=%u expected=%u ok=%u\n",
                             ackCmd, ackVal, expected, ok);
            LOGM("CFG_ACK", "sub=0x%02X,val=%u,expected=%u,ok=%u",
                 ackCmd, ackVal, expected, ok);
            g_cfg_waiting = false;
        } else {
            applyConfirmedConfig(ackCmd, ackVal);
            SLAVE_LOG_PRINTF("[SLAVE] PSoC cfg ack unsolicited sub=0x%02X val=%u\n",
                             ackCmd, ackVal);
            LOGM("CFG_ACK_UNSOL", "sub=0x%02X,val=%u", ackCmd, ackVal);
        }
    }

    if (g_cfg_waiting &&
        (uint32_t)(millis() - g_cfg_start_ms) >= g_cfg_timeout_ms) {
        const uint8_t expected = expectedPsocAckValue(g_cfg_sub_cmd, g_cfg_param);
        sendCfgAck(g_cfg_sub_cmd, 0);
        SLAVE_LOG_PRINTF("[SLAVE] PSoC cfg ack timeout sub=0x%02X expected=%u elapsed=%lu/%lu\n",
                         g_cfg_sub_cmd, expected,
                         (unsigned long)(millis() - g_cfg_start_ms),
                         (unsigned long)g_cfg_timeout_ms);
        LOGM("CFG_TIMEOUT", "sub=0x%02X,expected=%u,elapsed=%lu,timeout=%lu",
             g_cfg_sub_cmd, expected,
             (unsigned long)(millis() - g_cfg_start_ms),
             (unsigned long)g_cfg_timeout_ms);
        g_cfg_waiting = false;
    }

    /* "Calibrando..." (ok=2) hacia el maestro: por temporizador propio del
     * ESP, independiente de que el stream de diagnostico del PSoC llegue.
     * Mientras g_cfg_waiting este activo para CALIBRATE (hasta
     * PSOC_CAL_ACK_TIMEOUT_MS=450s o el ack final), repetir cada
     * PSOC_CAL_PROGRESS_ACK_PERIOD_MS para que el panel muestre el estado
     * "Calibrando..." de forma confiable. */
    if (g_cfg_waiting && g_cfg_sub_cmd == PSOC_CMD_CALIBRATE) {
        uint32_t now = millis();
        if ((uint32_t)(now - g_cal_progress_ack_ms) >= PSOC_CAL_PROGRESS_ACK_PERIOD_MS) {
            g_cal_progress_ack_ms = now;
            sendCfgAck(PSOC_CMD_CALIBRATE, 2);
        }
    }
}

static bool captureOrDumpBusy()
{
    return (g_state == SAMPLING || g_state == HOT_WAIT ||
            g_view_remaining != 0 ||
            (g_state == STOPPED && g_rec_n_batches > 0));
}

static bool requestPsocCalibration(const char *source)
{
    if (captureOrDumpBusy()) {
        SLAVE_LOG_PRINTF("[%s] cal ignored: capture/dump busy state=%d\n",
                         source, (int)g_state);
        LOGM("CAL_REQ", "source=%s,ok=0,reason=capture_busy,state=%d",
             source, (int)g_state);
        return false;
    }

    servicePsocConfigAck();
    if (g_cfg_waiting) {
        SLAVE_LOG_PRINTF("[%s] cal ignored: pending sub=0x%02X\n",
                         source, g_cfg_sub_cmd);
        LOGM("CAL_REQ", "source=%s,ok=0,reason=cfg_busy,pending=0x%02X",
             source, g_cfg_sub_cmd);
        return false;
    }

    if (!ensurePsocReadyForConfig()) {
        SLAVE_LOG_PRINTF("[%s] cal failed: PSoC not ready\n", source);
        LOGM("CAL_REQ", "source=%s,ok=0,reason=psoc_not_ready", source);
        return false;
    }

    psoc.calibrate();
    waitForPsocConfigAck(PSOC_CMD_CALIBRATE, 1);
    SLAVE_LOG_PRINTF("[%s] cal -> PSoC CMD 0x%02X\n", source, PSOC_CMD_CALIBRATE);
    LOGM("CAL_REQ", "source=%s,ok=1,sub=0x%02X", source, PSOC_CMD_CALIBRATE);
    return true;
}

static bool requestPsocAdcSnapshot(const char *source)
{
    if (captureOrDumpBusy()) {
        SLAVE_LOG_PRINTF("[%s] adc ignored: capture/dump busy state=%d\n",
                         source, (int)g_state);
        LOGM("ADC_REQ", "source=%s,ok=0,reason=capture_busy,state=%d",
             source, (int)g_state);
        return false;
    }

    servicePsocConfigAck();
    if (g_cfg_waiting) {
        SLAVE_LOG_PRINTF("[%s] adc ignored: pending sub=0x%02X\n",
                         source, g_cfg_sub_cmd);
        LOGM("ADC_REQ", "source=%s,ok=0,reason=cfg_busy,pending=0x%02X",
             source, g_cfg_sub_cmd);
        return false;
    }

    if (!ensurePsocReadyForConfig()) {
        SLAVE_LOG_PRINTF("[%s] adc failed: PSoC not ready\n", source);
        LOGM("ADC_REQ", "source=%s,ok=0,reason=psoc_not_ready", source);
        return false;
    }

    psoc.adcSnapshot();
    waitForPsocConfigAck(PSOC_CMD_ADC_SNAPSHOT, 1);
    SLAVE_LOG_PRINTF("[%s] adc -> PSoC CMD 0x%02X\n", source, PSOC_CMD_ADC_SNAPSHOT);
    LOGM("ADC_REQ", "source=%s,ok=1,sub=0x%02X", source, PSOC_CMD_ADC_SNAPSHOT);
    return true;
}

#if PSOC_AUTO_CAL_ON_READY
static void scheduleAutoCalibration(uint32_t delayMs)
{
    if (!g_auto_cal_requested) {
        g_auto_cal_due_ms = millis() + delayMs;
        SLAVE_LOG_PRINTF("[AUTO_CAL] scheduled in %lu ms (due=%lu)\n",
                         (unsigned long)delayMs, (unsigned long)g_auto_cal_due_ms);
        LOGM("AUTO_CAL", "ev=scheduled,delay_ms=%lu", (unsigned long)delayMs);
    }
}

static void serviceAutoCalibration()
{
    if (g_auto_cal_requested || g_auto_cal_due_ms == 0u) {
        return;
    }
    if ((int32_t)(millis() - g_auto_cal_due_ms) < 0) {
        return;
    }
    if (!g_psocConnected || captureOrDumpBusy() || g_cfg_waiting) {
        SLAVE_LOG_PRINTF("[AUTO_CAL] deferred (psocConnected=%d busy=%d cfg_waiting=%d) retry in %lu ms\n",
                         (int)g_psocConnected, (int)captureOrDumpBusy(), (int)g_cfg_waiting,
                         (unsigned long)PSOC_AUTO_CAL_RETRY_MS);
        LOGM("AUTO_CAL", "ev=deferred,psoc=%d,busy=%d,cfg_waiting=%d",
             (int)g_psocConnected, (int)captureOrDumpBusy(), (int)g_cfg_waiting);
        g_auto_cal_due_ms = millis() + PSOC_AUTO_CAL_RETRY_MS;
        return;
    }
    SLAVE_LOG_PRINTF("[AUTO_CAL] due -> requesting calibration\n");
    if (requestPsocCalibration("AUTO")) {
        g_auto_cal_requested = true;
        g_auto_cal_due_ms = 0u;
        LOGM("AUTO_CAL", "ev=requested,ok=1");
    } else {
        SLAVE_LOG_PRINTF("[AUTO_CAL] request failed, retry in %lu ms\n",
                         (unsigned long)PSOC_AUTO_CAL_RETRY_MS);
        LOGM("AUTO_CAL", "ev=requested,ok=0");
        g_auto_cal_due_ms = millis() + PSOC_AUTO_CAL_RETRY_MS;
    }
}
#else
static void scheduleAutoCalibration(uint32_t) {}
static void serviceAutoCalibration() {}
#endif

#if SLAVE_USB_CMD_ENABLE
static bool usbCommandEquals(const char *cmd, const char *word)
{
    while (*cmd != '\0' && *word != '\0') {
        char a = *cmd++;
        char b = *word++;
        if (a >= 'A' && a <= 'Z') { a = (char)(a - 'A' + 'a'); }
        if (b >= 'A' && b <= 'Z') { b = (char)(b - 'A' + 'a'); }
        if (a != b) { return false; }
    }
    return *cmd == '\0' && *word == '\0';
}

static bool usbCommandStartsWithWord(const char *cmd, const char *word)
{
    while (*cmd != '\0' && *word != '\0') {
        char a = *cmd++;
        char b = *word++;
        if (a >= 'A' && a <= 'Z') { a = (char)(a - 'A' + 'a'); }
        if (b >= 'A' && b <= 'Z') { b = (char)(b - 'A' + 'a'); }
        if (a != b) { return false; }
    }
    return *word == '\0' && (*cmd == ' ' || *cmd == '=' || *cmd == ':');
}

static bool usbParseGainParam(const char *cmd, const char *word, uint8_t &value)
{
    if (!usbCommandStartsWithWord(cmd, word)) {
        return false;
    }
    while (*cmd != '\0' && *cmd != ' ' && *cmd != '=' && *cmd != ':') {
        cmd++;
    }
    while (*cmd == ' ' || *cmd == '=' || *cmd == ':') {
        cmd++;
    }
    if (*cmd < '0' || *cmd > '8') {
        return false;
    }
    value = (uint8_t)(*cmd - '0');
    cmd++;
    while (*cmd == ' ') {
        cmd++;
    }
    return *cmd == '\0';
}

static void requestCalibrationFromUsb()
{
    const bool sent = requestPsocCalibration("USB");
    if (sent) {
        g_auto_cal_requested = true;
        g_auto_cal_due_ms = 0u;
    }
    LOGM("USB_CMD", "cmd=cal,ok=%u,sub=0x%02X",
         (unsigned)sent, PSOC_CMD_CALIBRATE);
}

static void requestAdcSnapshotFromUsb()
{
    const bool sent = requestPsocAdcSnapshot("USB");
    LOGM("USB_CMD", "cmd=adc,ok=%u,sub=0x%02X",
         (unsigned)sent, PSOC_CMD_ADC_SNAPSHOT);
}

static bool requestPsocGainFromUsb(uint8_t subCmd, uint8_t param, const char *name)
{
    bool sent = false;
    if (!isGainCode(param)) {
        LOGM("USB_CMD", "cmd=%s,ok=0,reason=bad_gain,value=%u", name, (unsigned)param);
        return false;
    }
    servicePsocConfigAck();
    if (g_cfg_waiting) {
        LOGM("USB_CMD", "cmd=%s,ok=0,reason=cfg_busy,pending=0x%02X",
             name, g_cfg_sub_cmd);
        return false;
    }
    if (ensurePsocReadyForConfig()) {
        if (subCmd == PSOC_CMD_PGA) {
            psoc.setPga(param);
        } else {
            psoc.setPgavdac(param);
        }
        waitForPsocConfigAck(subCmd, param);
        sent = true;
    }
    SLAVE_LOG_PRINTF("[USB] %s %u -> PSoC CMD 0x%02X\n",
                     name, (unsigned)param, subCmd);
    LOGM("USB_CMD", "cmd=%s,ok=%u,sub=0x%02X,value=%u",
         name, (unsigned)sent, subCmd, (unsigned)param);
    return sent;
}

static void handleUsbCommand(const char *cmd)
{
    uint8_t value = 0u;
    if (cmd[0] == '\0') {
        return;
    }
    if (usbCommandEquals(cmd, "c") ||
        usbCommandEquals(cmd, "cal") ||
        usbCommandEquals(cmd, "calibrate")) {
        requestCalibrationFromUsb();
    } else if (usbCommandEquals(cmd, "a") ||
               usbCommandEquals(cmd, "adc") ||
               usbCommandEquals(cmd, "snapshot")) {
        requestAdcSnapshotFromUsb();
    } else if (usbParseGainParam(cmd, "pga", value)) {
        (void)requestPsocGainFromUsb(PSOC_CMD_PGA, value, "pga");
    } else if (usbParseGainParam(cmd, "pgavdac", value)) {
        (void)requestPsocGainFromUsb(PSOC_CMD_PGAVDAC, value, "pgavdac");
    } else if (usbCommandEquals(cmd, "?") || usbCommandEquals(cmd, "help")) {
        SLAVE_LOG_PRINTF("[USB] commands: cal, adc, pga N, pgavdac N\n");
        LOGM("USB_CMD", "cmd=help,ok=1");
    } else {
        SLAVE_LOG_PRINTF("[USB] unknown command '%s' (use: cal, adc, pga N, pgavdac N)\n", cmd);
        LOGM("USB_CMD", "cmd=unknown,ok=0,text=%s", cmd);
    }
}

static void serviceUsbCommands()
{
    static char line[16];
    static uint8_t pos = 0;

    while (Serial.available() > 0) {
        char ch = (char)Serial.read();
        if (ch == '\r' || ch == '\n') {
            line[pos] = '\0';
            handleUsbCommand(line);
            pos = 0;
        } else if (ch == 8 || ch == 127) {
            if (pos > 0) { pos--; }
        } else if (ch >= 32 && ch <= 126) {
            if (pos < sizeof(line) - 1) {
                line[pos++] = ch;
            }
        }
    }
}
#else
static void serviceUsbCommands() {}
#endif

static void sendStartAck(uint8_t status, uint32_t startToken, uint32_t rxUs)
{
    MsgStartAck ack = { CMD_START_ACK, NODE_ID, status, startToken, rxUs };
    espnowSend(MASTER_MAC, (const uint8_t *)&ack, sizeof(ack));
}

static bool storeReadyForHotWait()
{
    return (g_rec_n_batches == 0) || (g_store_buf != nullptr && g_store_ts_us != nullptr);
}

static uint16_t effectivePsocSampleRateHz()
{
    uint16_t fs = psoc.sampleRate();
    if (fs == (uint16_t)PSOC_NOMINAL_SAMPLE_RATE_HZ) {
        return (uint16_t)PSOC_EFFECTIVE_SAMPLE_RATE_HZ;
    }
    return fs;
}

static uint16_t clampPsocCaptureBatches(uint16_t n)
{
    if (n > PSOC_CAPTURE_MAX_BATCHES) {
        return (uint16_t)PSOC_CAPTURE_MAX_BATCHES;
    }
    return n;
}

static uint32_t expectedViewCaptureMs(uint16_t n_batches)
{
    uint32_t fs = effectivePsocSampleRateHz();
    if (fs == 0) fs = 2929u;
    uint32_t ms = ((uint32_t)n_batches * (uint32_t)SPI_BATCH_SAMPLES * 1000u + fs - 1u) / fs;
    return ms + PSOC_VIEW_START_FALLBACK_EXTRA_MS;
}

static void enterHotWait(uint16_t n_batches)
{
    n_batches = clampPsocCaptureBatches(n_batches);
    allocStore(n_batches);
    digitalWrite(SYNC_TO_PSOC_PIN, LOW);
    g_t_start_us     = 0;
    g_store_fill     = 0;
    g_view_remaining = 0;
    g_view_store_active = false;
    g_debug_count    = 0;
    g_debug_last_us  = (uint32_t)micros();
    g_view_start_fallback_sent = false;
    g_state = HOT_WAIT;
    /* Armar el PSoC por UART: set N y pre-start (queda esperando flanco SYNC). */
    psoc.setN(n_batches);
    psoc.preStart();
    SLAVE_LOG_PRINTF("[SLAVE] HOT_WAIT n=%u ready=%u\n",
                     n_batches, (unsigned)storeReadyForHotWait());
    LOGM("HOTWAIT", "n=%u,ready=%u,sync=%d,bytes=%lu",
         n_batches, (unsigned)storeReadyForHotWait(),
         digitalRead(SYNC_TO_PSOC_PIN), (unsigned long)psoc.bytesRx());
}

static void sendHotWaitAck()
{
    uint8_t ok = (g_state == HOT_WAIT && storeReadyForHotWait()) ? 1 : 0;
    MsgHotWaitAck ack = {
        CMD_HOTWAIT_ACK, NODE_ID, ok, (uint8_t)g_state, g_rec_n_batches
    };
    espnowSend(MASTER_MAC, (const uint8_t *)&ack, sizeof(ack));
    SLAVE_LOG_PRINTF("[SLAVE] HOTWAIT_ACK ok=%u state=%u n=%u\n",
                     ok, (unsigned)g_state, g_rec_n_batches);
}

static void debugEspSetRamp(bool enable)
{
    g_debug_mode = enable;
    g_debug_count = 0;
    g_debug_last_us = (uint32_t)micros();
    if (enable) {
        g_store_fill  = 0;   /* reset store index para que cada test/debug empiece desde 0 */
        if (g_rec_n_batches == 0 && g_state != HOT_WAIT) {
            /* Modo streaming/test: A7 arranca la rampa inmediatamente. */
            g_t_start_us = (uint64_t)g_debug_last_us;
            g_state = SAMPLING;
        } else if (g_state == WAIT_ARM || g_state == STOPPED) {
            /* Store-and-forward: A7 solo selecciona debug; START/SYNC arranca. */
            g_state = ARMED;
        }
    } else if (g_state == SAMPLING) {
        g_state = STOPPED;
    } else if (g_rec_n_batches == 0 && g_state != HOT_WAIT) {
        g_state = WAIT_ARM;
    }
}

static void handleSetConfig(const MsgSetConfig *cfg)
{
    if (cfg->node_id != NODE_ID) return;
    if (g_state == SAMPLING) {
        SLAVE_LOG_PRINTF("[SLAVE] cfg ignored during capture sub=0x%02X\n",
                         cfg->sub_cmd);
        LOGM("CFG_IGN_CAPTURE", "sub=0x%02X", cfg->sub_cmd);
        return;   /* Sin ACK por radio mientras se muestrea. */
    }
    servicePsocConfigAck();
    if (g_cfg_waiting) {
        sendCfgAck(cfg->sub_cmd, 0);
        SLAVE_LOG_PRINTF("[SLAVE] cfg busy sub=0x%02X pending=0x%02X\n",
                         cfg->sub_cmd, g_cfg_sub_cmd);
        LOGM("CFG_BUSY", "sub=0x%02X,pending=0x%02X", cfg->sub_cmd, g_cfg_sub_cmd);
        return;
    }

    const uint8_t subCmd = cfg->sub_cmd;
    const uint8_t param = cfg->param;
    uint8_t ok = 1;
    bool waitAck = false;
    switch (cfg->sub_cmd) {
        case 0xA6:                       /* PGA */
            if (!isGainCode(param)) {
                ok = 0;
            } else {
                waitAck = true;
            }
            break;
        case 0xA9:                       /* PGAvdac */
            if (!isGainCode(param)) {
                ok = 0;
            } else {
                waitAck = true;
            }
            break;
        case 0xAA:                       /* VDAC (calibración) */
            waitAck = true;
            break;
        case PSOC_CMD_CALIBRATE:
            waitAck = true;
            break;
        case PSOC_CMD_SAVE_EEPROM:          /* 0xB6: guardar calibración en EEPROM */
            waitAck = true;
            break;
        case PSOC_CMD_SELECT_STREAM:        /* 0xB7: 0=crudo, 1=FIR hardware */
            waitAck = true;
            break;
        default:
            ok = 0;
            break;
    }
    if (ok && waitAck) {
        if (!ensurePsocReadyForConfig()) {
            ok = 0;
            sendCfgAck(subCmd, ok);
        } else {
            switch (subCmd) {
                case 0xA6:
                    psoc.setPga(param);
                    break;
                case 0xA9:
                    psoc.setPgavdac(param);
                    break;
                case 0xAA:
                    psoc.setVdac(param);
                    break;
                case PSOC_CMD_CALIBRATE:
                    psoc.calibrate();
                    break;
                case PSOC_CMD_SAVE_EEPROM:
                    psoc.saveEeprom();
                    break;
                case PSOC_CMD_SELECT_STREAM:
                    psoc.selectStream(param);
                    break;
                default:
                    ok = 0;
                    break;
            }
            if (ok) {
                waitForPsocConfigAck(subCmd, param);
            } else {
                sendCfgAck(subCmd, ok);
            }
        }
    } else {
        sendCfgAck(subCmd, ok);
    }
    SLAVE_LOG_PRINTF("[SLAVE] cfg sub=0x%02X p=%u ok=%u\n",
                     subCmd, param, ok);
    LOGM("CFG", "sub=0x%02X,p=%u,ok=%u,wait=%u",
         subCmd, param, ok, (unsigned)waitAck);
}

static void handleDebugNode(const MsgDebugNode *dbg)
{
    if (dbg->node_id != NODE_ID) return;
    bool enable = (dbg->enable != 0);
    debugEspSetRamp(enable);
    sendCfgAck(0xA7, 1);
    SLAVE_LOG_PRINTF("[SLAVE] debug_node=%d\n", (int)g_debug_mode);
}

#if defined(ESP8266)
static void onDataRecv(uint8_t *mac, uint8_t *data, uint8_t len)
#else
static void onDataRecv(const uint8_t *mac, const uint8_t *data, int len)
#endif
{
    (void)mac;
    if (len < 1) return;
    uint8_t cmd = data[0];
    if (g_state == SAMPLING && cmd != CMD_STOP) {
        /* En debug/test, VER puede interrumpir para evitar race condition */
        if (g_debug_mode && cmd == CMD_VIEW) {
            g_debug_mode = false;
            g_state = STOPPED;
        } else {
            return;   /* No ACK/radio durante muestreo real. */
        }
    }
    LOGM("RX", "cmd=0x%02X,len=%d,state=%d", cmd, len, (int)g_state);

    if (cmd == CMD_ARM &&
        (g_state == WAIT_ARM || g_state == ARMED || g_state == HOT_WAIT ||
         g_state == STOPPED || g_state == SAMPLING)) {
        g_debug_mode = false;   /* detener debug stream si estaba activo */
        g_state = ARMED;
        scheduleAutoCalibration(0u);
        MsgArmAck ack = { CMD_ARM_ACK, NODE_ID, 0 };
        espnowSend(MASTER_MAC, (const uint8_t *)&ack, sizeof(ack));
        SLAVE_LOG_PRINTLN("[SLAVE] ARMED");
    }
    else if (cmd == CMD_PRESTART && len >= (int)sizeof(MsgPrestart) &&
             g_state != SAMPLING) {
        if (!g_auto_cal_requested && g_auto_cal_due_ms != 0u) {
            serviceAutoCalibration();
        }
        if (g_cfg_waiting && g_cfg_sub_cmd == PSOC_CMD_CALIBRATE) {
            SLAVE_LOG_PRINTLN("[SLAVE] PRESTART deferred: calibration busy");
            LOGM("PRESTART_DEFER", "reason=cal_busy,state=%d", (int)g_state);
            return;
        }
        if (!g_auto_cal_requested && g_auto_cal_due_ms != 0u) {
            SLAVE_LOG_PRINTLN("[SLAVE] PRESTART deferred: auto calibration pending");
            LOGM("PRESTART_DEFER", "reason=auto_cal_pending,state=%d", (int)g_state);
            return;
        }
        const MsgPrestart *msg = (const MsgPrestart *)data;
        enterHotWait(msg->n_batches);
    }
    else if (cmd == CMD_HOTWAIT_QUERY && len >= (int)sizeof(MsgHotWaitQuery)) {
        const MsgHotWaitQuery *msg = (const MsgHotWaitQuery *)data;
        if (msg->node_id == NODE_ID) {
            sendHotWaitAck();
        }
    }
    else if (cmd == CMD_SCOPE_START && len >= (int)sizeof(MsgScopeStart)) {
        if (g_state == HOT_WAIT && storeReadyForHotWait()) {
            debugEspHardwareStartPulse();
            digitalWrite(SYNC_TO_PSOC_PIN, LOW);
            SLAVE_LOG_PRINTLN("[SLAVE] SCOPE_START");
        } else {
            SLAVE_LOG_PRINTF("[SLAVE] SCOPE_START ignored state=%d ready=%u\n",
                             (int)g_state, (unsigned)storeReadyForHotWait());
        }
    }
    else if (cmd == CMD_START && len >= (int)sizeof(MsgStart)) {
        const MsgStart *msg = (const MsgStart *)data;
        uint8_t targetNode = msg->target_node & 0x7Fu;
        bool probeOnly = (msg->target_node & 0x80u) != 0;
        if (targetNode != 0 && targetNode != NODE_ID) return;

        uint32_t nowUs = (uint32_t)micros();
        uint32_t startToken = (uint32_t)msg->t_start_us;
        if (probeOnly) {
            sendStartAck((g_state == HOT_WAIT && storeReadyForHotWait()) ? 2 : 0,
                         startToken, nowUs);
            SLAVE_LOG_PRINTF("[SLAVE] START probe token=%u state=%d\n",
                             (unsigned)startToken, (int)g_state);
            return;
        }
        if (g_state == SAMPLING) {
            sendStartAck(1, startToken, nowUs);
            SLAVE_LOG_PRINTF("[SLAVE] START already sampling token=%u\n",
                             (unsigned)startToken);
            return;
        }
        bool startAllowed = (g_state == HOT_WAIT && storeReadyForHotWait());
        if (!startAllowed) {
            sendStartAck(0, startToken, nowUs);
            SLAVE_LOG_PRINTF("[SLAVE] START ignored state=%d ready=%u\n",
                             (int)g_state, (unsigned)storeReadyForHotWait());
            return;
        }
        int syncBefore = digitalRead(SYNC_TO_PSOC_PIN);
        digitalWrite(SYNC_TO_PSOC_PIN, HIGH);
        int syncAfter = digitalRead(SYNC_TO_PSOC_PIN);
        sendStartAck(1, startToken, nowUs);
        g_t_start_us    = msg->t_start_us;
        g_store_fill    = 0;   /* reset store index para nueva grabación */
        g_debug_count   = 0;
        g_debug_last_us = nowUs;
        g_sampling_start_ms = millis();
        g_sampling_start_psoc_bytes = psoc.bytesRx();
        g_sampling_start_batches_ok = psoc.batchesOK();
        g_view_start_fallback_sent = false;
        g_state = SAMPLING;
        SLAVE_LOG_PRINTF("[SLAVE] START_OK target=%u token=%u sync=%d->%d bytes=%lu fill=%u/%u view=%u\n",
                         (unsigned)targetNode, (unsigned)startToken,
                         syncBefore, syncAfter,
                         (unsigned long)g_sampling_start_psoc_bytes,
                         (unsigned)g_store_fill, (unsigned)g_rec_n_batches,
                         (unsigned)g_view_store_active);
        LOGM("START_OK", "token=%u,target=%u,n_batches=%u,store=%d,sync=%d->%d,view=%u",
             (unsigned)startToken, (unsigned)targetNode,
             (unsigned)g_rec_n_batches, (int)(g_store_buf != nullptr),
             syncBefore, syncAfter, (unsigned)g_view_store_active);
    }
    else if (cmd == CMD_STOP) {
        digitalWrite(SYNC_TO_PSOC_PIN, LOW);
        g_debug_mode = false;
        if (g_view_remaining > 0) {
            g_view_remaining = 0;
        }
        psoc.debugRamp(false);      /* por seguridad: cortar cualquier debug PSoC */
        g_view_store_active = false;
        g_state = STOPPED;
        SLAVE_LOG_PRINTLN("[SLAVE] STOP");
    }
    else if (cmd == CMD_DEBUG && len >= (int)sizeof(MsgDebug)) {
        const MsgDebug *d = (const MsgDebug *)data;
        bool enable = (d->enable != 0);
        debugEspSetRamp(enable);
        MsgArmAck ack = { CMD_ARM_ACK, NODE_ID, (uint8_t)(g_debug_mode ? 0xDD : 0x00) };
        espnowSend(MASTER_MAC, (const uint8_t *)&ack, sizeof(ack));
        SLAVE_LOG_PRINTF("[SLAVE] debug=%d\n", (int)g_debug_mode);
    }
    else if (cmd == CMD_SET_CONFIG && len >= (int)sizeof(MsgSetConfig)) {
        handleSetConfig((const MsgSetConfig *)data);
    }
    else if (cmd == CMD_DEBUG_NODE && len >= (int)sizeof(MsgDebugNode)) {
        handleDebugNode((const MsgDebugNode *)data);
    }
    else if (cmd == CMD_VIEW && len >= (int)sizeof(MsgView)) {
        /* "Ver": igual que PRESTART en el flujo START.
         * El esclavo entra en HOT_WAIT y espera CMD_START del maestro
         * (el maestro hace HOTWAIT_QUERY → START dirigido a este nodo).
         * Sin auto-arme: todo el muestreo empieza solo cuando el maestro
         * confirma que no hay RF pendiente, igual que en START normal. */
        const MsgView *msg = (const MsgView *)data;
        if (msg->node_id != NODE_ID) return;
        uint16_t n = msg->n ? msg->n : 1;
        if (!g_psocConnected) {
            SLAVE_LOG_PRINTF("[SLAVE] VIEW: PSoC aun sin HELLO, intento igual\n");
            LOGM("VIEW_WARN", "psoc=0");
        }
        digitalWrite(SYNC_TO_PSOC_PIN, LOW);
        g_debug_mode = false;
        psoc.debugRamp(false);
        enterHotWait(n);
        uint8_t ok = storeReadyForHotWait() ? 1u : 0u;
        if (ok) {
            g_view_store_active = true;
        } else {
            g_view_store_active = false;
            g_state = STOPPED;
        }
        sendCfgAck(CMD_VIEW, ok);
        SLAVE_LOG_PRINTF("[SLAVE] VIEW n=%u HOT_WAIT ok=%u (esperando CMD_START)\n", n, ok);
        LOGM("VIEW", "n=%u,store=1,hotwait=1,ok=%u", n, ok);
    }
    else if (cmd == CMD_DEBUG_PSOC && len >= (int)sizeof(MsgDebugPsoc)) {
        const MsgDebugPsoc *msg = (const MsgDebugPsoc *)data;
        if (msg->node_id != NODE_ID) return;
        psoc.debugRamp(msg->enable != 0);
        sendCfgAck(CMD_DEBUG_PSOC, 1);
        SLAVE_LOG_PRINTF("[SLAVE] DEBUG_PSOC=%u\n", msg->enable);
        LOGM("DBGPSOC", "en=%u", msg->enable);
    }
    else if (cmd == CMD_SET_RECLEN && len >= (int)sizeof(MsgSetRecLen)) {
        const MsgSetRecLen *msg = (const MsgSetRecLen *)data;
        allocStore(msg->n_batches);
        /* Confirmar al maestro que el buffer está listo */
        MsgCfgAck ack = { CMD_CFG_ACK, NODE_ID, CMD_SET_RECLEN,
                          (uint8_t)((msg->n_batches == 0 || storeReadyForHotWait()) ? 1 : 0) };
        espnowSend(MASTER_MAC, (const uint8_t *)&ack, sizeof(ack));
    }
    else if (cmd == CMD_REQ_BATCH && len >= (int)sizeof(MsgReqBatch)) {
        handleReqBatch((const MsgReqBatch *)data);
    }
    else if (cmd == CMD_BLINK_LED && len >= (int)sizeof(MsgBlinkLed)) {
        const MsgBlinkLed *msg = (const MsgBlinkLed *)data;
        if (msg->node_id != 0u && msg->node_id != NODE_ID) return;
        g_blink_count   = (BLINK_TIMES * 2u) - 1u;  /* ya queda prendido ahora */
        g_blink_last_ms = millis();
        digitalWrite(BLINK_LED_PIN, BLINK_LED_ON_LEVEL);
        sendCfgAck(CMD_BLINK_LED, 1);
        SLAVE_LOG_PRINTF("[SLAVE] BLINK_LED node=%u\n", NODE_ID);
        LOGM("BLINK_LED", "node=%u", NODE_ID);
    }
}
/* SaveEEPROM (0xB6) y SelectStream (0xB7) llegan por CMD_SET_CONFIG → handleSetConfig */

/* ── Store-and-forward helpers ───────────────────────────────────────────── */

static void allocStore(uint16_t n_batches)
{
    n_batches = clampPsocCaptureBatches(n_batches);
    free(g_store_buf);  free(g_store_ts_us);
    g_store_buf    = nullptr;
    g_store_ts_us  = nullptr;
    g_store_fill   = 0;
    g_rec_n_batches= n_batches;
    if (n_batches == 0) return;
    g_store_buf   = (SampleBytes *)malloc((size_t)n_batches * SPI_BATCH_SAMPLES * sizeof(SampleBytes));
    g_store_ts_us = (uint32_t    *)malloc((size_t)n_batches * sizeof(uint32_t));
    if (!g_store_buf || !g_store_ts_us) {
        free(g_store_buf);
        free(g_store_ts_us);
        g_store_buf = nullptr;
        g_store_ts_us = nullptr;
    }
    SLAVE_LOG_PRINTF("[SLAVE] store alloc n=%u buf=%s\n",
                     n_batches, storeReadyForHotWait() ? "OK" : "FAIL");
}

static void handleReqBatch(const MsgReqBatch *msg)
{
    if (msg->node_id != NODE_ID) return;
    uint16_t seq = msg->batch_seq;
    if (g_state == SAMPLING) {
        return;   /* No TX ESP-NOW mientras el ADC está capturando. */
    }
    if (g_view_store_active && g_store_fill < g_rec_n_batches) {
        SLAVE_LOG_PRINTF("[SLAVE] REQ_BATCH ignored during capture seq=%u fill=%u/%u state=%d\n",
                         seq, g_store_fill, g_rec_n_batches, (int)g_state);
        return;
    }
    if (seq >= g_store_fill || !g_store_buf) {
        /* Batch fuera de rango: notificar al master para que avance sin esperar timeout */
        MsgCfgAck nack = { CMD_CFG_ACK, NODE_ID, CMD_REQ_BATCH, 0 };
        espnowSend(MASTER_MAC, (const uint8_t *)&nack, sizeof(nack));
        SLAVE_LOG_PRINTF("[SLAVE] REQ_BATCH NACK seq=%u fill=%u\n", seq, g_store_fill);
        return;
    }
    const SampleBytes *s  = &g_store_buf[(size_t)seq * SPI_BATCH_SAMPLES];
    uint64_t           ts = g_t_start_us + (uint64_t)g_store_ts_us[seq];
    transport.sendStoredBatch(s, seq, ts, NODE_ID);
}

/* ── Callback PSoC — batch recibido ─────────────────────────────────────── */

static void onBatch(const PsocBatch &batch)
{
    if (g_state != SAMPLING) return;
    g_psocConnected = true;

    if (g_view_remaining > 0) {
        g_view_remaining = 0;   /* fail-safe: VIEW live no debe transmitir mientras muestrea */
    }

    /* Routing normal: si g_store_buf está allocado → acumula; si no → en vivo. */
    if (g_store_buf && g_store_fill < g_rec_n_batches) {
        /* Store-and-forward: acumular en RAM, sin RF */
        SampleBytes *dst = &g_store_buf[(size_t)g_store_fill * SPI_BATCH_SAMPLES];
        for (int i = 0; i < SPI_BATCH_SAMPLES; i++) {
            const PsocSample &s = batch.samples[i];
            dst[i].raw_lo = (uint8_t)( s.raw_input         & 0xFF);
            dst[i].raw_hi = (uint8_t)((s.raw_input  >> 8)  & 0xFF);
            dst[i].alog0  = (uint8_t)( s.post_analog        & 0xFF);
            dst[i].alog1  = (uint8_t)((s.post_analog >>  8) & 0xFF);
            dst[i].alog2  = (uint8_t)((s.post_analog >> 16) & 0xFF);
            dst[i].digi0  = (uint8_t)( s.post_digital        & 0xFF);
            dst[i].digi1  = (uint8_t)((s.post_digital >>  8) & 0xFF);
            dst[i].digi2  = (uint8_t)((s.post_digital >> 16) & 0xFF);
            dst[i].gain   = s.gain_byte;
            dst[i].flags  = s.sample_flags;
        }
        g_store_ts_us[g_store_fill] = (uint32_t)batch.timestamp_us;
        g_store_fill++;
        if (g_store_fill >= g_rec_n_batches) {
            g_state = STOPPED;
            digitalWrite(SYNC_TO_PSOC_PIN, LOW);
            SLAVE_LOG_PRINTF("[SLAVE] FULL -> STOPPED (%u batches)\n", g_store_fill);
            if (g_view_store_active) {
                g_view_store_active = false;
                sendCfgAck(CMD_VIEW, 2);   /* VIEW_DONE: ahora sí puede empezar el dump */
                SLAVE_LOG_PRINTF("[SLAVE] VIEW capture done -> ACK\n");
                LOGM("VIEWDONE", "store=1,n=%u", g_store_fill);
            }
        }
    } else if (g_debug_mode) {
        /* Test: transmitir en vivo la rampa (no es señal ADC real, RF no afecta). */
        SampleBytes sb[SPI_BATCH_SAMPLES];
        for (int i = 0; i < SPI_BATCH_SAMPLES; i++) {
            const PsocSample &ps = batch.samples[i];
            sb[i].raw_lo = (uint8_t)( ps.raw_input          & 0xFF);
            sb[i].raw_hi = (uint8_t)((ps.raw_input   >>  8) & 0xFF);
            sb[i].alog0  = (uint8_t)( ps.post_analog         & 0xFF);
            sb[i].alog1  = (uint8_t)((ps.post_analog  >>  8) & 0xFF);
            sb[i].alog2  = (uint8_t)((ps.post_analog  >> 16) & 0xFF);
            sb[i].digi0  = (uint8_t)( ps.post_digital         & 0xFF);
            sb[i].digi1  = (uint8_t)((ps.post_digital >>  8) & 0xFF);
            sb[i].digi2  = (uint8_t)((ps.post_digital >> 16) & 0xFF);
            sb[i].gain   = ps.gain_byte;
            sb[i].flags  = ps.sample_flags;
        }
        uint64_t ts = g_t_start_us + (uint64_t)batch.timestamp_us;
        transport.sendStoredBatch(sb, g_store_fill, ts, NODE_ID);
        g_store_fill++;
    }
}

/* ── Setup ───────────────────────────────────────────────────────────────── */

void setup()
{
    dbgLogBegin(DBG_LOG_BAUD);   /* Serial USB del esclavo = canal de log */
    SLAVE_LOG_PRINTF("[SLAVE %d] boot\n", NODE_ID);
    samplePulseBegin();
    debugEspHardwareBegin();
    SLAVE_LOG_PRINTF("[SCOPE] sample pulse pin=%d idle=%d us=%d\n",
                     SAMPLE_PULSE_PIN, SAMPLE_PULSE_IDLE, SAMPLE_PULSE_US);
    SLAVE_LOG_PRINTF("[SCOPE] debugHardware=%d start pin=%d idle=%d us=%d\n",
                     DEBUG_HARDWARE, DEBUG_HW_START_PIN,
                     DEBUG_HW_START_IDLE, DEBUG_HW_START_US);

#if defined(ESP8266)
    /* Modo AP oculto en canal 1: evita el background scanning que hace el
     * ESP8266 en modo STA desconectado (recorre ch1-13 buscando APs y pierde
     * paquetes ESP-NOW del canal 1 a distancia real). */
    WiFi.mode(WIFI_AP);
    WiFi.softAP("GeoSlave", "", 1, 1);   /* ssid, pass vacío, canal 1, oculto */
    delay(100);
    SLAVE_LOG_PRINTF("[SLAVE] MAC: %s  ch=%d (AP-fixed)\n",
                     WiFi.macAddress().c_str(), wifi_get_channel());
#else
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    /* Fijar canal 1 explícitamente para coincidir con el AP del maestro.
     * Sin esto, STA desconectado puede quedar en canal 0 (indefinido) y los
     * paquetes ESP-NOW se pierden porque maestro y esclavo están en canales distintos. */
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    SLAVE_LOG_PRINTF("[SLAVE] MAC: %s  ch=%d\n",
                     WiFi.macAddress().c_str(), WiFi.channel());
#endif

    /* ESP-NOW */
    if (!transport.begin(MASTER_MAC)) {
        SLAVE_LOG_PRINTLN("[SLAVE] ESP-NOW FAIL - halt");
        while (true) { delay(1000); }
    }
    esp_now_register_recv_cb(onDataRecv);

    /* LED de identificación */
    pinMode(BLINK_LED_PIN, OUTPUT);
    digitalWrite(BLINK_LED_PIN, BLINK_LED_OFF_LEVEL);
    SLAVE_LOG_PRINTF("[SLAVE] blink LED pin=%d active_low=%u on=%d off=%d\n",
                     BLINK_LED_PIN, (unsigned)BLINK_LED_ACTIVE_LOW,
                     BLINK_LED_ON_LEVEL, BLINK_LED_OFF_LEVEL);

    /* GPIO hardware sync — pull-down para evitar ISR espurias cuando no hay cable */
    pinMode(SYNC_IN_PIN,      INPUT_PULLDOWN);
    pinMode(SYNC_TO_PSOC_PIN, OUTPUT);
    digitalWrite(SYNC_TO_PSOC_PIN, LOW);
    attachInterrupt(digitalPinToInterrupt(SYNC_IN_PIN), onSyncEdge, CHANGE);

    /* UART → PSoC */
    psoc.begin(onBatch);
    psoc.onDiag(onPsocDiag);
    g_psoc_rx_last_level = (uint8_t)digitalRead(PSOC_UART_RX);
    attachInterrupt(digitalPinToInterrupt(PSOC_UART_RX), onPsocRxEdge, CHANGE);
#if PSOC_RX_SCAN_ENABLE
    beginPsocRxScan();
#endif

    /* Intentar detectar el PSoC una vez. Si no responde, se reintenta cada 2 s
     * en loop() para no bloquear ESP-NOW ni la generación de batches debug. */
    SLAVE_LOG_PRINTF("[SLAVE %d] Buscando PSoC...\n", NODE_ID);
    g_psocConnected = psoc.probe(300);
    if (g_psocConnected) {
        SLAVE_LOG_PRINTF("[SLAVE %d] PSoC: DETECTADO\n", NODE_ID);
        LOGM("PSOC", "detected=1");
        scheduleAutoCalibration(PSOC_AUTO_CAL_DELAY_MS);
    } else {
        SLAVE_LOG_PRINTF("[SLAVE %d] PSoC sin respuesta — reintentando en loop\n", NODE_ID);
        LOGM("PSOC", "detected=0,retry=loop");
    }

    SLAVE_LOG_PRINTF("[SLAVE %d] listo, esperando ARM\n", NODE_ID);
    LOGM("BOOT", "node=%d,psoc=%d", NODE_ID, (int)g_psocConnected);
}

/* ── Loop ────────────────────────────────────────────────────────────────── */

void loop()
{
    if (g_state != SAMPLING) {
        debugEspHardwareService();
    }

    /* Drenar siempre la UART del PSoC (onBatch ignora si no está SAMPLING). */
    if (g_state == SAMPLING && g_debug_mode) {
        /* debug ESP: generar batch falso a 29412 µs (30 muestras @ 1020 Hz) */
        uint32_t nowUs = (uint32_t)micros();
        if ((uint32_t)(nowUs - g_debug_last_us) >= 29412) {
            g_debug_last_us += 29412;
            PsocBatch fake = {};
            fake.seq         = (uint16_t)(g_debug_count / SPI_BATCH_SAMPLES);
            fake.n_samples   = SPI_BATCH_SAMPLES;
            fake.timestamp_us= (uint64_t)micros() - g_t_start_us;
            for (int i = 0; i < SPI_BATCH_SAMPLES; i++) {
                fake.samples[i].post_digital = (int32_t)(g_debug_count & 0xFFFFFF);
                fake.samples[i].gain_byte = g_pga_code;
                fake.samples[i].sample_flags = 0;
                g_debug_count++;
            }
            onBatch(fake);
        }
    } else {
        psoc.poll();
    }
    servicePsocConfigAck();
    serviceUsbCommands();
    serviceAutoCalibration();

    /* LED blink para identificación: g_blink_count > 0 → toggle cada BLINK_INTERVAL_MS */
    if (g_blink_count > 0 && (millis() - g_blink_last_ms) >= BLINK_INTERVAL_MS) {
        g_blink_last_ms = millis();
        digitalWrite(BLINK_LED_PIN, (g_blink_count % 2u) ? BLINK_LED_OFF_LEVEL : BLINK_LED_ON_LEVEL);
        g_blink_count--;
        if (g_blink_count == 0) {
            digitalWrite(BLINK_LED_PIN, BLINK_LED_OFF_LEVEL);
        }
    }

    /* Mientras se llena el store (Ver/Test con n_batches>0), imprimir UART cada 1 s.
     * Esto separa cable/pin (uartBytes=0) de protocolo/baud (bytes sin frames)
     * de "onBatch no incrementa fill" (psoc.poll() no entrega PsocBatch). */
#if SLAVE_LOG_VIEW_UART
    static uint32_t lastViewDiagMs = 0;
    if (g_state == SAMPLING && g_rec_n_batches > 0 &&
        (millis() - lastViewDiagMs) >= 1000) {
        lastViewDiagMs = millis();
        logPsocUartDiag("VIEW_UART");
    }
#endif

    if (g_state == SAMPLING && g_view_store_active && g_rec_n_batches > 0 &&
        g_store_fill == 0 && !g_view_start_fallback_sent) {
        uint32_t elapsedMs = millis() - g_sampling_start_ms;
        uint32_t fallbackMs = expectedViewCaptureMs(g_rec_n_batches);
        uint32_t bytesNow = psoc.bytesRx();
        uint32_t batchesNow = psoc.batchesOK();
        if (elapsedMs >= fallbackMs && batchesNow == g_sampling_start_batches_ok) {
            g_view_start_fallback_sent = true;
            SLAVE_LOG_PRINTF("[SLAVE] VIEW_SYNC_STALL elapsed=%lu/%lu sync=%d bytes=%lu->%lu bOK=%lu->%lu fill=%u/%u -> START_NOW UART\n",
                             (unsigned long)elapsedMs, (unsigned long)fallbackMs,
                             digitalRead(SYNC_TO_PSOC_PIN),
                             (unsigned long)g_sampling_start_psoc_bytes,
                             (unsigned long)bytesNow,
                             (unsigned long)g_sampling_start_batches_ok,
                             (unsigned long)batchesNow,
                             (unsigned)g_store_fill, (unsigned)g_rec_n_batches);
            LOGM("VIEW_SYNC_STALL", "ms=%lu/%lu,sync=%d,bytes=%lu->%lu,bOK=%lu->%lu,fill=%u/%u",
                 (unsigned long)elapsedMs, (unsigned long)fallbackMs,
                 digitalRead(SYNC_TO_PSOC_PIN),
                 (unsigned long)g_sampling_start_psoc_bytes,
                 (unsigned long)bytesNow,
                 (unsigned long)g_sampling_start_batches_ok,
                 (unsigned long)batchesNow,
                 (unsigned)g_store_fill, (unsigned)g_rec_n_batches);
            psoc.startNow();
        }
    }

    /* Volver a esperar ARM después de STOP.
     * En modo store-and-forward (g_rec_n_batches > 0) permanecemos en STOPPED
     * respondiendo CMD_REQ_BATCH hasta que el maestro haya pedido todos los batches.
     * El maestro no envía ningún mensaje de "fin" — el slave sencillamente vuelve
     * a WAIT_ARM cuando recibe un CMD_ARM nuevo. */
    if (g_state == STOPPED && g_rec_n_batches == 0) {
        g_state = WAIT_ARM;
        SLAVE_LOG_PRINTF("[SLAVE %d] volviendo a WAIT_ARM\n", NODE_ID);
    }

    /* Reintento no-bloqueante de detección del PSoC — cada 2 s, fuera de captura activa */
    static uint32_t lastProbeMs = 0;
    if (!g_psocConnected && g_state != SAMPLING && g_state != HOT_WAIT &&
        g_view_remaining == 0 && (millis() - lastProbeMs) >= 2000) {
        lastProbeMs = millis();
        g_psocConnected = psoc.probe(300);
        if (g_psocConnected) {
            SLAVE_LOG_PRINTF("[SLAVE %d] PSoC: DETECTADO\n", NODE_ID);
            LOGM("PSOC", "detected=1");
            scheduleAutoCalibration(PSOC_AUTO_CAL_DELAY_MS);
        }
    }

    /* HELLO/Fs cada 2 s mientras no haya captura ni dump pendiente.
     * Si el maestro se reinicia con los esclavos ya ARMED, necesita recuperar
     * este dato sin obligar al operador a cargar Fs manualmente en la web. */
    static uint32_t lastHelloMs = 0;
    if (g_view_remaining == 0 &&
        g_state != SAMPLING &&
        g_state != HOT_WAIT &&
        !(g_state == STOPPED && g_rec_n_batches > 0) &&
        millis() - lastHelloMs >= 2000) {
        lastHelloMs = millis();
        MsgHello h = { CMD_HELLO, NODE_ID, (uint8_t)g_psocConnected, effectivePsocSampleRateHz() };
        esp_err_t err = espnowSend(MASTER_MAC, (const uint8_t *)&h, sizeof(h));
#if SLAVE_LOG_HELLO_TX
        SLAVE_LOG_PRINTF("[SLAVE] HELLO tx err=%d txOK=%u txFail=%u\n",
                         (int)err, transport.sentOK(), transport.sentFail());
#else
        (void)err;
#endif
    }

    /* Informe de estado cada 10 s */
    static uint32_t last_status = 0;
    if (g_state != HOT_WAIT && g_state != SAMPLING &&
        millis() - last_status > 10000) {
        last_status = millis();
        MsgStatus st = {
            CMD_STATUS, NODE_ID,
            psoc.batchesOK(), psoc.batchesBad(),
            transport.sentOK(), transport.sentFail(),
            (uint8_t)g_psocConnected
        };
        espnowSend(MASTER_MAC, (const uint8_t *)&st, sizeof(st));
#if SLAVE_LOG_STATUS_PERIODIC
        logPsocUartDiag("STATUS");
#if PSOC_RX_SCAN_ENABLE
        logPsocRxScan();
#endif
#endif
    }
}
