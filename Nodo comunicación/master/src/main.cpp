/*
 * ESP Maestro — Gateway geófono WiFi
 *
 * Roles:
 *   1. WiFi AP ("GeoNetwork") para que MATLAB y esclavos se conecten.
 *   2. ESP-NOW: recibe batches de los esclavos, les envía ARM/START/STOP.
 *   3. USB serie hacia MATLAB (datos binarios 0x56) + Serial1 para logging.
 *
 * El maestro ya NO muestrea el martillo: el martillo es un esclavo normal.
 *
 * Flujo de sincronización:
 *   MATLAB → cmd 0xA2 (ARM)   → maestro broadcast CMD_ARM a esclavos
 *   Esclavos responden CMD_ARM_ACK
 *   MATLAB → cmd 0xA3 (START) → maestro manda PRESTART, verifica HOT_WAIT
 *   y recién entonces emite CMD_START (por ESP-NOW; el pin SYNC_OUT es solo
 *   marcador de osciloscopio y NO llega a los esclavos).
 *   MATLAB → cmd 0xA4 (STOP)  → maestro broadcast CMD_STOP
 *
 * Flags en platformio.ini:
 *   -DNUM_SLAVES=3            cuántos esclavos soportar como máximo
 *   -DDEBUG_HARDWARE=1        pulso GPIO de scope al emitir CMD_START
 *   -DDBG_ENABLE=1            logging (humano+máquina) por Serial1
 */

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "sync_protocol.h"
#include "espnow_rx.h"
#include "matlab_transport.h"
#include "master_log.h"
#include "debug_log.h"
#include "web_server.h"
#include "../../scope_measurement.h"

/* ── Configuración ────────────────────────────────────────────────────────── */
static const char *AP_SSID = "GeoNetwork";
static const char *AP_PASS = "geophone2026";
static DNSServer dnsServer;
static const uint8_t ESPNOW_BROADCAST[6] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};
#define MATLAB_CMD_SCOPE_START 0xB0

#ifndef NUM_SLAVES
  #define NUM_SLAVES 3
#endif
#ifndef SAMPLE_PULSE_PIN
  #define SAMPLE_PULSE_PIN 27
#endif
#ifndef SAMPLE_PULSE_IDLE
  #define SAMPLE_PULSE_IDLE LOW
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
#ifndef SCOPE_MULTI_START_COUNT
  #define SCOPE_MULTI_START_COUNT 128
#endif
#ifndef SCOPE_MULTI_START_GAP_MS
  #define SCOPE_MULTI_START_GAP_MS 1000
#endif
#ifndef HOTWAIT_QUERY_DELAY_MS
  #define HOTWAIT_QUERY_DELAY_MS 20
#endif
#ifndef HOTWAIT_QUERY_TIMEOUT_MS
  #define HOTWAIT_QUERY_TIMEOUT_MS 120
#endif
#ifndef HOTWAIT_QUERY_RETRIES
  #define HOTWAIT_QUERY_RETRIES 3
#endif
#ifndef HOTWAIT_SETTLE_MS
  #define HOTWAIT_SETTLE_MS 50
#endif
#ifndef ARM_RETRY_MS
  #define ARM_RETRY_MS 300
#endif
#ifndef ARM_TIMEOUT_MS
  #define ARM_TIMEOUT_MS 3000
#endif
#ifndef ADC_SAMPLE_RATE_HZ
  #define ADC_SAMPLE_RATE_HZ 3000
#endif
#ifndef STORE_CAPTURE_MARGIN_MS
  #define STORE_CAPTURE_MARGIN_MS 750
#endif
#ifndef STORE_AUTO_DUMP_MIN_MS
  #define STORE_AUTO_DUMP_MIN_MS 500
#endif
#ifndef DUMP_NACK_RETRY_DELAY_MS
  #define DUMP_NACK_RETRY_DELAY_MS 100
#endif
#ifndef PSOC_CAPTURE_MAX_BATCHES
  #define PSOC_CAPTURE_MAX_BATCHES 512
#endif
#ifndef ESPNOW_USE_UNICAST_TO_SLAVES
  #define ESPNOW_USE_UNICAST_TO_SLAVES 0
#endif

/* MACs de los esclavos — actualizar con las MACs reales */
static const uint8_t SLAVE_MACS[NUM_SLAVES][6] = {
    {0xC8, 0x2E, 0x18, 0x67, 0x68, 0x6C},   /* Esclavo 1 — GEO actual */
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},   /* Esclavo 2 — broadcast hasta medir MAC */
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},   /* Esclavo 3 — broadcast hasta medir MAC */
};

/* ── Pin GPIO de scope ────────────────────────────────────────────────────────
 * SYNC_OUT_PIN es SOLO marcador de osciloscopio: NO está cableado a los esclavos
 * (el START real viaja por ESP-NOW). Se compila a nada si DEBUG_HARDWARE=0. */
#define SYNC_OUT_PIN 25
#define LED_PIN      2    /* GPIO2 = LED azul integrado ESP32-DevKitC V4 */

#if DEBUG_HARDWARE
  static inline void syncOutBegin()        { pinMode(SYNC_OUT_PIN, OUTPUT); digitalWrite(SYNC_OUT_PIN, LOW); }
  static inline void syncOutWrite(uint8_t l){ digitalWrite(SYNC_OUT_PIN, l); }
#else
  static inline void syncOutBegin()        {}
  static inline void syncOutWrite(uint8_t) {}
#endif

/* ── Estado ──────────────────────────────────────────────────────────────── */
enum MasterState { IDLE, ARMING, ARMED, RUNNING, STOPPING, DUMPING, PRESTART, SCOPE_MULTI };
enum PrestartAction { PRESTART_ACTION_START, PRESTART_ACTION_SCOPE_MULTI, PRESTART_ACTION_START_PROBE, PRESTART_ACTION_VER };
static MasterState g_state       = IDLE;
static PrestartAction g_prestartAction = PRESTART_ACTION_START;
static uint8_t     g_armedCount  = 0;
static uint8_t     g_expectedSlaves = NUM_SLAVES;
static bool        g_streaming   = false;
static bool        g_espnowReady = false;
static bool        g_testMode    = false;   /* rampa en stub martillo durante 0xA7 */
static uint64_t    g_t_start_us  = 0;
static uint32_t    g_startCmdTxUs = 0;
static uint32_t    g_startCmdToken = 0;
static uint8_t     g_startProbeNode = 0;
static volatile uint32_t g_armAckMask    = 0;
/* g_debugHwActive / g_debugHwFallUs viven en scope_pulse.h */
static uint16_t          g_scopeStartSent = 0;
static uint32_t          g_scopeStartNextMs = 0;
static uint8_t           g_prestartProbeNode = 0;
static uint32_t          g_hotWaitAckMask = 0;
static uint8_t           g_hotWaitQueryNode = 1;
static uint8_t           g_hotWaitRetries = 0;
static uint8_t           g_hotWaitReadyCount = 0;
static bool              g_hotWaitQueryInFlight = false;
static bool              g_hotWaitSettling = false;
static uint32_t          g_hotWaitQueryMs = 0;
static uint32_t          g_hotWaitSettleDueMs = 0;

struct StartProbePending {
    bool active;
    uint8_t node;
    uint32_t token;
    uint32_t txUs;
};

#define START_PROBE_PENDING_SLOTS 16
static StartProbePending g_startProbePending[START_PROBE_PENDING_SLOTS] = {};
static uint8_t g_startProbePendingNext = 0;

