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
#include <ESPmDNS.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include "sync_protocol.h"
#include "espnow_rx.h"
#include "ota_master.h"
#include "rt_archive.h"
#include "matlab_transport.h"
#include "master_log.h"
#include "debug_log.h"
#include "web_server.h"
#include "link_mode.h"
#include "rt_stream.h"
#include "../../scope_measurement.h"

/* ── Configuración ────────────────────────────────────────────────────────── */
static const char *AP_SSID = "GeoNetwork";
#ifndef MASTER_AP_PASS_OVERRIDE
static const char *AP_PASS = "geophone2026";
#else
static const char *AP_PASS = MASTER_AP_PASS_OVERRIDE;   /* banco: "" = AP abierto */
#endif
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
/* El esclavo reintenta el SETN al PSoC hasta 4 veces de 500 ms: el maestro
 * tiene que esperar mas que eso antes de saltear el nodo (2026-10-23). */
#ifndef HOTWAIT_QUERY_RETRIES
  #define HOTWAIT_QUERY_RETRIES 20
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
  #define ADC_SAMPLE_RATE_HZ 2604
#endif
#ifndef STORE_CAPTURE_MARGIN_MS
  #define STORE_CAPTURE_MARGIN_MS 750
#endif
#ifndef STORE_AUTO_DUMP_MIN_MS
  #define STORE_AUTO_DUMP_MIN_MS 500
#endif
#ifndef STORE_CAPTURE_TIMEOUT_MIN_MS
  #define STORE_CAPTURE_TIMEOUT_MIN_MS 2500
#endif
#ifndef DUMP_NACK_RETRY_DELAY_MS
  #define DUMP_NACK_RETRY_DELAY_MS 100
#endif
#ifndef PSOC_CAPTURE_MAX_BATCHES
  /* GEO persiste en FatFs del PSoC; el maestro solo releva lote por lote. */
  #define PSOC_CAPTURE_MAX_BATCHES 60000u
#endif
#define PSOC_RAM_CAPTURE_MAX_BATCHES 512u
#ifndef ESPNOW_USE_UNICAST_TO_SLAVES
  #define ESPNOW_USE_UNICAST_TO_SLAVES 0
#endif
#ifndef AP_BEACON_INTERVAL_TU
  #define AP_BEACON_INTERVAL_TU 100
#endif
#ifndef AP_BEACON_CAPTURE_INTERVAL_TU
  #define AP_BEACON_CAPTURE_INTERVAL_TU 1000
#endif
#ifndef AP_KEEP_BEACON_DURING_CAPTURE
  #define AP_KEEP_BEACON_DURING_CAPTURE 0
#endif
#ifndef WIFI_TX_POWER_QDBM
  #define WIFI_TX_POWER_QDBM 84   /* maximo del ESP32 (el chip la limita a 78) */
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
#ifndef SYNC_OUT_PIN
#define SYNC_OUT_PIN 25
#endif
#define LED_PIN      2    /* GPIO2 = LED azul integrado ESP32-DevKitC V4 */

#if DEBUG_HARDWARE
  static inline void syncOutBegin()        { pinMode(SYNC_OUT_PIN, OUTPUT); digitalWrite(SYNC_OUT_PIN, LOW); }
  static inline void syncOutWrite(uint8_t l){ digitalWrite(SYNC_OUT_PIN, l); }
#else
  static inline void syncOutBegin()        {}
  static inline void syncOutWrite(uint8_t) {}
#endif

/* ── Estado ──────────────────────────────────────────────────────────────── */
/* ENLACE: fase de subida. El maestro baja su AP y ESP-NOW y se asocia como STA
 * a una red con internet para vaciar la cola de capturas. Solo se entra desde
 * IDLE/ARMED — nunca a mitad de captura (ver PLAN_CONECTIVIDAD_MASTER.md). */
enum MasterState { IDLE, ARMING, ARMED, RUNNING, STOPPING, DUMPING, PRESTART, SCOPE_MULTI, ENLACE };
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
/* N efectivo (post-clamp del esclavo) reportado en el último HOTWAIT_ACK.
 * 0 = sin dato (usar el largo configurado). Ver dumpBatchesForNode(). */
static uint16_t          g_effNBatches[NUM_SLAVES + 1] = {0};
static uint8_t           g_hotWaitQueryNode = 1;
static uint8_t           g_hotWaitRetries = 0;
static uint8_t           g_hotWaitReadyCount = 0;
static bool              g_hotWaitQueryInFlight = false;
static bool              g_hotWaitSettling = false;
static uint32_t          g_hotWaitQueryMs = 0;
static uint32_t          g_hotWaitSettleDueMs = 0;

/* Rafaga RF de banco (cmd USB 0xC1): trafico ESP-NOW sintetico no bloqueante. */
static bool              g_rfBurstArmed = false;
static uint32_t          g_rfBurstStartMs = 0;
static uint32_t          g_rfBurstDurationMs = 0;
static uint32_t          g_rfBurstNextTxMs = 0;
static uint32_t          g_rfBurstCounter = 0;
static uint32_t          g_rfBurstSent = 0;

static void stopRfBurst()
{
    if (!g_rfBurstArmed) return;
    g_rfBurstArmed = false;
    MASTER_LOG_PRINTF("[MASTER] RFBURST fin enviadas=%lu\n",
                      (unsigned long)g_rfBurstSent);
}

static void armRfBurst(uint16_t duration10Ms)
{
    if (duration10Ms == 0) {
        stopRfBurst();
        return;
    }

    uint32_t now = millis();
    g_rfBurstArmed = true;
    g_rfBurstStartMs = now;
    g_rfBurstDurationMs = (uint32_t)duration10Ms * 10u;
    g_rfBurstNextTxMs = now;
    g_rfBurstCounter = 0;
    g_rfBurstSent = 0;
    MASTER_LOG_PRINTF("[MASTER] RFBURST inicio N=%u duracion_ms=%lu\n",
                      duration10Ms, (unsigned long)g_rfBurstDurationMs);
}

static void serviceRfBurst()
{
    if (!g_rfBurstArmed) return;

    uint32_t now = millis();
    if ((uint32_t)(now - g_rfBurstStartMs) >= g_rfBurstDurationMs) {
        stopRfBurst();
        return;
    }
    if (!g_espnowReady || (int32_t)(now - g_rfBurstNextTxMs) < 0) return;

    uint8_t payload[240];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(0x5A ^ i);
    }
    payload[0] = 'R';
    payload[1] = 'F';
    payload[2] = 'B';
    payload[3] = '1';
    payload[4] = (uint8_t)(g_rfBurstCounter & 0xFFu);
    payload[5] = (uint8_t)((g_rfBurstCounter >> 8) & 0xFFu);
    payload[6] = (uint8_t)((g_rfBurstCounter >> 16) & 0xFFu);
    payload[7] = (uint8_t)((g_rfBurstCounter >> 24) & 0xFFu);

    if (geonetSend(ESPNOW_BROADCAST, payload, sizeof(payload)) == ESP_OK) {
        g_rfBurstSent++;
    }
    g_rfBurstCounter++;
    g_rfBurstNextTxMs = now + 58u;
}

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
static uint16_t g_rec_n_batches   = 0;   /* batches a grabar por slave (canónico = GEO) */
/* Duración por tipo de nodo (H4, 2026-07-11): largo propio para HAMMER.
 * 0 = no configurado → todos los nodos usan g_rec_n_batches (camino legado
 * byte-idéntico al validado). Se setea con el comando web 0xAD. */
static uint16_t g_rec_n_batches_hammer = 0;
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
static uint32_t g_captureDoneMask = 0;
static bool     g_dumpRequiresWebClient = false;
static bool     g_dumpWaitingForWeb = false;
static uint8_t  g_dumpSkipStreak  = 0;   /* lotes seguidos que el esclavo no entregó */
static uint32_t g_dumpWsWaitMs    = 0;   /* desde cuándo se espera un cliente web */
#define DUMP_MAX_RETRIES  30
#define DUMP_BATCH_TIMEOUT_MS 300
/* Cortes de cordura del volcado. Sin ellos un esclavo que capturó 0 lotes deja
 * al maestro moliendo DUMP_MAX_RETRIES x DUMP_BATCH_TIMEOUT_MS por cada lote
 * pedido: con 261 lotes vacíos son 39 minutos sin atender nada más, que desde
 * afuera se ve como "el maestro se colgó en DUMPING" (visto 2026-09-18). */
#define DUMP_MAX_SKIP_STREAK  3
#define DUMP_WS_WAIT_MAX_MS   30000u

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
/* Captura en tiempo real (rt_stream.h) y modo pedido por la web/USB (0xAF). */
static RtStream       g_rt;
static uint32_t       g_rtStopReqMs = 0;   /* STOP pedido en modo stream; 0 = no */
static uint8_t        g_capMode = 0u;
/* SYNCMODE_* de la proxima captura (web: comando 0xB8). Viaja en PRESTART. */
static uint8_t        g_syncMode = SYNCMODE_FTM_ACTIVE;
static bool capModeRealtime() { return (g_capMode & CAPMODE_REALTIME) != 0u; }
static bool capModeStreams()
{
    return capModeRealtime() || (g_capMode & CAPMODE_STREAM_DUMP) != 0u;
}
static MatlabTransport matlab;

struct CachedHello {
    bool valid = false;
    uint8_t psoc_ok = 0;
    uint16_t sample_rate = 0;
    uint8_t hw_class = SLAVE_HW_UNKNOWN;
    uint8_t sd_present = 0;
    uint8_t link_model = SLAVE_LINK_UNKNOWN;
    uint8_t ctl_status = SLAVE_CTL_UNKNOWN;
    uint8_t pga_code = 0xFF;
    uint8_t pgaout_code = 0xFF;
    uint32_t ram_batches = 0;
    uint32_t sd_batches = 0;
    uint8_t reset_reason = 0xFFu;
    uint32_t uptime_s = 0;
    uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
};
static CachedHello g_cachedHello[NUM_SLAVES + 1];

/* Sub-comando dirigido 0xBB = factor de decimación (1..100). El PSoC promedia
 * N muestras y entrega una: Fs efectiva = ADC_SAMPLE_RATE_HZ / N. */
#define SUBCMD_DECIMATION 0xBBu
static uint8_t  g_wantDecim = 0;   /* 0 = el maestro todavía no tiene opinión */
static uint32_t g_authorityNextMs[NUM_SLAVES + 1] = {0};
static uint8_t  g_authorityTries[NUM_SLAVES + 1]  = {0};
static volatile bool g_webReplayCachedState = false;

static void beginDump(const char *reason);
static bool requestEnlace(const char **why);
static uint8_t collectLinkNodes(LinkNodeInfo *out, uint8_t maxNodes);
static uint8_t collectLinkNodeSingle(LinkNodeInfo *out, uint8_t nodeId);
static void abortCaptureToArmed(uint8_t nodeId, const char *reason);
static void beginPrestart(PrestartAction action);
static void beacon_pause(void);
static void beacon_resume(void);

static uint32_t g_debugRamp     = 0;   /* (reservado para tests) */

/* Helpers de pulsos GPIO de osciloscopio (módulo aparte). */
#include "scope_pulse.h"
/* Intensidad de señal esclavos<->maestro e interfaz<->maestro (módulo aparte). */
#include "link_rssi.h"

static bool sameMac(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, 6) == 0;
}

static bool isUsableUnicastMac(const uint8_t mac[6])
{
    static const uint8_t ZERO_MAC[6] = {0, 0, 0, 0, 0, 0};
    if (sameMac(mac, ZERO_MAC) || sameMac(mac, ESPNOW_BROADCAST)) return false;
    return !(mac[0] == 0xFF && mac[1] == 0xFF && mac[2] == 0xFF &&
             mac[3] == 0xFF && mac[4] == 0xFF);
}

static bool isConfiguredSlaveMac(const uint8_t mac[6])
{
#if !ESPNOW_USE_UNICAST_TO_SLAVES
    (void)mac;
    return false;
#else
    return isUsableUnicastMac(mac);
#endif
}

static void addPeerIfNeeded(const uint8_t mac[6])
{
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;
    peer.encrypt = false;
    /* Por la STA (por defecto). Por el AP el S3 retenia las unicast a
     * esclavos no asociados hasta la baliza siguiente: la descarga perdia
     * un pedido de cada dos (2026-10-24). Los esclavos aceptan la MAC de la
     * STA del S3 aunque difiera del BSSID en el bit de administrada
     * localmente (macIsMaster). */
    peer.ifidx = WIFI_IF_STA;
    esp_now_add_peer(&peer);
}

static const uint8_t *espnowDstForSlave(uint8_t nodeId)
{
    if (nodeId == 0 || nodeId > NUM_SLAVES) return ESPNOW_BROADCAST;

    CachedHello &h = g_cachedHello[nodeId];
    if (h.valid && isUsableUnicastMac(h.mac)) {
        addPeerIfNeeded(h.mac);
        return h.mac;
    }

    const uint8_t *configured = SLAVE_MACS[nodeId - 1];
    if (isConfiguredSlaveMac(configured)) {
        addPeerIfNeeded(configured);
        return configured;
    }
    return ESPNOW_BROADCAST;
}

