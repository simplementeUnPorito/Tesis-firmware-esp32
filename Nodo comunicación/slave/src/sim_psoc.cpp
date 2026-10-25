#include "sim_psoc.h"

#if defined(SIM_SIN_HW) && SIM_SIN_HW

#include <Arduino.h>
#include <esp_timer.h>
#include <math.h>
#include <string.h>

#include "psoc_uart.h"
#include "sim_sd.h"
#include "sync_protocol.h"

#ifndef SYNC_TO_PSOC_PIN
#define SYNC_TO_PSOC_PIN 27
#endif

namespace {
constexpr uint32_t NATIVE_FS = 2604u;
constexpr uint32_t RAM_BATCHES = 512u;
constexpr uint32_t LIVE_WINDOW = 16u;
constexpr uint32_t SEND_TIME_US = 2500u;
constexpr float SIM_TWO_PI = 6.2831853071795864769f;

enum SimState : uint8_t { IDLE = 0u, ARMED = 1u, SAMPLING = 2u, CALIBRATING = 3u };

bool g_enabled = true;
SimState g_state = IDLE;
uint16_t g_n = 0u;
uint8_t g_decimation = 1u;
uint16_t g_fs = NATIVE_FS;
uint8_t g_liveMode = 0u;
bool g_sdCapture = false;
bool g_debug = false;
bool g_lastSync = false;
bool g_doneReported = false;
uint32_t g_produced = 0u;
uint32_t g_sent = 0u;
uint32_t g_acked = 0u;
uint32_t g_dump = 0u;
uint32_t g_wireBatch = 0u;
uint32_t g_prevSendEnd = 0xFFFFFFFFu;
uint64_t g_nextBatchUs = 0u;
uint8_t g_pga = 0u, g_pgaout = 0u, g_pgavdac = 0u;
uint8_t g_fir[128u][3u] = {};
uint8_t g_capture[RAM_BATCHES][SPI_BATCH_SAMPLES * 3u] = {};
uint32_t g_artifactLast[5] = {
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu
};
uint32_t g_artifactStart[5] = {};

uint8_t xorBytes(const uint8_t *data, size_t begin, size_t end)
{
    uint8_t crc = 0u;
    for (size_t i = begin; i < end; ++i) crc ^= data[i];
    return crc;
}

void inject(const uint8_t *data, size_t len)
{
    if (g_enabled) PsocUART::simInjectRx(data, len);
}

void sendPing()
{
    const uint8_t f[4] = {PSOC_FRAME_MARKER, PSOC_CTRL_PING, 0u, PSOC_CTRL_PING};
    inject(f, sizeof(f));
}

void sendAck(uint8_t cmd, uint8_t value)
{
    const uint8_t f[5] = {PSOC_FRAME_MARKER, PSOC_CTRL_CFG_ACK, cmd, value,
                          (uint8_t)(PSOC_CTRL_CFG_ACK ^ cmd ^ value)};
    inject(f, sizeof(f));
}

void sendFs()
{
    const uint8_t lo = (uint8_t)g_fs, hi = (uint8_t)(g_fs >> 8u);
    const uint8_t f[5] = {PSOC_FRAME_MARKER, PSOC_CTRL_FS_REPORT, lo, hi,
                          (uint8_t)(PSOC_CTRL_FS_REPORT ^ lo ^ hi)};
    inject(f, sizeof(f));
}

void sendDiag(uint8_t event, uint8_t value)
{
    const uint8_t state = (uint8_t)g_state;
    const uint8_t f[6] = {PSOC_FRAME_MARKER, PSOC_CTRL_DIAG_EVT, event, value,
                          state, (uint8_t)(PSOC_CTRL_DIAG_EVT ^ event ^ value ^ state)};
    inject(f, sizeof(f));
}

void sendMetadata(uint16_t key, int32_t value)
{
    uint8_t f[11] = {PSOC_FRAME_MARKER, 0xC6u, PSOC_CONTROL_METADATA_VERSION,
                     0u, (uint8_t)key, (uint8_t)(key >> 8u)};
    const uint32_t v = (uint32_t)value;
    f[6] = (uint8_t)v; f[7] = (uint8_t)(v >> 8u);
    f[8] = (uint8_t)(v >> 16u); f[9] = (uint8_t)(v >> 24u);
    f[10] = xorBytes(f, 2u, 10u);
    inject(f, sizeof(f));
}

uint8_t sdStatus()
{
    return (uint8_t)(0x01u | 0x02u | 0x08u | 0x10u | (g_sdCapture ? 0x40u : 0u));
}

void announce()
{
    sendDiag(PSOC_EVT_BOOT, 0u);       /* PSOC_HW_GEO */
    sendDiag(PSOC_EVT_SD_STATUS, sdStatus());
    sendPing();
    sendFs();
    sendMetadata(0x100u, 2);           /* control running */
    sendMetadata(0x101u, 1);           /* learned profile */
    sendMetadata(0x102u, 1);           /* in band */
}

int32_t signalAt(uint32_t sample)
{
    const float t = (float)sample / (float)(g_fs ? g_fs : NATIVE_FS);
    uint32_t rnd = sample * 1664525u + 1013904223u;
    /* Amplitud de geofono quieto: los artefactos (3000-8000 cuentas) dominan
     * la alta frecuencia como en campo. Con 200000 la senal tapaba las firmas
     * y la limpieza no se podia evaluar. */
    const int32_t noise = ((int32_t)((rnd >> 24u) & 0xFFu) - 128) * 2;
    const float wave = 2000.0f * sinf(SIM_TWO_PI * 12.0f * t) +
                       1000.0f * sinf(SIM_TWO_PI * 37.0f * t);
    int32_t value = (int32_t)lroundf(wave) + noise;
    if (value > 0x7FFFFF) value = 0x7FFFFF;
    if (value < -0x800000) value = -0x800000;
    return value;
}

void put24(uint8_t *dst, int32_t value)
{
    const uint32_t v = (uint32_t)value & 0xFFFFFFu;
    dst[0] = (uint8_t)v;
    dst[1] = (uint8_t)(v >> 8u);
    dst[2] = (uint8_t)(v >> 16u);
}

void makeBatch(uint32_t batch)
{
    uint8_t *dst = g_capture[batch % RAM_BATCHES];
    for (uint32_t i = 0u; i < SPI_BATCH_SAMPLES; ++i) {
        put24(dst + i * 3u, signalAt(batch * SPI_BATCH_SAMPLES + i));
    }
}

void sendBatch(uint32_t batch, bool live)
{
    uint8_t f[PSOC_LIVE_FRAME_BYTES] = {};
    const size_t bytes = live ? PSOC_LIVE_FRAME_BYTES : PSOC_FRAME_BYTES;
    f[0] = PSOC_FRAME_MARKER;
    f[1] = live ? PSOC_LIVE_FRAME : SPI_BATCH_SAMPLES;
    f[2] = (uint8_t)batch;
    f[3] = (uint8_t)(batch >> 8u);
    memcpy(f + 4u, g_capture[batch % RAM_BATCHES], SPI_BATCH_SAMPLES * 3u);
    if (live) {
        const uint32_t anchor = g_produced * SPI_BATCH_SAMPLES;
        f[94] = (uint8_t)anchor; f[95] = (uint8_t)(anchor >> 8u);
        f[96] = (uint8_t)(anchor >> 16u); f[97] = (uint8_t)(anchor >> 24u);
        f[98] = (uint8_t)g_prevSendEnd; f[99] = (uint8_t)(g_prevSendEnd >> 8u);
        f[100] = (uint8_t)(g_prevSendEnd >> 16u); f[101] = (uint8_t)(g_prevSendEnd >> 24u);
        const uint32_t during = (g_fs * SEND_TIME_US + 999999u) / 1000000u;
        g_prevSendEnd = anchor + (during ? during - 1u : 0u);
    }
    f[bytes - 1u] = xorBytes(f, 0u, bytes - 1u);
    g_wireBatch = batch;
    inject(f, bytes);
}

void reportDone()
{
    if (g_doneReported) return;
    g_doneReported = true;
    if (g_liveMode != 0u) {
        sendDiag(PSOC_EVT_LIVE_TOTAL1, (uint8_t)(g_produced >> 8u));
        sendDiag(PSOC_EVT_LIVE_TOTAL2, (uint8_t)(g_produced >> 16u));
        sendDiag(PSOC_EVT_LIVE_DONE, (uint8_t)g_produced);
    } else {
        sendDiag(PSOC_EVT_CAPTURE_DONE,
                 (uint8_t)(g_produced > 255u ? 255u : g_produced));
        if (g_sdCapture) {
            sendDiag(PSOC_EVT_SD_SESSION, 1u);
            sendDiag(PSOC_EVT_DUMP_DONE,
                     (uint8_t)(g_produced > 255u ? 255u : g_produced));
        } else {
            sendDiag(PSOC_EVT_DUMP_START,
                     (uint8_t)(g_produced > 255u ? 255u : g_produced));
        }
    }
}

void startCapture()
{
    g_state = SAMPLING;
    g_produced = g_sent = g_acked = g_dump = 0u;
    g_prevSendEnd = 0xFFFFFFFFu;
    g_doneReported = false;
    g_nextBatchUs = (uint64_t)esp_timer_get_time() +
                    (uint64_t)SPI_BATCH_SAMPLES * 1000000ULL / g_fs;
    sendDiag(PSOC_EVT_SYNC_RISE, 1u);
    sendDiag(PSOC_EVT_SAMPLING_START, (uint8_t)(g_n > 255u ? 255u : g_n));
}

uint32_t unwrap16(uint16_t low, uint32_t reference)
{
    return reference - (uint32_t)(uint16_t)((uint16_t)reference - low);
}

int32_t readSample24(const SampleBytes &s)
{
    uint32_t v = (uint32_t)s.digi0 | ((uint32_t)s.digi1 << 8u) |
                 ((uint32_t)s.digi2 << 16u);
    return (v & 0x800000u) ? (int32_t)(v | 0xFF000000u) : (int32_t)v;
}

void writeSampleSignal(SampleBytes &s, int32_t value)
{
    if (value > 0x7FFFFF) value = 0x7FFFFF;
    if (value < -0x800000) value = -0x800000;
    const uint32_t v = (uint32_t)value & 0xFFFFFFu;
    s.digi0 = (uint8_t)v; s.digi1 = (uint8_t)(v >> 8u); s.digi2 = (uint8_t)(v >> 16u);
    const int16_t raw = (int16_t)value;
    s.raw_lo = (uint8_t)raw; s.raw_hi = (uint8_t)((uint16_t)raw >> 8u);
}
}  // namespace