/* ── Store-and-forward (dump request/response) ───────────────────────────── */
static uint16_t g_rec_n_batches   = 0;   /* batches a grabar por slave */
static uint8_t  g_dumpSlaveIdx    = 0;   /* esclavo siendo dumpeado (1-N) */
static uint16_t g_dumpBatchSeq    = 0;   /* batch actual siendo pedido */
static volatile bool     g_dumpBatchRx        = false;
static volatile bool     g_dumpBatchDelivered = false;  /* evita duplicados node/seq */
static volatile uint8_t  g_dumpDeliveredNode  = 0;
static volatile uint16_t g_dumpDeliveredSeq   = 0;
static uint8_t  g_dumpRetries     = 0;
static uint32_t g_dumpReqMs       = 0;   /* millis() del último REQ_BATCH enviado */
static bool     g_dumpRetryPaused = false;
static uint32_t g_dumpRetryDueMs  = 0;
static uint32_t g_captureDumpDueMs = 0;
#define DUMP_MAX_RETRIES  30
#define DUMP_BATCH_TIMEOUT_MS 300

static inline void dumpDeliveryGuardReset()
{
    g_dumpBatchDelivered = false;
    g_dumpDeliveredNode = 0;
    g_dumpDeliveredSeq = 0;
}

static inline void dumpDeliveryGuardMark(uint8_t nodeId, uint16_t seq)
{
    g_dumpDeliveredNode = nodeId;
    g_dumpDeliveredSeq = seq;
    g_dumpBatchDelivered = true;
}

/* ── "Ver" (un solo nodo, N batches, store-then-dump = sin RF durante muestreo) */
static uint8_t  g_viewNode = 0;
static uint16_t g_viewBatchesRemaining = 0;   /* conservado por compatibilidad; siempre 0 */
static bool     g_viewDump = false;            /* dump de un solo nodo tras VIEW */

/* ── Objetos ─────────────────────────────────────────────────────────────── */
static EspNowRx       espnowRx;
static MatlabTransport matlab;

struct CachedHello {
    bool valid = false;
    uint8_t psoc_ok = 0;
    uint16_t sample_rate = 0;
    uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
};
static CachedHello g_cachedHello[NUM_SLAVES + 1];
static volatile bool g_webReplayCachedState = false;

static void beginDump(const char *reason);
static void beginPrestart(PrestartAction action);
static void beacon_pause(void);
static void beacon_resume(void);

static uint32_t g_debugRamp     = 0;   /* (reservado para tests) */

/* Helpers de pulsos GPIO de osciloscopio (módulo aparte). */
#include "scope_pulse.h"

static bool sameMac(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, 6) == 0;
}

static bool isConfiguredSlaveMac(const uint8_t mac[6])
{
#if !ESPNOW_USE_UNICAST_TO_SLAVES
    (void)mac;
    return false;
#else
    static const uint8_t ZERO_MAC[6] = {0, 0, 0, 0, 0, 0};
    if (sameMac(mac, ZERO_MAC) || sameMac(mac, ESPNOW_BROADCAST)) return false;
    return !(mac[0] == 0xFF && mac[1] == 0xFF && mac[2] == 0xFF &&
             mac[3] == 0xFF && mac[4] == 0xFF);
#endif
}

static void addPeerIfNeeded(const uint8_t mac[6])
{
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
}

static uint8_t countArmAcks(uint32_t mask)
{
    uint8_t n = 0;
    for (uint8_t node = 1; node <= g_expectedSlaves; node++) {
        if (mask & (1UL << node)) n++;
    }
    return n;
}

static uint8_t countHotWaitAcks(uint32_t mask)
{
    uint8_t n = 0;
    for (uint8_t node = 1; node <= g_expectedSlaves; node++) {
        if (mask & (1UL << node)) n++;
    }
    return n;
}

static uint16_t clampRecordBatches(uint16_t n)
{
    if (n > PSOC_CAPTURE_MAX_BATCHES) {
        MASTER_LOG_PRINTF("[MASTER] n_batches %u > PSoC max %u, clamp\n",
                          n, (unsigned)PSOC_CAPTURE_MAX_BATCHES);
        return (uint16_t)PSOC_CAPTURE_MAX_BATCHES;
    }
    return n;
}

static void requestWebCachedStateReplay()
{
    g_webReplayCachedState = true;
}

static void sendCachedStateToWeb()
{
    matlab.sendHeartbeat(0x00, 0, 0, (uint8_t)g_state);
    matlab.sendReady(g_armedCount);
    for (uint8_t node = 1; node <= NUM_SLAVES; node++) {
        const CachedHello &h = g_cachedHello[node];
        if (h.valid) {
            matlab.sendHelloNotif(node, h.psoc_ok, h.mac, h.sample_rate);
        }
    }
    MASTER_LOG_PRINTLN("[WEB] cached READY/HELLO replay sent");
}

static MasterState armedOrIdleState()
{
    g_armedCount = countArmAcks(g_armAckMask);
    return (g_armedCount > 0) ? ARMED : IDLE;
}

static bool hotWaitAcked(uint8_t node)
{
    return (g_hotWaitAckMask & (1UL << node)) != 0;
}

static void rememberStartProbe(uint8_t node, uint32_t token, uint32_t txUs)
{
    StartProbePending &p = g_startProbePending[g_startProbePendingNext];
    p.active = true;
    p.node = node;
    p.token = token;
    p.txUs = txUs;
    g_startProbePendingNext =
        (uint8_t)((g_startProbePendingNext + 1u) % START_PROBE_PENDING_SLOTS);
}

static bool takeStartProbe(uint8_t node, uint32_t token, uint32_t &txUs)
{
    for (uint8_t i = 0; i < START_PROBE_PENDING_SLOTS; i++) {
        StartProbePending &p = g_startProbePending[i];
        if (p.active && p.node == node && p.token == token) {
            txUs = p.txUs;
            p.active = false;
            return true;
        }
    }
    return false;
}

static void onArmAck(const MsgArmAck &msg)
{
    if (msg.node_id == 0 || msg.node_id > NUM_SLAVES) return;
    if (msg.status == 0) {
        g_armAckMask |= (1UL << msg.node_id);
    }
}

static void onSlaveStatus(const MsgStatus &msg)
{
    MASTER_LOG_PRINTF("[MASTER] STATUS node=%d psoc=%d bOK=%u bBad=%u txOK=%u txFail=%u\n",
                      msg.node_id, msg.psoc_ok,
                      msg.batches_ok, msg.batches_bad,
                      msg.espnow_sent, msg.espnow_fail);
    LOGM("SLAVE_STATUS", "node=%d,psoc=%d,bOK=%u,bBad=%u,txOK=%u,txFail=%u",
         msg.node_id, msg.psoc_ok,
         msg.batches_ok, msg.batches_bad,
         msg.espnow_sent, msg.espnow_fail);
}