static uint8_t countArmAcks(uint32_t mask)
{
    uint8_t n = 0;
    for (uint8_t node = 1; node <= NUM_SLAVES; node++) {
        if (mask & (1UL << node)) n++;
    }
    return n;
}

static uint8_t countHotWaitAcks(uint32_t mask)
{
    uint8_t n = 0;
    for (uint8_t node = 1; node <= NUM_SLAVES; node++) {
        if (mask & (1UL << node)) n++;
    }
    return n;
}

static bool allArmedCapturesDone()
{
    if (g_armedCount == 0 || g_armAckMask == 0) return false;
    return (g_captureDoneMask & g_armAckMask) == g_armAckMask;
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

static uint16_t clampRamRecordBatches(uint16_t n)
{
    return (n > PSOC_RAM_CAPTURE_MAX_BATCHES)
         ? (uint16_t)PSOC_RAM_CAPTURE_MAX_BATCHES : n;
}

/* Largo de captura para un nodo según su hw_class cacheado (HELLO).
 * Un HAMMER nunca puede recibir más de la RAM local (512 lotes): AD=0
 * significa "sin override explícito", no permiso para desbordar RAM. Un nodo
 * sin HELLO (hw_class desconocido) cae al canónico (GEO). */
static uint16_t nBatchesForNode(uint8_t nodeId)
{
    if (nodeId >= 1 && nodeId <= NUM_SLAVES &&
        g_cachedHello[nodeId].valid &&
        g_cachedHello[nodeId].hw_class == SLAVE_HW_HAMMER) {
        return (g_rec_n_batches_hammer != 0)
             ? g_rec_n_batches_hammer
             : clampRamRecordBatches(g_rec_n_batches);
    }
    return g_rec_n_batches;
}

/* Largo REAL a dumpear por nodo: el HOTWAIT_ACK del esclavo trae el N
 * efectivo después de su clamp (512 lotes si la vía SD del PSoC no está
 * activa). Pedir seq que no existen generaba una tormenta de NACKs de
 * 30 reintentos × 100 ms por lote faltante. Con duraciones dentro del
 * límite, eff==n y el camino queda idéntico al de siempre. */
static uint16_t dumpBatchesForNode(uint8_t nodeId)
{
    uint16_t n = nBatchesForNode(nodeId);
    if (nodeId >= 1 && nodeId <= NUM_SLAVES) {
        uint16_t eff = g_effNBatches[nodeId];
        if (eff != 0 && eff < n) return eff;
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
            matlab.sendHelloNotif(node, h.psoc_ok, h.mac, h.sample_rate,
                                  h.hw_class, h.sd_present, h.link_model,
                                   h.ctl_status, h.pga_code, h.pgaout_code,
                                   h.ram_batches, h.sd_batches,
                                   h.reset_reason, h.uptime_s);
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
    /* En VER, reenviar el ACK al cliente antes de cambiar estado, iniciar el
     * PRESTART o pedir el primer lote. El protocolo ESP-NOW interno usa
     * CMD_VIEW=0x24, pero la API web/MATLAB que originó la operación usa
     * SUBCMD_VER=0xB2: devolver 0xB2 conserva la correlación request/ACK. */
    const bool viewAckForwarded = (msg.sub_cmd == CMD_VIEW);
    if (viewAckForwarded) {
        /* Dedup de completado: el esclavo reenvía ok=2 hasta 24 veces por RF
         * (por si se pierde); a la web solo va la primera copia (la que llega
         * con la captura todavía en RUNNING). */
        bool dupCompletion = (msg.ok == 2 && g_state != RUNNING);
        if (!dupCompletion) {
            matlab.sendAck(msg.node_id, 0xB2u, msg.ok);
        }
    }
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
        g_captureDoneMask = 0;
        g_dumpRetryPaused = false;
        g_dumpRequiresWebClient = false;
        g_dumpWaitingForWeb = false;
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
        memset(g_effNBatches, 0, sizeof(g_effNBatches));
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

    /* START ok=2: captura normal completa en el slave. Este es el camino
     * principal; el timer queda solo como seguridad si algún ACK se pierde.
     * Dedup: solo la PRIMERA copia del ok=2 de cada nodo se releva a la web
     * (el esclavo lo repite hasta 24 veces por RF y la web lograba 24 líneas
     * de "START captura completa" por captura). */
    bool suppressForward = false;
    if (msg.sub_cmd == CMD_START && msg.ok == 2 && g_viewDump) {
        suppressForward = true;   /* copia tardía durante un flujo VER */
    }
    if (msg.sub_cmd == CMD_START && !g_viewDump) {
        if (msg.ok == 2 && g_state != RUNNING) {
            suppressForward = true;   /* retry tardío: ya estamos en DUMPING/ARMED */
        }
        if (msg.node_id > 0 && msg.node_id <= NUM_SLAVES &&
            msg.ok == 2 && g_state == RUNNING && !capModeStreams()) {
            uint32_t bit = (1UL << msg.node_id);
            if (g_armAckMask & bit) {
                suppressForward = (g_captureDoneMask & bit) != 0;
                g_captureDoneMask |= bit;
                uint8_t doneCount = countArmAcks(g_captureDoneMask & g_armAckMask);
                MASTER_LOG_PRINTF("[MASTER] START capture done node=%d (%u/%u)\n",
                                  msg.node_id, doneCount, g_armedCount);
                LOGM("START_CAPTURE_DONE", "node=%d,done=%u,armed=%u,mask=0x%08lX",
                     msg.node_id, doneCount, g_armedCount,
                     (unsigned long)g_captureDoneMask);
                if (allArmedCapturesDone()) {
                    syncOutWrite(LOW);
                    beginDump("start_done");
                }
            } else {
                MASTER_LOG_PRINTF("[MASTER] START done from unarmed node=%d ignored\n",
                                  msg.node_id);
            }
        } else if (msg.ok == 0 && (g_state == RUNNING || g_state == PRESTART)) {
            MASTER_LOG_PRINTF("[MASTER] START failed node=%d state=%d\n",
                              msg.node_id, (int)g_state);
            LOGM("START_FAIL", "node=%d,state=%d", msg.node_id, (int)g_state);
        }
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
            g_dumpBatchSeq = dumpBatchesForNode(g_dumpSlaveIdx);
            g_dumpBatchRx  = true;
            dumpDeliveryGuardReset();
        }
        return;
    }
    if (!viewAckForwarded && !suppressForward) {
        matlab.sendAck(msg.node_id, msg.sub_cmd, msg.ok);
    }
}

static const char *slaveHwName(uint8_t hwClass)
{
    switch (hwClass) {
        case SLAVE_HW_GEO:    return "GEO";
        case SLAVE_HW_HAMMER: return "HAMMER";
        default:              return "UNKNOWN";
    }
}

static const char *slaveLinkName(uint8_t linkModel)
{
    switch (linkModel) {
        case SLAVE_LINK_SERIAL: return "SERIAL";
        case SLAVE_LINK_I2C:    return "I2C";
        default:                return "UNKNOWN";
    }
}

static void onHello(const MsgHello &msg, const uint8_t senderMac[6])
{
    MASTER_LOG_PRINTF("[MASTER] HELLO node=%d psoc=%d fs=%u hw=%u/%s link=%u/%s\n",
                      msg.node_id, msg.psoc_ok, msg.sample_rate,
                      msg.hw_class, slaveHwName(msg.hw_class),
                      msg.link_model, slaveLinkName(msg.link_model));
    if (msg.node_id > 0 && msg.node_id <= NUM_SLAVES) {
        CachedHello &h = g_cachedHello[msg.node_id];
        h.valid = true;
        h.psoc_ok = msg.psoc_ok;
        h.sample_rate = msg.sample_rate;
        h.hw_class = msg.hw_class;
        h.sd_present = msg.sd_present;
        h.link_model = msg.link_model;
        h.ctl_status = msg.ctl_status;
        h.pga_code = msg.pga_code;
        h.pgaout_code = msg.pgaout_code;
        h.ram_batches = msg.ram_batches;
        h.sd_batches = msg.sd_batches;
        h.reset_reason = msg.reset_reason;
        h.uptime_s = msg.uptime_s;
        memcpy(h.mac, senderMac, 6);
        if (isUsableUnicastMac(h.mac)) addPeerIfNeeded(h.mac);
    }
    matlab.sendHelloNotif(msg.node_id, msg.psoc_ok, senderMac, msg.sample_rate,
                          msg.hw_class, msg.sd_present, msg.link_model,
                          msg.ctl_status, msg.pga_code, msg.pgaout_code,
                          msg.ram_batches, msg.sd_batches,
                          msg.reset_reason, msg.uptime_s);
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
    /* Un esclavo rechazó el START real (no estaba en HOT_WAIT listo): abortar
     * ya, en vez de esperar el timeout de rescate y dumpear lotes vacíos. */
    /* Solo cuenta si ese nodo estaba listo en HOT_WAIT: uno salteado (sin
     * PSoC, sin memoria) siempre rechaza y no tiene que tirar a los demas. */
    if (msg.status == 0 && g_state == RUNNING &&
        (g_hotWaitAckMask & (1UL << msg.node_id)) != 0u) {
        abortCaptureToArmed(msg.node_id, "start_refused");
    } else if (msg.status == 0) {
        MASTER_LOG_PRINTF("[MASTER] START rechazado por S%d (no estaba listo): sigue sin el\n",
                          msg.node_id);
    }
}

static void onHotWaitAck(const MsgHotWaitAck &msg)
{
    if (msg.node_id == 0 || msg.node_id > NUM_SLAVES) return;
    if (msg.ok && msg.node_id <= NUM_SLAVES) {
        g_hotWaitAckMask |= (1UL << msg.node_id);
        /* N efectivo tras el clamp del esclavo (512 si la vía SD no está
         * activa): es el techo real del dump para este nodo. */
        g_effNBatches[msg.node_id] = msg.n_batches;
    }
    MASTER_LOG_PRINTF("[MASTER] HOTWAIT_ACK node=%d ok=%d state=%d n=%u mask=0x%08X\n",
                      msg.node_id, msg.ok, msg.state, msg.n_batches,
                      (unsigned)g_hotWaitAckMask);
    /* NACK espontáneo del watchdog HOT_WAIT del esclavo con captura en curso:
     * ese nodo ya no va a capturar nada, abortar limpio. */
    /* Un "todavia no listo" (state HOT_WAIT=2 o SAMPLING=3 en el esclavo)
     * puede llegar tarde, ya en RUNNING: no es un aborto. El aborto real
     * viene con el esclavo fuera de captura. */
    /* Y solo si el nodo participa: uno salteado en HOT_WAIT manda NACK
     * tardios que tiraban la captura de los demas. */
    if (!msg.ok && g_state == RUNNING && msg.state != 2u && msg.state != 3u &&
        (g_armAckMask & (1UL << msg.node_id)) != 0u) {
        abortCaptureToArmed(msg.node_id, "hotwait_abort");
    }
}

/* Pedido de lote que no pudo salir porque la cola de TX del WiFi estaba llena
 * (ESP_ERR_ESPNOW_NO_MEM: el WebSocket hacia la web la ocupa durante la
 * descarga). Se reintenta desde loop() a los 2 ms en vez de esperar el
 * timeout de 300 ms: en el S3 fallaba uno de cada dos pedidos. */
static volatile bool     g_reqResendPending = false;
static uint8_t           g_reqResendNode = 0;
static uint16_t          g_reqResendSeq = 0;
static uint32_t          g_reqResendDueMs = 0;
static uint8_t           g_reqResendTries = 0;

static void requestBatch(uint8_t nodeId, uint16_t seq)
{
    const uint8_t *dst = espnowDstForSlave(nodeId);
    MsgReqBatch msg = { CMD_REQ_BATCH, nodeId, seq, REQ_BATCH_COMPRESSED };
    esp_err_t err = ESP_FAIL;
    if (g_espnowReady) { err = geonetSend(dst, (uint8_t *)&msg, sizeof(msg)); }
    if (err == ESP_ERR_ESPNOW_NO_MEM) {
        const bool same = g_reqResendPending && g_reqResendNode == nodeId && g_reqResendSeq == seq;
        g_reqResendTries = same ? (uint8_t)(g_reqResendTries + 1u) : 1u;
        if (g_reqResendTries <= 50u) {
            g_reqResendNode = nodeId;
            g_reqResendSeq = seq;
            g_reqResendDueMs = millis() + 2u;
            g_reqResendPending = true;
        } else {
            g_reqResendPending = false;   /* queda el timeout normal */
        }
        return;
    }
    if (g_reqResendPending && g_reqResendNode == nodeId && g_reqResendSeq == seq) {
        g_reqResendPending = false;
    }
    MASTER_LOG_PRINTF("[MASTER] REQ_BATCH node=%d seq=%d err=%d txack=%lu/%lu\n", nodeId, seq, (int)err,
                      (unsigned long)espnowTxAckOk(), (unsigned long)espnowTxAckFail());
}

static uint16_t effectiveSampleRateHz()
{
    uint16_t minFs = 0;
    for (uint8_t i = 1; i <= NUM_SLAVES; i++) {
        if ((g_armAckMask & (1UL << i)) == 0u) continue;
        if (g_cachedHello[i].valid && g_cachedHello[i].sample_rate > 0 &&
            (minFs == 0u || g_cachedHello[i].sample_rate < minFs)) {
            minFs = g_cachedHello[i].sample_rate;
        }
    }
    if (minFs != 0u) return minFs;
    const uint8_t decim = (g_wantDecim >= 1u) ? g_wantDecim : 1u;
    const uint16_t fallback = (uint16_t)(ADC_SAMPLE_RATE_HZ / decim);
    return (fallback != 0u) ? fallback : 1u;
}

static uint32_t captureDurationMs(uint16_t nBatches)
{
    uint32_t samples = (uint32_t)nBatches * (uint32_t)(SAMPLES_PER_PART * 2);
    uint16_t fs = effectiveSampleRateHz();
    return (samples * 1000UL + (fs - 1)) / fs;
}

/* El softAP responde FTM para que los esclavos S3 midan RTT y desfasaje. En
 * el ESP32 clasico no existe y no se toca nada. */
static void apEnableFtmResponder()
{
#if defined(FTM_RESPONDER_DISABLE) && FTM_RESPONDER_DISABLE
    return;   /* banco: sin responder FTM */
#endif
#if defined(CONFIG_ESP_WIFI_FTM_RESPONDER_SUPPORT) && CONFIG_ESP_WIFI_FTM_RESPONDER_SUPPORT
    wifi_config_t ap_cfg = {};
    if (esp_wifi_get_config(WIFI_IF_AP, &ap_cfg) != ESP_OK) return;
    if (ap_cfg.ap.ftm_responder) return;
    ap_cfg.ap.ftm_responder = true;
    const esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    /* El responder se inicializa al arrancar el WiFi: con el AP ya levantado
     * el flag quedaba guardado pero sin efecto y los esclavos recibian
     * FTM_STATUS_CONF_REJECTED en toda sesion. Se reinicia el WiFi (antes de
     * ESP-NOW y del DNS, que se levantan despues). */
    /* FTM sensible al ancho: AP en HT20, igual que el iniciador. */
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    const esp_err_t e1 = esp_wifi_stop();
    const esp_err_t e2 = esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_max_tx_power(WIFI_TX_POWER_QDBM);
    MASTER_LOG_PRINTF("[MASTER] FTM responder err=%d restart=%d/%d\n", (int)err, (int)e1, (int)e2);
#endif
}

/* Ultimo reporte FTM de cada esclavo: el callback ESP-NOW lo copia y loop()
 * lo pasa a la web como JSON {"type":"ftm",...}. */
static MsgFtmReport  g_ftmLast[NUM_SLAVES + 1];
static volatile bool g_ftmNew[NUM_SLAVES + 1];

static uint32_t g_ftmLastMs[NUM_SLAVES + 1];

static void onFtmReport(const MsgFtmReport &m)
{
    if (m.node_id == 0 || m.node_id > NUM_SLAVES) return;
    g_ftmLast[m.node_id] = m;
    g_ftmLastMs[m.node_id] = millis();
    g_ftmNew[m.node_id] = true;
}

/* ── Sincronia por FTM (solo S3 / IDF 5) ─────────────────────────────────────
 * Baliza de tiempo: el reloj MAC del maestro, para que el esclavo resuelva la
 * epoca de T1/T4. START programado: si TODOS los esclavos de la captura tienen
 * modelo de reloj valido, el START lleva un instante futuro del reloj MAC del
 * maestro y cada esclavo sube SYNC en ese instante. */
#if ESP_ARDUINO_VERSION_MAJOR >= 3
#include <esp_private/wifi.h>
#define MASTER_FTM_SYNC 1
#else
#define MASTER_FTM_SYNC 0
#endif
static uint32_t g_runId = 0;
static constexpr uint32_t FTM_START_LEAD_US = 400000u;

static void serviceTimeBeacon()
{
#if MASTER_FTM_SYNC
    static uint32_t lastMs = 0;
    if (!g_espnowReady || millis() - lastMs < 500u) return;
    lastMs = millis();
    if (g_runId == 0) g_runId = esp_random() | 1u;
    MsgTimeBeacon b = { CMD_TIME_BEACON, g_runId, esp_wifi_internal_get_mac_clock_time() };
    geonetSend(ESPNOW_BROADCAST, (uint8_t *)&b, sizeof(b));
#endif
}

/* true si todos los esclavos de la captura informaron modelo valido hace poco. */
static bool ftmStartPosible(uint32_t mask)
{
#if MASTER_FTM_SYNC
    if (mask == 0u) return false;
    for (uint8_t n = 1; n <= NUM_SLAVES; n++) {
        if (!(mask & (1UL << n))) continue;
        if (!g_ftmLast[n].model_ok || millis() - g_ftmLastMs[n] > 4000u) return false;
    }
    return true;
#else
    (void)mask;
    return false;
#endif
}

static void serviceFtmReports()
{
    for (uint8_t n = 1; n <= NUM_SLAVES; n++) {
        if (!g_ftmNew[n]) continue;
        g_ftmNew[n] = false;
        const MsgFtmReport m = g_ftmLast[n];
        MASTER_LOG_PRINTF("[FTM] S%u st=%u ev=%u cfg=%u win=%u rtt=%lu ps ses=%u modelo=%u edad=%ld\n", (unsigned)n,
                          (unsigned)m.status, (unsigned)m.ev_status, (unsigned)m.cfg_idx,
                          (unsigned)m.window, (unsigned long)m.rtt_med_ps, (unsigned)m.sessions,
                          (unsigned)m.model_ok, (long)m.model_age_ms);
        char js[384];
        snprintf(js, sizeof(js),
                 "{\"type\":\"ftm\",\"node\":%u,\"status\":%u,\"sync_mode\":%u,"
                 "\"sessions\":%u,\"frames_total\":%u,\"frames_valid\":%u,\"window\":%u,"
                 "\"rtt_med_ps\":%lu,\"rtt_mad_ps\":%lu,\"rtt_p95_ps\":%lu,\"rssi_med\":%d,"
                 "\"dist_sdk_cm\":%lu,\"skew_ppb\":%ld,\"resid_ns\":%lu,\"age_ms\":%lu,"
                 "\"channel\":%u,\"init_err\":%d,\"ev_status\":%u,\"cfg_idx\":%u,\"model_ok\":%u}",
                 (unsigned)n, (unsigned)m.status, (unsigned)m.sync_mode,
                 (unsigned)m.sessions, (unsigned)m.frames_total, (unsigned)m.frames_valid,
                 (unsigned)m.window, (unsigned long)m.rtt_med_ps, (unsigned long)m.rtt_mad_ps,
                 (unsigned long)m.rtt_p95_ps, (int)m.rssi_med, (unsigned long)m.dist_est_cm,
                 (long)m.skew_ppb, (unsigned long)m.resid_ns, (unsigned long)m.age_ms,
                 (unsigned)m.channel, (int)m.init_err, (unsigned)m.ev_status, (unsigned)m.cfg_idx,
                 (unsigned)m.model_ok);
        webRelayText(js);
    }
}

static void setApBeaconInterval(uint16_t intervalTu, const char *reason)
{
    wifi_config_t ap_cfg = {};
    if (esp_wifi_get_config(WIFI_IF_AP, &ap_cfg) != ESP_OK) {
        MASTER_LOG_PRINTF("[MASTER] beacon cfg read fail (%s)\n", reason);
        return;
    }
    if (ap_cfg.ap.beacon_interval == intervalTu) return;
    ap_cfg.ap.beacon_interval = intervalTu;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    MASTER_LOG_PRINTF("[MASTER] beacon=%u TU (%s) err=%d\n",
                      intervalTu, reason, (int)err);
}

/* ── Beacon pause/resume — sin RF durante captura ─────────────────────────── */
static void beacon_pause(void)
{
#if AP_KEEP_BEACON_DURING_CAPTURE
    MASTER_LOG_PRINTLN("[MASTER] beacon sin cambios durante captura");
#else
    /* El SoftAP del maestro no puede estar conectado a telefonos sin beacon.
     * Durante captura lo dejamos lento para no volver al spike de ~10 Hz. */
    setApBeaconInterval(AP_BEACON_CAPTURE_INTERVAL_TU, "captura");
#endif
}

static void beacon_resume(void)
{
#if AP_KEEP_BEACON_DURING_CAPTURE
    /* Si no tocamos el beacon al entrar en captura, tampoco lo tocamos al salir:
     * evita reconfigurar el AP con clientes conectados. */
#else
    setApBeaconInterval(AP_BEACON_INTERVAL_TU, "dump/idle");
#endif
}

static void scheduleCaptureDumpTimeout(uint16_t nBatches, const char *reason)
{
    if (nBatches == 0) {
        g_captureDumpDueMs = 0;
        return;
    }
    /* captureDurationMs = tiempo ideal de muestreo (sin huecos). Con captura
     * encadenada el tiempo de pared agrega el volcado UART de cada trozo entre
     * corridas; ×3 da holgura de sobra. Igual el camino primario de cierre es
     * el ACK START-done del esclavo, no este timeout (solo rescate si se pierde
     * ese ACK). */
    /* Con duración por tipo, el rescate debe cubrir al nodo MÁS largo. */
    if (g_rec_n_batches_hammer > nBatches) {
        nBatches = g_rec_n_batches_hammer;
    }
    uint32_t durationMs = captureDurationMs(nBatches);
    uint32_t waitMs = durationMs * 3u + STORE_CAPTURE_MARGIN_MS;
    if (waitMs < STORE_CAPTURE_TIMEOUT_MIN_MS) waitMs = STORE_CAPTURE_TIMEOUT_MIN_MS;
    g_captureDumpDueMs = millis() + waitMs;
    MASTER_LOG_PRINTF("[MASTER] DUMP safety timeout in %u ms (n=%u, %s)\n",
                      (unsigned)waitMs, (unsigned)nBatches, reason);
    LOGM("DUMP_SAFETY_TIMEOUT", "reason=%s,n=%u,duration_ms=%u,wait_ms=%u",
         reason, nBatches, (unsigned)durationMs, (unsigned)waitMs);
}

static void scheduleStoreDump(uint16_t nBatches)
{
    if (nBatches == 0 || g_viewDump || g_armedCount == 0) {
        g_captureDumpDueMs = 0;
        return;
    }
    scheduleCaptureDumpTimeout(nBatches, "start");
}

static void beginDump(const char *reason)
{
    beacon_resume();
    g_captureDumpDueMs = 0;
    g_captureDoneMask = 0;
    g_dumpRetryPaused = false;
    g_dumpRetryDueMs = 0;
    g_dumpWaitingForWeb = false;
    /* VIEW dump: un solo nodo; normal dump: todos los esclavos armados */
    /* Primer nodo armado (por mask) para no iterar nodos inexistentes */
    uint8_t firstArmedNode = 1;
    if (!g_viewDump) {
        firstArmedNode = NUM_SLAVES + 1;
        for (uint8_t n = 1; n <= NUM_SLAVES; n++) {
            if (g_armAckMask & (1UL << n)) { firstArmedNode = n; break; }
        }
    }
    bool canDump = g_rec_n_batches > 0 && (g_armedCount > 0 || g_viewDump);
    if (canDump) {
        g_state        = DUMPING;
        g_streaming    = true;
        g_dumpSlaveIdx = g_viewDump ? g_viewNode : firstArmedNode;
        g_dumpBatchSeq = 0;
        g_dumpBatchRx  = false;
        dumpDeliveryGuardReset();
        g_dumpRetries  = 0;
        matlab.sendHeartbeat(0x00, 0, 0, (uint8_t)g_state);

        /* Encolar el dump en LittleFS ademas de espejarlo por WS. Es el buffer
         * que le falta al maestro para poder subir sin navegador.
         * Se encolan tambien los VIEW dumps: con un solo esclavo, VER es la
         * forma normal de capturar, y el dato es igual de real. El .geoq marca
         * cual es cual (flags bit0) y el server decide que hacer. */
        {
            LinkNodeInfo nodes[NUM_SLAVES];
            uint8_t nodeCount = g_viewDump
                              ? collectLinkNodeSingle(nodes, g_viewNode)
                              : collectLinkNodes(nodes, NUM_SLAVES);
            uint32_t expect = 0;
            for (uint8_t i = 0; i < nodeCount; i++) expect += (uint32_t)nodes[i].n_batches * 102u;
            linkQueueBegin(g_rec_n_batches, nodes, nodeCount, expect, g_viewDump);
        }

        g_dumpSkipStreak = 0;
        g_dumpWsWaitMs = millis();
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
        g_dumpRequiresWebClient = false;
        g_dumpWaitingForWeb = false;
        matlab.sendReady(g_armedCount);
        matlab.sendHeartbeat(0x00, 0, 0, (uint8_t)g_state);
        MASTER_LOG_PRINTF("[MASTER] %s\n", g_state == ARMED ? "ARMED" : "IDLE");
    }
}

static void sendStartProbe(uint8_t nodeId)
{
    const uint8_t *dst = espnowDstForSlave(nodeId);
    uint64_t t0 = (uint64_t)micros();
    MsgStart msg = { CMD_START, t0, (uint8_t)(0x80u | nodeId) };
    g_startProbeNode = nodeId;
    g_startCmdToken  = (uint32_t)t0;
    g_startCmdTxUs   = (uint32_t)micros();
    rememberStartProbe(nodeId, g_startCmdToken, g_startCmdTxUs);
    esp_err_t err = g_espnowReady
                  ? geonetSend(dst, (uint8_t *)&msg, sizeof(msg))
                  : ESP_FAIL;
    matlab.sendAck(nodeId, 0xAF, err == ESP_OK ? 1 : 0);
    MASTER_LOG_PRINTF("[MASTER] START_PROBE node=%d token=%u err=%d\n",
                      nodeId, (unsigned)g_startCmdToken, (int)err);
}

/* ── Comando dirigido a un esclavo específico ────────────────────────────── */

static void handleDirectedCmd(uint8_t node_id, uint8_t sub_cmd, uint8_t param,
                              uint8_t param2, bool hasParam2, bool fromWeb)
{
    if (node_id == 0 || node_id > NUM_SLAVES) {
        matlab.sendAck(node_id, sub_cmd, 0);
        return;
    }
    if (!g_espnowReady) {
        matlab.sendAck(node_id, sub_cmd, 0);
        return;
    }
    /* Captura en curso: el esclavo descartará el comando en silencio (RF
     * quieto durante muestreo) y la web quedaba esperando un ACK que nunca
     * llegaba. NACK inmediato para que la UI lo muestre y pueda reintentar. */
    if (g_state == RUNNING || g_state == PRESTART) {
        matlab.sendAck(node_id, sub_cmd, 0);
        MASTER_LOG_PRINTF("[MASTER] directed 0x%02X rechazado: captura en curso (state=%d)\n",
                          sub_cmd, (int)g_state);
        LOGM("DIRECTED_BUSY", "sub=0x%02X,state=%d", sub_cmd, (int)g_state);
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

    static constexpr uint8_t SERIAL_VDAC_FLAG = 0x40u;
    const CachedHello &hello = g_cachedHello[node_id];
    const bool serialAnalogModel = hello.valid &&
        (hello.link_model == SLAVE_LINK_SERIAL ||
         (hello.link_model == SLAVE_LINK_UNKNOWN && hello.hw_class == SLAVE_HW_HAMMER));

    /* Middleware de capacidades:
     * - 0xA8 (PGAout) solo existe en la electrónica I2C nueva.
     * - 0xAA con dos parámetros es IDAC firmado/por etapa (I2C).
     * - 0xAA con un parámetro conserva el VDAC global de los nodos SERIAL.
     * Nunca reenviar un IDAC moderno al PSoC serial: interpretaría p1 como
     * código VDAC y movería una referencia equivocada. */
    /* 0xA8 (PGAout) y 0xAB (forzar el lazo PI) solo existen en la electronica
     * I2C nueva: la SERIAL no tiene ni esa etapa ni lazo permanente. */
    if (serialAnalogModel && (sub_cmd == 0xA8u || sub_cmd == 0xABu)) {
        matlab.sendAck(node_id, sub_cmd, 0);
        LOGM("DIRECTED_UNSUPPORTED", "node=%u,link=serial,sub=0x%02X", node_id, sub_cmd);
        return;
    }

    if (sub_cmd == 0xAAu) {
        /* Por USB 0xAA siempre llega en 7 bytes, así que el VDAC serial se
         * codifica con p1=0x40 (bit reservado en el IDAC I2C) y p2=código.
         * La web puede seguir mandándolo sin param2. */
        if (serialAnalogModel && hasParam2 && param == SERIAL_VDAC_FLAG) {
            param = param2;
            param2 = 0;
            hasParam2 = false;
        }
        const uint8_t stage = (uint8_t)(param & 0x0Fu);
        const bool reservedBitsClear = (param & 0x70u) == 0u;
        const bool validSerialVdac = serialAnalogModel && !hasParam2;
        const bool validI2cIdac = !serialAnalogModel && hasParam2 &&
                                  stage < 4u && reservedBitsClear;
        if (!validSerialVdac && !validI2cIdac) {
            matlab.sendAck(node_id, sub_cmd, 0);
            LOGM("DIRECTED_BAD_ANALOG_REF", "node=%u,serial=%u,p1=%u,p2=%u,has2=%u",
                 node_id, (unsigned)serialAnalogModel, param, param2,
                 (unsigned)hasParam2);
            return;
        }
    }

    const uint8_t *dst = espnowDstForSlave(node_id);
    esp_err_t err;
    if (sub_cmd == 0xA7) {
        MsgDebugNode msg = { CMD_DEBUG_NODE, node_id, param };
        err = geonetSend(dst, (uint8_t *)&msg, sizeof(msg));
    } else if (sub_cmd == 0xB2) {
        /* "Ver": captura única de N lotes. El esclavo guarda en RAM (sin RF durante
         * el muestreo) y luego el maestro hace dump vía REQ_BATCH. */
        uint16_t n = nBatchesForNode(node_id);
        if (n == 0) n = 1;
        g_state = RUNNING;
        g_streaming = true;
        g_viewNode = node_id;
        g_viewBatchesRemaining = 0;  /* ya no se usa conteo live */
        g_viewDump = true;           /* dump de un solo nodo tras la captura */
        g_dumpSlaveIdx = 0;
        g_dumpBatchSeq = 0;
        g_dumpBatchRx = false;
        g_dumpRequiresWebClient = fromWeb;
        g_dumpWaitingForWeb = false;
        dumpDeliveryGuardReset();
        MsgView msg = { CMD_VIEW, node_id, n };
        err = geonetSend(dst, (uint8_t *)&msg, sizeof(msg));
        g_captureDumpDueMs = 0;      /* el dump arranca solo con VIEW_DONE del esclavo */
        g_captureDoneMask = 0;
        LOGM("VIEW", "node=%d,n=%u,store=1,wait_done=1", node_id, msg.n);
    } else if (sub_cmd == 0xB3) {
        MsgDebugPsoc msg = { CMD_DEBUG_PSOC, node_id, param };
        err = geonetSend(dst, (uint8_t *)&msg, sizeof(msg));
        LOGM("DBGPSOC", "node=%d,en=%d", node_id, param);
    } else if (sub_cmd == CMD_BLINK_LED) {
        /* LED blink: no pasa por PSoC, mensaje propio */
        MsgBlinkLed msg = { CMD_BLINK_LED, node_id };
        err = geonetSend(dst, (uint8_t *)&msg, sizeof(msg));
        LOGM("BLINK_LED", "node=%d", node_id);
    } else {
        /* SaveEEPROM (0xB6) y SelectStream (0xB7) van por MsgSetConfig igual que calibrate */
        MsgSetConfig msg = { CMD_SET_CONFIG, node_id, sub_cmd, param, param2 };
        err = geonetSend(dst, (uint8_t *)&msg, sizeof(msg));
    }
    if (err != ESP_OK) {
        matlab.sendAck(node_id, sub_cmd, 0);
    }
    /* La N es una propiedad del ARREGLO, no de cada placa: una captura
     * sincronizada exige que todos compartan Fs. El maestro se queda con la
     * última que se ordenó y desde ahí desempata (ver serviceSlaveAuthority). */
    if (sub_cmd == SUBCMD_DECIMATION && err == ESP_OK && param >= 1 && param <= 100) {
        if (g_wantDecim != param) {
            g_wantDecim = param;
            for (uint8_t i = 0; i <= NUM_SLAVES; i++) g_authorityTries[i] = 0;
        }
        g_authorityTries[node_id] = 0;
    }
    MASTER_LOG_PRINTF("[MASTER] directed node=%d sub=0x%02X p1=%d p2=%d err=%d\n",
                      node_id, sub_cmd, param, param2, (int)err);
}

/* ── Autoridad del maestro sobre la configuración de los esclavos ──────────
 * Un esclavo puede terminar con otra N que la ordenada: se reinició, entró a
 * la red después del último "Aplicar", o rechazó el config por estar ocupado.
 * El HELLO trae su Fs efectiva, así que acá se compara contra la N vigente y,
 * si difiere, el maestro se la vuelve a imponer. Se reintenta acotado: si tras
 * AUTHORITY_MAX_TRIES el nodo sigue en otra Fs, es que no puede cumplir (otra
 * frecuencia nativa, por ejemplo) y se deja de insistir hasta que alguien
 * ordene una N nueva — insistir para siempre sería ruido de radio inútil. */
#define AUTHORITY_RETRY_MS  5000u
#define AUTHORITY_MAX_TRIES 5

static uint16_t expectedFsForDecim(uint8_t decim)
{
    if (decim < 1) decim = 1;
    return (uint16_t)(ADC_SAMPLE_RATE_HZ / decim);
}

static void serviceSlaveAuthority()
{
    if (g_wantDecim == 0) return;                  /* nadie ordenó una N todavía */
    if (g_state != IDLE && g_state != ARMED) return;   /* config solo en reposo */

    const uint32_t now = millis();
    const uint16_t wantFs = expectedFsForDecim(g_wantDecim);
    for (uint8_t node = 1; node <= NUM_SLAVES; node++) {
        CachedHello &h = g_cachedHello[node];
        if (!h.valid || h.sample_rate == 0) continue;
        if (h.sample_rate == wantFs) { g_authorityTries[node] = 0; continue; }
        if (g_authorityTries[node] >= AUTHORITY_MAX_TRIES) continue;
        if (g_authorityNextMs[node] != 0 &&
            (int32_t)(now - g_authorityNextMs[node]) < 0) continue;

        g_authorityNextMs[node] = now + AUTHORITY_RETRY_MS;
        g_authorityTries[node]++;
        MASTER_LOG_PRINTF("[MASTER] autoridad node=%d: fs=%u != %u, impongo decim=%u (intento %u)\n",
                          node, h.sample_rate, wantFs, g_wantDecim,
                          (unsigned)g_authorityTries[node]);
        LOGM("AUTHORITY_DECIM", "node=%u,fs=%u,want_fs=%u,decim=%u,try=%u",
             (unsigned)node, (unsigned)h.sample_rate, (unsigned)wantFs,
             (unsigned)g_wantDecim, (unsigned)g_authorityTries[node]);
        handleDirectedCmd(node, SUBCMD_DECIMATION, g_wantDecim, 0, false, false);
        return;   /* uno por vuelta: no amontonar config en el aire */
    }
}

/* ── Broadcast ESP-NOW a todos los esclavos ──────────────────────────────── */

/* Duración por tipo: primero el broadcast con el largo canónico (GEO, llega a
 * todos incluso sin HELLO cacheado) y después un unicast que lo pisa SOLO en
 * los nodos HAMMER conocidos. AD=0 aplica el límite RAM automático de 512. */
static void broadcastRecordLength(uint16_t nBatches)
{
    MsgSetRecLen msg = { CMD_SET_RECLEN, nBatches };
    if (g_espnowReady) {
        geonetSend(ESPNOW_BROADCAST, (uint8_t *)&msg, sizeof(msg));
    }
    MASTER_LOG_PRINTF("[MASTER] SET_RECLEN=%u\n", nBatches);

    if (nBatches == 0 || !g_espnowReady) {
        return;
    }
    for (uint8_t node = 1; node <= NUM_SLAVES; node++) {
        uint16_t n = nBatchesForNode(node);
        if (n == nBatches) continue;
        MsgSetRecLen m = { CMD_SET_RECLEN, n };
        geonetSend(espnowDstForSlave(node), (uint8_t *)&m, sizeof(m));
        MASTER_LOG_PRINTF("[MASTER] SET_RECLEN node=%u n=%u (hammer)\n", node, n);
    }
}

/* Termina un volcado que no va a completarse nunca y devuelve el maestro a
 * reposo. Se usa cuando el esclavo no entrega lotes (capturó 0) o cuando no
 * queda cliente web al que entregárselos: en los dos casos seguir reintentando
 * solo sirve para que el maestro deje de atender todo lo demás. */
static void abortDump(const char *reason)
{
    MASTER_LOG_PRINTF("[MASTER] DUMP abortado: %s (node=%d seq=%d)\n",
                      reason, g_dumpSlaveIdx, g_dumpBatchSeq);
    LOGM("DUMP_ABORT", "node=%u,seq=%u,reason=%s",
         (unsigned)g_dumpSlaveIdx, (unsigned)g_dumpBatchSeq, reason);
    broadcastRecordLength(0);
    g_viewDump = false;
    g_viewNode = 0;
    g_state = armedOrIdleState();
    g_streaming = false;
    g_rec_n_batches = 0;
    g_captureDumpDueMs = 0;
    g_captureDoneMask = 0;
    g_dumpBatchSeq = 0;
    g_dumpRetries = 0;
    g_dumpRetryPaused = false;
    g_dumpRequiresWebClient = false;
    g_dumpWaitingForWeb = false;
    g_dumpSkipStreak = 0;
    g_dumpWsWaitMs = 0;
    dumpDeliveryGuardReset();
    linkQueueEnd(false);   /* sesión incompleta: no queda en la cola */
    matlab.sendReady(g_armedCount);
    matlab.sendHeartbeat(0x00, 0, 0, (uint8_t)g_state);
}

static void broadcastPrestart(uint16_t nBatches)
{
    /* Los modos silenciosos no conviven con el tiempo real: el esclavo los
     * anula igual, pero se avisa aca para que quede en el log. */
    if (g_syncMode != SYNCMODE_FTM_ACTIVE && capModeRealtime()) {
        MASTER_LOG_PRINTF("[SYNC] modo %u ignorado en tiempo real\n", (unsigned)g_syncMode);
    }
    MsgPrestart msg = { CMD_PRESTART, nBatches, g_capMode, g_syncMode };
    if (g_espnowReady) {
        geonetSend(ESPNOW_BROADCAST, (uint8_t *)&msg, sizeof(msg));
    }
    MASTER_LOG_PRINTF("[MASTER] PRESTART sent n=%u\n", nBatches);

    if (nBatches == 0 || !g_espnowReady) {
        return;
    }
    for (uint8_t node = 1; node <= NUM_SLAVES; node++) {
        uint16_t n = nBatchesForNode(node);
        if (n == nBatches) continue;
        MsgPrestart m = { CMD_PRESTART, n, g_capMode, g_syncMode };
        geonetSend(espnowDstForSlave(node), (uint8_t *)&m, sizeof(m));
        MASTER_LOG_PRINTF("[MASTER] PRESTART node=%u n=%u (hammer)\n", node, n);
    }
}

static void sendHotWaitQuery(uint8_t nodeId)
{
    const uint8_t *dst = espnowDstForSlave(nodeId);
    MsgHotWaitQuery msg = { CMD_HOTWAIT_QUERY, nodeId };
    if (g_espnowReady) {
        geonetSend(dst, (uint8_t *)&msg, sizeof(msg));
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
        geonetSend(ESPNOW_BROADCAST, (uint8_t *)&msg, sizeof(msg));
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
    MsgStart msg = { CMD_START, g_t_start_us, 0, 0, 0 };
#if MASTER_FTM_SYNC
    if (ftmStartPosible(g_armAckMask)) {
        msg.sched = 1;
        msg.start_mac_us = esp_wifi_internal_get_mac_clock_time() + FTM_START_LEAD_US;
    }
#endif
    if (g_espnowReady) {
        g_startCmdTxUs = (uint32_t)micros();
        geonetSend(ESPNOW_BROADCAST, (uint8_t *)&msg, sizeof(msg));
    }
    {
        char js[160];
        snprintf(js, sizeof(js), "{\"type\":\"sync_start\",\"method\":\"%s\",\"start_mac_us\":%lu,\"lead_us\":%lu}",
                 msg.sched ? "ftm_scheduled" : "broadcast_start", (unsigned long)msg.start_mac_us,
                 (unsigned long)(msg.sched ? FTM_START_LEAD_US : 0u));
        webRelayText(js);
    }
    MASTER_LOG_PRINTF("[MASTER] START t0=%llu ftm=%u mac=%lu\n", g_t_start_us, (unsigned)msg.sched,
                      (unsigned long)msg.start_mac_us);
}

static void broadcastScopeStart()
{
    debugEspHardwareStartPulse();
    MsgScopeStart msg = { CMD_SCOPE_START };
    if (g_espnowReady) {
        geonetSend(ESPNOW_BROADCAST, (uint8_t *)&msg, sizeof(msg));
    }
    MASTER_LOG_PRINTLN("[MASTER] SCOPE_START");
}

static void broadcastDebug(uint8_t enable)
{
    MsgDebug msg = { CMD_DEBUG, enable ? (uint8_t)1 : (uint8_t)0 };
    if (g_espnowReady) {
        geonetSend(ESPNOW_BROADCAST, (uint8_t *)&msg, sizeof(msg));
    }
    MASTER_LOG_PRINTF("[MASTER] DEBUG broadcast=%u\n", (unsigned)enable);
}

static void broadcastStop()
{
    syncOutWrite(LOW);
    MsgStop msg = { CMD_STOP };
    if (g_espnowReady) {
        geonetSend(ESPNOW_BROADCAST, (uint8_t *)&msg, sizeof(msg));
    }
    MASTER_LOG_PRINTLN("[MASTER] STOP enviado");
}

/* Abort limpio de una captura en curso cuando un esclavo la rechaza (START
 * status=0 o HOTWAIT_ACK ok=0 del watchdog del esclavo). Antes el maestro
 * quedaba en RUNNING hasta el timeout de rescate y después intentaba dumpear
 * lotes inexistentes. Vuelve a ARMED/IDLE y avisa a la web con CMD_START ok=0
 * (la UI ya imprime "START falló"). */
static void abortCaptureToArmed(uint8_t nodeId, const char *reason)
{
    if (g_state != RUNNING && g_state != PRESTART) return;
    MASTER_LOG_PRINTF("[MASTER] capture abort node=%d: %s\n", nodeId, reason);
    LOGM("CAPTURE_ABORT", "node=%d,reason=%s", nodeId, reason);
    beacon_resume();
    broadcastStop();          /* SYNC low + STOP a los esclavos */
    g_streaming = false;
    g_captureDumpDueMs = 0;
    g_captureDoneMask = 0;
    g_dumpRetryPaused = false;
    g_dumpRequiresWebClient = false;
    g_dumpWaitingForWeb = false;
    g_viewDump = false;
    g_viewNode = 0;
    dumpDeliveryGuardReset();
    g_state = armedOrIdleState();
    matlab.sendAck(nodeId, CMD_START, 0);
    matlab.sendReady(g_armedCount);
    matlab.sendHeartbeat(0x00, 0, 0, (uint8_t)g_state);
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
    memset(g_effNBatches, 0, sizeof(g_effNBatches));
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
    g_captureDoneMask = 0;
    g_dumpRetryPaused = false;
    g_dumpRequiresWebClient = false;
    g_dumpWaitingForWeb = false;
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
            const uint8_t *dst = espnowDstForSlave(g_viewNode);
            MsgStart msg = { CMD_START, g_t_start_us, g_viewNode };
            if (g_espnowReady) {
                g_startCmdTxUs = (uint32_t)micros();
                geonetSend(dst, (uint8_t *)&msg, sizeof(msg));
            }
        }
        scheduleCaptureDumpTimeout(g_rec_n_batches, "view");
        g_captureDoneMask = 0;
        g_state = RUNNING;
        matlab.sendHeartbeat(0x00, 0, 0, (uint8_t)g_state);
        MASTER_LOG_PRINTF("[MASTER] VER HOT_WAIT done -> CMD_START node=%d\n", g_viewNode);
        LOGM("VER_START", "node=%d", g_viewNode);
    } else {
        /* No modificar g_armedCount: intentar dump desde todos los slaves ARM'd.
           Los que no grabaron responderán NACK y se saltean via retry. */
        /* Los nodos que no llegaron a HOT_WAIT no graban: fuera de la captura,
         * si no el fin (todos los armados completos) y el tiempo real los
         * esperan hasta el timeout. */
        if ((g_armAckMask & g_hotWaitAckMask) != 0u && (g_armAckMask & ~g_hotWaitAckMask) != 0u) {
            MASTER_LOG_PRINTF("[MASTER] fuera de la captura (sin HOT_WAIT): mask=0x%08lX\n",
                              (unsigned long)(g_armAckMask & ~g_hotWaitAckMask));
            g_armAckMask &= g_hotWaitAckMask;
        }
        g_armedCount = countArmAcks(g_armAckMask);
        if ((g_rec_n_batches == 0 && !capModeRealtime()) || g_armedCount == 0 || g_armAckMask == 0) {
            MASTER_LOG_PRINTF("[MASTER] START rejected after PRESTART n=%u armed=%u mask=0x%08lX\n",
                              g_rec_n_batches, g_armedCount,
                              (unsigned long)g_armAckMask);
            LOGM("START_REJECT_PRESTART", "n=%u,armed=%u,mask=0x%08lX",
                 g_rec_n_batches, g_armedCount, (unsigned long)g_armAckMask);
            abortPrestart("no armed slaves");
            return;
        }
        g_captureDoneMask = 0;
        matlab.sendReady(g_armedCount);
        if (capModeStreams()) {
            /* Antes del START: los primeros lotes llegan enseguida. */
            g_rtStopReqMs = 0;
            rtarc::start(g_armAckMask);
            if (!g_rt.start(g_armAckMask)) {
                MASTER_LOG_PRINTLN("[MASTER] RT sin RAM: captura cancelada");
                abortCaptureToArmed(0, "rt_no_ram");
                return;
            }
        }
        broadcastStart();
        if (!capModeStreams()) scheduleStoreDump(g_rec_n_batches);
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
        if (g_dumpRequiresWebClient && !webRelayHasActiveClient()) {
            if (!g_dumpWaitingForWeb) {
                MASTER_LOG_PRINTF("[MASTER] DUMP pause: esperando reconexion WS node=%d seq=%d\n",
                                  g_dumpSlaveIdx, g_dumpBatchSeq);
                LOGM("DUMP_WAIT_WS", "node=%u,seq=%u", g_dumpSlaveIdx, g_dumpBatchSeq);
            }
            g_dumpWaitingForWeb = true;
            return;
        }
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

    const bool zweb = webRelayZEnabled();
    matlab.setBatchRelayEnabled(!zweb);
    /* Reenviar las 30 muestras (post_digital) al MATLAB. */
    for (int i = 0; i < SAMPLES_PER_PART * 2; i++) {
        const SampleBytes &s = batch.samples[i];
        int32_t val = (int32_t)s.digi0
                    | ((int32_t)s.digi1 << 8)
                    | ((int32_t)s.digi2 << 16);
        if (val & 0x800000) val |= (int32_t)0xFF000000;
        if (s.flags != 0u) matlab.sendSampleMark(batch.node_id, s.flags);
        matlab.sendSample(batch.node_id, val);
        samplePulse();
    }
    matlab.setBatchRelayEnabled(true);
    if (zweb) {
        int32_t values[GEOZ_SAMPLES];
        uint8_t flags[GEOZ_SAMPLES], payload[GEOZ_MAX_BYTES];
        for (int i = 0; i < GEOZ_SAMPLES; i++) {
            const SampleBytes &s = batch.samples[i];
            values[i] = geoz_sext24((uint32_t)s.digi0 | ((uint32_t)s.digi1 << 8) |
                                    ((uint32_t)s.digi2 << 16));
            flags[i] = s.flags;
        }
        const size_t len = geoz_encode(values, flags, payload);
        if (len > 0u) webRelayZ(batch.node_id, batch.seq, payload, (uint8_t)len);
    }

    /* Durante dump: marcar batch como recibido si es el que esperábamos */
    if (g_state == DUMPING) {
        /* Tee a la cola de ENLACE: mismo lote crudo que ya fue al WS. Se guarda
         * empaquetado tal cual vino (3 bytes por muestra); la conversion a f32
         * y el armado de carpetas los hace el sink en Python, no el firmware. */
        if (linkQueueActive()) {
            uint8_t packed[SAMPLES_PER_PART * 2 * 3];
            for (int i = 0; i < SAMPLES_PER_PART * 2; i++) {
                packed[i * 3 + 0] = batch.samples[i].digi0;
                packed[i * 3 + 1] = batch.samples[i].digi1;
                packed[i * 3 + 2] = batch.samples[i].digi2;
            }
            linkQueueBatch(batch.node_id, batch.seq, batch.global_flags,
                           batch.timestamp_us, packed);
        }
        g_dumpBatchRx = true;
        g_dumpWaitingForWeb = false;
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

/* ── Tiempo real ─────────────────────────────────────────────────────────── */

/* Mismo envio que onBatchReady: 30 muestras de 24 bits por lote. */
/* Un lote con su numero; tambien lo usa el relleno de huecos (FILL). */
static void emitNumberedBatch(uint8_t node, const SampleBytes *s, uint32_t seq)
{
    const bool zweb = webRelayZEnabled();
    matlab.setBatchRelayEnabled(!zweb);
    matlab.sendBatchHeader(node, seq);
    for (int i = 0; i < SAMPLES_PER_PART * 2; i++) {
        int32_t val = (int32_t)s[i].digi0 | ((int32_t)s[i].digi1 << 8) |
                      ((int32_t)s[i].digi2 << 16);
        if (val & 0x800000) val |= (int32_t)0xFF000000;
        if (s[i].flags != 0u) matlab.sendSampleMark(node, s[i].flags);
        matlab.sendSample(node, val);
    }
    matlab.setBatchRelayEnabled(true);
    if (zweb) {
        int32_t values[GEOZ_SAMPLES];
        uint8_t flags[GEOZ_SAMPLES], payload[GEOZ_MAX_BYTES];
        for (int i = 0; i < GEOZ_SAMPLES; i++) {
            values[i] = geoz_sext24((uint32_t)s[i].digi0 | ((uint32_t)s[i].digi1 << 8) |
                                    ((uint32_t)s[i].digi2 << 16));
            flags[i] = s[i].flags;
        }
        const size_t len = geoz_encode(values, flags, payload);
        if (len > 0u) webRelayZ(node, seq, payload, (uint8_t)len);
    }
}

/* Fin del tiempo real: cada nodo avisa cuantos lotes tuvo, asi la web
 * detecta tambien los que faltan al final. */
static void rtSendEnds()
{
    for (uint8_t n = 1; n <= NUM_SLAVES; n++) {
        if (g_rt.nodeOn(n)) matlab.sendBatchEnd(n, g_rt.delivered(n));
    }
}

/* Atiende los FILL de la web desde la copia, sin ahogar el tiempo real. */
static void serviceWebFills()
{
    rtarc::Fill f;
    if (!rtarc::peekFill(f)) return;
    const uint16_t budget = g_rt.active() ? 2u : 16u;
    uint16_t done = 0;
    static SampleBytes buf[SAMPLES_PER_PART * 2];
    while (done < f.count && done < budget) {
        if (!webRelayReadyForDumpRequest(false)) break;
        const uint32_t seq = f.seq + done;
        if (rtarc::get(f.node, seq, buf)) {
            emitNumberedBatch(f.node, buf, seq);
        } else {
            char msg[64];
            snprintf(msg, sizeof(msg), "{\"type\":\"fill_miss\",\"node\":%u,\"seq\":%lu}",
                     (unsigned)f.node, (unsigned long)seq);
            webRelayText(msg);
        }
        done++;
    }
    if (done) rtarc::advanceFill(done);
}

/* Banco: 1 de cada N lotes RT no sale (queda en la copia) para probar el
 * relleno de huecos de la web. 0 = apagado. */
static uint32_t g_dropTestEvery = 0;
static void setDropTest(uint32_t every) { g_dropTestEvery = every; }

static void rtEmit(uint8_t node, const SampleBytes *s, uint32_t seq)
{
    rtarc::put(node, seq, s);
    if (g_dropTestEvery > 1u && (seq % g_dropTestEvery) == (g_dropTestEvery - 1u)) return;
    emitNumberedBatch(node, s, seq);
}

/* Contrapresion solo si hay un navegador recibiendo: uno conectado pero
 * callado no puede frenar la entrega por USB (lo que no le llega lo
 * recupera de la copia .geoq, como en el volcado). */
static bool rtReady()
{
    /* Contrapresion de la web, pero con limite: una pestana en segundo plano
     * (Chrome la frena) o un socket medio muerto que el maestro cree vivo
     * congelaban el tiempo real para siempre (2026-10-23). Pasados 10 s sin
     * poder entregar se sigue igual; la copia .geoq del maestro guarda todo. */
    static uint32_t s_blockedSinceMs = 0;
    static bool s_warned = false;
    if (!webRelayHasActiveClient() || webRelayReadyForDumpRequest(false)) {
        s_blockedSinceMs = 0;
        s_warned = false;
        return true;
    }
    const uint32_t now = millis();
    if (s_blockedSinceMs == 0) s_blockedSinceMs = now | 1u;
    if ((now - s_blockedSinceMs) < 10000u) return false;
    if (!s_warned) {
        s_warned = true;
        MASTER_LOG_PRINTLN("[MASTER] RT: la web no consume hace 10 s; entrego sin esperarla");
    }
    return true;
}

/* STREAM_ACK que no salio por la cola de TX llena (ESPNOW_NO_MEM, la ocupa el
 * WebSocket): se reintenta desde loop(). Sin esto el esclavo no veia avanzar
 * la confirmacion, se le llenaba la ventana y frenaba: con la web conectada
 * el tiempo real no pasaba de ~16 lotes/s por nodo (2026-10-24). */
static MsgStreamAck g_ackPending[NUM_SLAVES + 1];
static bool         g_ackPendingOn[NUM_SLAVES + 1];

static void rtAck(uint8_t node, uint32_t next, uint32_t sack)
{
    if (node == 0 || node > NUM_SLAVES) return;
    MsgStreamAck a = { CMD_STREAM_ACK, node, next, sack };
    esp_err_t err = ESP_FAIL;
    if (g_espnowReady) err = geonetSend(espnowDstForSlave(node), (uint8_t *)&a, sizeof(a));
    g_ackPending[node] = a;
    g_ackPendingOn[node] = (err == ESP_ERR_ESPNOW_NO_MEM);
}

static void serviceAckResend()
{
    for (uint8_t n = 1; n <= NUM_SLAVES; n++) {
        if (!g_ackPendingOn[n] || !g_espnowReady) continue;
        const esp_err_t err = geonetSend(espnowDstForSlave(n), (uint8_t *)&g_ackPending[n],
                                         sizeof(g_ackPending[n]));
        g_ackPendingOn[n] = (err == ESP_ERR_ESPNOW_NO_MEM);
    }
}

static void rtOnData(const uint8_t *data, int len)
{
    if (g_state == DUMPING && len >= MSG_DATAZ_HEADER && data[0] == DATAZ_PKT_MARKER) {
        const MsgDataZ *z = (const MsgDataZ *)data;
        if (z->node_id != g_dumpSlaveIdx || z->seq != g_dumpBatchSeq ||
            z->len == 0u || z->len > MSG_DATAZ_MAX ||
            len != MSG_DATAZ_HEADER + z->len || z->crc != dataZCrc(z)) {
            return;
        }
        int32_t values[GEOZ_SAMPLES];
        uint8_t flags[GEOZ_SAMPLES];
        if (!geoz_decode(z->data, z->len, values, flags)) return;
        ReassembledBatch batch = {};
        batch.node_id = z->node_id;
        batch.seq = (uint16_t)z->seq;
        batch.active = true;
        batch.part_rx[0] = batch.part_rx[1] = true;
        for (int i = 0; i < GEOZ_SAMPLES; i++) {
            const uint32_t v = (uint32_t)values[i] & 0xFFFFFFu;
            batch.samples[i].digi0 = (uint8_t)v;
            batch.samples[i].digi1 = (uint8_t)(v >> 8);
            batch.samples[i].digi2 = (uint8_t)(v >> 16);
            batch.samples[i].flags = flags[i];
        }
        /* No tocar LittleFS ni WebSocket desde la tarea WiFi. */
        espnowRx.enqueueBatch(batch);
        return;
    }
    g_rt.onFrame(data, len);
}
static void rtOnEnd(const MsgStreamEnd &e) { g_rt.onEnd(e); }

/* ── Procesar comandos de MATLAB ─────────────────────────────────────────── */

static void handleMatlabCmd(const MatlabTransport::RxCmd &rxCmd, bool fromWeb = false)
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
                    g_captureDoneMask = 0;
                    g_dumpRetryPaused  = false;
                    g_dumpRequiresWebClient = false;
                    g_dumpWaitingForWeb = false;
                } else {
                    g_armedCount = countArmAcks(g_armAckMask);
                }
                g_state = ARMING;
                broadcastArm();
                matlab.sendAck(0xFF, cmd, 0);
            }
            break;

        case 0xA3: { /* START — N de 16 bits en rxCmd.value */
            uint16_t requestedBatches = clampRecordBatches(rxCmd.value);
            g_armedCount = countArmAcks(g_armAckMask);
            bool stateOk = (g_state == IDLE || g_state == ARMED || g_state == ARMING);
            /* N = 0 en tiempo real es "sin limite": termina con STOP. */
            const bool nOk = requestedBatches != 0 || capModeRealtime();
            if (!stateOk || !nOk || g_armedCount == 0 || g_armAckMask == 0) {
                MASTER_LOG_PRINTF("[MASTER] START rejected state=%d n=%u armed=%u mask=0x%08lX\n",
                                  (int)g_state, requestedBatches, g_armedCount,
                                  (unsigned long)g_armAckMask);
                LOGM("START_REJECT", "state=%d,n=%u,armed=%u,mask=0x%08lX",
                     (int)g_state, requestedBatches, g_armedCount,
                     (unsigned long)g_armAckMask);
                if (g_state == IDLE || g_state == ARMED || g_state == ARMING) {
                    g_state = armedOrIdleState();
                    matlab.sendReady(g_armedCount);
                    matlab.sendHeartbeat(0x00, 0, 0, (uint8_t)g_state);
                }
                matlab.sendAck(0xFF, cmd, 0);
                break;
            }
            g_viewNode = 0;
            g_viewBatchesRemaining = 0;
            g_viewDump = false;
            g_rec_n_batches = requestedBatches;
            g_captureDoneMask = 0;
            g_captureDumpDueMs = 0;
            g_debugRamp = 0;
            g_streaming = true;
            g_dumpRequiresWebClient = fromWeb;
            g_dumpWaitingForWeb = false;
            beginPrestart(PRESTART_ACTION_START);
            matlab.sendAck(0xFF, cmd, 1);
            break;
        }

        case 0xA4:   /* STOP */
            if (capModeStreams()) {
                /* Camino stream: los esclavos cortan y siguen pasando lo
                 * pendiente; se cierra con sus STREAM_END, sin volcado. */
                broadcastStop();
                if (g_rt.active()) g_rtStopReqMs = millis() | 1u;
                matlab.sendAck(0xFF, cmd, 0);
                break;
            }
            g_viewNode = 0;
            g_viewBatchesRemaining = 0;
            g_captureDumpDueMs = 0;
            g_captureDoneMask = 0;
            g_dumpRetryPaused = false;
            if (fromWeb) g_dumpRequiresWebClient = true;
            g_dumpWaitingForWeb = false;
            broadcastStop();
            g_state = STOPPING;
            matlab.sendAck(0xFF, cmd, 0);
            break;

        case 0xAF:   /* modo de captura: CAPMODE_* */
            if (g_state == IDLE || g_state == ARMED || g_state == ARMING) {
                g_capMode = param & (CAPMODE_STORAGE_MASK | CAPMODE_REALTIME |
                                     CAPMODE_STREAM_DUMP);
            }
            matlab.sendAck(0xFF, cmd, g_capMode);
            MASTER_LOG_PRINTF("[MASTER] capmode=0x%02X\n", g_capMode);
            break;

        case 0xB8:   /* modo de sincronizacion: SYNCMODE_* */
            if ((g_state == IDLE || g_state == ARMED || g_state == ARMING) &&
                param <= SYNCMODE_RADIO_OFF) {
                g_syncMode = (uint8_t)param;
            }
            matlab.sendAck(0xFF, cmd, g_syncMode);
            MASTER_LOG_PRINTF("[MASTER] syncmode=%u\n", (unsigned)g_syncMode);
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
            handleDirectedCmd(rxCmd.node_id, rxCmd.sub_cmd, rxCmd.param,
                              rxCmd.param2, rxCmd.has_param2, fromWeb);
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
                g_captureDoneMask = 0;
                g_dumpRetryPaused = false;
                g_dumpRequiresWebClient = false;
                g_dumpWaitingForWeb = false;
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

        case 0xAD: {  /* SET_RECORD_LEN_HAMMER: largo propio de nodos HAMMER
                       * (16 bits). 0 = volver al largo único (canónico). */
            g_rec_n_batches_hammer = clampRamRecordBatches(rxCmd.value);
            if (g_rec_n_batches > 0) {
                /* Refrescar los largos ya repartidos con el override nuevo. */
                broadcastRecordLength(g_rec_n_batches);
            }
            matlab.sendAck(0xFF, 0xAD, (uint8_t)(g_rec_n_batches_hammer & 0xFF));
            MASTER_LOG_PRINTF("[MASTER] SET_RECLEN_HAMMER=%u\n", g_rec_n_batches_hammer);
            break;
        }

        case 0xC1:   /* RFBURST: N unidades de 10 ms; 0 = parar */
            armRfBurst(rxCmd.value);
            matlab.sendAck(0xFF, 0xC1, (uint8_t)(rxCmd.value & 0xFFu));
            break;

        case 0xC0: {  /* ENLACE: param=1 subir ahora, param=0 auto on/off por value */
            if (param == 0) {
                /* Configurar el trigger automatico (por captura) sin tocar el
                 * resto de la config, que se edita por HTTP. */
                g_linkCfg.auto_upload = (rxCmd.value != 0);
                linkConfigSave();
                MASTER_LOG_PRINTF("[ENLACE] auto=%d\n", g_linkCfg.auto_upload ? 1 : 0);
                matlab.sendAck(0xFF, 0xC0, g_linkCfg.auto_upload ? 1 : 0);
            } else {
                const char *why = "";
                bool ok = requestEnlace(&why);
                if (!ok) MASTER_LOG_PRINTF("[ENLACE] rechazado: %s\n", why);
                matlab.sendAck(0xFF, 0xC0, ok ? 1 : 0);
            }
            break;
        }

        default:
            break;
    }
}