void simPsocBegin()
{
    simSdBegin();
    g_enabled = true;
    g_state = IDLE;
    g_lastSync = digitalRead(SYNC_TO_PSOC_PIN) == HIGH;
    announce();
}

void simPsocService()
{
    if (!g_enabled) return;
    const bool sync = digitalRead(SYNC_TO_PSOC_PIN) == HIGH;
    if (g_state == ARMED && sync && !g_lastSync) startCapture();
    if (g_state == SAMPLING && !sync && g_lastSync && g_n == 0u && !g_debug) {
        g_state = IDLE;
        sendDiag(PSOC_EVT_SYNC_FALL, 0u);
        reportDone();
    }
    g_lastSync = sync;

    if (g_state == SAMPLING) {
        const uint64_t now = (uint64_t)esp_timer_get_time();
        const uint64_t period = (uint64_t)SPI_BATCH_SAMPLES * 1000000ULL / g_fs;
        uint8_t catchup = 0u;
        while (now >= g_nextBatchUs && catchup++ < 4u) {
            if (g_liveMode == 0u && !g_debug && g_produced >= RAM_BATCHES) {
                sendDiag(PSOC_EVT_CAPTURE_CLAMPED, (uint8_t)RAM_BATCHES);
                g_state = IDLE;
                reportDone();
                break;
            }
            makeBatch(g_produced);
            g_produced++;
            if (g_debug) {
                sendBatch(g_produced - 1u, false);
                g_sent = g_produced;
            }
            g_nextBatchUs += period;
            if (!g_debug && g_n != 0u && g_produced >= g_n) {
                g_state = IDLE;
                reportDone();
                break;
            }
        }
    }

    if (g_liveMode != 0u) {
        if (g_sent < g_produced && (g_sent <= g_acked || g_sent - g_acked < LIVE_WINDOW)) {
            sendBatch(g_sent++, true);
        }
    } else if (g_doneReported && !g_sdCapture && g_dump < g_produced) {
        sendBatch(g_dump++, false);
        if (g_dump == g_produced) {
            sendDiag(PSOC_EVT_DUMP_DONE,
                     (uint8_t)(g_dump > 255u ? 255u : g_dump));
        }
    }
}