static void onCfgAck(const MsgCfgAck &msg)
{
    if (msg.sub_cmd == CMD_VIEW) {
        /* Diagnóstico: registra cada sub-condición de la rama VER-success
         * para ver cuál falla si "Ver" no avanza a HOTWAIT_QUERY/CMD_START. */
        LOGM("CFGACK_VIEW", "node=%d,ok=%d,viewDump=%d,viewNode=%d,state=%d",
             msg.node_id, msg.ok, (int)g_viewDump, g_viewNode, (int)g_state);
    }
    /* VER ok=0: el esclavo no pudo completar la captura/dump. Salir de RUNNING
     * para que la UI no quede esperando muestras que no existen. */
    if (msg.sub_cmd == CMD_VIEW && msg.ok == 0 &&
        g_viewDump && msg.node_id == g_viewNode &&
        (g_state == RUNNING || g_state == PRESTART)) {
        MASTER_LOG_PRINTF("[MASTER] VIEW failed node=%d -> abort\n", msg.node_id);
        LOGM("VIEW_FAIL", "node=%d,state=%d", msg.node_id, (int)g_state);
        syncOutWrite(LOW);
        g_viewDump = false;
        g_viewNode = 0;
        g_streaming = false;
        g_captureDumpDueMs = 0;
        g_dumpRetryPaused = false;
        dumpDeliveryGuardReset();
        g_state = armedOrIdleState();
        matlab.sendReady(g_armedCount);
        matlab.sendHeartbeat(0x00, 0, 0, (uint8_t)g_state);
    }
    /* VER ok=1: esclavo confirmó HOT_WAIT → iniciar HOTWAIT_QUERY/START igual que START */
    if (msg.sub_cmd == CMD_VIEW && msg.ok == 1 &&
        g_viewDump && msg.node_id == g_viewNode && g_state == RUNNING) {
        MASTER_LOG_PRINTF("[MASTER] VIEW HOT_WAIT ready node=%d -> VER PRESTART\n", msg.node_id);
        LOGM("VIEW_HOTWAIT_READY", "node=%d", msg.node_id);
        g_hotWaitAckMask        = 0;
        g_hotWaitQueryNode      = g_viewNode;
        g_hotWaitRetries        = 0;
        g_hotWaitReadyCount     = 0;
        g_hotWaitQueryInFlight  = false;
        g_hotWaitSettling       = false;
        g_hotWaitQueryMs        = millis() + HOTWAIT_QUERY_DELAY_MS;
        g_prestartAction        = PRESTART_ACTION_VER;
        g_state                 = PRESTART;
        beacon_pause();   /* pausar al inicio del PRESTART VER */
    }
    /* VER ok=2: captura completada → volcar datos */
    if (msg.sub_cmd == CMD_VIEW && msg.ok == 2 &&
        g_viewDump && msg.node_id == g_viewNode && g_state == RUNNING) {
        MASTER_LOG_PRINTF("[MASTER] VIEW capture done node=%d -> DUMP\n", msg.node_id);
        LOGM("VIEW_CAPTURE_DONE", "node=%d", msg.node_id);
        beginDump("view_done");
    }

    /* REQ_BATCH NACK: si el slave todavía no tiene ese lote, reintentar antes de avanzar. */
    if (msg.sub_cmd == CMD_REQ_BATCH && !msg.ok &&
        g_state == DUMPING && msg.node_id == g_dumpSlaveIdx) {
        if (g_dumpRetries < DUMP_MAX_RETRIES) {
            g_dumpRetries++;
            g_dumpRetryPaused = true;
            g_dumpRetryDueMs = millis() + DUMP_NACK_RETRY_DELAY_MS;
            MASTER_LOG_PRINTF("[MASTER] DUMP NACK retry %d node=%d seq=%d\n",
                              g_dumpRetries, g_dumpSlaveIdx, g_dumpBatchSeq);
        } else {
            MASTER_LOG_PRINTF("[MASTER] DUMP NACK max retries slave=%d -> next\n",
                              g_dumpSlaveIdx);
            g_dumpBatchSeq = g_rec_n_batches;
            g_dumpBatchRx  = true;
            dumpDeliveryGuardReset();
        }
        return;
    }
    matlab.sendAck(msg.node_id, msg.sub_cmd, msg.ok);
}

static void onHello(const MsgHello &msg, const uint8_t senderMac[6])
{
    MASTER_LOG_PRINTF("[MASTER] HELLO node=%d psoc=%d fs=%u\n", msg.node_id, msg.psoc_ok, msg.sample_rate);
    if (msg.node_id > 0 && msg.node_id <= NUM_SLAVES) {
        CachedHello &h = g_cachedHello[msg.node_id];
        h.valid = true;
        h.psoc_ok = msg.psoc_ok;
        h.sample_rate = msg.sample_rate;
        memcpy(h.mac, senderMac, 6);
    }
    matlab.sendHelloNotif(msg.node_id, msg.psoc_ok, senderMac, msg.sample_rate);
}

static void onStartAck(const MsgStartAck &msg)
{
    if (msg.node_id == 0 || msg.node_id > NUM_SLAVES) return;

    uint32_t probeTxUs = 0;
    if (takeStartProbe(msg.node_id, msg.start_token, probeTxUs)) {
        uint32_t rttUs = (uint32_t)((uint32_t)micros() - probeTxUs);
        uint32_t tofUs = rttUs / 2U;
        matlab.sendStartLatency(msg.node_id, tofUs);
        MASTER_LOG_PRINTF("[MASTER] START_PROBE_ACK node=%d status=%d rtt=%u us tof~=%u us\n",
                          msg.node_id, msg.status, (unsigned)rttUs, (unsigned)tofUs);
        return;
    }

    if (g_startCmdTxUs == 0) return;
    if (msg.start_token != g_startCmdToken) {
        MASTER_LOG_PRINTF("[MASTER] START_ACK stale node=%d token=%u expected=%u\n",
                          msg.node_id, (unsigned)msg.start_token,
                          (unsigned)g_startCmdToken);
        return;
    }
    uint32_t rttUs = (uint32_t)((uint32_t)micros() - g_startCmdTxUs);
    uint32_t tofUs = rttUs / 2U;
    if (msg.status != 0 || msg.node_id == g_startProbeNode) {
        matlab.sendStartLatency(msg.node_id, tofUs);
    }
    MASTER_LOG_PRINTF("[MASTER] START_ACK node=%d status=%d rtt=%u us tof~=%u us\n",
                      msg.node_id, msg.status, (unsigned)rttUs, (unsigned)tofUs);
}

static void onHotWaitAck(const MsgHotWaitAck &msg)
{
    if (msg.node_id == 0 || msg.node_id > NUM_SLAVES) return;
    if (msg.ok && msg.node_id <= g_expectedSlaves) {
        g_hotWaitAckMask |= (1UL << msg.node_id);
    }
    MASTER_LOG_PRINTF("[MASTER] HOTWAIT_ACK node=%d ok=%d state=%d n=%u mask=0x%08X\n",
                      msg.node_id, msg.ok, msg.state, msg.n_batches,
                      (unsigned)g_hotWaitAckMask);
}

static void requestBatch(uint8_t nodeId, uint16_t seq)
{
    const uint8_t *dst = isConfiguredSlaveMac(SLAVE_MACS[nodeId - 1])
                       ? SLAVE_MACS[nodeId - 1]
                       : ESPNOW_BROADCAST;
    MsgReqBatch msg = { CMD_REQ_BATCH, nodeId, seq };
    if (g_espnowReady) { esp_now_send(dst, (uint8_t *)&msg, sizeof(msg)); }
    MASTER_LOG_PRINTF("[MASTER] REQ_BATCH node=%d seq=%d\n", nodeId, seq);
}

static uint16_t effectiveSampleRateHz()
{
    for (uint8_t i = 1; i <= NUM_SLAVES; i++) {
        if (g_cachedHello[i].valid && g_cachedHello[i].sample_rate > 0)
            return g_cachedHello[i].sample_rate;
    }
    return ADC_SAMPLE_RATE_HZ;
}

static uint32_t captureDurationMs(uint16_t nBatches)
{
    uint32_t samples = (uint32_t)nBatches * (uint32_t)(SAMPLES_PER_PART * 2);
    uint16_t fs = effectiveSampleRateHz();
    return (samples * 1000UL + (fs - 1)) / fs;
}

/* ── Beacon pause/resume — sin RF durante captura ─────────────────────────── */
static void beacon_pause(void)
{
    /* El beacon ya está en 1000 TU desde setup(). No llamamos
     * esp_wifi_set_config() aquí porque reinicializar la config del AP
     * puede desconectar brevemente a los clientes WiFi activos. */
    MASTER_LOG_PRINTLN("[MASTER] beacon lento (captura activa, AP visible)");
}

static void beacon_resume(void)
{
    MASTER_LOG_PRINTLN("[MASTER] beacon resumido (inicio dump)");
}

static void scheduleStoreDump(uint16_t nBatches)
{
    if (nBatches == 0 || (g_armedCount == 0 && !g_viewDump)) {
        g_captureDumpDueMs = 0;
        return;
    }
    uint32_t waitMs = captureDurationMs(nBatches) + STORE_CAPTURE_MARGIN_MS;
    if (waitMs < STORE_AUTO_DUMP_MIN_MS) waitMs = STORE_AUTO_DUMP_MIN_MS;
    g_captureDumpDueMs = millis() + waitMs;
    MASTER_LOG_PRINTF("[MASTER] auto DUMP in %u ms (n=%u)\n",
                      (unsigned)waitMs, (unsigned)nBatches);
    LOGM("DUMP_SCHEDULE", "n=%u,wait_ms=%u", nBatches, (unsigned)waitMs);
}