/* ── Radio: bajar/levantar la fase CAPTURA (para el modo ENLACE) ─────────── */

/* Levanta el AP en canal 1 + DNS captivo + ESP-NOW, exactamente como setup().
 * Se usa al volver de ENLACE. Los esclavos no negocian nada: tienen el canal 1
 * hardcodeado, asi que apenas el AP vuelve a canal 1 el ESP-NOW retoma solo. */
static void radioBringUpCapture()
{
    WiFi.mode(WIFI_AP_STA);
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1),
                      IPAddress(192, 168, 4, 1),
                      IPAddress(255, 255, 255, 0));
    bool apOk = WiFi.softAP(AP_SSID, AP_PASS, 1);   /* canal 1 fijo */
    esp_wifi_set_max_tx_power(WIFI_TX_POWER_QDBM);
    if (apOk) apEnableFtmResponder();
    if (apOk) {
        dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));
    }
    setApBeaconInterval(AP_BEACON_INTERVAL_TU, "post-enlace");

    g_espnowReady = espnowRx.begin(onBatchReady, onArmAck, onSlaveStatus, onCfgAck,
                                   onHello, onStartAck, onHotWaitAck);
    if (g_espnowReady) {
        addPeerIfNeeded(ESPNOW_BROADCAST);
        for (int i = 0; i < NUM_SLAVES; i++) {
            if (isConfiguredSlaveMac(SLAVE_MACS[i])) {
                addPeerIfNeeded(SLAVE_MACS[i]);
            }
        }
    }
    MASTER_LOG_PRINTF("[MASTER] CAPTURA restaurada ap=%s espnow=%s\n",
                      apOk ? "OK" : "FAIL", g_espnowReady ? "OK" : "FAIL");
    LOGM("ENLACE_RESTORE", "ap=%d,espnow=%d", apOk ? 1 : 0, g_espnowReady ? 1 : 0);
    matlab.sendStatus(g_espnowReady ? 1 : 0, 1);
}

