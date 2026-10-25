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
#include <esp_arduino_version.h>
#if ESP_ARDUINO_VERSION_MAJOR >= 3
#include <esp_private/wifi.h>
#define FTM_CLOCK_MODEL 1
#endif
#else
#define FTM_AVAILABLE 0
#endif

namespace ftm {

#if FTM_AVAILABLE

static constexpr int      WINDOW        = 128;   /* tramas validas que se conservan */
static constexpr int      MAX_ENTRIES   = 64;    /* tramas por sesion */
static constexpr uint32_t SESSION_TIMEOUT_MS = 4000u;
static constexpr int64_t  RTT_MAX_PS    = 2000000;  /* 300 m de ida y vuelta */

#ifndef FTM_CLOCK_MODEL
#define FTM_CLOCK_MODEL 0
#endif

/* ── Modelo de reloj ──────────────────────────────────────────────────────
 * T2/T3 = reloj MAC local en ps (modulo 2^32 us); T1/T4 = reloj MAC del
 * maestro en ps recortado a 48 bits. Con la estimacion gruesa de la baliza se
 * reconstruye T1/T4 completo y sale theta exacto por trama. */
static constexpr int64_t W32_PS = 4294967296LL * 1000000LL;   /* 2^32 us en ps */
static constexpr int64_t W48_PS = 281474976710656LL;          /* 2^48 ps */
static constexpr int     MODEL_N = 16;                        /* sesiones en la recta */
static bool     s_coarseOk = false;
static uint32_t s_coarseThetaUs = 0;   /* local_mac - master_mac de la baliza */
struct SessionTheta {
    uint32_t localMacUs;   /* reloj MAC local de la sesion */
    int64_t  thetaPs;      /* modulo W32_PS */
    uint32_t ms;           /* millis() al procesarla */
};
static SessionTheta s_ses[MODEL_N];
static int      s_sesHead = 0, s_sesCount = 0;
static double   s_mSlope = 0;      /* ps de theta por us local (= deriva) */
static int64_t  s_mThetaRef = 0;   /* theta en el instante de referencia (ps) */
static uint32_t s_mRefLocal = 0;   /* reloj MAC local de referencia (us) */
static double   s_mResidNs = 0;
static bool     s_mOk = false;
static uint32_t s_mLastMs = 0;

static int64_t wrapW32(int64_t v)
{
    v %= W32_PS;
    if (v < 0) v += W32_PS;
    if (v >= W32_PS / 2) v -= W32_PS;
    return v;
}

static int64_t modW32(int64_t v)
{
    v %= W32_PS;
    return v < 0 ? v + W32_PS : v;
}

/* Valor completo (modulo W32) de un tiempo del maestro recortado a 48 bits,
 * el mas cercano a la estimacion. */
static int64_t unwrapMaster(int64_t raw48, int64_t estPs)
{
    int64_t k = (estPs - raw48) / W48_PS;
    int64_t best = raw48 + k * W48_PS;
    for (int64_t c = best - W48_PS; c <= best + W48_PS; c += W48_PS) {
        if (llabs(wrapW32(c - estPs)) < llabs(wrapW32(best - estPs))) best = c;
    }
    return modW32(best);
}

static int cmpI64m(const void *a, const void *b)
{
    const int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

static void refitModel()
{
    s_mOk = false;
    if (s_sesCount < 4) return;
    const int last = (s_sesHead + MODEL_N - 1) % MODEL_N;
    const SessionTheta &r = s_ses[last];
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int n = 0;
    for (int i = 0; i < s_sesCount; i++) {
        const SessionTheta &e = s_ses[i];
        if ((uint32_t)(r.ms - e.ms) > 30000u) continue;   /* solo sesiones recientes */
        const double x = (double)(int32_t)(e.localMacUs - r.localMacUs);
        const double y = (double)wrapW32(e.thetaPs - r.thetaPs);
        sx += x; sy += y; sxx += x * x; sxy += x * y; n++;
    }
    if (n < 4) return;
    const double den = sxx - sx * sx / n;
    const double slope = den > 0 ? (sxy - sx * sy / n) / den : 0.0;
    const double icpt = (sy - slope * sx) / n;
    double ss = 0;
    for (int i = 0; i < s_sesCount; i++) {
        const SessionTheta &e = s_ses[i];
        if ((uint32_t)(r.ms - e.ms) > 30000u) continue;
        const double x = (double)(int32_t)(e.localMacUs - r.localMacUs);
        const double y = (double)wrapW32(e.thetaPs - r.thetaPs);
        const double d = y - (icpt + slope * x);
        ss += d * d;
    }
    s_mSlope = slope;
    s_mRefLocal = r.localMacUs;
    s_mThetaRef = modW32(r.thetaPs + (int64_t)llround(icpt));
    s_mResidNs = sqrt(ss / n) / 1000.0;
    s_mLastMs = r.ms;
    /* Deriva fisica < 100 ppm (= 1e6 ps/s = 1 ps/us * 100) y dispersion chica. */
    s_mOk = fabs(slope) < 100.0 && s_mResidNs < 500.0;
}

/* theta de una sesion: mediana por trama, con T1/T4 desenrollados. */
static void addSessionTheta(const wifi_ftm_report_entry_t *e, uint8_t n)
{
    if (!s_coarseOk || n == 0) return;
    static int64_t th[64];
    int k = 0;
    uint32_t midLocal = 0;
    for (uint8_t i = 0; i < n && k < 64; i++) {
        const int64_t t1r = (int64_t)(e[i].t1 % (uint64_t)W48_PS);
        const int64_t t4r = (int64_t)(e[i].t4 % (uint64_t)W48_PS);
        const int64_t t2 = modW32((int64_t)(e[i].t2 % (uint64_t)W32_PS));
        const int64_t t3 = modW32((int64_t)(e[i].t3 % (uint64_t)W32_PS));
        if (e[i].t1 == 0 || e[i].t2 == 0) continue;
        const int64_t estT1 = modW32(t2 - (int64_t)s_coarseThetaUs * 1000000LL);
        const int64_t t1 = unwrapMaster(t1r, estT1);
        const int64_t t4 = unwrapMaster(t4r, modW32(estT1 + (t3 - t2)));
        const int64_t rtt = wrapW32(t4 - t1) - wrapW32(t3 - t2);
        if (rtt <= 0 || rtt > RTT_MAX_PS) continue;
        /* theta = local - maestro = ((t2 - t1) + (t3 - t4)) / 2 */
        th[k++] = wrapW32((wrapW32(t2 - t1) + wrapW32(t3 - t4)) / 2);
        midLocal = (uint32_t)(t2 / 1000000LL);
    }
    if (k < 4) return;
    /* La mediana se toma alrededor de la estimacion gruesa (sin saltos de modulo). */
    const int64_t c = (int64_t)s_coarseThetaUs * 1000000LL;
    for (int i = 0; i < k; i++) th[i] = wrapW32(th[i] - c);
    qsort(th, k, sizeof(int64_t), cmpI64m);
    SessionTheta &s = s_ses[s_sesHead];
    s.localMacUs = midLocal;
    s.thetaPs = modW32(c + th[k / 2]);
    s.ms = millis();
    s_sesHead = (s_sesHead + 1) % MODEL_N;
    if (s_sesCount < MODEL_N) s_sesCount++;
    refitModel();
}

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
/* Turnos: el responder del maestro tiene 3 plazas y una sesion cuya limpieza
 * se pierde bajo carga deja la plaza ocupada para siempre (NO_RESPONSE
 * permanente, 2026-10-25; ver lab/s3/INFORME_BANCO_S3.md). Con la hora del
 * maestro que da la baliza, cada esclavo mide solo en su ventana: nunca hay
 * dos sesiones a la vez. */
static constexpr uint32_t TURN_MS = 1000u;
static constexpr uint32_t TURN_SLOTS = 8u;
static uint8_t  s_turnSlot = 0xFF;      /* 0xFF = sin turnos (periodo libre) */
static uint32_t s_lastTurn = 0xFFFFFFFFu;
static uint16_t s_sessions = 0;
static uint16_t s_framesTotal = 0;
static uint16_t s_framesValid = 0;
static uint8_t  s_status = FTM_ST_IDLE;
static uint32_t s_distEstCm = 0;
static uint8_t  s_channel = 0;
static int16_t  s_initErr = 0;
/* Configuraciones a probar si el responder rechaza (FTM_STATUS_CONF_REJECTED):
 * {tramas, periodo de rafaga en 100 ms}. Se queda con la primera aceptada. */
static const uint8_t  CFG_FRAMES[] = {16, 32, 0, 64, 24};
static const uint16_t CFG_PERIOD[] = { 2,  2, 0,  0,  0};
static uint8_t  s_cfgIdx = 0;
/* El modo promiscuo (marcas de baliza) se apaga mientras dura la sesion. */
static bool     s_promiscWas = false;
static bool     s_cfgLocked = false;
static uint8_t  s_evStatus = 0xFF;
static bool     s_newReport = false;
static wifi_ftm_report_entry_t s_last[MAX_ENTRIES];
static uint8_t  s_lastCount = 0;

static void onFtmReport(void *, esp_event_base_t, int32_t, void *data)
{
    const wifi_event_ftm_report_t *ev = (const wifi_event_ftm_report_t *)data;
    if (ev == nullptr) return;
    const uint8_t n = (ev->ftm_report_num_entries < MAX_ENTRIES) ? ev->ftm_report_num_entries
                                                                  : (uint8_t)MAX_ENTRIES;
    /* Las tramas se copian FUERA de la seccion critica: esp_wifi_ftm_get_report
     * bloquea y adentro de portENTER_CRITICAL disparaba el watchdog de
     * interrupciones (bucle de reinicio con IDF 5.5, 2026-10-24). */
    static wifi_ftm_report_entry_t tmp[MAX_ENTRIES];
    uint8_t got = 0;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    /* IDF 5: las tramas se piden con esp_wifi_ftm_get_report (use_get_report_api). */
    if (ev->status == FTM_STATUS_SUCCESS && n > 0 && esp_wifi_ftm_get_report(tmp, n) == ESP_OK) got = n;
#else
    if (ev->ftm_report_data != nullptr && n > 0) {
        memcpy(tmp, ev->ftm_report_data, (size_t)n * sizeof(wifi_ftm_report_entry_t));
        got = n;
    }
#endif
    portENTER_CRITICAL(&s_mux);
    memcpy(s_raw, tmp, (size_t)got * sizeof(wifi_ftm_report_entry_t));
    s_rawCount = got;
    s_rawStatus = (uint8_t)ev->status;
    s_rawDistCm = ev->dist_est;
    s_rawReady = true;
    portEXIT_CRITICAL(&s_mux);
#if ESP_ARDUINO_VERSION_MAJOR < 3
    /* Contrato de IDF 4.4: el reporte lo libera quien lo recibe. */
    if (ev->ftm_report_data != nullptr) free(ev->ftm_report_data);
#endif
}

void begin()
{
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_FTM_REPORT, &onFtmReport, nullptr);
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);   /* mismo ancho que el AP */
}