static void beginDump(const char *reason)
{
    beacon_resume();
    g_captureDumpDueMs = 0;
    g_dumpRetryPaused = false;
    g_dumpRetryDueMs = 0;
    /* VIEW dump: un solo nodo; normal dump: todos los esclavos armados */
    bool canDump = g_rec_n_batches > 0 && (g_armedCount > 0 || g_viewDump);
    if (canDump) {
        g_state        = DUMPING;
        g_streaming    = true;
        g_dumpSlaveIdx = g_viewDump ? g_viewNode : 1;
        g_dumpBatchSeq = 0;
        g_dumpBatchRx  = false;
        dumpDeliveryGuardReset();
        g_dumpRetries  = 0;
        matlab.sendHeartbeat(0x00, 0, 0, (uint8_t)g_state);
        requestBatch(g_dumpSlaveIdx, g_dumpBatchSeq);
        g_dumpReqMs = millis();
        MASTER_LOG_PRINTF("[MASTER] DUMPING (%s) slave %d/%d seq 0/%d\n",
                          reason, g_dumpSlaveIdx,
                          g_viewDump ? g_viewNode : g_armedCount,
                          g_rec_n_batches);
        LOGM("DUMP_BEGIN", "reason=%s,node=%u,slaves=%u,n=%u",
             reason, g_dumpSlaveIdx, g_armedCount, g_rec_n_batches);
    } else {
        g_state = armedOrIdleState();
        g_streaming = false;
        matlab.sendReady(g_armedCount);
        matlab.sendHeartbeat(0x00, 0, 0, (uint8_t)g_state);
        MASTER_LOG_PRINTF("[MASTER] %s\n", g_state == ARMED ? "ARMED" : "IDLE");
    }
}

static void sendStartProbe(uint8_t nodeId)
{
    const uint8_t *dst = isConfiguredSlaveMac(SLAVE_MACS[nodeId - 1])
                       ? SLAVE_MACS[nodeId - 1]
                       : ESPNOW_BROADCAST;
    uint64_t t0 = (uint64_t)micros();
    MsgStart msg = { CMD_START, t0, (uint8_t)(0x80u | nodeId) };
    g_startProbeNode = nodeId;
    g_startCmdToken  = (uint32_t)t0;
    g_startCmdTxUs   = (uint32_t)micros();
    rememberStartProbe(nodeId, g_startCmdToken, g_startCmdTxUs);
    esp_err_t err = g_espnowReady
                  ? esp_now_send(dst, (uint8_t *)&msg, sizeof(msg))
                  : ESP_FAIL;
    matlab.sendAck(nodeId, 0xAF, err == ESP_OK ? 1 : 0);
    MASTER_LOG_PRINTF("[MASTER] START_PROBE node=%d token=%u err=%d\n",
                      nodeId, (unsigned)g_startCmdToken, (int)err);
}

/* ── Comando dirigido a un esclavo específico ────────────────────────────── */

static void handleDirectedCmd(uint8_t node_id, uint8_t sub_cmd, uint8_t param)
{
    if (node_id == 0 || node_id > NUM_SLAVES) {
        matlab.sendAck(node_id, sub_cmd, 0);
        return;
    }
    if (!g_espnowReady) {
        matlab.sendAck(node_id, sub_cmd, 0);
        return;
    }

    if (sub_cmd == 0xAF) {
        /* Probe directo: no necesita PRESTART — solo mide RTT al esclavo */
        g_prestartProbeNode = node_id;
        sendStartProbe(node_id);
        MASTER_LOG_PRINTF("[MASTER] START_PROBE direct node=%d state=%d\n",
                          node_id, (int)g_state);
        return;
    }

    const uint8_t *dst = isConfiguredSlaveMac(SLAVE_MACS[node_id - 1])
                       ? SLAVE_MACS[node_id - 1]
                       : ESPNOW_BROADCAST;
    esp_err_t err;
    if (sub_cmd == 0xA7) {
        MsgDebugNode msg = { CMD_DEBUG_NODE, node_id, param };
        err = esp_now_send(dst, (uint8_t *)&msg, sizeof(msg));
    } else if (sub_cmd == 0xB2) {
        /* "Ver": captura única de N lotes. El esclavo guarda en RAM (sin RF durante
         * el muestreo) y luego el maestro hace dump vía REQ_BATCH. */
        uint16_t n = g_rec_n_batches ? g_rec_n_batches : (uint16_t)1;
        g_state = RUNNING;
        g_streaming = true;
        g_viewNode = node_id;
        g_viewBatchesRemaining = 0;  /* ya no se usa conteo live */
        g_viewDump = true;           /* dump de un solo nodo tras la captura */
        g_dumpSlaveIdx = 0;
        g_dumpBatchSeq = 0;
        g_dumpBatchRx = false;
        dumpDeliveryGuardReset();
        MsgView msg = { CMD_VIEW, node_id, n };
        err = esp_now_send(dst, (uint8_t *)&msg, sizeof(msg));
        g_captureDumpDueMs = 0;      /* el dump arranca solo con VIEW_DONE del esclavo */
        LOGM("VIEW", "node=%d,n=%u,store=1,wait_done=1", node_id, msg.n);
    } else if (sub_cmd == 0xB3) {
        /* Debug PSoC: rampa on/off en el PSoC del nodo. */
        MsgDebugPsoc msg = { CMD_DEBUG_PSOC, node_id, param };
        err = esp_now_send(dst, (uint8_t *)&msg, sizeof(msg));
        LOGM("DBGPSOC", "node=%d,en=%d", node_id, param);
    } else {
        MsgSetConfig msg = { CMD_SET_CONFIG, node_id, sub_cmd, param };
        err = esp_now_send(dst, (uint8_t *)&msg, sizeof(msg));
    }
    if (err != ESP_OK) {
        matlab.sendAck(node_id, sub_cmd, 0);
    }
    MASTER_LOG_PRINTF("[MASTER] directed node=%d sub=0x%02X param=%d err=%d\n",
                      node_id, sub_cmd, param, (int)err);
}

/* ── Broadcast ESP-NOW a todos los esclavos ──────────────────────────────── */

static void broadcastRecordLength(uint16_t nBatches)
{
    MsgSetRecLen msg = { CMD_SET_RECLEN, nBatches };
    if (g_espnowReady) {
        esp_now_send(ESPNOW_BROADCAST, (uint8_t *)&msg, sizeof(msg));
    }
    MASTER_LOG_PRINTF("[MASTER] SET_RECLEN=%u\n", nBatches);
}

static void broadcastPrestart(uint16_t nBatches)
{
    MsgPrestart msg = { CMD_PRESTART, nBatches };
    if (g_espnowReady) {
        esp_now_send(ESPNOW_BROADCAST, (uint8_t *)&msg, sizeof(msg));
    }
    MASTER_LOG_PRINTF("[MASTER] PRESTART sent n=%u\n", nBatches);
}

static void sendHotWaitQuery(uint8_t nodeId)
{
    const uint8_t *dst = isConfiguredSlaveMac(SLAVE_MACS[nodeId - 1])
                       ? SLAVE_MACS[nodeId - 1]
                       : ESPNOW_BROADCAST;
    MsgHotWaitQuery msg = { CMD_HOTWAIT_QUERY, nodeId };
    if (g_espnowReady) {
        esp_now_send(dst, (uint8_t *)&msg, sizeof(msg));
    }
    MASTER_LOG_PRINTF("[MASTER] HOTWAIT_QUERY node=%d retry=%d\n",
                      nodeId, g_hotWaitRetries);
}

