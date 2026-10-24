#include "live_capture.h"
#include "geo_codec.h"

#if defined(SIM_SIN_HW) && SIM_SIN_HW
#include "sim_sd.h"
#define LIVE_FS SimSD
#else
#include <SD.h>
#define LIVE_FS SD
#endif
#include <SPI.h>
#include <errno.h>
#include <esp_timer.h>
#include "debug_log.h"
#include "espnow_transport.h"

#if defined(SIM_SIN_HW) && SIM_SIN_HW
#include "sim_psoc.h"
#endif

/* Pines de la SD del ESP. Un solo lugar, pensando en el ESP32-S3: ahi el 25 y
 * el 32 no existen y el host SPI se llama distinto. */
#ifndef LIVE_SD_SCK
#define LIVE_SD_SCK 14
#endif
#ifndef LIVE_SD_MISO
#define LIVE_SD_MISO 32
#endif
#ifndef LIVE_SD_MOSI
#define LIVE_SD_MOSI 13
#endif
#ifndef LIVE_SD_CS
#define LIVE_SD_CS 25
#endif
#ifndef LIVE_SD_HZ
#define LIVE_SD_HZ 1000000
#endif
#ifndef LIVE_SD_SPI_HOST
#define LIVE_SD_SPI_HOST HSPI
#endif

#if DBG_ENABLE
#define LIVE_RAW(...) DBG_STREAM.printf(__VA_ARGS__)
#else
#define LIVE_RAW(...) do {} while (0)
#endif

