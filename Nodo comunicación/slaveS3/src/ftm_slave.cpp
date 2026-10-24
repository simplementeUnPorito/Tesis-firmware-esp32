/*
 * ftm_slave.cpp — ver ftm_slave.h.
 */
#include "ftm_slave.h"
#include "debug_log.h"

#ifndef SLAVE_LOG_RAW
#define SLAVE_LOG_RAW(...) Serial.printf(__VA_ARGS__)
#endif

#if defined(ESP32) && defined(CONFIG_ESP_WIFI_FTM_INITIATOR_SUPPORT) && CONFIG_ESP_WIFI_FTM_INITIATOR_SUPPORT
#define FTM_AVAILABLE 1
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_timer.h>
#else
#define FTM_AVAILABLE 0
#endif

namespace ftm {

#if FTM_AVAILABLE

static constexpr int      WINDOW        = 128;   /* tramas validas que se conservan */
static constexpr int      MAX_ENTRIES   = 64;    /* tramas por sesion */
static constexpr uint32_t SESSION_TIMEOUT_MS = 4000u;
static constexpr int64_t  RTT_MAX_PS    = 2000000;  /* 300 m de ida y vuelta */

struct Entry {
    int64_t local_us;   /* esp_timer al procesar la sesion */
    int64_t t2_ps;      /* reloj MAC local en la llegada (para la deriva) */
    int64_t rtt_ps;
    int64_t theta_ps;
    int8_t  rssi;
};

/* Lo que copia el evento WiFi (contexto de la tarea de eventos). */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static wifi_ftm_report_entry_t s_raw[MAX_ENTRIES];
static volatile uint8_t  s_rawCount = 0;
static volatile bool     s_rawReady = false;
static volatile uint8_t  s_rawStatus = 0;
static volatile uint32_t s_rawDistCm = 0;

static Entry    s_win[WINDOW];
static int      s_winHead = 0;
static int      s_winCount = 0;
static bool     s_busy = false;
static uint32_t s_sessionStartMs = 0;
static uint32_t s_lastSessionMs = 0;
static uint32_t s_lastValidMs = 0;
static uint32_t s_periodMs = 1000u;
static uint16_t s_sessions = 0;
static uint16_t s_framesTotal = 0;
static uint16_t s_framesValid = 0;
static uint8_t  s_status = FTM_ST_IDLE;
static uint32_t s_distEstCm = 0;
static uint8_t  s_channel = 0;
static bool     s_newReport = false;
static wifi_ftm_report_entry_t s_last[MAX_ENTRIES];
static uint8_t  s_lastCount = 0;

static void onFtmReport(void *, esp_event_base_t, int32_t, void *data)
{
    const wifi_event_ftm_report_t *ev = (const wifi_event_ftm_report_t *)data;
    if (ev == nullptr) return;
    portENTER_CRITICAL(&s_mux);
    const uint8_t n = (ev->ftm_report_num_entries < MAX_ENTRIES) ? ev->ftm_report_num_entries
                                                                  : (uint8_t)MAX_ENTRIES;
    if (ev->ftm_report_data != nullptr && n > 0) {
        memcpy(s_raw, ev->ftm_report_data, (size_t)n * sizeof(wifi_ftm_report_entry_t));
    }
    s_rawCount = (ev->ftm_report_data != nullptr) ? n : 0;
    s_rawStatus = (uint8_t)ev->status;
    s_rawDistCm = ev->dist_est;
    s_rawReady = true;
    portEXIT_CRITICAL(&s_mux);
    /* Contrato de IDF 4.4: el reporte lo libera quien lo recibe. */
    if (ev->ftm_report_data != nullptr) free(ev->ftm_report_data);
}

void begin()
{
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_FTM_REPORT, &onFtmReport, nullptr);
}

void setPeriodMs(uint32_t ms) { s_periodMs = ms; }

