/*
 * ESP Esclavo — Geofono node
 *
 * Lee muestras del PSoC via UART (raw) y las envía al maestro via ESP-NOW.
 *
 * Configurar en platformio.ini:
 *   build_flags = -DNODE_ID=1        (1 o 2 según el nodo físico)
 *
 * Configurar MAC del maestro en MASTER_MAC[].
 *
 * Estados:
 *   WAIT_ARM  → espera CMD_ARM del maestro
 *   ARMED     → envia CMD_ARM_ACK, espera PRESTART
 *   HOT_WAIT  → PRESTART recibido; no hace nada salvo esperar START/consulta
 *   SAMPLING  → lee PSoC y envía batches
 *   STOPPED   → vuelve a WAIT_ARM al recibir CMD_STOP
 */

#include <Arduino.h>
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
  #if defined(ESP8266)
    #define SAMPLE_PULSE_PIN 2
  #else
    #define SAMPLE_PULSE_PIN 14
  #endif
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
/* Logging (humano + máquina) en debug_log.h, gateado por DBG_ENABLE. */

/* MAC del ESP maestro — cambiar según el hardware */
static const uint8_t MASTER_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* ── Pines GPIO hardware sync ─────────────────────────────────────────────── */
#if defined(ESP8266)
  #define SYNC_IN_PIN      5    /* NodeMCU/D1 mini D1 */
  #define SYNC_TO_PSOC_PIN 16   /* NodeMCU/D1 mini D0 */
#else
  #define SYNC_IN_PIN      26   /* Entrada: flanco del maestro (level: HIGH=ON, LOW=OFF) */
  #define SYNC_TO_PSOC_PIN 27   /* Salida hacia PSoC SYNC_IN */
#endif

/* ── Estado ──────────────────────────────────────────────────────────────── */
enum SlaveState { WAIT_ARM, ARMED, HOT_WAIT, SAMPLING, STOPPED };
static volatile SlaveState g_state       = WAIT_ARM;
static volatile uint64_t   g_t_start_us  = 0;
static volatile bool       g_debug_mode  = false;
static          uint32_t   g_debug_count = 0;
static volatile uint32_t   g_debug_last_us = 0;
static volatile bool       g_debugHwActive = false;
static volatile uint32_t   g_debugHwFallUs = 0;
static          uint8_t    g_pga_code    = 0;
static          uint8_t    g_pgavdac     = 0;
static          uint8_t    g_vdac_byte   = 128;

/* ── Store-and-forward ───────────────────────────────────────────────────── */
static uint16_t    g_rec_n_batches = 0;          /* 0 = modo streaming clásico */
static SampleBytes *g_store_buf    = nullptr;    /* n_batches × 30 SampleBytes */
static uint32_t   *g_store_ts_us   = nullptr;    /* timestamp relativo por batch */
static uint16_t    g_store_fill    = 0;

/* ── Modo "Ver" (disparo único, streaming en vivo de un nodo) ────────────── */
static volatile uint16_t g_view_remaining = 0;   /* lotes en vivo restantes (0=off) */

/* ── Objetos ─────────────────────────────────────────────────────────────── */
static PsocUART       psoc;
static EspNowTransport transport;

static inline uint8_t samplePulseActiveLevel()
{
    return (SAMPLE_PULSE_IDLE == LOW) ? HIGH : LOW;
}

static inline uint8_t debugHwActiveLevel()
{
    return (DEBUG_HW_START_IDLE == LOW) ? HIGH : LOW;
}

static void samplePulseBegin()
{
#if SAMPLE_PULSE_PIN >= 0
    pinMode(SAMPLE_PULSE_PIN, OUTPUT);
    digitalWrite(SAMPLE_PULSE_PIN, SAMPLE_PULSE_IDLE);
#endif
}

static void debugEspHardwareBegin()
{
#if DEBUG_HARDWARE && DEBUG_HW_START_PIN >= 0
    pinMode(DEBUG_HW_START_PIN, OUTPUT);
    digitalWrite(DEBUG_HW_START_PIN, DEBUG_HW_START_IDLE);
#endif
}

static inline void samplePulse()
{
#if SAMPLE_PULSE_PIN >= 0
    digitalWrite(SAMPLE_PULSE_PIN, samplePulseActiveLevel());
#if SAMPLE_PULSE_US > 0
    delayMicroseconds(SAMPLE_PULSE_US);
#endif
    digitalWrite(SAMPLE_PULSE_PIN, SAMPLE_PULSE_IDLE);
#endif
}