static void broadcastArm()
{
    if (g_expectedSlaves == 0) {
        MASTER_LOG_PRINTLN("[MASTER] ARM omitido (0 esclavos)");
        return;
    }
    MsgArm msg = { CMD_ARM };
    if (g_espnowReady) {
        esp_now_send(ESPNOW_BROADCAST, (uint8_t *)&msg, sizeof(msg));
    }
    MASTER_LOG_PRINTLN("[MASTER] ARM enviado");
}

static void broadcastStart()
{
    g_t_start_us = (uint64_t)micros();
    g_startProbeNode = 0;
    g_startCmdToken = (uint32_t)g_t_start_us;
    /* GPIO primero — flanco hardware llega a los PSoC antes que el ESP-NOW */
    syncOutWrite(HIGH);
    debugEspHardwareStartPulse();
    MsgStart msg = { CMD_START, g_t_start_us, 0 };
    if (g_espnowReady) {
        g_startCmdTxUs = (uint32_t)micros();
        esp_now_send(ESPNOW_BROADCAST, (uint8_t *)&msg, sizeof(msg));
    }
    MASTER_LOG_PRINTF("[MASTER] START t0=%llu\n", g_t_start_us);
}

static void broadcastScopeStart()
{
    debugEspHardwareStartPulse();
    MsgScopeStart msg = { CMD_SCOPE_START };
    if (g_espnowReady) {
        esp_now_send(ESPNOW_BROADCAST, (uint8_t *)&msg, sizeof(msg));
    }
    MASTER_LOG_PRINTLN("[MASTER] SCOPE_START");
}

static void broadcastDebug(uint8_t enable)
{
    MsgDebug msg = { CMD_DEBUG, enable ? (uint8_t)1 : (uint8_t)0 };
    if (g_espnowReady) {
        esp_now_send(ESPNOW_BROADCAST, (uint8_t *)&msg, sizeof(msg));
    }
    MASTER_LOG_PRINTF("[MASTER] DEBUG broadcast=%u\n", (unsigned)enable);
}

static void broadcastStop()
{
    syncOutWrite(LOW);
    MsgStop msg = { CMD_STOP };
    if (g_espnowReady) {
        esp_now_send(ESPNOW_BROADCAST, (uint8_t *)&msg, sizeof(msg));
    }
    MASTER_LOG_PRINTLN("[MASTER] STOP enviado");
}

static void beginPrestart(PrestartAction action)
{
    beacon_pause();   /* pausar beacon al inicio de PRESTART — toma efecto antes del START */
    syncOutWrite(LOW);
    g_prestartAction = action;
    const char *actionName = "START";
    if (action == PRESTART_ACTION_SCOPE_MULTI) {
        actionName = "SCOPE_MULTI";
    } else if (action == PRESTART_ACTION_START_PROBE) {
        actionName = "START_PROBE";
    }
    g_hotWaitAckMask = 0;
    g_hotWaitQueryNode = 1;
    g_hotWaitRetries = 0;
    g_hotWaitReadyCount = 0;
    g_hotWaitQueryInFlight = false;
    g_hotWaitSettling = false;
    g_hotWaitQueryMs = millis() + HOTWAIT_QUERY_DELAY_MS;
    g_state = PRESTART;
    broadcastPrestart(g_rec_n_batches);
    MASTER_LOG_PRINTF("[MASTER] PRESTART HOT_WAIT before %s n=%u queryDelay=%u ms\n",
                      actionName, g_rec_n_batches,
                      (unsigned)HOTWAIT_QUERY_DELAY_MS);
}

static void abortPrestart(const char *reason)
{
    MASTER_LOG_PRINTF("[MASTER] PRESTART abort: %s ready=%u/%u\n",
                      reason, g_hotWaitReadyCount, g_expectedSlaves);
    g_hotWaitQueryInFlight = false;
    g_hotWaitSettling = false;
    g_streaming = false;
    g_captureDumpDueMs = 0;
    g_dumpRetryPaused = false;
    syncOutWrite(LOW);
    g_state = armedOrIdleState();
    matlab.sendReady(g_armedCount);
    matlab.sendHeartbeat(0x00, 0, 0, (uint8_t)g_state);
}

static void finishPrestartAction()
{
    if (g_prestartAction == PRESTART_ACTION_SCOPE_MULTI) {
        broadcastScopeStart();
        g_scopeStartSent++;
        MASTER_LOG_PRINTF("[MASTER] SCOPE_MULTI %u/%u\n",
                          (unsigned)g_scopeStartSent,
                          (unsigned)SCOPE_MULTI_START_COUNT);
        if (g_scopeStartSent >= SCOPE_MULTI_START_COUNT) {
            g_state = ARMED;
            matlab.sendReady(g_armedCount);
            MASTER_LOG_PRINTLN("[MASTER] SCOPE_MULTI completo -> ARMED");
        } else {
            g_scopeStartNextMs = millis() + SCOPE_MULTI_START_GAP_MS;
            g_state = SCOPE_MULTI;
            MASTER_LOG_PRINTF("[MASTER] SCOPE_MULTI next PRESTART in %u ms\n",
                              (unsigned)SCOPE_MULTI_START_GAP_MS);
        }
    } else if (g_prestartAction == PRESTART_ACTION_START_PROBE) {
        sendStartProbe(g_prestartProbeNode);
        g_state = ARMED;
        matlab.sendReady(g_armedCount);
        MASTER_LOG_PRINTF("[MASTER] HOT_WAIT done -> START_PROBE node=%d\n",
                          g_prestartProbeNode);
    } else if (g_prestartAction == PRESTART_ACTION_VER) {
        /* VER: CMD_START dirigido solo al nodo de VIEW (igual que START pero unicast) */
        g_t_start_us    = (uint64_t)micros();
        g_startProbeNode = 0;
        g_startCmdToken  = (uint32_t)g_t_start_us;
        syncOutWrite(HIGH);
        debugEspHardwareStartPulse();
        {
            const uint8_t *dst = isConfiguredSlaveMac(SLAVE_MACS[g_viewNode - 1])
                               ? SLAVE_MACS[g_viewNode - 1]
                               : ESPNOW_BROADCAST;
            MsgStart msg = { CMD_START, g_t_start_us, g_viewNode };
            if (g_espnowReady) {
                g_startCmdTxUs = (uint32_t)micros();
                esp_now_send(dst, (uint8_t *)&msg, sizeof(msg));
            }
        }
        g_captureDumpDueMs = 0;   /* dump al recibir VIEW_DONE del esclavo */
        g_state = RUNNING;
        matlab.sendHeartbeat(0x00, 0, 0, (uint8_t)g_state);
        MASTER_LOG_PRINTF("[MASTER] VER HOT_WAIT done -> CMD_START node=%d\n", g_viewNode);
        LOGM("VER_START", "node=%d", g_viewNode);
    } else {
        /* No modificar g_armedCount: intentar dump desde todos los slaves ARM'd.
           Los que no grabaron responderán NACK y se saltean via retry. */
        matlab.sendReady(g_armedCount);
        broadcastStart();
        scheduleStoreDump(g_rec_n_batches);
        g_state = RUNNING;
        matlab.sendHeartbeat(0x00, 0, 0, (uint8_t)g_state);
        MASTER_LOG_PRINTF("[MASTER] HOT_WAIT done (ready=%u/armed=%u) -> RUNNING\n",
                          g_hotWaitReadyCount, g_armedCount);
    }
}

/* ── Callback: batch reensamblado de un esclavo ─────────────────────────── */