void setPeriodMs(uint32_t ms) { s_periodMs = ms; }
void setTurn(uint8_t nodeId) { s_turnSlot = (uint8_t)(nodeId % TURN_SLOTS); }

#if FTM_CLOCK_MODEL
void noteMasterBeacon(uint32_t masterMacUs, uint32_t localRxMacUs)
{
    /* La latencia de ESP-NOW solo suma; alcanza con ms de exactitud. */
    s_coarseThetaUs = localRxMacUs - masterMacUs;
    s_coarseOk = true;
}

/* Con turnos cada nodo mide cada TURN_SLOTS*TURN_MS = 8 s: la recta se
 * extrapola hasta 20 s (deriva de ppm, error de us). */
bool modelOk() { return s_mOk && (uint32_t)(millis() - s_mLastMs) < 20000u; }
uint32_t modelAgeMs() { return s_mLastMs ? (uint32_t)(millis() - s_mLastMs) : 0xFFFFFFFFu; }

/* theta (ps, modulo W32) en un instante del reloj MAC local. */
static int64_t thetaAtLocal(uint32_t localUs)
{
    const double dx = (double)(int32_t)(localUs - s_mRefLocal);
    return modW32(s_mThetaRef + (int64_t)llround(s_mSlope * dx));
}

bool masterMacToLocalEsp(uint32_t masterMacUs, int64_t &deadlineEsp, uint32_t &uncertNs)
{
    if (!modelOk()) return false;
    /* local = maestro + theta(local): dos pasadas alcanzan (theta varia ppm). */
    int64_t localPs = modW32((int64_t)masterMacUs * 1000000LL + thetaAtLocal(masterMacUs + (uint32_t)(s_mThetaRef / 1000000LL)));
    localPs = modW32((int64_t)masterMacUs * 1000000LL + thetaAtLocal((uint32_t)(localPs / 1000000LL)));
    /* Reloj MAC y esp_timer salen del mismo cristal: delta directo. */
    const uint32_t macNow = esp_wifi_internal_get_mac_clock_time();
    const int64_t espNow = esp_timer_get_time();
    const int64_t nowPs = (int64_t)macNow * 1000000LL;
    const int64_t deltaPs = wrapW32(localPs - nowPs);
    if (deltaPs < 50000000000LL || deltaPs > 5000000000000LL) return false;   /* 50 ms .. 5 s */
    deadlineEsp = espNow + (deltaPs + 500000) / 1000000LL;
    uncertNs = (uint32_t)(s_mResidNs + 1000.0);   /* + 1 us por la cuantizacion del esp_timer */
    return true;
}