/* Apaga ESP-NOW antes de irse a otro canal: los peers quedan atados a la
 * interfaz y al canal actual, y dejarlos vivos mientras la STA se asocia a otra
 * red solo genera TX fallidos. El AP lo baja link_mode.h. */
static void radioTearDownCapture()
{
    dnsServer.stop();
    esp_now_deinit();
    g_espnowReady = false;
    MASTER_LOG_PRINTLN("[MASTER] CAPTURA abajo (ESP-NOW off, AP off)");
}

/* Arma la tabla de nodos del .geoq desde el cache de HELLO: rol (GEO/HAMMER),
 * fs real del PSoC, largo propio del nodo y MAC. Es lo que le permite al sink
 * escribir el metadata.json sin que haya un navegador conectado. */
static uint8_t collectLinkNodes(LinkNodeInfo *out, uint8_t maxNodes)
{
    uint8_t n = 0;
    for (uint8_t id = 1; id <= NUM_SLAVES && n < maxNodes; id++) {
        if (!(g_armAckMask & (1UL << id))) continue;
        const CachedHello &h = g_cachedHello[id];
        LinkNodeInfo &e = out[n++];
        e.node_id     = id;
        e.hw_class    = h.valid ? h.hw_class : 0;
        e.sample_rate = h.valid ? h.sample_rate : 0;
        e.n_batches   = dumpBatchesForNode(id);
        e.psoc_ok     = h.valid ? (h.psoc_ok != 0) : false;
        e.sd_present  = h.valid ? (h.sd_present != 0) : false;
        memcpy(e.mac, h.mac, 6);
    }
    return n;
}