static inline void debugEspHardwareStartPulse()
{
#if DEBUG_HARDWARE && DEBUG_HW_START_PIN >= 0
    digitalWrite(DEBUG_HW_START_PIN, debugHwActiveLevel());
#if DEBUG_HW_START_US > 0
    g_debugHwFallUs = (uint32_t)micros() + (uint32_t)DEBUG_HW_START_US;
    g_debugHwActive = true;
#else
    digitalWrite(DEBUG_HW_START_PIN, DEBUG_HW_START_IDLE);
    g_debugHwActive = false;
#endif
#endif
}

static inline void debugEspHardwareService()
{
#if DEBUG_HARDWARE && DEBUG_HW_START_PIN >= 0
    if (g_debugHwActive &&
        (int32_t)((uint32_t)micros() - g_debugHwFallUs) >= 0) {
        digitalWrite(DEBUG_HW_START_PIN, DEBUG_HW_START_IDLE);
        g_debugHwActive = false;
    }
#endif
}

static void pulseSampleCount(uint8_t n)
{
    for (uint8_t i = 0; i < n; i++) {
        samplePulse();
    }
}

/* ── ISR GPIO hardware sync (IRAM para máxima velocidad) ────────────────── */
void IRAM_ATTR onSyncEdge()
{
    int level = digitalRead(SYNC_IN_PIN);
    if (level == HIGH && g_state == HOT_WAIT) {
        digitalWrite(SYNC_TO_PSOC_PIN, HIGH);
        debugEspHardwareStartPulse();   /* pulso inmediato: camino HW más rápido que ESP-NOW */
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

static void sendStartAck(uint8_t status, uint32_t startToken, uint32_t rxUs)
{
    MsgStartAck ack = { CMD_START_ACK, NODE_ID, status, startToken, rxUs };
    espnowSend(MASTER_MAC, (const uint8_t *)&ack, sizeof(ack));
}

static bool storeReadyForHotWait()
{
    return (g_rec_n_batches == 0) || (g_store_buf != nullptr && g_store_ts_us != nullptr);
}

static void enterHotWait(uint16_t n_batches)
{
    allocStore(n_batches);
    digitalWrite(SYNC_TO_PSOC_PIN, LOW);
    g_t_start_us     = 0;
    g_store_fill     = 0;
    g_view_remaining = 0;
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

    uint8_t ok = 1;
    switch (cfg->sub_cmd) {
        case 0xA6:                       /* PGA */
            g_pga_code = cfg->param;
            psoc.setPga(cfg->param);
            break;
        case 0xA9:                       /* PGAvdac */
            g_pgavdac = cfg->param;
            psoc.setPgavdac(cfg->param);
            break;
        case 0xAA:                       /* VDAC (calibración) */
            g_vdac_byte = cfg->param;
            psoc.setVdac(cfg->param);
            break;
        default:
            ok = 0;
            break;
    }
    sendCfgAck(cfg->sub_cmd, ok);
    SLAVE_LOG_PRINTF("[SLAVE] cfg sub=0x%02X p=%u ok=%u\n",
                     cfg->sub_cmd, cfg->param, ok);
    LOGM("CFG", "sub=0x%02X,p=%u,ok=%u", cfg->sub_cmd, cfg->param, ok);
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
    LOGM("RX", "cmd=0x%02X,len=%d,state=%d", cmd, len, (int)g_state);

    if (cmd == CMD_ARM &&
        (g_state == WAIT_ARM || g_state == ARMED || g_state == HOT_WAIT || g_state == STOPPED)) {
        g_state = ARMED;
        MsgArmAck ack = { CMD_ARM_ACK, NODE_ID, 0 };
        espnowSend(MASTER_MAC, (const uint8_t *)&ack, sizeof(ack));
        SLAVE_LOG_PRINTLN("[SLAVE] ARMED");
    }
    else if (cmd == CMD_PRESTART && len >= (int)sizeof(MsgPrestart)) {
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
        debugEspHardwareStartPulse();   /* START real por ESP-NOW */
        digitalWrite(SYNC_TO_PSOC_PIN, HIGH);
        /* START por software: usar como fallback si no hay cable GPIO */
        g_t_start_us    = msg->t_start_us;
        g_store_fill    = 0;   /* reset store index para nueva grabación */
        g_debug_count   = 0;
        g_debug_last_us = nowUs;
        g_state = SAMPLING;
        sendStartAck(1, startToken, nowUs);
        SLAVE_LOG_PRINTF("[SLAVE] START(SW) t0=%llu\n", (unsigned long long)g_t_start_us);
    }
    else if (cmd == CMD_STOP) {
        digitalWrite(SYNC_TO_PSOC_PIN, LOW);
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
        /* "Ver": disparo único, streaming en vivo de un solo nodo. */
        const MsgView *msg = (const MsgView *)data;
        if (msg->node_id != NODE_ID) return;
        allocStore(0);                 /* sin store-and-forward → envío en vivo */
        g_view_remaining = msg->n;
        g_t_start_us     = (uint64_t)micros();
        g_store_fill     = 0;
        g_state          = SAMPLING;
        psoc.setN(msg->n);
        psoc.captureNow();             /* PSoC arranca solo, sin SYNC */
        sendCfgAck(CMD_VIEW, 1);
        SLAVE_LOG_PRINTF("[SLAVE] VIEW n=%u\n", msg->n);
        LOGM("VIEW", "n=%u", msg->n);
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
    pulseSampleCount(batch.n_samples);

    /* Debug y real: el routing depende del modo store, no del origen del dato.
       Si g_store_buf está allocado → acumula (también en debug).
       Si no hay store buffer → transmite en tiempo real (streaming / TestEsclavo). */
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
        }
    } else if (!g_store_buf) {
        /* Sin store: enviar en tiempo real (streaming / "Ver") */
        transport.sendBatch(batch, g_t_start_us, NODE_ID);
        LOGM("BATCH", "seq=%u,live=1", batch.seq);
        if (g_view_remaining > 0) {
            /* "Ver" es disparo único: tras N lotes volvemos a reposo. */
            if (--g_view_remaining == 0) {
                g_state = STOPPED;
                SLAVE_LOG_PRINTF("[SLAVE] VIEW done\n");
                LOGM("VIEWDONE", "");
            }
        }
    }
}

/* ── Setup ───────────────────────────────────────────────────────────────── */

void setup()
{
    dbgLogBegin(115200);   /* Serial USB del esclavo = canal de log (humano+máquina) */
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

    SLAVE_LOG_PRINTF("[SLAVE %d] listo, esperando ARM\n", NODE_ID);
    LOGM("BOOT", "node=%d", NODE_ID);
}

/* ── Loop ────────────────────────────────────────────────────────────────── */

void loop()
{
    debugEspHardwareService();

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

    /* Volver a esperar ARM después de STOP.
     * En modo store-and-forward (g_rec_n_batches > 0) permanecemos en STOPPED
     * respondiendo CMD_REQ_BATCH hasta que el maestro haya pedido todos los batches.
     * El maestro no envía ningún mensaje de "fin" — el slave sencillamente vuelve
     * a WAIT_ARM cuando recibe un CMD_ARM nuevo. */
    if (g_state == STOPPED && g_rec_n_batches == 0) {
        g_state = WAIT_ARM;
        SLAVE_LOG_PRINTF("[SLAVE %d] volviendo a WAIT_ARM\n", NODE_ID);
    }

    /* HELLO beacon cada 2 s en WAIT_ARM — diagnóstico de ESP-NOW bidireccional */
    static uint32_t lastHelloMs = 0;
    if (g_state == WAIT_ARM && millis() - lastHelloMs >= 2000) {
        lastHelloMs = millis();
        MsgHello h = { CMD_HELLO, NODE_ID };
        esp_err_t err = espnowSend(MASTER_MAC, (const uint8_t *)&h, sizeof(h));
        SLAVE_LOG_PRINTF("[SLAVE] HELLO tx err=%d txOK=%u txFail=%u\n",
                         (int)err, transport.sentOK(), transport.sentFail());
    }

    /* Informe de estado cada 10 s */
    static uint32_t last_status = 0;
    if (g_state != HOT_WAIT && millis() - last_status > 10000) {
        last_status = millis();
        MsgStatus st = {
            CMD_STATUS, NODE_ID,
            psoc.batchesOK(), psoc.batchesBad(),
            transport.sentOK(), transport.sentFail()
        };
        espnowSend(MASTER_MAC, (const uint8_t *)&st, sizeof(st));
        SLAVE_LOG_PRINTF("[SLAVE %d] bOK=%u bBad=%u txOK=%u txFail=%u state=%d\n",
                         NODE_ID,
                         psoc.batchesOK(), psoc.batchesBad(),
                         transport.sentOK(), transport.sentFail(),
                         (int)g_state);
    }
}