namespace live {
namespace {

constexpr uint32_t PAGE_BATCHES    = 32;
constexpr uint32_t MAX_PAGES       = 48;    /* 1536 lotes: mas de lo que entra sin PSRAM */
constexpr uint32_t PSOC_RING       = 512;   /* anillo del PSoC, en lotes */
constexpr uint32_t PSOC_WINDOW     = 16;    /* LIVE_WINDOW_BATCHES del PSoC */
constexpr uint32_t MASTER_WINDOW   = 64;    /* lotes en vuelo hacia el maestro */
constexpr uint32_t RADIO_INFLIGHT  = 6;     /* tramas en la cola de ESP-NOW */
constexpr uint32_t INFLIGHT_MS     = 50;    /* si no vuelve el aviso, se da por salida */
constexpr uint32_t RTO_MS          = 150;
constexpr uint32_t PSOC_ACK_MS     = 50;
constexpr uint32_t REWIND_MS       = 30;
constexpr uint32_t PSOC_RESEND_MS  = 20;    /* el PSoC reenvia un lote por pasada (~2,5 ms) */
constexpr uint32_t PSOC_RESEND_Q   = 16;
constexpr size_t   HEAP_RESERVE    = 36u * 1024u;
constexpr uint32_t PSOC_RT_RING    = 64;
/* Con SD el anillo solo cubre el atraso del maestro: lo demas esta en la
 * tarjeta, y la memoria que sobra la necesitan la radio y FatFs (con el
 * anillo grande una escritura fallo por falta de heap). */
constexpr uint32_t SD_RT_RING      = 160;
constexpr uint32_t SD_READ_BATCHES = 32;
constexpr size_t   BATCH_BYTES     = (size_t)SPI_BATCH_SAMPLES * sizeof(SampleBytes);
constexpr size_t   PAGE_BYTES      = (size_t)PAGE_BATCHES * BATCH_BYTES;
constexpr uint32_t NONE            = 0xFFFFFFFFu;
constexpr uint16_t MARK_RING       = 256u;
constexpr uint8_t  RADIO_TIME_RING = 16u;
const char *const  SD_PATH         = "/live.bin";

PsocUART *g_psoc = nullptr;
RawSendFn g_radio = nullptr;
uint8_t g_node = 0;

Config g_cfg = {};
bool g_active = false;
bool g_started = false;
uint64_t g_t0 = 0;
uint32_t g_fs = 2604;

SampleBytes *g_pages[MAX_PAGES] = {};
uint32_t g_npages = 0;
uint32_t g_cap = 0;
/* Paginas que consiguio la ultima captura compartida que quiso mas de las que
 * entraron. 0 = todavia no se midio. El HELLO no puede anunciar mas que eso. */
uint32_t g_lastSharedPages = 0;

volatile uint32_t g_recv = 0;
uint32_t g_txNext = 0;
uint32_t g_mack = 0;
uint32_t g_sack = 0;
uint32_t g_dumpAck = 0;
uint32_t g_mackMs = 0;
uint32_t g_ackRxMs = 0;   /* ultimo STREAM_ACK recibido, avance o no */
uint32_t g_retxMs = 0;
uint32_t g_total = NONE;
uint8_t  g_totalB1 = 0;
uint8_t  g_totalB2 = 0;
bool g_psocDone = false;
bool g_overrun = false;
bool g_stopReq = false;

uint32_t g_psocAckSent = NONE;
uint32_t g_psocAckMs = 0;
uint32_t g_rewindMs = 0;
uint32_t g_psocResend = NONE;     /* reenvio pedido al PSoC, en vuelo */
uint32_t g_psocResendMs = 0;
uint32_t g_psocQ[PSOC_RESEND_Q];   /* reenvios esperando turno */
uint32_t g_psocQn = 0;

uint32_t g_gaps = 0, g_dups = 0, g_retx = 0, g_txOk = 0, g_txFail = 0;
uint32_t g_txBytes = 0;   /* bytes de datos enviados por radio (para medir la compresion) */
uint32_t g_badRamp = 0, g_noRoom = 0;
esp_err_t g_lastTxErr = ESP_OK;
volatile uint32_t g_inflight = 0;
uint32_t g_inflightMs = 0;

void onRadioSent(bool ok)
{
    (void)ok;
    if (g_inflight > 0u) g_inflight = g_inflight - 1u;
}

/* Lugar para un lote (dos tramas) en la cola de la radio. */
bool radioRoom()
{
    if (g_inflight > 0u && (millis() - g_inflightMs) >= INFLIGHT_MS) g_inflight = 0u;
    return g_inflight + 2u <= RADIO_INFLIGHT;
}

/* SD: la escribe y la lee SOLO la tarea; el lazo pide por banderas. */
SPIClass g_spi(LIVE_SD_SPI_HOST);
File g_file;
bool g_sdMounted = false;
uint64_t g_sdFreeBytes = 0;
uint32_t g_sdHz = LIVE_SD_HZ;
uint32_t g_sdMountedHz = 0;
volatile bool g_sdOpen = false;
volatile bool g_sdFull = false;
volatile bool g_sdErr = false;   /* fallo una escritura con lugar libre */
volatile uint32_t g_sdRetries = 0;
constexpr uint32_t SD_RETRY_MS = 2500u;
/* Lecturas de la SD fallidas seguidas para el stream; con muchas se cierra. */
uint32_t g_sdReadFails = 0;
constexpr uint32_t SD_READ_FAIL_MAX = 20u;
volatile uint32_t g_sdRemounts = 0;
volatile int g_sdErrno = 0;
volatile uint32_t g_sdw = 0;
/* Lotes que un flush sin error dejo en la tarjeta. Lo escrito despues vive en
 * el cache de FatFs y se pierde si la SD se remonta: la RAM solo se libera
 * hasta aca, y un remontaje vuelve g_sdw a este punto y reescribe (2026-10-23:
 * sin esto, tras un EIO se releian clusters viejos de otra captura). */
volatile uint32_t g_sdSynced = 0;
volatile uint8_t g_sdReadState = 0;     /* 0 libre, 1 pedido, 2 listo, 3 error */
volatile uint32_t g_sdReadSeq = 0;
volatile uint32_t g_sdReadCount = 0;
bool g_sdReadForRt = false;
bool g_sdReadPendingSend = false;
SampleBytes g_sdReadBuf[SD_READ_BATCHES * SPI_BATCH_SAMPLES];
TaskHandle_t g_sdTask = nullptr;
SemaphoreHandle_t g_sdMutex = nullptr;
bool g_radioReady = true;

struct NoiseMark {
    uint32_t s0;
    uint32_t s1;
    uint8_t bits;
};
NoiseMark g_marks[MARK_RING];
uint16_t g_markHead = 0;
uint16_t g_markTail = 0;
uint32_t g_markApplied[6] = {};
uint32_t g_radioAirtimeUs[RADIO_TIME_RING] = {};
uint8_t g_radioTimeHead = 0;
uint8_t g_radioTimeTail = 0;
bool g_havePrevAnchor = false;
uint32_t g_prevAnchor = 0;
uint64_t g_prevRxUs = 0;
bool g_havePair = false;
uint32_t g_pairSample = 0;
uint64_t g_pairUs = 0;
uint32_t g_pairCount = 0;
uint32_t g_fitSample = 0;
uint64_t g_fitUs = 0;
uint32_t g_fsEstimate = 2604;
uint32_t g_highestStored = 0;
portMUX_TYPE g_markMux = portMUX_INITIALIZER_UNLOCKED;

void pushMarkLocked(uint32_t s0, uint32_t s1, uint8_t bits)
{
    if (bits == 0u) return;
    if (s1 < s0) {
        const uint32_t t = s0;
        s0 = s1;
        s1 = t;
    }
    const uint16_t next = (uint16_t)((g_markHead + 1u) % MARK_RING);
    if (next == g_markTail) g_markTail = (uint16_t)((g_markTail + 1u) % MARK_RING);
    g_marks[g_markHead] = {s0, s1, bits};
    g_markHead = next;
}

uint32_t sampleForTimeLocked(uint64_t t)
{
    const int64_t dt = (t >= g_pairUs) ? (int64_t)(t - g_pairUs)
                                       : -(int64_t)(g_pairUs - t);
    const int64_t ds = (dt * (int64_t)g_fsEstimate) / 1000000LL;
    const int64_t sample = (int64_t)g_pairSample + ds;
    if (sample <= 0) return 0u;
    if (sample >= (int64_t)UINT32_MAX) return UINT32_MAX;
    return (uint32_t)sample;
}

void processAnchor(const PsocBatch &b)
{
    if (!b.hasAnchor) return;
    portENTER_CRITICAL(&g_markMux);
    if (g_havePrevAnchor && b.prevEnd != NONE) {
        const uint64_t pairUs = g_prevRxUs;
        pushMarkLocked(g_prevAnchor, b.prevEnd, SAMPLE_FLAG_I2C);
        if (g_havePair && pairUs > g_fitUs &&
            (pairUs - g_fitUs) >= 1000000ULL && b.prevEnd > g_fitSample) {
            uint32_t measured = (uint32_t)(((uint64_t)(b.prevEnd - g_fitSample) * 1000000ULL) /
                                           (pairUs - g_fitUs));
            const uint32_t lo = (g_fs * 97u) / 100u;
            const uint32_t hi = (g_fs * 103u + 99u) / 100u;
            if (measured < lo) measured = lo;
            if (measured > hi) measured = hi;
            g_fsEstimate = (3u * g_fsEstimate + measured + 2u) / 4u;
            g_fitSample = b.prevEnd;
            g_fitUs = pairUs;
        } else if (!g_havePair) {
            g_fitSample = b.prevEnd;
            g_fitUs = pairUs;
        }
        g_pairSample = b.prevEnd;
        g_pairUs = pairUs;
        g_havePair = true;
        g_pairCount++;
        if (g_pairCount == 1u) {
            g_fitSample = g_pairSample;
            g_fitUs = g_pairUs;
        }
    }
    g_prevAnchor = b.anchorStart;
    g_prevRxUs = b.rxUs;
    g_havePrevAnchor = true;
    portEXIT_CRITICAL(&g_markMux);
}

void applyMarks(SampleBytes *dst, uint32_t firstSample, uint32_t batch)
{
    portENTER_CRITICAL(&g_markMux);
    if (batch > g_highestStored) g_highestStored = batch;
    for (int i = 0; i < SPI_BATCH_SAMPLES; i++) {
        const uint32_t sample = firstSample + (uint32_t)i;
        uint8_t bits = 0u;
        for (uint16_t p = g_markTail; p != g_markHead; p = (uint16_t)((p + 1u) % MARK_RING)) {
            if (g_marks[p].s0 <= sample && sample <= g_marks[p].s1) bits |= g_marks[p].bits;
        }
        dst[i].flags |= bits;
#if defined(SIM_SIN_HW) && SIM_SIN_HW
        simPsocApplyNoiseArtifact(dst[i], bits, sample);
#endif
        for (uint8_t bit = 0; bit < 6u; bit++) {
            if ((bits & (1u << bit)) != 0u) g_markApplied[bit]++;
        }
    }
    const uint32_t keepFrom = (g_highestStored > 64u)
                            ? (g_highestStored * SPI_BATCH_SAMPLES - 64u * SPI_BATCH_SAMPLES)
                            : 0u;
    while (g_markTail != g_markHead && g_marks[g_markTail].s1 < keepFrom) {
        g_markTail = (uint16_t)((g_markTail + 1u) % MARK_RING);
    }
    portEXIT_CRITICAL(&g_markMux);
}

/* Maestro simulado (banco): recibe las tramas en vez de la radio. */
bool g_simOn = false;
uint8_t g_simLoss = 0;
uint32_t g_simNext = 0;
uint32_t g_simTag[256];
uint8_t g_simParts[256];
uint32_t g_simRx = 0, g_simDup = 0, g_simDrop = 0, g_simWin = 0, g_simBad = 0;
uint32_t g_simAckMs = 0;

SampleBytes *slotPtr(uint32_t seq)
{
    const uint32_t idx = seq % g_cap;
    return g_pages[idx / PAGE_BATCHES] + (size_t)(idx % PAGE_BATCHES) * SPI_BATCH_SAMPLES;
}

bool streaming()
{
    return g_cfg.realtime || (g_cfg.streamDump && g_psocDone);
}

/* Lote mas viejo que tiene que seguir en la RAM del ESP. */
uint32_t tailNow()
{
    uint32_t t;
    const uint32_t sdw = g_sdSynced;
    if (g_cfg.realtime) {
        if (g_cfg.storage == CAPMODE_STORAGE_SD) {
            /* Retiene hasta que el maestro confirma, como el compartido: los
             * reenvios salen de la RAM. Leerlos de la SD frenaba todo.
             * Pero si la radio se traba y la RAM se llena, se suelta lo que ya
             * esta en la SD y esos reenvios se leen de ahi: sin esto, 8 s de
             * radio parada (ESPNOW_NO_MEM, 2026-10-23) cortaban la captura por
             * memoria con la SD guardando todo. Los dos terminos crecen solos,
             * asi que la cola nunca retrocede. */
            t = (sdw < g_mack) ? sdw : g_mack;
            const uint32_t keep = (g_cap > 2u * PSOC_WINDOW) ? g_cap - PSOC_WINDOW : g_cap / 2u;
            const uint32_t r0 = g_recv;
            const uint32_t press = (r0 > keep) ? r0 - keep : 0u;
            const uint32_t onSd = (press < sdw) ? press : sdw;
            if (onSd > t) t = onSd;
        } else if (g_cfg.storage == CAPMODE_STORAGE_PSOC) {
            t = g_txNext;   /* el respaldo es la RAM del PSoC */
        } else {
            t = g_mack;
        }
    } else if (g_cfg.streamDump) {
        if (g_cfg.storage == CAPMODE_STORAGE_SD) {
            t = sdw;
        } else if (g_cfg.storage == CAPMODE_STORAGE_PSOC) {
            /* Como en tiempo real: el ESP retiene hasta transmitir y el
             * PSoC guarda el resto (N <= 512) y los reenvios. */
            t = g_txNext;
        } else {
            t = g_mack;
        }
    } else {
        t = (g_cfg.storage == CAPMODE_STORAGE_SD) ? sdw : g_dumpAck;
    }
    const uint32_t r = g_recv;
    return (t < r) ? t : r;
}

uint64_t tsFor(uint32_t seq)
{
    return g_t0 + ((uint64_t)seq * SPI_BATCH_SAMPLES * 1000000ULL) / (g_fs ? g_fs : 2604u);
}

void toStored(const PsocBatch &b, SampleBytes *dst)
{
    for (int i = 0; i < SPI_BATCH_SAMPLES; i++) {
        const PsocSample &s = b.samples[i];
        dst[i].raw_lo = (uint8_t)(s.raw_input & 0xFF);
        dst[i].raw_hi = (uint8_t)((s.raw_input >> 8) & 0xFF);
        dst[i].alog0 = (uint8_t)(s.post_analog & 0xFF);
        dst[i].alog1 = (uint8_t)((s.post_analog >> 8) & 0xFF);
        dst[i].alog2 = (uint8_t)((s.post_analog >> 16) & 0xFF);
        dst[i].digi0 = (uint8_t)(s.post_digital & 0xFF);
        dst[i].digi1 = (uint8_t)((s.post_digital >> 8) & 0xFF);
        dst[i].digi2 = (uint8_t)((s.post_digital >> 16) & 0xFF);
        dst[i].gain = s.gain_byte;
        dst[i].flags = s.sample_flags;
    }
}

uint32_t digi24(const SampleBytes &s)
{
    return (uint32_t)s.digi0 | ((uint32_t)s.digi1 << 8) | ((uint32_t)s.digi2 << 16);
}

/* Rampa de banco: la muestra k de la captura vale k (24 bits). */
uint32_t rampErrors(const SampleBytes *s, uint32_t firstSample, int n)
{
    uint32_t bad = 0;
    for (int i = 0; i < n; i++) {
        if (digi24(s[i]) != ((firstSample + (uint32_t)i) & 0x00FFFFFFu)) bad++;
    }
    return bad;
}

uint8_t xorCrc(const uint8_t *d, size_t n)
{
    uint8_t c = 0;
    for (size_t i = 0; i < n; i++) c ^= d[i];
    return c;
}

/* ── Maestro simulado ──────────────────────────────────────────────────── */
esp_err_t simReceive(const uint8_t *data, size_t len)
{
    if (len >= MSG_DATAZ_HEADER && data[0] == DATAZ_PKT_MARKER) {
        /* Lote comprimido: decodificar como lo haria el maestro. */
        if ((esp_random() % 100u) < g_simLoss) { g_simDrop++; return ESP_OK; }
        const MsgDataZ *z = (const MsgDataZ *)data;
        int32_t x[SPI_BATCH_SAMPLES];
        uint8_t fl[SPI_BATCH_SAMPLES];
        if (len != (size_t)MSG_DATAZ_HEADER + z->len || dataZCrc(z) != z->crc ||
            !geoz_decode(z->data, z->len, x, fl)) { g_simBad++; return ESP_OK; }
        const uint32_t seq = z->seq;
        if (seq < g_simNext) { g_simDup++; return ESP_OK; }
        if (seq >= g_simNext + MASTER_WINDOW) { g_simWin++; return ESP_OK; }
        const uint32_t slot = seq & 0xFFu;
        if (g_simTag[slot] == seq && g_simParts[slot] == 3u) { g_simDup++; return ESP_OK; }
        if (g_cfg.ramp) {
            for (int i = 0; i < SPI_BATCH_SAMPLES; i++) {
                if (((uint32_t)x[i] & 0x00FFFFFFu) != ((seq * SPI_BATCH_SAMPLES + (uint32_t)i) & 0x00FFFFFFu)) g_simBad++;
            }
        }
        g_simTag[slot] = seq;
        g_simParts[slot] = 3u;
        g_simRx++;
        while (g_simTag[g_simNext & 0xFFu] == g_simNext && g_simParts[g_simNext & 0xFFu] == 3u) {
            g_simParts[g_simNext & 0xFFu] = 0;
            g_simTag[g_simNext & 0xFFu] = NONE;
            g_simNext++;
        }
        return ESP_OK;
    }
    if (len != sizeof(MsgData32) || data[0] != DATA32_PKT_MARKER) return ESP_OK;
    if ((esp_random() % 100u) < g_simLoss) { g_simDrop++; return ESP_OK; }
    const MsgData32 *m = (const MsgData32 *)data;
    if (xorCrc(data, sizeof(MsgData32) - 1) != m->crc || m->part > 1) { g_simBad++; return ESP_OK; }
    const uint32_t seq = m->seq;
    if (seq < g_simNext) { g_simDup++; return ESP_OK; }
    if (seq >= g_simNext + MASTER_WINDOW) { g_simWin++; return ESP_OK; }
    const uint32_t slot = seq & 0xFFu;
    if (g_simTag[slot] != seq) { g_simTag[slot] = seq; g_simParts[slot] = 0; }
    if (g_simParts[slot] & (1u << m->part)) { g_simDup++; return ESP_OK; }
    if (g_cfg.ramp) {
        g_simBad += rampErrors(m->samples,
                               seq * SPI_BATCH_SAMPLES + m->part * SAMPLES_PER_PART,
                               SAMPLES_PER_PART);
    }
    g_simParts[slot] |= (uint8_t)(1u << m->part);
    if (g_simParts[slot] == 3u) g_simRx++;
    while (g_simTag[g_simNext & 0xFFu] == g_simNext && g_simParts[g_simNext & 0xFFu] == 3u) {
        g_simParts[g_simNext & 0xFFu] = 0;
        g_simTag[g_simNext & 0xFFu] = NONE;
        g_simNext++;
    }
    return ESP_OK;
}

void simService()
{
    if (!g_simOn || !g_active || (millis() - g_simAckMs) < 30u) return;
    g_simAckMs = millis();
    uint32_t sack = 0;
    for (uint32_t i = 0; i < 32; i++) {
        const uint32_t s = g_simNext + 1u + i;
        if (g_simTag[s & 0xFFu] == s && g_simParts[s & 0xFFu] == 3u) sack |= (1u << i);
    }
    onMasterAck(g_simNext, sack);
}

esp_err_t sendRaw(const uint8_t *d, size_t n)
{
    if (g_simOn) return simReceive(d, n);
    if (!g_radio) return ESP_FAIL;
    const esp_err_t err = g_radio(d, n);
    if (err == ESP_OK) {
        g_inflight = g_inflight + 1u;
        g_inflightMs = millis();
    }
    return err;
}

/* Un lote son dos tramas de 15 muestras. El camino stream lleva lote de 32
 * bits; el volcado asincronico clasico conserva la trama de 16 bits. */
#ifndef LIVE_COMPRESS
#define LIVE_COMPRESS 1
#endif

/* Las 30 muestras de 24 bits y sus marcas de un lote. */
static void batchValues(const SampleBytes *s, int32_t *x, uint8_t *flags)
{
    for (int i = 0; i < SPI_BATCH_SAMPLES; i++) {
        uint32_t v = (uint32_t)s[i].digi0 | ((uint32_t)s[i].digi1 << 8) |
                     ((uint32_t)s[i].digi2 << 16);
        x[i] = (v & 0x800000u) ? (int32_t)(v | 0xFF000000u) : (int32_t)v;
        flags[i] = s[i].flags;
    }
}

bool sendBatch(uint32_t seq, const SampleBytes *s, bool rt)
{
#if LIVE_COMPRESS
    if (rt) {
        /* Una sola trama comprimida por lote (geo_codec.h). */
        MsgDataZ m;
        int32_t x[SPI_BATCH_SAMPLES];
        uint8_t fl[SPI_BATCH_SAMPLES];
        batchValues(s, x, fl);
        m.marker = DATAZ_PKT_MARKER;
        m.node_id = g_node;
        m.seq = seq;
        m.len = (uint8_t)geoz_encode(x, fl, m.data);
        m.crc = dataZCrc(&m);
        const esp_err_t err = sendRaw((const uint8_t *)&m, MSG_DATAZ_HEADER + m.len);
        if (err != ESP_OK) { g_txFail++; g_lastTxErr = err; return false; }
        g_txOk++;
        g_txBytes += MSG_DATAZ_HEADER + m.len;
        return true;
    }
#endif
    for (uint8_t part = 0; part < 2; part++) {
        esp_err_t err;
        if (rt) {
            MsgData32 m = {};
            m.marker = DATA32_PKT_MARKER;
            m.node_id = g_node;
            m.seq = seq;
            m.part = part;
            m.n_samples = SAMPLES_PER_PART;
            m.timestamp_us = tsFor(seq);
            memcpy(m.samples, s + part * SAMPLES_PER_PART, sizeof(m.samples));
            m.crc = xorCrc((const uint8_t *)&m, sizeof(m) - 1);
            err = sendRaw((const uint8_t *)&m, sizeof(m));
        } else {
            MsgData m = {};
            m.marker = DATA_PKT_MARKER;
            m.node_id = g_node;
            m.seq = (uint16_t)seq;
            m.part = part;
            m.n_samples = SAMPLES_PER_PART;
            m.timestamp_us = tsFor(seq);
            memcpy(m.samples, s + part * SAMPLES_PER_PART, sizeof(m.samples));
            m.crc = xorCrc((const uint8_t *)&m, sizeof(m) - 1);
            /* La segunda mitad espera el aviso de envio de la primera: pegadas,
             * el S3 perdia una de cada dos respuestas a REQ_BATCH. */
            const int64_t t0 = esp_timer_get_time();
            while (espnowBatchInflight() > 0 && (esp_timer_get_time() - t0) < 6000) {
                delayMicroseconds(50);
            }
            err = sendRaw((const uint8_t *)&m, sizeof(m));
            if (err == ESP_OK) espnowBatchInflight() = 1;
        }
        if (err != ESP_OK) { g_txFail++; g_lastTxErr = err; return false; }
    }
    g_txOk++;
    return true;
}

/* ── Tarea de la SD ────────────────────────────────────────────────────── */
/* Desmonta, vuelve a montar y reabre /live.bin sin truncar. Si la tarjeta
 * quedo colgada tras una escritura fallida, esto la reinicia. Corre en la
 * tarea, con el mutex tomado. */
/* Mas de 74 ciclos de reloj con CS alto: el procedimiento del estandar SD para
 * que una tarjeta colgada en medio de un comando vuelva a escuchar. */
void sdWake()
{
#if defined(SIM_SIN_HW) && SIM_SIN_HW
    return;
#else
    pinMode(LIVE_SD_CS, OUTPUT);
    digitalWrite(LIVE_SD_CS, HIGH);
    g_spi.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
    for (int i = 0; i < 20; i++) g_spi.transfer(0xFF);
    g_spi.endTransaction();
#endif
}

bool sdRemount()
{
    g_sdRemounts = g_sdRemounts + 1u;
    g_file.close();
    LIVE_FS.end();
    g_sdMounted = false;
    g_sdMountedHz = 0u;
    vTaskDelay(pdMS_TO_TICKS(50));
    sdWake();
    if (!LIVE_FS.begin(LIVE_SD_CS, g_spi, g_sdHz, "/sd", 5, false)) return false;
    g_sdMounted = true;
    g_sdMountedHz = g_sdHz;
    g_file = LIVE_FS.open(SD_PATH, "r+");
    if (g_file && g_sdw > g_sdSynced) g_sdw = g_sdSynced;
    return (bool)g_file;
}

void sdTaskFn(void *)
{
    uint32_t lastFlushMs = 0;
    bool dirty = false;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
        xSemaphoreTake(g_sdMutex, portMAX_DELAY);
        /* Un remontaje fallido deja g_file invalido: sin esto el pedido de
         * lectura quedaba en 1 para siempre y la descarga se colgaba. */
        if (g_sdOpen && !g_file && g_sdReadState == 1u && !sdRemount()) {
            g_sdReadCount = 0u;
            __sync_synchronize();
            g_sdReadState = 3u;
        }
        if (g_sdOpen && g_file) {
            if (g_sdReadState == 1u) {
                __sync_synchronize();
                const uint32_t seq = g_sdReadSeq;
                const uint32_t sdw = g_sdw;
                const uint32_t avail = (seq < sdw) ? (sdw - seq) : 0u;
                const uint32_t count = (avail < SD_READ_BATCHES) ? avail : SD_READ_BATCHES;
                const size_t bytes = (size_t)count * BATCH_BYTES;
                bool ok = count > 0u && g_file.seek((uint64_t)seq * BATCH_BYTES);
                if (ok) {
                    const uint64_t t0 = (uint64_t)esp_timer_get_time();
                    ok = g_file.read((uint8_t *)g_sdReadBuf, bytes) == bytes;
                    noteEvent(SAMPLE_FLAG_SD_IO, t0, (uint64_t)esp_timer_get_time());
                }
                g_sdReadCount = ok ? count : 0u;
                __sync_synchronize();
                g_sdReadState = ok ? 2u : 3u;
            }
            for (int n = 0; n < 8 && !g_sdFull && g_sdw < g_recv; n++) {
                uint64_t pos = (uint64_t)g_sdw * BATCH_BYTES;
                /* Con la radio transmitiendo alguna escritura falla y la
                 * siguiente anda: se reintenta antes de darla por perdida. */
                /* El EIO aparece aun con la radio callada (2026-10-23) y a
                 * veces un remontaje no alcanza. Se insiste ~2,5 s, remontando
                 * cada 3 fallas: las RAM del ESP y del PSoC aguantan ese atraso. */
                bool ok = false;
                const uint32_t t0 = millis();
                for (int intento = 0; !ok; intento++) {
                    if (intento > 0) {
                        if ((millis() - t0) > SD_RETRY_MS) break;
                        g_sdRetries = g_sdRetries + 1u;
                        if ((intento % 3) == 0) {
                            if (!sdRemount()) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
                            pos = (uint64_t)g_sdw * BATCH_BYTES;   /* rebobino al ultimo flush */
                        } else {
                            vTaskDelay(pdMS_TO_TICKS(30));
                        }
                    }
                    /* El seek tambien falla con la SD en error: va por el mismo reintento. */
                    if (!g_file || (g_file.position() != pos && !g_file.seek(pos))) continue;
                    errno = 0;
                    if (g_file) {
                        const uint64_t writeStart = (uint64_t)esp_timer_get_time();
                        ok = g_file.write((const uint8_t *)slotPtr(g_sdw), BATCH_BYTES) == BATCH_BYTES;
                        noteEvent(SAMPLE_FLAG_SD_IO, writeStart,
                                  (uint64_t)esp_timer_get_time());
                    }
                    if (!ok) g_sdErrno = errno;
                }
                if (!ok) {
                    /* Llena si ya no entra otro lote; si no, fue un error. */
                    if (pos + 2u * BATCH_BYTES < g_sdFreeBytes) g_sdErr = true;
                    g_sdFull = true;
                    break;
                }
                g_sdw = g_sdw + 1u;
                dirty = true;
            }
            if (dirty && (millis() - lastFlushMs) >= 500u) {
                const uint32_t upTo = g_sdw;
                errno = 0;
                const uint64_t flushStart = (uint64_t)esp_timer_get_time();
                g_file.flush();
                noteEvent(SAMPLE_FLAG_SD_SYNC, flushStart,
                          (uint64_t)esp_timer_get_time());
                if (errno == 0) g_sdSynced = upTo;
                else g_sdErrno = errno;
                dirty = false;
                lastFlushMs = millis();
            }
        }
        xSemaphoreGive(g_sdMutex);
    }
}

bool sdMount()
{
    if (g_sdMounted && g_sdMountedHz == g_sdHz) return true;
    if (g_sdMounted) {
        LIVE_FS.end();
        g_sdMounted = false;
        g_sdMountedHz = 0u;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
#if !(defined(SIM_SIN_HW) && SIM_SIN_HW)
    g_spi.begin(LIVE_SD_SCK, LIVE_SD_MISO, LIVE_SD_MOSI, LIVE_SD_CS);
#endif
    /* 2026-10-23: tras un reset del ESP en plena transaccion la tarjeta queda
     * colgada y el primer SD.begin falla ("SD ausente"). Se insiste, despertandola
     * antes de cada intento. */
    for (int intento = 0; intento < 4 && !g_sdMounted; intento++) {
        if (intento > 0) {
            LIVE_FS.end();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        sdWake();
        g_sdMounted = LIVE_FS.begin(LIVE_SD_CS, g_spi, g_sdHz, "/sd", 5, false);
    }
    if (g_sdMounted) {
        g_sdMountedHz = g_sdHz;
        g_sdFreeBytes = LIVE_FS.totalBytes() - LIVE_FS.usedBytes();
    } else {
        g_sdMountedHz = 0u;
        LIVE_FS.end();
    }
    return g_sdMounted;
}

void freePages()
{
    for (uint32_t i = 0; i < g_npages; i++) {
        heap_caps_free(g_pages[i]);
        g_pages[i] = nullptr;
    }
    g_npages = 0;
    g_cap = 0;
}

/* Reserva paginas hasta `want` lotes o hasta dejar HEAP_RESERVE libre. Con
 * PSRAM (S3) la saca de ahi primero. */
void allocPages(uint32_t want)
{
    const uint32_t pages = (want + PAGE_BATCHES - 1) / PAGE_BATCHES;
    while (g_npages < pages && g_npages < MAX_PAGES) {
        void *p = heap_caps_malloc(PAGE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) {
            if (ESP.getFreeHeap() < PAGE_BYTES + HEAP_RESERVE) break;
            p = heap_caps_malloc(PAGE_BYTES, MALLOC_CAP_8BIT);
        }
        if (!p) break;
        g_pages[g_npages++] = (SampleBytes *)p;
    }
    g_cap = g_npages * PAGE_BATCHES;
}

bool sdReadContains(uint32_t seq)
{
    if (g_sdReadState != 2u || seq < g_sdReadSeq) return false;
    __sync_synchronize();
    return (seq - g_sdReadSeq) < g_sdReadCount;
}

const SampleBytes *sdReadPtr(uint32_t seq)
{
    return g_sdReadBuf + (size_t)(seq - g_sdReadSeq) * SPI_BATCH_SAMPLES;
}

bool requestSdRead(uint32_t seq, bool forRt, bool sendWhenReady)
{
    if (seq >= g_sdw) return false;
    if (sdReadContains(seq)) return true;
    if (g_sdReadState == 1u || (g_sdReadState == 2u && g_sdReadPendingSend)) return false;
    g_sdReadSeq = seq;
    g_sdReadCount = 0u;
    g_sdReadForRt = forRt;
    g_sdReadPendingSend = sendWhenReady;
    __sync_synchronize();
    g_sdReadState = 1u;
    if (g_sdTask) xTaskNotifyGive(g_sdTask);
    return false;
}

void queuePsocResend(uint32_t seq)
{
    if (seq == g_psocResend) return;
    for (uint32_t i = 0; i < g_psocQn; i++) {
        if (g_psocQ[i] == seq) return;
    }
    if (g_psocQn < PSOC_RESEND_Q) g_psocQ[g_psocQn++] = seq;
}

bool sendStreamBatch(uint32_t seq)
{
    if (seq >= tailNow()) return sendBatch(seq, slotPtr(seq), true);
    if (g_cfg.storage == CAPMODE_STORAGE_SD) {
        if (sdReadContains(seq)) return sendBatch(seq, sdReadPtr(seq), true);
        requestSdRead(seq, true, false);
    } else if (g_cfg.storage == CAPMODE_STORAGE_PSOC) {
        queuePsocResend(seq);
    }
    return false;
}

void retransmit(uint32_t seq)
{
    if (seq >= g_recv) return;
    g_retx++;
    if (seq >= tailNow()) {
        sendBatch(seq, slotPtr(seq), true);
    } else if (g_cfg.storage == CAPMODE_STORAGE_SD) {
        if (sdReadContains(seq)) sendBatch(seq, sdReadPtr(seq), true);
        else requestSdRead(seq, true, true);
    } else if (g_cfg.storage == CAPMODE_STORAGE_PSOC) {
        queuePsocResend(seq);
    }
}

void servicePsocFlow()
{
    if (!g_active) return;
    if (g_psocDone && g_recv >= g_total) return;
    const uint32_t tail = tailNow();
    const uint32_t room = tail + g_cap - PSOC_WINDOW;
    const uint32_t ackv = (g_recv < room) ? g_recv : room;
    const uint32_t now = millis();
    if (ackv != g_psocAckSent || (now - g_psocAckMs) >= PSOC_ACK_MS) {
        g_psoc->liveAck((uint16_t)ackv, 0u);
        g_psocAckSent = ackv;
        g_psocAckMs = now;
    }
}

void serviceRealtime()
{
    if (!streaming() || !g_started) return;
    if (g_cfg.streamDump && !g_cfg.realtime && !g_radioReady) return;
    const uint32_t now = millis();

    /* Reenvios desde la RAM del PSoC: uno en vuelo a la vez. */
    if (g_psocQn > 0u &&
        (g_psocResend == NONE || (now - g_psocResendMs) >= PSOC_RESEND_MS)) {
        g_psocResend = g_psocQ[0];
        for (uint32_t i = 1; i < g_psocQn; i++) g_psocQ[i - 1] = g_psocQ[i];
        g_psocQn--;
        g_psocResendMs = now;
        g_psoc->liveAck((uint16_t)g_psocResend, PSOC_LIVE_ACK_ONE);
    }

    if (g_sdReadState == 2u && g_sdReadForRt && g_sdReadPendingSend) {
        if (sdReadContains(g_sdReadSeq)) sendBatch(g_sdReadSeq, sdReadPtr(g_sdReadSeq), true);
        g_sdReadPendingSend = false;
        g_sdReadFails = 0u;
    } else if (g_sdReadState == 3u && g_sdReadForRt) {
        g_sdReadPendingSend = false;
        g_sdReadState = 0u;
        /* La SD no devuelve lo viejo: antes de colgar la descarga, se cierra
         * en lo ya confirmado y el fin sale con motivo de error. */
        if (++g_sdReadFails >= SD_READ_FAIL_MAX && g_psocDone && g_total != NONE) {
            SLAVE_LOG_PRINTF("[LIVE] SD ilegible: cierro el stream en %lu de %lu\n",
                             (unsigned long)g_mack, (unsigned long)g_total);
            g_sdErr = true;
            g_total = g_mack;
            if (g_txNext > g_total) g_txNext = g_total;
        }
    }

    if (g_mack < g_txNext && (now - g_mackMs) >= RTO_MS && (now - g_retxMs) >= RTO_MS &&
        radioRoom()) {
        g_retxMs = now;
        retransmit(g_mack);
        int budget = 7;
        for (uint32_t i = 0; i < 32 && budget > 0; i++) {
            const uint32_t s = g_mack + 1u + i;
            if (s >= g_txNext) break;
            if ((g_sack & (1u << i)) == 0u) { retransmit(s); budget--; }
        }
    }

    for (int budget = 4; budget > 0; budget--) {
        if (g_txNext >= g_recv || (g_txNext - g_mack) >= MASTER_WINDOW) break;
        if (!radioRoom()) break;
        if (!sendStreamBatch(g_txNext)) break;
        g_txNext++;
    }
}

}  // namespace

/* ── API ───────────────────────────────────────────────────────────────── */

void begin(PsocUART *psoc, RawSendFn send, uint8_t nodeId)
{
    g_psoc = psoc;
    g_radio = send;
    espnowSentHook() = onRadioSent;
    g_node = nodeId;
    for (uint32_t i = 0; i < 256; i++) g_simTag[i] = NONE;
    if (!g_sdMutex) g_sdMutex = xSemaphoreCreateMutex();
    if (!g_sdTask) {
        /* Nucleo 1, con loop(): en el 0 la tarea WiFi (mas prioritaria) la
         * dejaba sin CPU en plena transaccion y la SD quedaba en error. */
        xTaskCreatePinnedToCore(sdTaskFn, "live_sd", 4096, nullptr, 1, &g_sdTask, 1);
    }
    sdMount();
    SLAVE_LOG_PRINTF("[LIVE] SD %s libre=%llu MB\n", g_sdMounted ? "ok" : "ausente",
                     (unsigned long long)(g_sdFreeBytes / (1024ull * 1024ull)));
}

void setSdHz(uint32_t hz)
{
    if (hz < 400000u || hz > 20000000u) return;
    if (!g_sdMutex) {
        g_sdHz = hz;
        return;
    }
    xSemaphoreTake(g_sdMutex, portMAX_DELAY);
    g_sdHz = hz;
    if (g_sdMounted && !g_sdOpen) {
        LIVE_FS.end();
        g_sdMounted = false;
        g_sdMountedHz = 0u;
        vTaskDelay(pdMS_TO_TICKS(50));
        g_sdMounted = LIVE_FS.begin(LIVE_SD_CS, g_spi, g_sdHz, "/sd", 5, false);
        if (g_sdMounted) {
            g_sdMountedHz = g_sdHz;
            g_sdFreeBytes = LIVE_FS.totalBytes() - LIVE_FS.usedBytes();
        } else {
            LIVE_FS.end();
        }
    }
    xSemaphoreGive(g_sdMutex);
}

uint32_t sdHz() { return g_sdHz; }
void setRadioReady(bool ready) { g_radioReady = ready; }

int32_t prepare(const Config &c)
{
    close();
    g_cfg = c;
    portENTER_CRITICAL(&g_markMux);
    g_markHead = g_markTail = 0u;
    g_radioTimeHead = g_radioTimeTail = 0u;
    g_havePrevAnchor = false;
    g_havePair = false;
    g_pairCount = 0u;
    g_highestStored = 0u;
    for (uint8_t i = 0; i < 6u; i++) g_markApplied[i] = 0u;
    portEXIT_CRITICAL(&g_markMux);
    uint32_t n = c.nBatches;
    uint32_t want;
    if (c.storage == CAPMODE_STORAGE_PSOC) {
        want = PSOC_RT_RING;
    } else if (c.storage == CAPMODE_STORAGE_SD) {
        want = SD_RT_RING;
    } else {
        want = MAX_PAGES * PAGE_BATCHES;
        if (n != 0u && n < want) want = n;
    }
    if (want < 2u * PSOC_WINDOW) want = 2u * PSOC_WINDOW;
    allocPages(want);
    if (c.storage == CAPMODE_STORAGE_SHARED && g_cap < want) g_lastSharedPages = g_npages;
    if (g_cap < 2u * PSOC_WINDOW) {
        freePages();
        SLAVE_LOG_PRINTLN("[LIVE] sin RAM para el anillo");
        return -1;
    }

    if (c.storage == CAPMODE_STORAGE_SD) {
        if (!sdMount()) {
            freePages();
            SLAVE_LOG_PRINTLN("[LIVE] sin SD");
            return -1;
        }
        xSemaphoreTake(g_sdMutex, portMAX_DELAY);
        g_file = LIVE_FS.open(SD_PATH, "w+");
        const bool ok = (bool)g_file;
        g_sdFull = false;
        g_sdReadFails = 0u;
        g_sdErr = false;
        g_sdRetries = 0;
        g_sdRemounts = 0;
        g_sdErrno = 0;
        g_sdw = 0;
        g_sdSynced = 0;
        g_sdReadState = 0;
        g_sdReadCount = 0;
        g_sdReadPendingSend = false;
        g_sdOpen = ok;
        xSemaphoreGive(g_sdMutex);
        if (!ok) {
            freePages();
            SLAVE_LOG_PRINTLN("[LIVE] no se pudo abrir /live.bin");
            return -1;
        }
        const uint64_t fit = g_sdFreeBytes / BATCH_BYTES;
        if (n != 0u && n > fit) n = (uint32_t)fit;
    }

    if (!c.realtime && c.storage == CAPMODE_STORAGE_SHARED) {
        /* Sin el maestro descargando, entra lo del ESP mas lo del PSoC. */
        const uint32_t fit = g_cap + PSOC_RING - PSOC_WINDOW;
        if (n == 0u || n > fit) n = fit;
    }
    if (c.storage == CAPMODE_STORAGE_PSOC && (n == 0u || n > PSOC_RING)) n = PSOC_RING;
    if (!c.realtime && n == 0u) n = 1u;
    if (n > 0xFFFFu) n = 0xFFFFu;   /* N del PSoC es de 16 bits */
    g_cfg.nBatches = n;

    g_recv = 0;
    g_txNext = g_mack = g_sack = g_dumpAck = 0;
    g_mackMs = g_retxMs = millis();
    g_total = NONE;
    g_totalB1 = g_totalB2 = 0;
    g_psocDone = g_overrun = g_stopReq = false;
    g_psocAckSent = NONE;
    g_psocAckMs = 0;
    g_rewindMs = 0;
    g_psocResend = NONE;
    g_psocQn = 0;
    g_sdReadCount = 0;
    g_sdReadPendingSend = false;
    g_gaps = g_dups = g_retx = g_txOk = g_txFail = g_badRamp = g_noRoom = 0;
    g_txBytes = 0;
    g_simNext = 0;
    g_simRx = g_simDup = g_simDrop = g_simWin = g_simBad = 0;
    for (uint32_t i = 0; i < 256; i++) { g_simTag[i] = NONE; g_simParts[i] = 0; }
    g_started = false;
    g_active = true;
    SLAVE_LOG_PRINTF("[LIVE] prepare storage=%u rt=%u n=%lu cap=%lu heap=%u\n",
                     (unsigned)c.storage, (unsigned)c.realtime, (unsigned long)n,
                     (unsigned long)g_cap, (unsigned)ESP.getFreeHeap());
    return (int32_t)n;
}

void close()
{
    portENTER_CRITICAL(&g_markMux);
    g_started = false;
    g_active = false;
    portEXIT_CRITICAL(&g_markMux);
    if (g_sdMutex) {
        xSemaphoreTake(g_sdMutex, portMAX_DELAY);
        if (g_sdOpen) {
            g_sdOpen = false;
            g_file.close();
            if (g_sdMounted) g_sdFreeBytes = LIVE_FS.totalBytes() - LIVE_FS.usedBytes();
        }
        xSemaphoreGive(g_sdMutex);
    }
    freePages();
}

bool active() { return g_active; }
const Config &config() { return g_cfg; }

void start(uint64_t tStartUs, uint32_t fsHz)
{
    if (!g_active) return;
    g_t0 = tStartUs;
    portENTER_CRITICAL(&g_markMux);
    g_fs = fsHz ? fsHz : 2604u;
    g_fsEstimate = g_fs;
    g_started = true;
    portEXIT_CRITICAL(&g_markMux);
    g_mackMs = g_retxMs = millis();
}

void noteEvent(uint8_t bits, uint64_t t0Us, uint64_t t1Us)
{
    portENTER_CRITICAL(&g_markMux);
    if (g_active && g_started && g_havePair) {
        uint32_t s0 = sampleForTimeLocked(t0Us);
        uint32_t s1 = sampleForTimeLocked(t1Us);
        if (s1 < s0) {
            const uint32_t t = s0;
            s0 = s1;
            s1 = t;
        }
        pushMarkLocked(s0, s1, bits);
    }
    portEXIT_CRITICAL(&g_markMux);
}

void noteRadioTxStart(size_t len)
{
    const uint32_t airtimeUs = (uint32_t)((len + 50u) * 8u + 300u);
    portENTER_CRITICAL(&g_markMux);
    const uint8_t next = (uint8_t)((g_radioTimeHead + 1u) % RADIO_TIME_RING);
    if (next == g_radioTimeTail) {
        g_radioTimeTail = (uint8_t)((g_radioTimeTail + 1u) % RADIO_TIME_RING);
    }
    g_radioAirtimeUs[g_radioTimeHead] = airtimeUs;
    g_radioTimeHead = next;
    portEXIT_CRITICAL(&g_markMux);
}

void noteRadioTxDone(uint64_t t1Us)
{
    uint32_t airtimeUs = 0u;
    bool found = false;
    portENTER_CRITICAL(&g_markMux);
    if (g_radioTimeTail != g_radioTimeHead) {
        airtimeUs = g_radioAirtimeUs[g_radioTimeTail];
        g_radioTimeTail = (uint8_t)((g_radioTimeTail + 1u) % RADIO_TIME_RING);
        found = true;
    }
    portEXIT_CRITICAL(&g_markMux);
    if (found) {
        const uint64_t t0Us = t1Us > airtimeUs ? t1Us - airtimeUs : 0u;
        noteEvent(SAMPLE_FLAG_RADIO_TX, t0Us, t1Us);
    }
}

void onPsocBatch(const PsocBatch &b)
{
    if (!g_active) return;
    processAnchor(b);
    const uint32_t recv = g_recv;
    const uint32_t seq = recv + (uint32_t)(int32_t)(int16_t)(uint16_t)(b.seq - (uint16_t)recv);
    if (seq < recv) {
        if (seq == g_psocResend) {
            SampleBytes tmp[SPI_BATCH_SAMPLES];
            toStored(b, tmp);
            applyMarks(tmp, seq * SPI_BATCH_SAMPLES, seq);
            const bool sent = sendBatch(seq, tmp, true);
            if (sent && seq == g_txNext) g_txNext++;
            g_psocResend = NONE;
        } else {
            g_dups++;
        }
        return;
    }
    const uint32_t now = millis();
    if (seq > recv) {
        /* Se perdio una trama I2C: que el PSoC vuelva a mandar desde recv. */
        g_gaps++;
        if ((now - g_rewindMs) >= REWIND_MS) {
            g_rewindMs = now;
            g_psoc->liveAck((uint16_t)recv, PSOC_LIVE_ACK_REWIND);
        }
        return;
    }
    if ((recv - tailNow()) >= g_cap) {
        g_noRoom++;
        return;
    }
    SampleBytes *dst = slotPtr(seq);
    toStored(b, dst);
    applyMarks(dst, seq * SPI_BATCH_SAMPLES, seq);
    if (g_cfg.ramp) g_badRamp += rampErrors(dst, seq * SPI_BATCH_SAMPLES, SPI_BATCH_SAMPLES);
    __sync_synchronize();   /* el lote completo antes que el contador (tarea SD) */
    g_recv = recv + 1u;
    if (g_cfg.storage == CAPMODE_STORAGE_SD && g_sdTask) xTaskNotifyGive(g_sdTask);
}

void onPsocTotalByte(uint8_t index, uint8_t value)
{
    if (index == 1u) g_totalB1 = value;
    else if (index == 2u) g_totalB2 = value;
}

void onPsocDone(uint8_t lowByte)
{
    if (!g_active) return;
    g_total = (uint32_t)lowByte | ((uint32_t)g_totalB1 << 8) | ((uint32_t)g_totalB2 << 16);
    g_psocDone = true;
    /* El plazo de abandono por maestro ausente cuenta desde aca: la descarga
     * rapida recien empieza a transmitir ahora. */
    g_ackRxMs = millis();
    SLAVE_LOG_PRINTF("[LIVE] PSoC cerro: total=%lu recv=%lu\n",
                     (unsigned long)g_total, (unsigned long)g_recv);
}

void onPsocOverrun() { g_overrun = true; }

void onMasterAck(uint32_t nextExpected, uint32_t sack)
{
    if (!g_active || !streaming()) return;
    g_ackRxMs = millis();
    if (nextExpected > g_txNext) nextExpected = g_txNext;
    if (nextExpected > g_mack) {
        g_mack = nextExpected;
        g_mackMs = millis();
    }
    g_sack = sack;
}

void requestStop() { g_stopReq = true; }

bool serveReq(uint32_t seq)
{
    if (!g_active) return false;
    if (g_psocDone && seq >= g_total) return false;
    if (g_cfg.storage == CAPMODE_STORAGE_SHARED && seq > g_dumpAck) g_dumpAck = seq;
    if (seq < g_recv && seq >= tailNow()) {
        return sendBatch(seq, slotPtr(seq), false);
    }
    if (g_cfg.storage == CAPMODE_STORAGE_SD && seq < g_sdw) {
        /* La tarea lee bloques de 32 lotes: el pedido siguiente casi siempre
         * ya esta en el buffer. Antes se mandaba uno y se tiraba el resto, y
         * la descarga clasica desde SD iba a 4 lotes/s (2026-10-23). */
        if (!g_sdReadForRt && sdReadContains(seq)) return sendBatch(seq, sdReadPtr(seq), false);
        requestSdRead(seq, false, true);
        return true;   /* sale en service() cuando la tarea lo lea */
    }
    return false;
}

void service()
{
    if (!g_active) return;
    servicePsocFlow();
    if (!streaming() && g_sdReadState == 2u && !g_sdReadForRt && g_sdReadPendingSend) {
        sendBatch(g_sdReadSeq, sdReadPtr(g_sdReadSeq), false);
        g_sdReadPendingSend = false;   /* el bloque queda para los pedidos siguientes */
    } else if (!streaming() && g_sdReadState == 3u && !g_sdReadForRt) {
        g_sdReadPendingSend = false;
        g_sdReadState = 0u;
    }
    serviceRealtime();
    simService();
}

bool psocDone() { return g_psocDone; }

uint32_t msSinceMasterAck()
{
    if (!g_active || !g_started || !streaming()) return 0u;
    const uint32_t ref = g_ackRxMs ? g_ackRxMs : g_mackMs;
    return millis() - ref;
}
uint32_t total() { return g_total; }

bool finished()
{
    if (!g_active || !g_psocDone) return false;
    if (g_cfg.realtime || g_cfg.streamDump) return g_mack >= g_total;
    if (g_cfg.storage == CAPMODE_STORAGE_SD) return g_recv >= g_total && g_sdSynced >= g_total;
    return true;
}

uint8_t endReason()
{
    if (g_sdErr) return STREAM_END_ERROR;
    if (g_sdFull) return STREAM_END_SD_FULL;
    if (g_overrun) return STREAM_END_NO_MEMORY;
    if (g_stopReq) return STREAM_END_STOP;
    if (g_cfg.nBatches != 0u && g_total == g_cfg.nBatches) return STREAM_END_COMPLETE;
    return STREAM_END_ERROR;
}

bool wantsStop() { return g_active && g_started && !g_psocDone && g_sdFull; }

uint32_t ramCapacityBatches()
{
    if (g_active && g_cfg.storage == CAPMODE_STORAGE_SHARED) {
        return g_cap + PSOC_RING - PSOC_WINDOW;
    }
    const size_t freeHeap = ESP.getFreeHeap();
    uint32_t pages = (freeHeap > HEAP_RESERVE) ? (uint32_t)((freeHeap - HEAP_RESERVE) / PAGE_BYTES) : 0u;
    /* El heap libre total no es lo que despues reserva prepare(): la cabecera
     * de cada malloc y lo que se toma entre ARM y PRESTART se comen ~2 paginas
     * (2026-10-23: el HELLO decia 848 y prepare consiguio 784). Si la web pide
     * mas de lo que entra, el esclavo recorta y la captura llega incompleta. */
    pages = (pages > 2u) ? pages - 2u : 0u;
    if (g_lastSharedPages != 0u && pages > g_lastSharedPages) pages = g_lastSharedPages;
    if (pages > MAX_PAGES) pages = MAX_PAGES;
    return pages * PAGE_BATCHES + PSOC_RING - PSOC_WINDOW;
}

uint32_t sdCapacityBatches()
{
    if (!g_sdMounted) return 0u;
    const uint64_t fit = g_sdFreeBytes / BATCH_BYTES;
    return (fit > 0xFFFFu) ? 0xFFFFu : (uint32_t)fit;
}

void printStatus()
{
    LIVE_RAW("#LIVE act=%u st=%u rt=%u n=%lu cap=%lu recv=%lu tail=%lu sdw=%lu tx=%lu mack=%lu "
             "total=%ld done=%u ovr=%u sdfull=%u gaps=%lu dups=%lu noroom=%lu retx=%lu "
             "txok=%lu txfail=%lu badramp=%lu sim=%u srx=%lu snext=%lu sdup=%lu sdrop=%lu "
             "swin=%lu sbad=%lu heap=%u txerr=0x%x inflight=%lu sdretry=%lu remount=%lu errno=%d "
             "marks=%lu/%lu/%lu/%lu/%lu/%lu pairs=%lu txbytes=%lu\n",
             (unsigned)g_active, (unsigned)g_cfg.storage, (unsigned)g_cfg.realtime,
             (unsigned long)g_cfg.nBatches, (unsigned long)g_cap,
             (unsigned long)g_recv, (unsigned long)(g_active ? tailNow() : 0u),
             (unsigned long)g_sdw, (unsigned long)g_txNext, (unsigned long)g_mack,
             (long)(g_total == NONE ? -1 : (long)g_total), (unsigned)g_psocDone,
             (unsigned)g_overrun, (unsigned)g_sdFull,
             (unsigned long)g_gaps, (unsigned long)g_dups, (unsigned long)g_noRoom,
             (unsigned long)g_retx, (unsigned long)g_txOk, (unsigned long)g_txFail,
             (unsigned long)g_badRamp, (unsigned)g_simOn, (unsigned long)g_simRx,
             (unsigned long)g_simNext, (unsigned long)g_simDup, (unsigned long)g_simDrop,
             (unsigned long)g_simWin, (unsigned long)g_simBad, (unsigned)ESP.getFreeHeap(), (unsigned)g_lastTxErr,
             (unsigned long)g_inflight, (unsigned long)g_sdRetries,
             (unsigned long)g_sdRemounts, g_sdErrno,
             (unsigned long)g_markApplied[0], (unsigned long)g_markApplied[1],
             (unsigned long)g_markApplied[2], (unsigned long)g_markApplied[3],
             (unsigned long)g_markApplied[4], (unsigned long)g_markApplied[5],
             (unsigned long)g_pairCount, (unsigned long)g_txBytes);
}

void simMaster(bool on, uint8_t lossPct)
{
    g_simOn = on;
    g_simLoss = lossPct > 90u ? 90u : lossPct;
}

bool simEnabled() { return g_simOn; }

/* Banco: hace de maestro asincronico por USB. Pide los lotes en orden, como
 * el volcado REQ_BATCH (libera lo ya pedido en modo compartido) y verifica
 * la rampa. Con hex imprime el mismo "#DUMP" que dumpusb. */
void dumpUsb(bool hex)
{
    if (!g_active) {
        LIVE_RAW("#LDUMP sin_sesion\n");
        return;
    }
    static const char hexd[] = "0123456789abcdef";
    static char line[32 + BATCH_BYTES * 2];
    uint32_t seq = 0, bad = 0;
    uint32_t lastProgress = millis();
    LIVE_RAW("#LDUMP_BEGIN storage=%u rt=%u n=%lu\n", (unsigned)g_cfg.storage,
             (unsigned)g_cfg.realtime, (unsigned long)g_cfg.nBatches);
    for (;;) {
        g_psoc->poll();
        servicePsocFlow();
        if (g_psocDone && seq >= g_total) break;
        if ((millis() - lastProgress) > 5000u) {
            LIVE_RAW("#LDUMP timeout seq=%lu recv=%lu\n", (unsigned long)seq,
                     (unsigned long)g_recv);
            break;
        }
        if (g_cfg.storage == CAPMODE_STORAGE_SHARED && seq > g_dumpAck) g_dumpAck = seq;
        const SampleBytes *p = nullptr;
        if (seq < g_recv && seq >= tailNow()) {
            p = slotPtr(seq);
        } else if (g_cfg.storage == CAPMODE_STORAGE_SD && seq < g_sdw) {
            if (g_sdReadState == 0u) {
                g_sdReadSeq = seq;
                g_sdReadForRt = false;
                g_sdReadState = 1u;
                xTaskNotifyGive(g_sdTask);
            } else if (g_sdReadState == 2u && g_sdReadSeq == seq) {
                p = g_sdReadBuf;
            } else if (g_sdReadState == 3u) {
                LIVE_RAW("#LDUMP sd_read_err seq=%lu\n", (unsigned long)seq);
                g_sdReadState = 0u;
                break;
            }
        } else if (seq < tailNow()) {
            LIVE_RAW("#LDUMP perdido seq=%lu tail=%lu\n", (unsigned long)seq,
                     (unsigned long)tailNow());
            break;
        }
        if (!p) { delay(1); continue; }
        if (g_cfg.ramp) bad += rampErrors(p, seq * SPI_BATCH_SAMPLES, SPI_BATCH_SAMPLES);
        if (hex) {
            const uint8_t *b = (const uint8_t *)p;
            int pos = snprintf(line, sizeof(line), "#DUMP %lu ", (unsigned long)seq);
            uint8_t crc = 0;
            for (size_t i = 0; i < BATCH_BYTES; i++) {
                line[pos++] = hexd[(b[i] >> 4) & 0x0Fu];
                line[pos++] = hexd[b[i] & 0x0Fu];
                crc ^= b[i];
                for (int k = 0; k < 8; k++) crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x07u) : (uint8_t)(crc << 1);
            }
            snprintf(line + pos, sizeof(line) - (size_t)pos, " %02x\n", crc);
            LIVE_RAW("%s", line);
        }
        if (p == g_sdReadBuf) g_sdReadState = 0u;
        seq++;
        lastProgress = millis();
        if ((seq & 63u) == 0u) yield();
    }
    LIVE_RAW("#LDUMP_END n=%lu total=%ld bad=%lu\n", (unsigned long)seq,
             (long)(g_total == NONE ? -1 : (long)g_total), (unsigned long)bad);
}

}  // namespace live