/* Igual que collectLinkNodes pero para un VIEW dump: un solo nodo, que puede no
 * estar en g_armAckMask (VER no requiere ARM). */
static uint8_t collectLinkNodeSingle(LinkNodeInfo *out, uint8_t nodeId)
{
    if (nodeId == 0 || nodeId > NUM_SLAVES) return 0;
    const CachedHello &h = g_cachedHello[nodeId];
    LinkNodeInfo &e = out[0];
    e.node_id     = nodeId;
    e.hw_class    = h.valid ? h.hw_class : 0;
    e.sample_rate = h.valid ? h.sample_rate : 0;
    e.n_batches   = dumpBatchesForNode(nodeId);
    e.psoc_ok     = h.valid ? (h.psoc_ok != 0) : false;
    e.sd_present  = h.valid ? (h.sd_present != 0) : false;
    memcpy(e.mac, h.mac, 6);
    return 1;
}

/* Pide entrar en ENLACE. Regla dura del diseño: solo desde IDLE/ARMED. */
static bool requestEnlace(const char **why)
{
    if (g_state != IDLE && g_state != ARMED) {
        if (why) *why = "solo desde IDLE/ARMED";
        return false;
    }
    if (!linkRequestUpload(why)) return false;
    g_state = ENLACE;
    matlab.sendHeartbeat(0x00, 0, 0, (uint8_t)g_state);
    return true;
}