static void onBatchReady(const ReassembledBatch &batch)
{
    if (g_state == DUMPING) {
        if (batch.node_id != g_dumpSlaveIdx || batch.seq != g_dumpBatchSeq) {
            MASTER_LOG_PRINTF("[MASTER] DUMP ignore node=%d seq=%d expected node=%d seq=%d\n",
                              batch.node_id, batch.seq,
                              g_dumpSlaveIdx, g_dumpBatchSeq);
            return;
        }
        if (g_dumpBatchDelivered &&
            g_dumpDeliveredNode == batch.node_id &&
            g_dumpDeliveredSeq == batch.seq) {
            MASTER_LOG_PRINTF("[MASTER] DUMP duplicate delivered ignored node=%d seq=%d\n",
                              batch.node_id, batch.seq);
            return;
        }
        if (g_dumpBatchRx) {
            MASTER_LOG_PRINTF("[MASTER] DUMP duplicate ignored node=%d seq=%d\n",
                              batch.node_id, batch.seq);
            return;
        }
        dumpDeliveryGuardMark(batch.node_id, batch.seq);
    } else if (!g_streaming) {
        return;
    }
    if (g_viewNode != 0 && batch.node_id != g_viewNode) {
        return;
    }

    /* Reenviar las 30 muestras (post_digital) al MATLAB */
    for (int i = 0; i < SAMPLES_PER_PART * 2; i++) {
        const SampleBytes &s = batch.samples[i];
        int32_t val = (int32_t)s.digi0
                    | ((int32_t)s.digi1 << 8)
                    | ((int32_t)s.digi2 << 16);
        if (val & 0x800000) val |= (int32_t)0xFF000000;
        matlab.sendSample(batch.node_id, val);
        samplePulse();
    }

    /* Durante dump: marcar batch como recibido si es el que esperábamos */
    if (g_state == DUMPING) {
        g_dumpBatchRx = true;
    } else if (g_viewNode != 0 && batch.node_id == g_viewNode &&
               g_viewBatchesRemaining > 0) {
        g_viewBatchesRemaining--;
        if (g_viewBatchesRemaining == 0) {
            MASTER_LOG_PRINTF("[MASTER] VIEW done node=%d\n", g_viewNode);
            LOGM("VIEWDONE", "node=%d", g_viewNode);
            g_viewNode = 0;
            g_streaming = false;
            g_rec_n_batches = 0;
            g_state = armedOrIdleState();
            matlab.sendReady(g_armedCount);
        }
    }
}

/* ── Procesar comandos de MATLAB ─────────────────────────────────────────── */

static void handleMatlabCmd(const MatlabTransport::RxCmd &rxCmd)
{
    uint8_t cmd   = rxCmd.cmd;
    uint8_t param = rxCmd.param;
    LOGM("CMD", "cmd=0x%02X,param=%d,value=%u,node=%d,sub=0x%02X",
         cmd, param, rxCmd.value, rxCmd.node_id, rxCmd.sub_cmd);
    switch (cmd) {
        case 0xA1:   /* stream on/off */
            g_streaming = (param != 0);
            matlab.sendAck(0xFF, cmd, g_streaming ? 1 : 0);
            MASTER_LOG_PRINTF("[MASTER] stream %s\n", g_streaming ? "ON" : "OFF");
            break;

        case 0xA2:   /* ARM esclavos */
            if (g_state == IDLE || g_state == ARMED || g_state == ARMING ||
                (g_state == RUNNING && g_testMode)) {
                g_expectedSlaves = (param == 0) ? NUM_SLAVES : param;
                if (g_expectedSlaves > NUM_SLAVES) g_expectedSlaves = NUM_SLAVES;
                /* Preservar ACKs existentes en re-ARM — no perder esclavos ya conectados */
                if (g_state == IDLE || (g_state == RUNNING && g_testMode)) {
                    g_armAckMask   = 0;
                    g_armedCount   = 0;
                    g_testMode     = false;
                    g_streaming    = false;
                    g_captureDumpDueMs = 0;
                    g_dumpRetryPaused  = false;
                } else {
                    g_armedCount = countArmAcks(g_armAckMask);
                }
                g_state = ARMING;
                broadcastArm();
                matlab.sendAck(0xFF, cmd, 0);
            }
            break;

        case 0xA3:   /* START — N de 16 bits en rxCmd.value */
            if (g_state == IDLE || g_state == ARMED || g_state == ARMING) {
                g_viewNode = 0;
                g_viewBatchesRemaining = 0;
                g_rec_n_batches = clampRecordBatches(rxCmd.value);
                g_debugRamp = 0;
                g_streaming = true;
                beginPrestart(PRESTART_ACTION_START);
                matlab.sendAck(0xFF, cmd, 1);
            }
            break;

        case 0xA4:   /* STOP */
            g_viewNode = 0;
            g_viewBatchesRemaining = 0;
            g_captureDumpDueMs = 0;
            g_dumpRetryPaused = false;
            broadcastStop();
            g_state = STOPPING;
            matlab.sendAck(0xFF, cmd, 0);
            break;

        case 0xA5:   /* STATUS */
            sendCachedStateToWeb();
            break;

        case MATLAB_CMD_SCOPE_START:
            if (param == 0) {
                if (g_state == PRESTART || g_state == SCOPE_MULTI) {
                    g_state = ARMED;
                    MASTER_LOG_PRINTLN("[MASTER] SCOPE_MULTI cancelado");
                }
                matlab.sendAck(0xFF, cmd, 0);
            } else if (g_state == IDLE || g_state == ARMED || g_state == ARMING) {
                (void)param;
                g_rec_n_batches = 0;   /* START falso: no preparar buffers ni dump */
                g_scopeStartSent = 0;
                beginPrestart(PRESTART_ACTION_SCOPE_MULTI);
                matlab.sendAck(0xFF, cmd, 1);
            }
            break;

        case 0xBD:   /* comando dirigido a esclavo específico */
            handleDirectedCmd(rxCmd.node_id, rxCmd.sub_cmd, rxCmd.param);
            break;

        case 0xA7: {  /* debug mode on/off — maestro y esclavos */
            broadcastDebug(param ? 1 : 0);
            if (param) {
                g_debugRamp   = 0;
                g_testMode    = true;
                g_streaming   = true;
                if (g_rec_n_batches > 0 &&
                    (g_state == ARMED || g_state == ARMING || g_state == PRESTART)) {
                    /* Store-and-forward: A7 solo prepara la rampa.
                       A3/START debe conservar el estado ARMED y disparar SYNC. */
                } else {
                    g_state      = RUNNING;
                    g_t_start_us = (uint64_t)micros();
                }
            } else {
                g_testMode  = false;
                g_streaming = false;
                g_captureDumpDueMs = 0;
                g_dumpRetryPaused = false;
                g_state     = armedOrIdleState();
                syncOutWrite(LOW);
            }
            matlab.sendAck(0x00, cmd, param);
            MASTER_LOG_PRINTF("[MASTER] debug=%d (streaming=%d state=%d)\n",
                              param, g_streaming, g_state);
            break;
        }

        case 0xAE: {  /* SET_RECORD_LEN: n_batches (16 bits en rxCmd.value) */
            g_rec_n_batches = clampRecordBatches(rxCmd.value);
            broadcastRecordLength(g_rec_n_batches);
            matlab.sendAck(0xFF, 0xAE, (uint8_t)(g_rec_n_batches & 0xFF));
            break;
        }

        default:
            break;
    }
}

/* ── Setup ───────────────────────────────────────────────────────────────── */