static void absorb()
{
    wifi_ftm_report_entry_t raw[MAX_ENTRIES];
    uint8_t n, st;
    uint32_t dist;
    portENTER_CRITICAL(&s_mux);
    n = s_rawCount;
    st = s_rawStatus;
    dist = s_rawDistCm;
    memcpy(raw, s_raw, (size_t)n * sizeof(wifi_ftm_report_entry_t));
    s_rawReady = false;
    portEXIT_CRITICAL(&s_mux);

    s_busy = false;
    memcpy(s_last, raw, (size_t)n * sizeof(wifi_ftm_report_entry_t));
    s_lastCount = n;
    if (st != FTM_STATUS_SUCCESS) {
        s_status = (st == FTM_STATUS_UNSUPPORTED) ? FTM_ST_UNSUPPORTED_PEER
                 : (st == FTM_STATUS_NO_RESPONSE) ? FTM_ST_NO_RESPONSE : FTM_ST_FAIL;
        s_newReport = true;
        return;
    }
    s_status = FTM_ST_OK;
    s_sessions++;
    s_distEstCm = dist;
    const int64_t now = esp_timer_get_time();
    for (uint8_t i = 0; i < n; i++) {
        const wifi_ftm_report_entry_t &e = raw[i];
        s_framesTotal++;
        const int64_t t1 = (int64_t)e.t1, t2 = (int64_t)e.t2;
        const int64_t t3 = (int64_t)e.t3, t4 = (int64_t)e.t4;
        const int64_t rtt = (t4 - t1) - (t3 - t2);
        if (t1 == 0 || t2 == 0 || rtt <= 0 || rtt > RTT_MAX_PS) continue;
        s_framesValid++;
        Entry &w = s_win[s_winHead];
        w.local_us = now;
        w.t2_ps = t2;
        w.rtt_ps = rtt;
        w.theta_ps = ((t2 - t1) + (t3 - t4)) / 2;
        w.rssi = e.rssi;
        s_winHead = (s_winHead + 1) % WINDOW;
        if (s_winCount < WINDOW) s_winCount++;
        s_lastValidMs = millis();
    }
    s_newReport = true;
}

void service(bool allowed, const uint8_t responder[6], uint8_t channel)
{
    if (s_rawReady) absorb();
    const uint32_t now = millis();
    if (s_busy) {
        if (now - s_sessionStartMs > SESSION_TIMEOUT_MS) {
            esp_wifi_ftm_end_session();
            s_busy = false;
            s_status = FTM_ST_NO_RESPONSE;
            s_newReport = true;
        }
        return;
    }
    if (!allowed || s_periodMs == 0u || channel == 0u) return;
    if (s_lastSessionMs != 0u && now - s_lastSessionMs < s_periodMs) return;
    wifi_ftm_initiator_cfg_t cfg = {};
    memcpy(cfg.resp_mac, responder, 6);
    cfg.channel = channel;
    cfg.frm_count = 16;
    cfg.burst_period = 2;   /* 200 ms */
    s_lastSessionMs = now;
    s_channel = channel;
    if (esp_wifi_ftm_initiate_session(&cfg) == ESP_OK) {
        s_busy = true;
        s_sessionStartMs = now;
    } else {
        s_status = FTM_ST_FAIL;
    }
}