bool localEspToMasterMac(int64_t esp, uint32_t &masterMacUs)
{
    if (s_sesCount == 0) return false;
    const uint32_t macNow = esp_wifi_internal_get_mac_clock_time();
    const int64_t espNow = esp_timer_get_time();
    const uint32_t localMac = macNow + (uint32_t)(esp - espNow);
    const int64_t masterPs = modW32((int64_t)localMac * 1000000LL - thetaAtLocal(localMac));
    masterMacUs = (uint32_t)(masterPs / 1000000LL);
    return true;
}
#else
void noteMasterBeacon(uint32_t, uint32_t) {}
bool modelOk() { return false; }
uint32_t modelAgeMs() { return 0xFFFFFFFFu; }
bool masterMacToLocalEsp(uint32_t, int64_t &, uint32_t &) { return false; }
bool localEspToMasterMac(int64_t, uint32_t &) { return false; }
#endif

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
    if (s_promiscWas) esp_wifi_set_promiscuous(true);
    memcpy(s_last, raw, (size_t)n * sizeof(wifi_ftm_report_entry_t));
    s_lastCount = n;
    s_evStatus = st;
    if (st != FTM_STATUS_SUCCESS) {
        s_status = (st == FTM_STATUS_UNSUPPORTED) ? FTM_ST_UNSUPPORTED_PEER
                 : (st == FTM_STATUS_NO_RESPONSE) ? FTM_ST_NO_RESPONSE : FTM_ST_FAIL;
        if (st == FTM_STATUS_CONF_REJECTED && !s_cfgLocked) {
            s_cfgIdx = (uint8_t)((s_cfgIdx + 1u) % sizeof(CFG_FRAMES));
        }
        s_newReport = true;
        return;
    }
    s_status = FTM_ST_OK;
    s_cfgLocked = true;
    s_sessions++;