/* ── Setup ───────────────────────────────────────────────────────────────── */

/* Apoyo de ota_master.h. */
/* FIR del PSoC desde la web: 128 taps de 3 bytes (hex) -> dos tramas de 64 y
 * el cierre. El ACK vuelve como CFG_ACK sub 0xCB del nodo. */
static void sendFirToNode(uint8_t node, uint8_t action, const String &hex)
{
    if (!g_espnowReady || node == 0 || node > NUM_SLAVES) return;
    const uint8_t *dst = espnowDstForSlave(node);
    uint8_t bytes[FIR_HW_TAPS * 3] = {0};
    uint16_t sum = 0;
    if (action != 0u) {
        if (hex.length() != FIR_HW_TAPS * 6) {
            MASTER_LOG_PRINTF("[FIR] largo hex %u invalido\n", (unsigned)hex.length());
            return;
        }
        for (int i = 0; i < FIR_HW_TAPS * 3; i++) {
            bytes[i] = (uint8_t)strtoul(hex.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
            sum = (uint16_t)(sum + bytes[i]);
        }
        for (int first = 0; first < FIR_HW_TAPS; first += FIR_HW_CHUNK) {
            MsgFirCoeffs m = {};
            m.cmd = CMD_FIR_COEFFS;
            m.node_id = node;
            m.first = (uint8_t)first;
            m.count = FIR_HW_CHUNK;
            memcpy(m.coef, &bytes[first * 3], FIR_HW_CHUNK * 3);
            geonetSend(dst, (const uint8_t *)&m, sizeof(m));
            delay(5);
        }
    }
    MsgFirCommit c = { CMD_FIR_COMMIT, node, action, sum };
    geonetSend(dst, (const uint8_t *)&c, sizeof(c));
    MASTER_LOG_PRINTF("[FIR] S%u accion %u suma %u\n", (unsigned)node, (unsigned)action, (unsigned)sum);
}

static bool otaSendToNode(uint8_t node, const uint8_t *data, size_t len)
{
    if (!g_espnowReady || node == 0 || node > NUM_SLAVES) return false;
    return geonetSend(espnowDstForSlave(node), data, len) == ESP_OK;
}

static bool otaCaptureBusy()
{
    return g_rt.active() || g_state == ARMING || g_state == PRESTART ||
           g_state == RUNNING || g_state == STOPPING || g_state == DUMPING ||
           g_state == SCOPE_MULTI;
}

static bool otaNodeSeen(uint8_t node)
{
    return node >= 1 && node <= NUM_SLAVES && g_cachedHello[node].valid;
}

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
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1),
                      IPAddress(192, 168, 4, 1),
                      IPAddress(255, 255, 255, 0));
    bool apOk = WiFi.softAP(AP_SSID, AP_PASS, 1);   /* canal 1 fijo */
    esp_wifi_set_max_tx_power(WIFI_TX_POWER_QDBM);
    if (apOk) apEnableFtmResponder();
    MASTER_LOG_PRINTF("[MASTER] softAP %s\n", apOk ? "OK" : "FAIL");
    if (apOk) {
        dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));
        MASTER_LOG_PRINTLN("[MASTER] DNS catchall :53 -> 192.168.4.1");
    }
    /* Beacon intermedio fuera de captura.
     * 100 TU (default) mete energia a ~10 Hz cerca del ADC; 1000 TU estabiliza
     * menos los telefonos. Usamos 100 TU en idle/dump y 1000 TU durante captura. */
    {
        setApBeaconInterval(AP_BEACON_INTERVAL_TU, "setup");
    }
    MASTER_LOG_PRINTF("[MASTER] WiFi ps=off tx=%d qdBm beacon idle=%d TU capture=%d TU keep=%d\n",
                      WIFI_TX_POWER_QDBM, AP_BEACON_INTERVAL_TU,
                      AP_BEACON_CAPTURE_INTERVAL_TU,
                      AP_KEEP_BEACON_DURING_CAPTURE);
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
        webRelaySetBusyHook([]() {
            return g_state == PRESTART || g_state == RUNNING ||
                   g_state == DUMPING || g_state == SCOPE_MULTI;
        });
        /* Modo ENLACE: config persistida + cola en el mismo LittleFS que la
         * SPA, por eso va después de webServerBegin() (que hace el mount). */
        linkModeBegin();
        linkSetEnterHook(requestEnlace);   /* habilita POST /enlace/now */
        /* OTA de esclavos: el .bin vive en este LittleFS. */
        otam::begin(webServer, otaSendToNode, otaCaptureBusy, otaNodeSeen);
    }

    /* mDNS: el maestro queda alcanzable como http://geo.local desde las dos
     * redes, sin tener que averiguar qué IP le dio el DHCP del celular. En el AP
     * propio sigue estando 192.168.4.1, que no depende de mDNS. */
    if (MDNS.begin(LINK_MDNS_NAME)) {
        MDNS.addService("http", "tcp", 80);
        MASTER_LOG_PRINTF("[MASTER] mDNS: http://%s.local\n", LINK_MDNS_NAME);
    }

    /* ESP-NOW (convive con AP mode) */
    g_rt.begin(rtEmit, rtReady, rtAck);
    espnowRx.setRtHandlers(rtOnData, rtOnEnd);
    espnowRx.setOtaHandler(otam::onStatus);
    espnowRx.setFtmHandler(onFtmReport);
    webRelaySetFillHook(rtarc::pushFill);
    webRelaySetDropTestHook(setDropTest);
    webRelaySetFirHook(sendFirToNode);
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

    linkRssiBegin();

    MASTER_LOG_PRINTLN("[MASTER] listo");
    LOGM("BOOT", "slaves=%d,espnow=%u", NUM_SLAVES, (unsigned)g_espnowReady);
}