void setup()
{
    matlab.begin();
    /* Log del maestro por Serial1 (Serial = binario 0x56 hacia MATLAB).
     * Pines configurables por -DDBG_LOG_RX / -DDBG_LOG_TX en platformio.ini. */
#if DBG_ENABLE
    Serial1.begin(DBG_LOG_BAUD, SERIAL_8N1, DBG_LOG_RX, DBG_LOG_TX);
#endif
    MASTER_LOG_PRINTLN("[MASTER] boot");
    samplePulseBegin();
    debugEspHardwareBegin();
    MASTER_LOG_PRINTF("[SCOPE] sample pulse pin=%d idle=%d us=%d\n",
                      SAMPLE_PULSE_PIN, SAMPLE_PULSE_IDLE, SAMPLE_PULSE_US);
    MASTER_LOG_PRINTF("[SCOPE] debugHardware=%d start pin=%d idle=%d us=%d\n",
                      DEBUG_HARDWARE, DEBUG_HW_START_PIN,
                      DEBUG_HW_START_IDLE, DEBUG_HW_START_US);

    /* WiFi AP_STA — AP_STA es necesario para que los action frames de ESP-NOW
     * sean compatibles con esclavos ESP8266 en modo STA no conectado.
     * El modo puro WIFI_AP lleva el AP MAC como BSSID en el action frame,
     * que el ESP8266 puede descartar si no reconoce ese BSSID. */
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1),
                      IPAddress(192, 168, 4, 1),
                      IPAddress(255, 255, 255, 0));
    bool apOk = WiFi.softAP(AP_SSID, AP_PASS, 1);   /* canal 1 fijo */
    MASTER_LOG_PRINTF("[MASTER] softAP %s\n", apOk ? "OK" : "FAIL");
    if (apOk) {
        dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));
        MASTER_LOG_PRINTLN("[MASTER] DNS catchall :53 -> 192.168.4.1");
    }
    /* Aumentar intervalo de beacon para reducir interferencia RF en el ADC.
     * El default (100 TU ≈ 100 ms) genera spikes de 10 Hz visibles en el geófono.
     * ESP-NOW no depende de beacons, así que 1000 ms no afecta la comunicación. */
    {
        wifi_config_t ap_cfg = {};
        esp_wifi_get_config(WIFI_IF_AP, &ap_cfg);
        ap_cfg.ap.beacon_interval = 1000;   /* 1000 TU ≈ 1024 ms */
        esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    }
    MASTER_LOG_PRINTF("[MASTER] AP IP:     %s\n", WiFi.softAPIP().toString().c_str());
    MASTER_LOG_PRINTF("[MASTER] STA MAC:   %s\n", WiFi.macAddress().c_str());
    MASTER_LOG_PRINTF("[MASTER] AP MAC:    %s  ch=%d\n",
                      WiFi.softAPmacAddress().c_str(), 1);

    /* Interfaz web de campo — sirve la SPA desde LittleFS sobre el AP propio
     * (192.168.4.1). No requiere router: el celular se conecta directo a
     * GeoNetwork. Ver web_server.h / web_relay.h. */
    if (webServerBegin()) {
        /* Espejar cada paquete de 6 bytes que ya va hacia MATLAB también
         * hacia los clientes WS — protocol.js decodifica el mismo framing. */
        matlab.setPacketRelay(webRelayPacket);
        webRelaySetClientHook(requestWebCachedStateReplay);
    }

    /* ESP-NOW (convive con AP mode) */
    if (!espnowRx.begin(onBatchReady, onArmAck, onSlaveStatus, onCfgAck,
                        onHello, onStartAck, onHotWaitAck)) {
        MASTER_LOG_PRINTLN("[MASTER] ESP-NOW FAIL - USB test continues");
    } else {
        g_espnowReady = true;
        /* Broadcast para ARM/START/STOP y MACs reales opcionales para unicast. */
        addPeerIfNeeded(ESPNOW_BROADCAST);
        for (int i = 0; i < NUM_SLAVES; i++) {
            if (isConfiguredSlaveMac(SLAVE_MACS[i])) {
                addPeerIfNeeded(SLAVE_MACS[i]);
            }
        }
    }

    /* Informar a MATLAB el estado de ESP-NOW y el canal del AP */
    matlab.sendStatus(g_espnowReady ? 1 : 0, 1);  /* canal fijo = 1 */

    /* Pin de scope (solo osciloscopio; gateado por DEBUG_HARDWARE) */
    syncOutBegin();

    MASTER_LOG_PRINTLN("[MASTER] listo");
    LOGM("BOOT", "slaves=%d,espnow=%u", NUM_SLAVES, (unsigned)g_espnowReady);
}

/* ── Loop ────────────────────────────────────────────────────────────────── */

