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
/* Logging (humano + máquina) en debug_log.h, gateado por DBG_ENABLE. */

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
static          bool       g_cfg_waiting = false;
static          uint8_t    g_cfg_sub_cmd = 0;
static          uint8_t    g_cfg_param   = 0;
static          uint32_t   g_cfg_due_ms  = 0;

/* ── Store-and-forward ───────────────────────────────────────────────────── */
static uint16_t    g_rec_n_batches = 0;          /* 0 = modo streaming clásico */
static SampleBytes *g_store_buf    = nullptr;    /* n_batches × 30 SampleBytes */
static uint32_t   *g_store_ts_us   = nullptr;    /* timestamp relativo por batch */
static uint16_t    g_store_fill    = 0;

/* ── Modo "Ver" (disparo único, store primero y dump después) ────────────── */
static volatile uint16_t g_view_remaining    = 0; /* legado live deshabilitado */
static volatile bool     g_view_store_active = false;

#define PSOC_CFG_ACK_TIMEOUT_MS 250u

/* ── Objetos ─────────────────────────────────────────────────────────────── */
static PsocUART       psoc;
static EspNowTransport transport;

/* Helpers de pulsos GPIO de osciloscopio (módulo aparte). */
#include "scope_pulse.h"

static void logPsocUartDiag(const char *tag)
{
    const unsigned long bytes = (unsigned long)psoc.bytesRx();
    const unsigned long mark  = (unsigned long)psoc.markersRx();
    const unsigned long drop  = (unsigned long)psoc.syncDrops();
    const unsigned long badLn = (unsigned long)psoc.badLen();
    const unsigned long ping  = (unsigned long)psoc.pingsRx();
    const int rxLevel = digitalRead(PSOC_UART_RX);

    if (psoc.hasLastByte()) {
        SLAVE_LOG_PRINTF(
            "[SLAVE %d] %s bOK=%lu bBad=%lu uartBytes=%lu mark=%lu drop=%lu badLen=%lu ping=%lu rx=%d last=0x%02X age=%lu txOK=%lu txFail=%lu state=%d view=%u\n",
            NODE_ID, tag,
            (unsigned long)psoc.batchesOK(), (unsigned long)psoc.batchesBad(),
            bytes, mark, drop, badLn, ping, rxLevel,
            psoc.lastByte(), (unsigned long)psoc.lastByteAgeMs(),
            (unsigned long)transport.sentOK(), (unsigned long)transport.sentFail(),
            (int)g_state, (unsigned)g_view_remaining);
    } else {
        SLAVE_LOG_PRINTF(
            "[SLAVE %d] %s bOK=%lu bBad=%lu uartBytes=%lu mark=%lu drop=%lu badLen=%lu ping=%lu rx=%d last=none txOK=%lu txFail=%lu state=%d view=%u\n",
            NODE_ID, tag,
            (unsigned long)psoc.batchesOK(), (unsigned long)psoc.batchesBad(),
            bytes, mark, drop, badLn, ping, rxLevel,
            (unsigned long)transport.sentOK(), (unsigned long)transport.sentFail(),
            (int)g_state, (unsigned)g_view_remaining);
    }
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

static void waitForPsocConfigAck(uint8_t sub_cmd, uint8_t param)
{
    g_cfg_waiting = true;
    g_cfg_sub_cmd = sub_cmd;
    g_cfg_param = param;
    g_cfg_due_ms = millis() + PSOC_CFG_ACK_TIMEOUT_MS;
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
            const uint8_t ok = (ackVal == g_cfg_param) ? 1u : 0u;
            if (ok) {
                applyConfirmedConfig(ackCmd, ackVal);
            }
            sendCfgAck(g_cfg_sub_cmd, ok);
            SLAVE_LOG_PRINTF("[SLAVE] PSoC cfg ack sub=0x%02X val=%u expected=%u ok=%u\n",
                             ackCmd, ackVal, g_cfg_param, ok);
            LOGM("CFG_ACK", "sub=0x%02X,val=%u,expected=%u,ok=%u",
                 ackCmd, ackVal, g_cfg_param, ok);
            g_cfg_waiting = false;
        } else {
            applyConfirmedConfig(ackCmd, ackVal);
            SLAVE_LOG_PRINTF("[SLAVE] PSoC cfg ack unsolicited sub=0x%02X val=%u\n",
                             ackCmd, ackVal);
            LOGM("CFG_ACK_UNSOL", "sub=0x%02X,val=%u", ackCmd, ackVal);
        }
    }

    if (g_cfg_waiting && (int32_t)(millis() - g_cfg_due_ms) >= 0) {
        sendCfgAck(g_cfg_sub_cmd, 0);
        SLAVE_LOG_PRINTF("[SLAVE] PSoC cfg ack timeout sub=0x%02X expected=%u\n",
                         g_cfg_sub_cmd, g_cfg_param);
        LOGM("CFG_TIMEOUT", "sub=0x%02X,expected=%u", g_cfg_sub_cmd, g_cfg_param);
        g_cfg_waiting = false;
    }
}

static void sendStartAck(uint8_t status, uint32_t startToken, uint32_t rxUs)
{
    MsgStartAck ack = { CMD_START_ACK, NODE_ID, status, startToken, rxUs };
    espnowSend(MASTER_MAC, (const uint8_t *)&ack, sizeof(ack));
}