static int cmpI64(const void *a, const void *b)
{
    const int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

struct Stats {
    int     n;
    int64_t rtt_med, rtt_mad, rtt_p95;
    int8_t  rssi_med;
    double  skew_ppb;
    double  resid_ns;
};

static Stats compute()
{
    Stats s = {};
    s.n = s_winCount;
    if (s.n == 0) return s;
    static int64_t v[WINDOW];
    for (int i = 0; i < s.n; i++) v[i] = s_win[i].rtt_ps;
    qsort(v, s.n, sizeof(int64_t), cmpI64);
    s.rtt_med = v[s.n / 2];
    s.rtt_p95 = v[(s.n * 95) / 100 < s.n ? (s.n * 95) / 100 : s.n - 1];
    for (int i = 0; i < s.n; i++) {
        const int64_t d = s_win[i].rtt_ps - s.rtt_med;
        v[i] = d < 0 ? -d : d;
    }
    qsort(v, s.n, sizeof(int64_t), cmpI64);
    s.rtt_mad = v[s.n / 2];
    for (int i = 0; i < s.n; i++) v[i] = s_win[i].rssi;
    qsort(v, s.n, sizeof(int64_t), cmpI64);
    s.rssi_med = (int8_t)v[s.n / 2];

    /* Recta theta(t2) por minimos cuadrados, centrada para no perder bits. */
    if (s.n >= 8) {
        const int64_t x0 = s_win[0].t2_ps, y0 = s_win[0].theta_ps;
        double mx = 0, my = 0;
        for (int i = 0; i < s.n; i++) {
            mx += (double)(s_win[i].t2_ps - x0);
            my += (double)(s_win[i].theta_ps - y0);
        }
        mx /= s.n; my /= s.n;
        double sxx = 0, sxy = 0;
        for (int i = 0; i < s.n; i++) {
            const double dx = (double)(s_win[i].t2_ps - x0) - mx;
            const double dy = (double)(s_win[i].theta_ps - y0) - my;
            sxx += dx * dx; sxy += dx * dy;
        }
        if (sxx > 0) {
            const double b = sxy / sxx;
            double ss = 0;
            for (int i = 0; i < s.n; i++) {
                const double dx = (double)(s_win[i].t2_ps - x0) - mx;
                const double dy = (double)(s_win[i].theta_ps - y0) - my;
                const double r = dy - b * dx;
                ss += r * r;
            }
            s.skew_ppb = b * 1e9;
            s.resid_ns = sqrt(ss / s.n) / 1000.0;
        }
    }
    return s;
}

bool takeReport(MsgFtmReport &out, uint8_t nodeId, uint8_t syncMode)
{
    if (!s_newReport) return false;
    s_newReport = false;
    const Stats s = compute();
    out = {};
    out.cmd = CMD_FTM_REPORT;
    out.node_id = nodeId;
    out.status = s_status;
    out.sync_mode = syncMode;
    out.sessions = s_sessions;
    out.frames_total = s_framesTotal;
    out.frames_valid = s_framesValid;
    out.window = (uint8_t)s.n;
    out.rtt_med_ps = (uint32_t)s.rtt_med;
    out.rtt_mad_ps = (uint32_t)s.rtt_mad;
    out.rtt_p95_ps = (uint32_t)s.rtt_p95;
    out.rssi_med = s.rssi_med;
    out.dist_est_cm = s_distEstCm;
    out.skew_ppb = (int32_t)s.skew_ppb;
    out.resid_ns = (uint32_t)s.resid_ns;
    out.age_ms = s_lastValidMs ? millis() - s_lastValidMs : 0xFFFFFFFFu;
    out.channel = s_channel;
    return true;
}

void printStatus(bool raw)
{
    const Stats s = compute();
    SLAVE_LOG_RAW("#FTM st=%u ses=%u tramas=%u/%u ventana=%d rtt_med=%lld ps mad=%lld p95=%lld "
                  "rssi=%d dist_sdk=%lu cm skew=%.0f ppb resid=%.1f ns periodo=%lu ms\n",
                  (unsigned)s_status, (unsigned)s_sessions, (unsigned)s_framesValid,
                  (unsigned)s_framesTotal, s.n, (long long)s.rtt_med, (long long)s.rtt_mad,
                  (long long)s.rtt_p95, (int)s.rssi_med, (unsigned long)s_distEstCm,
                  s.skew_ppb, s.resid_ns, (unsigned long)s_periodMs);
    if (!raw) return;
    /* Para medir el origen de T1/T4: se imprime junto al esp_timer y al TSF. */
    SLAVE_LOG_RAW("#FTMRAW esp_timer=%lld tsf_sta=%lld n=%u\n", (long long)esp_timer_get_time(),
                  (long long)esp_wifi_get_tsf_time(WIFI_IF_STA), (unsigned)s_lastCount);
    for (uint8_t i = 0; i < s_lastCount; i++) {
        const wifi_ftm_report_entry_t &e = s_last[i];
        SLAVE_LOG_RAW("#FTMRAW %u rssi=%d rtt=%lu t1=%llu t2=%llu t3=%llu t4=%llu\n",
                      (unsigned)e.dlog_token, (int)e.rssi, (unsigned long)e.rtt,
                      (unsigned long long)e.t1, (unsigned long long)e.t2,
                      (unsigned long long)e.t3, (unsigned long long)e.t4);
    }
}

#else   /* sin FTM (ESP32 clasico, ESP8266) */

void begin() {}
void service(bool, const uint8_t *, uint8_t) {}
void setPeriodMs(uint32_t) {}
static bool s_reported = false;
bool takeReport(MsgFtmReport &out, uint8_t nodeId, uint8_t syncMode)
{
    if (s_reported) return false;
    s_reported = true;
    out = {};
    out.cmd = CMD_FTM_REPORT;
    out.node_id = nodeId;
    out.status = FTM_ST_UNSUPPORTED;
    out.sync_mode = syncMode;
    out.age_ms = 0xFFFFFFFFu;
    return true;
}
void printStatus(bool) { SLAVE_LOG_RAW("#FTM sin soporte en esta placa\n"); }

#endif

}  // namespace ftm