void simPsocWrite(const uint8_t *data, size_t len)
{
    if (!g_enabled || data == nullptr || len < 4u || data[0] != PSOC_FRAME_MARKER) return;
    if (xorBytes(data, 1u, len - 1u) != data[len - 1u]) return;
    const uint8_t cmd = data[1], p1 = data[2];
    const uint8_t p2 = len > 4u ? data[3] : 0u;
    const uint8_t p3 = len > 5u ? data[4] : 0u;

    switch (cmd) {
        case PSOC_CTRL_PONG: break;
        case PSOC_CMD_STATUS: announce(); break;
        case PSOC_CMD_SETN:
            g_n = (uint16_t)p1 | ((uint16_t)p2 << 8u);
            sendDiag(PSOC_EVT_SETN, (uint8_t)(g_n > 255u ? 255u : g_n));
            break;
        case PSOC_CMD_PRESTART:
            g_state = ARMED;
            g_lastSync = digitalRead(SYNC_TO_PSOC_PIN) == HIGH;
            sendDiag(PSOC_EVT_ARMED, (uint8_t)(g_n > 255u ? 255u : g_n));
            break;
        case PSOC_CMD_START_NOW: sendAck(cmd, 1u); startCapture(); break;
        case PSOC_CMD_PGA: g_pga = p1; sendAck(cmd, g_pga); break;
        case PSOC_CMD_PGAOUT: g_pgaout = p1; sendAck(cmd, g_pgaout); break;
        case PSOC_CMD_PGAVDAC: g_pgavdac = p1; sendAck(cmd, g_pgavdac); break;
        case PSOC_CMD_VDAC: sendAck(cmd, p1); break;
        case PSOC_CMD_CAL_PARAM: sendAck(cmd, p2); break;
        case PSOC_CMD_CALIBRATE:
            g_state = CALIBRATING;
            sendDiag(PSOC_EVT_CAL_START, 0u);
            for (uint8_t stage = 0u; stage < 4u; ++stage) {
                sendDiag(PSOC_EVT_CAL_STAGE_BEGIN, stage);
                sendDiag(PSOC_EVT_CAL_STAGE_OK, stage);
            }
            g_state = IDLE;
            sendDiag(PSOC_EVT_CAL_DONE, 1u);
            sendAck(cmd, 1u);
            break;
        case PSOC_CMD_ACCEPT_EXTERNAL_CAL: sendAck(cmd, p1 == PSOC_ACCEPT_EXTERNAL_CAL_MAGIC); break;
        case PSOC_CMD_SAVE_EEPROM: sendAck(cmd, 1u); break;
        case PSOC_CMD_SELECT_STREAM: sendAck(cmd, p1 ? 1u : 0u); break;
        case PSOC_CMD_ADC_CONFIG: sendAck(cmd, p1); sendFs(); break;
        case PSOC_CMD_SET_DECIMATION:
            g_decimation = (p1 >= 1u && p1 <= 100u) ? p1 : 1u;
            g_fs = (uint16_t)(NATIVE_FS / g_decimation);
            if (g_fs == 0u) g_fs = 1u;
            sendAck(cmd, g_decimation); sendFs();
            break;
        case PSOC_CMD_VIEW_CHANNEL: sendAck(cmd, p1); break;
        case PSOC_CMD_FORCE_PI: sendAck(cmd, p1); break;
        case PSOC_CMD_ADC_SNAPSHOT: sendAck(cmd, 1u); break;
        case PSOC_CMD_BLINK_LED: sendAck(cmd, 1u); break;
        case PSOC_CMD_SD_STATUS: sendDiag(PSOC_EVT_SD_STATUS, sdStatus()); sendAck(cmd, sdStatus()); break;
        case PSOC_CMD_SD_TEST: sendAck(cmd, 1u); break;
        case PSOC_CMD_SD_PROBE: sendAck(cmd, 1u); break;
        case PSOC_CMD_SD_CAPTURE:
            g_sdCapture = p1 != 0u;
            sendAck(cmd, g_sdCapture ? 1u : 0u);
            sendDiag(PSOC_EVT_SD_STATUS, sdStatus());
            break;
        case PSOC_CMD_SD_READ_BATCH: {
            const uint16_t batch = (uint16_t)p1 | ((uint16_t)p2 << 8u);
            if (batch < g_produced && batch < RAM_BATCHES) sendBatch(batch, false);
            else sendAck(cmd, 0u);
            break;
        }
        case PSOC_CMD_LIVE_MODE:
            g_liveMode = p1 <= 2u ? p1 : 0u;
            sendAck(cmd, g_liveMode);
            break;
        case PSOC_CMD_LIVE_ACK: {
            const uint32_t batch = unwrap16((uint16_t)p1 | ((uint16_t)p2 << 8u), g_sent);
            if ((p3 & PSOC_LIVE_ACK_ONE) != 0u) {
                if (batch < g_produced && g_produced - batch <= RAM_BATCHES) sendBatch(batch, true);
            } else if ((p3 & PSOC_LIVE_ACK_REWIND) != 0u) {
                if (batch <= g_sent && g_produced - batch <= RAM_BATCHES) g_sent = batch;
            } else if (batch > g_acked) {
                g_acked = batch;
            }
            break;
        }
        case PSOC_CMD_DEBUG:
            g_debug = p1 != 0u;
            if (g_debug) startCapture();
            else { g_state = IDLE; reportDone(); }
            sendDiag(PSOC_EVT_DEBUG_MODE, g_debug ? 1u : 0u);
            break;
        case 0xC9u:
            if (p1 < 128u) { g_fir[p1][0] = p2; g_fir[p1][1] = p3; }
            break;
        case 0xCAu:
            if (p1 < 128u) g_fir[p1][2] = p2;
            break;
        case 0xCBu: {
            uint16_t sum = 0u;
            for (uint16_t i = 0u; i < 128u; ++i)
                sum = (uint16_t)(sum + g_fir[i][0] + g_fir[i][1] + g_fir[i][2]);
            const uint16_t wanted = (uint16_t)p2 | ((uint16_t)p3 << 8u);
            sendAck(cmd, (p1 == 0u || sum == wanted) ? 1u : 0u);
            break;
        }
        default:
            if (cmd >= 0xD0u && cmd <= 0xD7u) sendAck(cmd, 1u);
            else sendAck(cmd, p1);
            break;
    }
}