static bool storeReadyForHotWait()
{
    return (g_rec_n_batches == 0) || (g_store_buf != nullptr && g_store_ts_us != nullptr);
}

static uint16_t clampPsocCaptureBatches(uint16_t n)
{
    if (n > PSOC_CAPTURE_MAX_BATCHES) {
        return (uint16_t)PSOC_CAPTURE_MAX_BATCHES;
    }
    return n;
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
    g_state = HOT_WAIT;
    /* Armar el PSoC por UART: set N y pre-start (queda esperando flanco SYNC). */
    psoc.setN(n_batches);
    psoc.preStart();
    SLAVE_LOG_PRINTF("[SLAVE] HOT_WAIT n=%u ready=%u\n",
                     n_batches, (unsigned)storeReadyForHotWait());
    LOGM("HOTWAIT", "n=%u,ready=%u", n_batches, (unsigned)storeReadyForHotWait());
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

    uint8_t ok = 1;
    bool waitAck = false;
    switch (cfg->sub_cmd) {
        case 0xA6:                       /* PGA */
            if (!isGainCode(cfg->param)) {
                ok = 0;
            } else {
                psoc.setPga(cfg->param);
                waitAck = true;
            }
            break;
        case 0xA9:                       /* PGAvdac */
            if (!isGainCode(cfg->param)) {
                ok = 0;
            } else {
                psoc.setPgavdac(cfg->param);
                waitAck = true;
            }
            break;
        case 0xAA:                       /* VDAC (calibración) */
            psoc.setVdac(cfg->param);
            waitAck = true;
            break;
        default:
            ok = 0;
            break;
    }
    if (ok && waitAck) {
        waitForPsocConfigAck(cfg->sub_cmd, cfg->param);
    } else {
        sendCfgAck(cfg->sub_cmd, ok);
    }
    SLAVE_LOG_PRINTF("[SLAVE] cfg sub=0x%02X p=%u ok=%u\n",
                     cfg->sub_cmd, cfg->param, ok);
    LOGM("CFG", "sub=0x%02X,p=%u,ok=%u,wait=%u",
         cfg->sub_cmd, cfg->param, ok, (unsigned)waitAck);
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
        MsgArmAck ack = { CMD_ARM_ACK, NODE_ID, 0 };
        espnowSend(MASTER_MAC, (const uint8_t *)&ack, sizeof(ack));
        SLAVE_LOG_PRINTLN("[SLAVE] ARMED");
    }
    else if (cmd == CMD_PRESTART && len >= (int)sizeof(MsgPrestart) &&
             g_state != SAMPLING) {
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
        sendStartAck(1, startToken, nowUs);
        digitalWrite(SYNC_TO_PSOC_PIN, HIGH);
        /* START por software: usar como fallback si no hay cable GPIO */
        g_t_start_us    = msg->t_start_us;
        g_store_fill    = 0;   /* reset store index para nueva grabación */
        g_debug_count   = 0;
        g_debug_last_us = nowUs;
        g_state = SAMPLING;
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
}

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

    /* GPIO hardware sync — pull-down para evitar ISR espurias cuando no hay cable */
    pinMode(SYNC_IN_PIN,      INPUT_PULLDOWN);
    pinMode(SYNC_TO_PSOC_PIN, OUTPUT);
    digitalWrite(SYNC_TO_PSOC_PIN, LOW);
    attachInterrupt(digitalPinToInterrupt(SYNC_IN_PIN), onSyncEdge, CHANGE);

    /* UART → PSoC */
    psoc.begin(onBatch);

    /* Intentar detectar el PSoC una vez. Si no responde, se reintenta cada 2 s
     * en loop() para no bloquear ESP-NOW ni la generación de batches debug. */
    SLAVE_LOG_PRINTF("[SLAVE %d] Buscando PSoC...\n", NODE_ID);
    g_psocConnected = psoc.probe(300);
    if (g_psocConnected) {
        SLAVE_LOG_PRINTF("[SLAVE %d] PSoC: DETECTADO\n", NODE_ID);
        LOGM("PSOC", "detected=1");
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

    /* Mientras "Ver" espera batches, imprimir UART cada 1 s.
     * Esto separa cable/pin (uartBytes=0) de protocolo/baud (bytes sin frames). */
    static uint32_t lastViewDiagMs = 0;
    if (g_state == SAMPLING && g_view_remaining > 0 &&
        (millis() - lastViewDiagMs) >= 1000) {
        lastViewDiagMs = millis();
        logPsocUartDiag("VIEW_UART");
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
        }
    }

    /* HELLO beacon cada 2 s en WAIT_ARM — diagnóstico de ESP-NOW bidireccional */
    static uint32_t lastHelloMs = 0;
    if (g_state == WAIT_ARM && millis() - lastHelloMs >= 2000) {
        lastHelloMs = millis();
        MsgHello h = { CMD_HELLO, NODE_ID, (uint8_t)g_psocConnected };
        esp_err_t err = espnowSend(MASTER_MAC, (const uint8_t *)&h, sizeof(h));
        SLAVE_LOG_PRINTF("[SLAVE] HELLO tx err=%d txOK=%u txFail=%u\n",
                         (int)err, transport.sentOK(), transport.sentFail());
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
        logPsocUartDiag("STATUS");
    }
}