/* ── Loop ────────────────────────────────────────────────────────────────── */

void loop()
{
    serviceAckResend();
    serviceTimeBeacon();
    if (g_reqResendPending && (int32_t)(millis() - g_reqResendDueMs) >= 0) {
        if (g_state == DUMPING && g_reqResendNode == g_dumpSlaveIdx &&
            g_reqResendSeq == g_dumpBatchSeq) {
            requestBatch(g_reqResendNode, g_reqResendSeq);
        } else {
            g_reqResendPending = false;
        }
    }
    debugEspHardwareService();
    serviceFtmReports();
    espnowRx.service();
#if DBG_ENABLE
    {   /* Vigilar fragmentacion interna sin cargar el firmware de campo. */
        static uint32_t heapDiagMs = 0;
        if (millis() - heapDiagMs >= 5000u) {
            heapDiagMs = millis();
            MASTER_LOG_PRINTF("[HEAP] libre=%u mayor=%u\n",
                              (unsigned)ESP.getFreeHeap(),
                              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        }
    }
#endif
    otam::service();
    serviceRfBurst();

    /* Fase ENLACE: ESP-NOW esta abajo (no hay esclavos que atender) pero la SPA
     * y los endpoints /enlace/* siguen vivos sobre la red del cliente — es toda
     * la razon de estar aca. AsyncWebServer atiende en su propia task; lo que
     * hay que sostener a mano es el WS (si no, el navegador corta la conexion
     * "inactiva") y el USB hacia MATLAB. */
    if (g_state == ENLACE) {
        if (!linkService(radioTearDownCapture, radioBringUpCapture)) {
            g_state = armedOrIdleState();
            matlab.sendReady(g_armedCount);
            matlab.sendHeartbeat(0x00, 0, 0, (uint8_t)g_state);
        }
        matlab.loop();
        webRelayService();
        static uint32_t lastEnlaceHbMs = 0;
        if (millis() - lastEnlaceHbMs > 1000) {
            lastEnlaceHbMs = millis();
            if (matlab.connected() || ws.count() > 0) {
                matlab.sendHeartbeat(0x00, 0, 0, (uint8_t)g_state);
            }
        }
        return;
    }

    dnsServer.processNextRequest();

    /* Gestionar conexión TCP de MATLAB */
    matlab.loop();

    /* Procesar TODOS los comandos encolados (evita pérdida de A1+A3 back-to-back) */
    while (matlab.hasCmd()) {
        handleMatlabCmd(matlab.lastCmd(), false);
    }

    /* El maestro desempata: si algún esclavo quedó con otra N que la vigente,
     * se la vuelve a imponer (nodo recién llegado, reinicio, config rechazada). */
    serviceSlaveAuthority();

    /* Comandos llegados por la interfaz web (mismo despachador — ver
     * web_relay.h: se decodifican a RxCmd para no duplicar lógica). */
    while (webRelayHasCmd()) {
        handleMatlabCmd(webRelayPopCmd(), true);
    }
    webRelayService();
    serviceWebFills();   /* huecos que pide la web, desde la copia */
    linkRssiLoop();
    /* Mantiene la STA asociada a la red del cliente sin bajar el AP ni ESP-NOW:
     * el maestro habita las dos redes. Ver linkStaService() para la letra chica
     * del canal compartido. Con captura en marcha no se reintenta. */
    g_linkStaHold = g_rt.active() || g_state == ARMING || g_state == PRESTART ||
                    g_state == RUNNING || g_state == STOPPING || g_state == DUMPING ||
                    g_state == SCOPE_MULTI;
    linkStaService();

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
                /* Avanzar: saltar nodos ya confirmados O que nunca ARM'd */
                while (g_hotWaitQueryNode <= NUM_SLAVES &&
                       (hotWaitAcked(g_hotWaitQueryNode) ||
                        !(g_armAckMask & (1UL << g_hotWaitQueryNode)))) {
                    g_hotWaitQueryNode++;
                    g_hotWaitRetries = 0;
                    g_hotWaitQueryInFlight = false;
                    g_hotWaitQueryMs = millis();
                }

                if (g_hotWaitQueryNode > NUM_SLAVES) {
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
                        /* Solo re-consultar, NUNCA re-broadcastear PRESTART aca: el
                         * esclavo puede ya estar HOT_WAIT/armado (el PSoC real tarda
                         * ~130 ms en confirmar EVT_ARMED, mas que este timeout de
                         * 120 ms, asi que el primer query casi siempre le gana de
                         * mano). Un broadcastPrestart redundante fuerza al esclavo a
                         * reenviar SETN a un PSoC que ya entro en silencio UART total
                         * en PSOC_ARMED -> SETN nunca ackea -> el esclavo aborta,
                         * libera el store y el PSoC queda armado y sordo para
                         * siempre (roto hasta reset fisico). VER nunca tuvo este bug
                         * porque su retry (mas abajo) tampoco re-arma. */
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

    /* Tiempo real: entrega en orden y cierre por STREAM_END. */
    if (g_rt.active()) {
        g_rt.service();
        /* El STOP sale por broadcast (sin ACK): si un nodo no lo oyo seguia
         * transmitiendo para siempre (2026-10-25). Mientras haya nodos sin
         * STREAM_END se les reenvia por unicast cada 300 ms. */
        if (g_rtStopReqMs != 0u) {
            static uint32_t lastStopResendMs = 0;
            if (millis() - lastStopResendMs >= 300u) {
                lastStopResendMs = millis();
                MsgStop st = { CMD_STOP };
                for (uint8_t n = 1; n <= NUM_SLAVES; n++) {
                    if (g_rt.nodeOn(n) && !g_rt.nodeEnded(n) && g_espnowReady) {
                        geonetSend(espnowDstForSlave(n), (uint8_t *)&st, sizeof(st));
                    }
                }
            }
        }
#if DBG_ENABLE
        {   /* Diagnostico del tiempo real, una linea por segundo. */
            static uint32_t lastDiagMs = 0;
            if (millis() - lastDiagMs >= 1000u) {
                lastDiagMs = millis();
                g_rt.printStats(Serial);
                MASTER_LOG_PRINTF("[RTDIAG] txack=%lu/%lu ackpend=%u/%u\n",
                                  (unsigned long)espnowTxAckOk(), (unsigned long)espnowTxAckFail(),
                                  (unsigned)g_ackPendingOn[2], (unsigned)g_ackPendingOn[4]);
            }
        }
#endif
        /* Vigia: un esclavo que se resetea o pierde el enlace nunca manda su
         * STREAM_END. La descarga rapida puede callar durante toda la captura:
         * usar la duracion a la menor Fs efectiva armada, con 20 % de margen. */
        const uint32_t quiet = g_rt.msSinceProgress();
        const uint32_t captureMs = captureDurationMs(g_rec_n_batches);
        const uint32_t limitMs = capModeRealtime() ? 15000u
                               : ((captureMs * 12u + 9u) / 10u) + 20000u;
        if ((g_rtStopReqMs != 0u && quiet > 5000u) || quiet > limitMs) {
            MASTER_LOG_PRINTF("[MASTER] RT sin progreso %lu ms (stop=%u) -> ARMED\n",
                              (unsigned long)quiet, (unsigned)(g_rtStopReqMs != 0u));
            g_rt.printStats(Serial);
            rtSendEnds();
            g_rt.stop();
            g_rtStopReqMs = 0;
            syncOutWrite(LOW);
            g_state = ARMED;
            matlab.sendReady(g_armedCount);
            matlab.sendHeartbeat(0x00, 0, 0, (uint8_t)g_state);
        }
    }
    if (g_rt.active()) {
        if (g_rt.allDone()) {
            g_rtStopReqMs = 0;
            rtSendEnds();
            g_rt.stop();
            syncOutWrite(LOW);
            g_state = ARMED;
            matlab.sendReady(g_armedCount);
            matlab.sendHeartbeat(0x00, 0, 0, (uint8_t)g_state);
            MASTER_LOG_PRINTLN("[MASTER] RT completo -> ARMED");
        }
    }

    /* Store-and-forward: timeout de seguridad. El camino normal es ok=2
     * desde el/los slaves; esto solo rescata ACKs perdidos. */
    if (g_state == RUNNING && g_rec_n_batches > 0 && g_captureDumpDueMs != 0 &&
        (int32_t)(millis() - g_captureDumpDueMs) >= 0) {
        syncOutWrite(LOW);
        if (g_viewDump) {
            MASTER_LOG_PRINTF("[MASTER] VIEW done ACK timeout node=%d -> DUMP\n",
                              g_viewNode);
            LOGM("VIEW_DONE_TIMEOUT", "node=%u,n=%u",
                 g_viewNode, g_rec_n_batches);
            beginDump("view_timeout");
        } else {
            MASTER_LOG_PRINTF("[MASTER] START done ACK timeout mask=0x%08lX armed=0x%08lX -> DUMP\n",
                              (unsigned long)g_captureDoneMask,
                              (unsigned long)g_armAckMask);
            LOGM("START_DONE_TIMEOUT", "done=0x%08lX,armed=0x%08lX",
                 (unsigned long)g_captureDoneMask, (unsigned long)g_armAckMask);
            beginDump("capture_timeout");
        }
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
        bool waitingForWeb = g_dumpRequiresWebClient && !webRelayHasActiveClient();

        if (waitingForWeb) {
            webRelayService();
            if (!g_dumpWaitingForWeb) {
                MASTER_LOG_PRINTF("[MASTER] DUMP pause: sin WS node=%d seq=%d\n",
                                  g_dumpSlaveIdx, g_dumpBatchSeq);
                LOGM("DUMP_PAUSE_WS", "node=%u,seq=%u", g_dumpSlaveIdx, g_dumpBatchSeq);
                g_dumpWsWaitMs = millis();
            }
            g_dumpWaitingForWeb = true;
            g_dumpReqMs = millis();
            /* La pausa por falta de WS no tiene fin propio: si la pestaña se
             * cerró o se recargó, el maestro se quedaba acá para siempre. */
            if (millis() - g_dumpWsWaitMs > DUMP_WS_WAIT_MAX_MS) {
                abortDump("no quedo cliente web para entregar");
                return;
            }
        } else if (g_dumpWaitingForWeb) {
            MASTER_LOG_PRINTF("[MASTER] DUMP resume WS node=%d seq=%d\n",
                              g_dumpSlaveIdx, g_dumpBatchSeq);
            LOGM("DUMP_RESUME_WS", "node=%u,seq=%u", g_dumpSlaveIdx, g_dumpBatchSeq);
            g_dumpWaitingForWeb = false;
            g_dumpRetries = 0;
            if (!g_dumpBatchRx) {
                requestBatch(g_dumpSlaveIdx, g_dumpBatchSeq);
                g_dumpReqMs = millis();
            }
        }

        if (!waitingForWeb && g_dumpBatchRx &&
            webRelayReadyForDumpRequest(g_dumpRequiresWebClient)) {
            g_dumpBatchRx = false;
            g_dumpRetries = 0;
            g_dumpRetryPaused = false;
            g_dumpSkipStreak = 0;   /* el esclavo sí entrega: sigue vivo */
            advance = true;
        } else if (!waitingForWeb && g_dumpRetryPaused) {
            if ((int32_t)(millis() - g_dumpRetryDueMs) >= 0) {
                g_dumpRetryPaused = false;
                requestBatch(g_dumpSlaveIdx, g_dumpBatchSeq);
                g_dumpReqMs = millis();
            }
        } else if (!waitingForWeb && !g_dumpBatchRx &&
                   millis() - g_dumpReqMs > DUMP_BATCH_TIMEOUT_MS) {
            /* Solo cuenta como timeout la espera AL ESCLAVO. Con el lote ya
             * recibido y el navegador sin lugar, reintentar lo volvia a pedir
             * (duplicado en la web) y al agotar reintentos lo saltaba (hueco). */
            g_dumpRetries++;
            if (g_dumpRetries >= DUMP_MAX_RETRIES) {
                MASTER_LOG_PRINTF("[MASTER] DUMP skip node=%d seq=%d\n",
                                  g_dumpSlaveIdx, g_dumpBatchSeq);
                g_dumpRetries = 0;
                g_dumpRetryPaused = false;
                /* Varios lotes seguidos sin respuesta = el esclavo no tiene la
                 * captura (o se fue). Pedir los 261 igual no la va a traer. */
                if (++g_dumpSkipStreak >= DUMP_MAX_SKIP_STREAK) {
                    abortDump("el esclavo no entrega lotes");
                    return;
                }
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
            /* Corte por nodo: cada esclavo se dumpea hasta SU largo (HAMMER
             * puede tener menos lotes que GEO), acotado por el N efectivo que
             * el esclavo confirmó en HOTWAIT_ACK (clamp de 512 sin SD). Con
             * duraciones dentro del límite es g_rec_n_batches, como siempre. */
            if (g_dumpBatchSeq >= dumpBatchesForNode(g_dumpSlaveIdx)) {
                g_dumpBatchSeq = 0;
                g_dumpSlaveIdx++;
                /* VIEW dump: solo un nodo; dump normal: saltar nodos no armados */
                if (!g_viewDump) {
                    while (g_dumpSlaveIdx <= NUM_SLAVES &&
                           !(g_armAckMask & (1UL << g_dumpSlaveIdx))) {
                        g_dumpSlaveIdx++;
                    }
                }
                uint8_t dumpLimit = g_viewDump ? g_viewNode : NUM_SLAVES;
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
                    g_captureDoneMask = 0;
                    g_dumpRetryPaused = false;
                    g_dumpRequiresWebClient = false;
                    g_dumpWaitingForWeb = false;
                    dumpDeliveryGuardReset();
                    linkQueueEnd(true);   /* sesion completa: queda en la cola */
                    matlab.sendReady(g_armedCount);
                    matlab.sendHeartbeat(0x00, 0, 0, (uint8_t)g_state);
                    MASTER_LOG_PRINTF("[MASTER] DUMP completo -> %s\n",
                                      g_state == ARMED ? "ARMED" : "IDLE");

                    /* Trigger de ENLACE: por captura, si esta configurado en
                     * auto. Recien aca es seguro cambiar de fase — el dump ya
                     * termino y no hay nada en vuelo con los esclavos. */
                    if (g_linkCfg.auto_upload) {
                        const char *why = "";
                        if (!requestEnlace(&why)) {
                            MASTER_LOG_PRINTF("[ENLACE] auto omitido: %s\n", why);
                        }
                    }
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