void loop()
{
    debugEspHardwareService();
    dnsServer.processNextRequest();

    /* Gestionar conexión TCP de MATLAB */
    matlab.loop();

    /* Procesar TODOS los comandos encolados (evita pérdida de A1+A3 back-to-back) */
    while (matlab.hasCmd()) {
        handleMatlabCmd(matlab.lastCmd());
    }

    /* Comandos llegados por la interfaz web (mismo despachador — ver
     * web_relay.h: se decodifican a RxCmd para no duplicar lógica). */
    while (webRelayHasCmd()) {
        handleMatlabCmd(webRelayPopCmd());
    }
    webRelayService();

    if (g_webReplayCachedState) {
        g_webReplayCachedState = false;
        sendCachedStateToWeb();
    }

    /* El maestro ya NO muestrea martillo: ahora es un esclavo normal. */

    /* Detectar transición ARMING → ARMED cuando llegan suficientes ACK
     * (los ACK se imprimieron en espnow_rx.h; acá solo chequeamos con timeout) */
    static uint32_t armStartMs = 0;
    static uint32_t armLastTxMs = 0;
    if (g_state == ARMING) {
        if (armStartMs == 0) {
            armStartMs = millis();
            armLastTxMs = armStartMs;
        }
        if (millis() - armLastTxMs >= ARM_RETRY_MS &&
            g_armedCount < g_expectedSlaves) {
            broadcastArm();
            armLastTxMs = millis();
        }
        uint8_t ackCount = countArmAcks(g_armAckMask);
        if (ackCount != g_armedCount) {
            g_armedCount = ackCount;
            matlab.sendReady(g_armedCount);
        }
        if (g_armedCount >= g_expectedSlaves) {
            g_state = ARMED;
            matlab.sendReady(g_armedCount);
            armStartMs = 0;
            armLastTxMs = 0;
        }
        else if (millis() - armStartMs > ARM_TIMEOUT_MS) {
            /* Timeout: reportar cuántos respondieron igual */
            MASTER_LOG_PRINTF("[MASTER] ARM timeout - %d esclavos listos\n", g_armedCount);
            g_state = ARMED;
            matlab.sendReady(g_armedCount);
            armStartMs = 0;
            armLastTxMs = 0;
        }
    } else {
        armStartMs = 0;
        armLastTxMs = 0;
    }

    /* PRESTART: entrar en HOT_WAIT, consultar esclavos y recién disparar. */
    if (g_state == PRESTART) {
        if (g_prestartAction == PRESTART_ACTION_VER) {
            /* VER PRESTART: solo consultar al nodo de VIEW — sin broadcastPrestart */
            if (g_hotWaitSettling) {
                if ((int32_t)(millis() - g_hotWaitSettleDueMs) >= 0) {
                    finishPrestartAction();
                }
            } else if (hotWaitAcked(g_viewNode)) {
                g_hotWaitSettling    = true;
                g_hotWaitSettleDueMs = millis() + HOTWAIT_SETTLE_MS;
                MASTER_LOG_PRINTF("[MASTER] VER HOT_WAIT ok node=%d, settle %u ms\n",
                                  g_viewNode, (unsigned)HOTWAIT_SETTLE_MS);
            } else if (!g_hotWaitQueryInFlight &&
                       (int32_t)(millis() - g_hotWaitQueryMs) >= 0) {
                sendHotWaitQuery(g_viewNode);
                g_hotWaitQueryInFlight = true;
                g_hotWaitQueryMs       = millis();
            } else if (g_hotWaitQueryInFlight &&
                       millis() - g_hotWaitQueryMs > HOTWAIT_QUERY_TIMEOUT_MS) {
                if (g_hotWaitRetries < HOTWAIT_QUERY_RETRIES) {
                    g_hotWaitRetries++;
                    sendHotWaitQuery(g_viewNode);
                    g_hotWaitQueryMs = millis();
                } else {
                    MASTER_LOG_PRINTF("[MASTER] VER HOT_WAIT timeout node=%d -> continuar\n",
                                      g_viewNode);
                    g_hotWaitSettling    = true;
                    g_hotWaitSettleDueMs = millis() + HOTWAIT_SETTLE_MS;
                }
            }
        } else {
            /* PRESTART normal: consultar esclavos en orden */
            uint8_t readyCount = countHotWaitAcks(g_hotWaitAckMask);
            if (readyCount != g_hotWaitReadyCount) {
                g_hotWaitReadyCount = readyCount;
            }

            if (g_expectedSlaves == 0 && !g_hotWaitSettling) {
                g_hotWaitSettling    = true;
                g_hotWaitSettleDueMs = millis() + HOTWAIT_SETTLE_MS;
            }

            if (g_hotWaitSettling) {
                if ((int32_t)(millis() - g_hotWaitSettleDueMs) >= 0) {
                    finishPrestartAction();
                }
            } else {
                while (g_hotWaitQueryNode <= g_expectedSlaves &&
                       hotWaitAcked(g_hotWaitQueryNode)) {
                    g_hotWaitQueryNode++;
                    g_hotWaitRetries = 0;
                    g_hotWaitQueryInFlight = false;
                    g_hotWaitQueryMs = millis();
                }

                if (g_hotWaitQueryNode > g_expectedSlaves) {
                    g_hotWaitSettling    = true;
                    g_hotWaitSettleDueMs = millis() + HOTWAIT_SETTLE_MS;
                    MASTER_LOG_PRINTF("[MASTER] HOT_WAIT all ready, settle %u ms\n",
                                      (unsigned)HOTWAIT_SETTLE_MS);
                } else if (!g_hotWaitQueryInFlight &&
                           (int32_t)(millis() - g_hotWaitQueryMs) >= 0) {
                    sendHotWaitQuery(g_hotWaitQueryNode);
                    g_hotWaitQueryInFlight = true;
                    g_hotWaitQueryMs       = millis();
                } else if (g_hotWaitQueryInFlight &&
                           millis() - g_hotWaitQueryMs > HOTWAIT_QUERY_TIMEOUT_MS) {
                    if (g_hotWaitRetries < HOTWAIT_QUERY_RETRIES) {
                        g_hotWaitRetries++;
                        broadcastPrestart(g_rec_n_batches);
                        sendHotWaitQuery(g_hotWaitQueryNode);
                        g_hotWaitQueryMs = millis();
                    } else {
                        MASTER_LOG_PRINTF("[MASTER] HOT_WAIT skip node=%d\n", g_hotWaitQueryNode);
                        g_hotWaitQueryNode++;
                        g_hotWaitRetries = 0;
                        g_hotWaitQueryInFlight = false;
                        g_hotWaitQueryMs       = millis();
                    }
                }
            }
        }
    }

    /* STARTs falsos para osciloscopio: cada pulso repite PRESTART completo. */
    if (g_state == SCOPE_MULTI) {
        if (SCOPE_MULTI_START_COUNT <= 0) {
            g_state = ARMED;
            MASTER_LOG_PRINTLN("[MASTER] SCOPE_MULTI completo -> ARMED");
        } else if ((int32_t)(millis() - g_scopeStartNextMs) >= 0) {
            beginPrestart(PRESTART_ACTION_SCOPE_MULTI);
        }
    }

    /* Store-and-forward: cuando termina la ventana N, dumpear sin depender de STOP. */
    if (g_state == RUNNING && g_rec_n_batches > 0 && g_captureDumpDueMs != 0 &&
        (int32_t)(millis() - g_captureDumpDueMs) >= 0) {
        syncOutWrite(LOW);
        beginDump("capture_done");
    }

    /* Transición STOPPING → DUMPING (store-and-forward) o → IDLE (clásico) */
    static uint32_t stopStartMs = 0;
    if (g_state == STOPPING) {
        if (stopStartMs == 0) stopStartMs = millis();
        if (millis() - stopStartMs > 500) {
            stopStartMs = 0;
            beginDump("stop");
        }
    } else {
        stopStartMs = 0;
    }

    /* DUMPING — stop-and-wait con retry por batch */
    if (g_state == DUMPING) {
        bool advance = false;

        if (g_dumpBatchRx && webRelayReadyForDumpRequest()) {
            g_dumpBatchRx = false;
            g_dumpRetries = 0;
            g_dumpRetryPaused = false;
            advance = true;
        } else if (g_dumpRetryPaused) {
            if ((int32_t)(millis() - g_dumpRetryDueMs) >= 0) {
                g_dumpRetryPaused = false;
                requestBatch(g_dumpSlaveIdx, g_dumpBatchSeq);
                g_dumpReqMs = millis();
            }
        } else if (millis() - g_dumpReqMs > DUMP_BATCH_TIMEOUT_MS) {
            g_dumpRetries++;
            if (g_dumpRetries >= DUMP_MAX_RETRIES) {
                MASTER_LOG_PRINTF("[MASTER] DUMP skip node=%d seq=%d\n",
                                  g_dumpSlaveIdx, g_dumpBatchSeq);
                g_dumpRetries = 0;
                g_dumpRetryPaused = false;
                advance = true;
            } else {
                MASTER_LOG_PRINTF("[MASTER] DUMP retry %d node=%d seq=%d\n",
                                  g_dumpRetries, g_dumpSlaveIdx, g_dumpBatchSeq);
                requestBatch(g_dumpSlaveIdx, g_dumpBatchSeq);
                g_dumpReqMs = millis();
            }
        }

        if (advance) {
            g_dumpBatchSeq++;
            dumpDeliveryGuardReset();
            if (g_dumpBatchSeq >= g_rec_n_batches) {
                g_dumpBatchSeq = 0;
                g_dumpSlaveIdx++;
                /* VIEW dump: solo un nodo; dump normal: todos los armados */
                uint8_t dumpLimit = g_viewDump ? g_viewNode : g_armedCount;
                if (g_dumpSlaveIdx > dumpLimit) {
                    if (g_viewDump) {
                        broadcastRecordLength(0);
                        g_viewDump = false;
                        g_viewNode = 0;
                        MASTER_LOG_PRINTLN("[MASTER] VIEW DUMP completo");
                        LOGM("VIEWDUMP_DONE", "");
                    }
                    g_state = armedOrIdleState();
                    g_streaming = false;
                    g_rec_n_batches = 0;
                    g_captureDumpDueMs = 0;
                    g_dumpRetryPaused = false;
                    dumpDeliveryGuardReset();
                    matlab.sendReady(g_armedCount);
                    matlab.sendHeartbeat(0x00, 0, 0, (uint8_t)g_state);
                    MASTER_LOG_PRINTF("[MASTER] DUMP completo -> %s\n",
                                      g_state == ARMED ? "ARMED" : "IDLE");
                } else {
                    MASTER_LOG_PRINTF("[MASTER] DUMP slave %d/%d seq 0/%d\n",
                                      g_dumpSlaveIdx, dumpLimit, g_rec_n_batches);
                    requestBatch(g_dumpSlaveIdx, g_dumpBatchSeq);
                    g_dumpReqMs = millis();
                }
            } else {
                g_dumpRetryPaused = false;
                requestBatch(g_dumpSlaveIdx, g_dumpBatchSeq);
                g_dumpReqMs = millis();
            }
        }
    }

    /* Heartbeat cada ~1 s cuando no hay stream activo.
     * Se envía siempre que haya un cliente WS o MATLAB conectado — sin esto,
     * el navegador del teléfono cierra la conexión WebSocket "inactiva". */
    static uint32_t lastHbMs = 0;
    if (!g_streaming && millis() - lastHbMs > 1000) {
        lastHbMs = millis();
        if (matlab.connected() || ws.count() > 0) {
            matlab.sendHeartbeat(0x00, 0, 0, (uint8_t)g_state);
        }
    }
    webRelayService();
}