void simPsocDecorateBatch(PsocBatch &batch)
{
    const uint32_t first = g_wireBatch * SPI_BATCH_SAMPLES;
    for (uint32_t i = 0u; i < SPI_BATCH_SAMPLES; ++i) {
        PsocSample &s = batch.samples[i];
        s.post_analog = (int32_t)((first + i) & 0xFFFFFFu);
        s.raw_input = (int16_t)s.post_digital;
    }
}

void simPsocApplyNoiseArtifact(SampleBytes &sample, uint8_t flags, uint32_t sampleIndex)
{
    int32_t add = 0;
    const uint8_t bits[4] = {SAMPLE_FLAG_I2C, SAMPLE_FLAG_SD_IO,
                             SAMPLE_FLAG_RADIO_TX, SAMPLE_FLAG_AP_BEACON};
    for (uint8_t k = 0u; k < 4u; ++k) {
        if ((flags & bits[k]) == 0u) continue;
        const bool contiguous = g_artifactLast[k] != 0xFFFFFFFFu &&
                                sampleIndex == g_artifactLast[k] + 1u;
        if (!contiguous) g_artifactStart[k] = sampleIndex;
        const uint32_t phase = sampleIndex - g_artifactStart[k];
        g_artifactLast[k] = sampleIndex;
        if (bits[k] == SAMPLE_FLAG_I2C) {
            const uint32_t n = (g_fs + 999u) / 1000u;
            if (phase < n) add += (int32_t)(3000u * (n - phase) / n);
        } else if (bits[k] == SAMPLE_FLAG_SD_IO) {
            const uint32_t n = (g_fs * 5u + 999u) / 1000u;
            const uint32_t ramp = phase < n ? phase + 1u : n;
            add += (int32_t)(8000u * ramp / n);
        } else if (bits[k] == SAMPLE_FLAG_RADIO_TX) {
            if (!contiguous) add += 6000;
        } else if (bits[k] == SAMPLE_FLAG_AP_BEACON) {
            add += 2000;
        }
    }
    if (add != 0) writeSampleSignal(sample, readSample24(sample) + add);
}

void simPsocSetEnabled(bool enabled)
{
    if (g_enabled == enabled) return;
    g_enabled = enabled;
    g_state = IDLE;
    g_debug = false;
    if (enabled) announce();
}

bool simPsocEnabled() { return g_enabled; }

void simPsocPrintState()
{
    Serial.printf("#SIM enabled=%u psoc_state=%u batches=%lu sent=%lu acked=%lu "
                  "N=%u fs=%u live=%u sd_capture=%u sd_free=%llu\n",
                  (unsigned)g_enabled, (unsigned)g_state,
                  (unsigned long)g_produced, (unsigned long)g_sent,
                  (unsigned long)g_acked, (unsigned)g_n, (unsigned)g_fs,
                  (unsigned)g_liveMode, (unsigned)g_sdCapture,
                  (unsigned long long)simSdFreeBytes());
}

#endif