#if FTM_CLOCK_MODEL
    addSessionTheta(raw, n);
#endif
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
            if (s_promiscWas) esp_wifi_set_promiscuous(true);
            s_status = FTM_ST_NO_RESPONSE;
            s_newReport = true;
        }
        return;
    }
    if (!allowed || s_periodMs == 0u || channel == 0u) return;
#if FTM_CLOCK_MODEL
    if (s_turnSlot != 0xFFu && s_coarseOk) {
        /* Hora del maestro (ms de exactitud, de la baliza) -> ventana. */
        const uint32_t masterMs = (esp_wifi_internal_get_mac_clock_time() - s_coarseThetaUs) / 1000u;
        const uint32_t turn = masterMs / TURN_MS;
        if (turn % TURN_SLOTS != s_turnSlot || turn == s_lastTurn) return;
        /* Solo en los primeros 300 ms de la ventana: la sesion termina adentro. */
        if (masterMs % TURN_MS > 300u) return;
        s_lastTurn = turn;
    } else
#endif
    if (s_lastSessionMs != 0u && now - s_lastSessionMs < s_periodMs) return;
    wifi_ftm_initiator_cfg_t cfg = {};
    memcpy(cfg.resp_mac, responder, 6);
    cfg.channel = channel;
    cfg.frm_count = CFG_FRAMES[s_cfgIdx];
    cfg.burst_period = CFG_PERIOD[s_cfgIdx];
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    cfg.use_get_report_api = true;
#endif
    s_lastSessionMs = now;
    s_channel = channel;
    bool prom = false;
    esp_wifi_get_promiscuous(&prom);
    s_promiscWas = prom;
    if (prom) esp_wifi_set_promiscuous(false);
    const esp_err_t err = esp_wifi_ftm_initiate_session(&cfg);
    s_initErr = (int16_t)err;
    if (err != ESP_OK && prom) esp_wifi_set_promiscuous(true);
    if (err == ESP_OK) {
        s_busy = true;
        s_sessionStartMs = now;
    } else {
        s_status = FTM_ST_FAIL;
        s_newReport = true;
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
    out.init_err = s_initErr;
    out.ev_status = s_evStatus;
    out.cfg_idx = s_cfgIdx;
    out.model_ok = modelOk() ? 1u : 0u;
    out.model_age_ms = (int32_t)modelAgeMs();
    return true;
}

void printStatus(bool raw)
{
    const Stats s = compute();
    SLAVE_LOG_RAW("#FTM st=%u ses=%u tramas=%u/%u ventana=%d rtt_med=%lld ps mad=%lld p95=%lld "
                  "rssi=%d dist_sdk=%lu cm skew=%.0f ppb resid=%.1f ns periodo=%lu ms err=%d ev=%u\n",
                  (unsigned)s_status, (unsigned)s_sessions, (unsigned)s_framesValid,
                  (unsigned)s_framesTotal, s.n, (long long)s.rtt_med, (long long)s.rtt_mad,
                  (long long)s.rtt_p95, (int)s.rssi_med, (unsigned long)s_distEstCm,
                  s.skew_ppb, s.resid_ns, (unsigned long)s_periodMs, (int)s_initErr,
                  (unsigned)s_evStatus);
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
void setTurn(uint8_t) {}
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
void noteMasterBeacon(uint32_t, uint32_t) {}
bool modelOk() { return false; }
uint32_t modelAgeMs() { return 0xFFFFFFFFu; }
bool masterMacToLocalEsp(uint32_t, int64_t &, uint32_t &) { return false; }
bool localEspToMasterMac(int64_t, uint32_t &) { return false; }

#endif

}  // namespace ftm
