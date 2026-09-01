/*
 * main_selftest.cpp — Firmware de AUTOTEST del nodo esclavo.
 *
 * Reemplaza a main.cpp solo en el env `slaveTest` (ver build_src_filter en
 * platformio.ini). Comparte todos los drivers con el firmware de campo:
 * psoc_uart, espnow_transport, local_ui. Si se arregla un driver, el autotest
 * hereda el arreglo.
 *
 * QUE PRUEBA
 * ----------
 * Que ESTA placa este bien armada. No prueba el protocolo ni el sistema (eso
 * ya lo cubren los runners E1..E19 desde la PC): prueba soldaduras, ruteo,
 * componentes y cables, sin tocar el circuito y sin instrumental.
 *
 * REPARTO CON EL PSoC
 * -------------------
 * El PSoC expone primitivas atomicas (poner un IDAC, medir un tap del AMux,
 * contar flancos de SYNC) y contesta una trama 0xC5 por medicion. Este
 * archivo secuencia y decide. La aritmetica vive aca porque aca hay punto
 * flotante comodo y no hay presion de SRAM.
 *
 * COMO SE LEE EL RESULTADO
 * ------------------------
 * Checklist por USB a 115200, mas una linea #JSON para automatizar, mas un
 * resumen en el OLED para usarlo en campo sin PC.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_system.h>

#include "psoc_uart.h"
#include "espnow_compat.h"
#include "espnow_transport.h"
#include "sync_protocol.h"
#include "local_ui.h"
#include "selftest_report.h"

#ifndef NODE_ID
#define NODE_ID 2
#endif
#ifndef SYNC_TO_PSOC_PIN
#define SYNC_TO_PSOC_PIN 27
#endif
#ifndef PSOC_UART_RX
#define PSOC_UART_RX 25
#endif
#ifndef LOCAL_BTN_UP_PIN
#define LOCAL_BTN_UP_PIN 34
#endif
#ifndef LOCAL_BTN_DOWN_PIN
#define LOCAL_BTN_DOWN_PIN 35
#endif
#ifndef LOCAL_BTN_OK_PIN
#define LOCAL_BTN_OK_PIN 36
#endif
#ifndef LOCAL_BTN_BACK_PIN
#define LOCAL_BTN_BACK_PIN 39
#endif
#ifndef MASTER_AP_SSID
#define MASTER_AP_SSID "GeoNetwork"
#endif

/* ── Umbrales ──────────────────────────────────────────────────────────────
 * TODOS los criterios de veredicto viven en este bloque. La regla de diseño
 * es fallar solo ante fallas GRUESAS: la cadena tiene un 680 uF y el
 * compensador esta documentado ~10,7 dB fuera de nominal aun despues de
 * calibrar, asi que un umbral apretado reprobaria placas sanas. Todo lo fino
 * se informa como valor medido con ventana WARN.
 * ------------------------------------------------------------------------- */

/* Riel del ADC. El criterio de saturacion del banco es |V| >= 2,3 V como
 * margen frente a los rieles de +-2,5 V (ver AnalisisCircuito/README.md). */
static const int32_t TH_RAIL_UV = 2300000;

/* Escalon del barrido de IDAC para la matriz D2, en codigos. 20 LSB a
 * 3,75 mV/LSB son 75 mV en la referencia. Chico a proposito: el sumador
 * tiene -27k/6,8k ~ -4 y despues viene PGAout, asi que un escalon grande
 * satura los taps de aguas abajo y comprime justo las pendientes que
 * interesan. Si una medicion satura igual, D2 lo reintenta con la mitad. */
static const int  TH_D2_STEP_CODES   = 20;
static const int  TH_D2_MIN_STEP     = 4;

/* Aislamiento hacia atras: una etapa aguas abajo no deberia mover un tap
 * aguas arriba. Se compara contra la pendiente directa de esa misma etapa. */
static const float TH_D2_ISOLATION_RATIO = 0.10f;   /* 10 % */

/* Propagacion hacia adelante: pendiente minima para considerar que la etapa
 * responde. Por debajo de esto la etapa esta muerta (resistor abierto,
 * soldadura fria, opamp quemado). */
static const int32_t TH_D2_MIN_SLOPE_UV_PER_CODE = 200;   /* 0,2 mV por codigo */

/* Cociente de ganancia de PGAout entre codigo 0 (1x) y codigo 2 (4x).
 * Al ser un COCIENTE cancela toda la incertidumbre de los resistores, asi
 * que aca si se puede apretar. */
static const float TH_GAIN_RATIO_NOM = 4.0f;
static const float TH_GAIN_RATIO_TOL = 0.25f;   /* +-25 % */

/* Coherencia entre las 4 configs del ADC: miden la misma tension, tienen que
 * coincidir. Es una autoconsistencia, tampoco depende de tolerancias. */
static const float TH_ADCCFG_SPREAD_MAX = 0.08f;   /* 8 % */

/* Piso de ruido: solo se reprueba por absurdo. */
static const int32_t TH_NOISE_RMS_DEAD_UV = 2;         /* ADC congelado */
static const int32_t TH_NOISE_RMS_MAX_UV  = 200000;    /* etapa oscilando */
static const int32_t TH_NOISE_MAINS_WARN_UV = 100000;  /* 100 mV de 50 Hz */

/* Flancos que manda el ESP para probar la linea de SYNC. */
static const int TH_SYNC_EDGES = 20;

/* Selectores de asentamiento y de largo de serie (indices de las tablas de
 * psoc_selftest.h del PSoC). */
static const uint8_t SEL_SETTLE_FAST = 0;   /* 5 ms   */
static const uint8_t SEL_SETTLE_DC   = 3;   /* 500 ms */
static const uint8_t SEL_N_NOISE     = 3;   /* 2048 muestras ~ 0,79 s a 2604 Hz */

/* ── Estado ──────────────────────────────────────────────────────────────── */
PsocUART psoc;
EspNowTransport transport;

static uint8_t g_masterMac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* Buzon de tramas 0xC5. Un comando puede contestar mas de una (identity
 * manda 2, la calibracion una por etapa, la medicion de ruido 2). */
#define ST_RX_MAX 12
static PsocSelfTestResult g_stRx[ST_RX_MAX];
static volatile uint8_t   g_stRxN = 0;

/* Ultimos eventos DIAG que le importan al secuenciador. */
static volatile bool     g_evArmed = false;
static volatile bool     g_evCalDone = false;
static volatile uint8_t  g_evCalOk = 0;
static volatile uint8_t  g_sdStatus = 0;
static volatile bool     g_sdStatusSeen = false;
static volatile uint32_t g_diagCount = 0;
static volatile uint16_t g_irqTraps = 0;

/* Captura minima para el test del camino digital: solo cuenta lotes y guarda
 * el primero para revisar la rampa. No hace falta el store paginado del
 * firmware de campo. */
static volatile uint16_t g_batchCount = 0;
static PsocBatch         g_firstBatch;
static volatile bool     g_firstBatchSeen = false;

/* Analisis en streaming de la captura. Los lotes no se guardan (no hay lugar
 * y no hace falta): se acumula lo justo para C5 y D7. */
static volatile uint32_t g_anSamples = 0;
static int64_t           g_anSum = 0;
static int32_t           g_anMin = 0, g_anMax = 0;
static uint32_t          g_anMono = 0;       /* muestras crecientes seguidas */
static int32_t           g_anPrev = 0;
static int32_t           g_anBaseline = 0;   /* media de las primeras muestras */
static bool              g_anBaselineSet = false;
static int32_t           g_anPeakDev = 0;    /* mayor |x - baseline| */
static int32_t           g_anFirstSign = 0;  /* signo de la 1a excursion grande */
static int32_t           g_anTrig = 0;       /* umbral de excursion, en counts */

static void anReset(int32_t trigger)
{
    g_anSamples = 0; g_anSum = 0; g_anMin = 0; g_anMax = 0; g_anMono = 0;
    g_anPrev = 0; g_anBaseline = 0; g_anBaselineSet = false;
    g_anPeakDev = 0; g_anFirstSign = 0; g_anTrig = trigger;
}

/* Datos del PSoC descubiertos en C1, que despues usan los tests analogicos. */
static uint8_t g_psocStages = 0;
static uint8_t g_amuxChannels = 0;
static uint8_t g_hwClass = 0xFF;

/* ── Callbacks ───────────────────────────────────────────────────────────── */
static void onSelfTest(const PsocSelfTestResult &r)
{
    if (g_stRxN < ST_RX_MAX) { g_stRx[g_stRxN++] = r; }
}

static void onDiag(const PsocDiagEvent &e)
{
    g_diagCount++;
    switch (e.event) {
        case 0x32: g_evArmed = true; break;                 /* PSOC_EVT_ARMED */
        case 0x11: g_evCalDone = true; g_evCalOk = e.value; break;  /* CAL_DONE */
        case 0x48: g_sdStatus = e.value; g_sdStatusSeen = true; break; /* SD_STATUS */
        case 0x7E: case 0x7F: g_irqTraps++; break;          /* trampas de IRQ */
        default: break;
    }
}

static void onBatch(const PsocBatch &b)
{
    if (!g_firstBatchSeen) { g_firstBatch = b; g_firstBatchSeen = true; }
    g_batchCount++;

    for (uint8_t i = 0; i < b.n_samples; i++) {
        int32_t x = b.samples[i].post_digital;
        if (g_anSamples == 0) { g_anMin = x; g_anMax = x; }
        else {
            if (x < g_anMin) { g_anMin = x; }
            if (x > g_anMax) { g_anMax = x; }
            if (x > g_anPrev) { g_anMono++; }
        }
        g_anPrev = x;
        g_anSum += (int64_t)x;
        g_anSamples++;

        /* Las primeras 256 muestras fijan la linea de base. Recien despues se
         * busca la excursion: si se tomara la media de TODA la captura, el
         * propio golpe la correria y el signo de la primera excursion, que es
         * el dato de polaridad, saldria mal. */
        if (!g_anBaselineSet && g_anSamples == 256) {
            g_anBaseline = (int32_t)(g_anSum / (int64_t)g_anSamples);
            g_anBaselineSet = true;
        }
        if (g_anBaselineSet) {
            int32_t dev = x - g_anBaseline;
            int32_t adev = (dev < 0) ? -dev : dev;
            if (adev > g_anPeakDev) { g_anPeakDev = adev; }
            if (g_anFirstSign == 0 && g_anTrig > 0 && adev > g_anTrig) {
                g_anFirstSign = (dev > 0) ? 1 : -1;
            }
        }
    }
}

/* ── Utilidades de espera ────────────────────────────────────────────────── */

/* Bombea el enlace durante ms. Es la unica forma de esperar: si no se llama
 * a poll() los bytes que sube el PSoC por I2C se acumulan en el ring y se
 * pierden por overrun. */
static void stPump(uint32_t ms)
{
    uint32_t t0 = millis();
    do { psoc.poll(); delay(1); } while ((millis() - t0) < ms);
}

static void stRxClear() { g_stRxN = 0; }

/* Espera hasta n tramas 0xC5 o hasta que venza el timeout. Devuelve cuantas
 * llegaron: el que llama decide si alcanzan. */
static uint8_t stAwait(uint8_t n, uint32_t timeoutMs)
{
    uint32_t t0 = millis();
    while (g_stRxN < n && (millis() - t0) < timeoutMs) {
        psoc.poll();
        delay(1);
    }
    return g_stRxN;
}

static const PsocSelfTestResult *stFind(uint8_t id)
{
    for (uint8_t i = 0; i < g_stRxN; i++) {
        if (g_stRx[i].test_id == id) { return &g_stRx[i]; }
    }
    return nullptr;
}

/* Una medicion DC de un tap, con reintento. El reintento acotado de 2
 * intentos es el mismo patron que usan todos los runners del banco: hay una
 * clase de transitorio (F6) que hace que el primer comando despues de un
 * cambio de configuracion se ignore en silencio. */
static bool stMeasDc(uint8_t ch, uint8_t settleSel, int32_t &meanUv, int32_t &ppUv)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        stRxClear();
        psoc.stMeasDc(settleSel, ch);
        /* El timeout tiene que cubrir el asentamiento pedido mas las 32
         * conversiones del promedio. */
        if (stAwait(1, 4000 + (uint32_t)settleSel * 3000) >= 1) {
            const PsocSelfTestResult *r = stFind((uint8_t)(ST_ID_DC_BASE | ch));
            if (r && r->status == ST_OK) { meanUv = r->v0; ppUv = r->v1; return true; }
        }
    }
    return false;
}

static bool stSetIdac(uint8_t stage, uint8_t code)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        stRxClear();
        psoc.stSetIdac(stage, code);
        if (stAwait(1, 1500) >= 1) {
            const PsocSelfTestResult *r = stFind((uint8_t)(ST_ID_IDAC_BASE | stage));
            if (r && r->status == ST_OK) { return true; }
        }
    }
    return false;
}

static bool isSaturated(int32_t uv) { return (uv >= TH_RAIL_UV) || (uv <= -TH_RAIL_UV); }

/* ==========================================================================
 * GRUPO A — ESP32 solo. No necesita al PSoC, asi que corre siempre y sirve
 * para separar "la placa esta mal" de "el PSoC no contesta".
 * ========================================================================== */
static void groupA()
{
    /* A1 — arranque */
    esp_reset_reason_t rr = esp_reset_reason();
    const char *rrName = "?";
    switch (rr) {
        case ESP_RST_POWERON:  rrName = "POWERON"; break;
        case ESP_RST_SW:       rrName = "SW"; break;
        case ESP_RST_PANIC:    rrName = "PANIC"; break;
        case ESP_RST_INT_WDT:  rrName = "INT_WDT"; break;
        case ESP_RST_TASK_WDT: rrName = "TASK_WDT"; break;
        case ESP_RST_BROWNOUT: rrName = "BROWNOUT"; break;
        case ESP_RST_EXT:      rrName = "EXT"; break;
        default: break;
    }
    /* Un reset por panico o por brownout no es un detalle: el primero es un
     * bug de firmware y el segundo es alimentacion insuficiente, que en una
     * placa recien armada suele ser un regulador mal puesto. */
    bool badReset = (rr == ESP_RST_PANIC || rr == ESP_RST_BROWNOUT ||
                     rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT);
    stReportItem("A1", "Arranque ESP32", badReset ? ST_V_FAIL : ST_V_PASS,
                 "%s, heap %u KB, flash %u MB",
                 rrName, (unsigned)(ESP.getFreeHeap() / 1024),
                 (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));

    /* A2 — OLED */
    stReportItem("A2", "OLED SSD1306 por SPI",
                 localUiReady() ? ST_V_PASS : ST_V_FAIL,
                 localUiReady() ? "responde en SPI, patron dibujado"
                                : "no responde: revisar CS/DC/RESET o alimentacion");

    /* A3 — pull-ups de los botones.
     * GPIO34/35/36/39 son SOLO ENTRADA y NO tienen pull-up interno; el driver
     * los configura como INPUT pelado. Si faltan los 10 k externos a 3V3 el
     * pin flota y la UI de campo queda inservible. Se exige nivel alto
     * ESTABLE: un pin flotante puede leer alto por casualidad en una lectura
     * suelta, pero no se mantiene 200 ms. */
    const uint8_t btnPins[4] = { LOCAL_BTN_UP_PIN, LOCAL_BTN_DOWN_PIN,
                                 LOCAL_BTN_OK_PIN, LOCAL_BTN_BACK_PIN };
    const char *btnName[4] = { "UP", "DOWN", "OK", "BACK" };
    char btnBad[64]; btnBad[0] = '\0';
    int nBad = 0;
    for (int i = 0; i < 4; i++) {
        pinMode(btnPins[i], INPUT);
        int lows = 0;
        for (int k = 0; k < 200; k++) { if (digitalRead(btnPins[i]) == LOW) { lows++; } delay(1); }
        if (lows > 0) {
            nBad++;
            char one[20];
            snprintf(one, sizeof(one), "%s%s(%u)", nBad > 1 ? "," : "",
                     btnName[i], (unsigned)btnPins[i]);
            strncat(btnBad, one, sizeof(btnBad) - strlen(btnBad) - 1);
        }
    }
    if (nBad == 0) {
        stReportItem("A3", "Pull-ups de botones", ST_V_PASS, "4/4 en alto estable");
    } else {
        stReportItem("A3", "Pull-ups de botones", ST_V_FAIL,
                     "%s sin nivel alto -> falta R 10k a 3V3 (o boton pegado)", btnBad);
    }

    /* A4 — ESP-NOW */
    uint8_t bc[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    MsgHello hello;
    hello.cmd = CMD_HELLO; hello.node_id = NODE_ID; hello.psoc_ok = 0;
    hello.sample_rate = 0; hello.hw_class = SLAVE_HW_UNKNOWN; hello.sd_present = 0;
    esp_err_t e = espnowSend(bc, (const uint8_t *)&hello, sizeof(hello));
    stReportItem("A4", "ESP-NOW (init + peer + TX)",
                 (e == ESP_OK) ? ST_V_PASS : ST_V_FAIL,
                 "esp_now_send=%d, canal %u", (int)e, (unsigned)WiFi.channel());

    /* A5 — radio. Informativo: que el maestro no este encendido no es una
     * falla de ESTA placa. */
    int n = WiFi.scanNetworks(false, true);
    int ch = 0; int32_t best = -1000;
    for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i) == MASTER_AP_SSID && WiFi.RSSI(i) > best) {
            best = WiFi.RSSI(i); ch = WiFi.channel(i);
        }
    }
    WiFi.scanDelete();
    if (ch != 0) {
        stReportItem("A5", "Radio / maestro visible", ST_V_INFO,
                     "%s en canal %d, RSSI %d dBm", MASTER_AP_SSID, ch, (int)best);
    } else {
        stReportItem("A5", "Radio / maestro visible", ST_V_INFO,
                     "%d redes, %s no visible (no es falla de placa)", n, MASTER_AP_SSID);
    }

    /* A6 — GPIO25 */
    stReportItem("A6", "GPIO25 (ex PSOC_UART_RX)", ST_V_SKIP,
                 "sin conectar por diseno: el PSoC nuevo no tiene pin Tx");
}

/* ==========================================================================
 * GRUPO B — enlace ESP <-> PSoC.
 * ========================================================================== */
static bool groupB()
{
    /* B1 — subida por I2C. El PSoC es maestro y manda pings solo; si no
     * llega nada, o no hay pull-ups en SDA/SCL, o el PSoC no arranco. */
    uint32_t p0 = psoc.pingsRx(), d0 = psoc.diagEventsRx(), b0 = psoc.bytesRx();
    stPump(1500);
    uint32_t dPing = psoc.pingsRx() - p0;
    uint32_t dDiag = psoc.diagEventsRx() - d0;
    uint32_t dBytes = psoc.bytesRx() - b0;
    bool up = (dBytes > 0);
    stReportItem("B1", "Subida I2C PSoC->ESP (0x42)",
                 up ? (psoc.i2cOverruns() == 0 ? ST_V_PASS : ST_V_WARN) : ST_V_FAIL,
                 up ? "%lu B/1.5s, %lu pings, %lu diag, overruns=%lu"
                    : "silencio total: revisar pull-ups de SDA/SCL a 3V3 y GND comun",
                 (unsigned long)dBytes, (unsigned long)dPing,
                 (unsigned long)dDiag, (unsigned long)psoc.i2cOverruns());
    if (!up) {
        /* Sin enlace no tiene sentido seguir: todo lo demas daria FAIL en
         * cascada y taparia la causa real. */
        stReportItem("B2", "Bajada UART ESP->PSoC", ST_V_SKIP, "sin enlace de subida");
        stReportItem("B3", "Linea SYNC GPIO27 -> P0[4]", ST_V_SKIP, "sin enlace de subida");
        return false;
    }

    /* B2 — bajada por UART. Se manda STATUS y tienen que volver eventos.
     * Un solo test prueba LOS DOS cables: si el 0xA5 no llegara por GPIO26,
     * no habria respuesta; si la respuesta no volviera por I2C, tampoco. */
    uint32_t d1 = psoc.diagEventsRx();
    uint32_t t0 = millis();
    psoc.requestStatus();
    while ((psoc.diagEventsRx() == d1) && (millis() - t0) < 800) { psoc.poll(); delay(1); }
    uint32_t rtt = millis() - t0;
    bool down = (psoc.diagEventsRx() > d1);
    stReportItem("B2", "Bajada UART ESP->PSoC + vuelta por I2C",
                 down ? ST_V_PASS : ST_V_FAIL,
                 down ? "STATUS respondido en %lu ms"
                      : "sin respuesta en 800 ms: revisar GPIO26 -> Rx P15[0]",
                 (unsigned long)rtt);

    /* B3 — linea de SYNC. Es el unico cable que no se puede probar de otra
     * forma: el PSoC cuenta flancos y el ESP compara con los que genero. */
    stRxClear();
    psoc.stSync(1);
    bool armed = (stAwait(1, 1200) >= 1);
    if (!armed) {
        stReportItem("B3", "Linea SYNC GPIO27 -> P0[4]", ST_V_FAIL,
                     "el PSoC no acepto armar el contador");
    } else {
        pinMode(SYNC_TO_PSOC_PIN, OUTPUT);
        digitalWrite(SYNC_TO_PSOC_PIN, LOW);
        delay(5);
        for (int i = 0; i < TH_SYNC_EDGES / 2; i++) {
            digitalWrite(SYNC_TO_PSOC_PIN, HIGH); delay(6);
            digitalWrite(SYNC_TO_PSOC_PIN, LOW);  delay(6);
        }
        delay(30);
        stRxClear();
        psoc.stSync(0);
        if (stAwait(1, 1200) >= 1) {
            const PsocSelfTestResult *r = stFind(ST_ID_SYNC);
            int got = r ? (int)r->v0 : -1;
            /* Se generaron TH_SYNC_EDGES/2 ciclos completos. Segun como este
             * configurada la interrupcion del pin, la ISR puede disparar en
             * los dos flancos o en uno solo: las dos cuentas prueban el cable
             * igual de bien. Exigir una sola convertiria una configuracion
             * legitima en un FAIL. */
            bool both = (got == TH_SYNC_EDGES);
            bool one  = (got == TH_SYNC_EDGES / 2);
            /* Dos llamadas separadas y no un formato condicional: los dos
             * formatos toman argumentos distintos y mezclarlos corre los
             * varargs. */
            if (both || one) {
                stReportItem("B3", "Linea SYNC GPIO27 -> P0[4]", ST_V_PASS,
                             "%d flancos en %d ciclos (%s)",
                             got, TH_SYNC_EDGES / 2,
                             both ? "ambos flancos" : "un flanco");
            } else {
                stReportItem("B3", "Linea SYNC GPIO27 -> P0[4]", ST_V_FAIL,
                             "%d flancos en %d ciclos: se esperaban %d o %d",
                             got, TH_SYNC_EDGES / 2, TH_SYNC_EDGES, TH_SYNC_EDGES / 2);
            }
        } else {
            stReportItem("B3", "Linea SYNC GPIO27 -> P0[4]", ST_V_FAIL,
                         "el PSoC no contesto la lectura del contador");
        }
    }
    return true;
}

/* Dispara una captura de n lotes y espera a que lleguen. Es el mismo camino
 * que usa el firmware de campo (SETN -> PRESTART -> flanco de SYNC); la unica
 * diferencia es que aca los lotes se analizan al vuelo en vez de guardarse.
 * La usan C4 (rampa cruda), C5 (rampa por el FIR) y D7 (golpe al geofono). */
static bool stCapture(uint16_t n, int32_t trigger)
{
    g_batchCount = 0;
    g_firstBatchSeen = false;
    anReset(trigger);

    psoc.setN(n);
    stPump(150);
    g_evArmed = false;
    psoc.preStart();
    uint32_t t0 = millis();
    while (!g_evArmed && (millis() - t0) < 2000) { psoc.poll(); delay(1); }
    if (!g_evArmed) { return false; }

    pinMode(SYNC_TO_PSOC_PIN, OUTPUT);
    digitalWrite(SYNC_TO_PSOC_PIN, HIGH);
    /* Duracion nominal de la captura mas margen para el volcado: n lotes de
     * 30 muestras a 2604 Hz, y el volcado sale de a un lote por vuelta del
     * lazo principal del PSoC. */
    uint32_t nominal = ((uint32_t)n * 30UL * 1000UL) / 2604UL;
    stPump(nominal + 2000 + (uint32_t)n * 4UL);
    digitalWrite(SYNC_TO_PSOC_PIN, LOW);
    stPump(400);
    return true;
}

/* ==========================================================================
 * GRUPO C — infraestructura del PSoC.
 * ========================================================================== */
static void groupC()
{
    /* C1 — identidad */
    stRxClear();
    psoc.stReport(ST_REP_IDENTITY);
    stAwait(2, 2000);
    const PsocSelfTestResult *id = stFind(ST_ID_IDENTITY);
    const PsocSelfTestResult *sg = stFind(ST_ID_STAGES);
    if (id && sg) {
        g_hwClass = (uint8_t)id->v0;
        g_psocStages = (uint8_t)sg->v0;
        g_amuxChannels = (uint8_t)sg->v1;
        stReportItem("C1", "Identidad del PSoC", ST_V_PASS,
                     "clase=%s, Fs=%ld Hz, %u etapas, %u canales AMux",
                     (g_hwClass == 0) ? "GEO" : "HAMMER", (long)id->v1,
                     (unsigned)g_psocStages, (unsigned)g_amuxChannels);
    } else {
        stReportItem("C1", "Identidad del PSoC", ST_V_FAIL, "sin respuesta");
    }

    /* C2 — trampas de IRQ. Cualquier IRQ inesperada o HardFault es una
     * bandera roja: el firmware de campo las reporta pero nadie las mira. */
    stRxClear();
    psoc.stReport(ST_REP_IRQ);
    stAwait(1, 1500);
    const PsocSelfTestResult *tr = stFind(ST_ID_IRQTRAP);
    if (tr) {
        bool clean = (tr->v0 == 0 && tr->v1 == 0 && g_irqTraps == 0);
        stReportItem("C2", "Sin trampas de IRQ / HardFault",
                     clean ? ST_V_PASS : ST_V_FAIL,
                     "IRQ inesperadas=%ld, hardfaults=%ld, vistas por el ESP=%u",
                     (long)tr->v0, (long)tr->v1, (unsigned)g_irqTraps);
    } else {
        stReportItem("C2", "Sin trampas de IRQ / HardFault", ST_V_FAIL, "sin respuesta");
    }

    /* C3 — EEPROM. Solo lectura. Cero slots validos es lo NORMAL en una placa
     * recien armada, asi que es WARN y no FAIL. */
    stRxClear();
    psoc.stReport(ST_REP_EEPROM);
    stAwait(1, 3000);
    const PsocSelfTestResult *ee = stFind(ST_ID_EEPROM);
    if (ee) {
        stReportItem("C3", "EEPROM de calibracion (CRC-16)",
                     (ee->v0 > 0) ? ST_V_PASS : ST_V_WARN,
                     (ee->v0 > 0) ? "%ld/9 slots con CRC valido, mascara 0x%03lX"
                                  : "0/9 slots: placa sin calibrar todavia (esperable)",
                     (long)ee->v0, (long)ee->v1);
    } else {
        stReportItem("C3", "EEPROM de calibracion (CRC-16)", ST_V_FAIL, "sin respuesta");
    }

    /* C4 — camino digital de punta a punta con la rampa de debug.
     * No pasa por el analogico: si esto anda y lo analogico no, el problema
     * esta acotado al front end. Si esto NO anda, no tiene sentido creerle a
     * ninguna medicion posterior. */
    psoc.debugRamp(true);
    stPump(150);
    psoc.selectStream(0);          /* camino crudo */
    stPump(150);
    if (!stCapture(8, 0)) {
        stReportItem("C4", "Camino digital E2E (rampa cruda)", ST_V_FAIL,
                     "el PSoC no llego a ARMED");
    } else {
        /* La rampa es monotona creciente salvo en el wrap, que ocurre una vez
         * por vuelta del contador. Se tolera un 5 % de no-crecientes. */
        uint32_t n = g_anSamples;
        bool ramp = (n > 60) && (g_anMono * 100UL >= (n - 1) * 95UL);
        stReportItem("C4", "Camino digital E2E (rampa cruda)",
                     (g_batchCount >= 8 && ramp) ? ST_V_PASS : ST_V_FAIL,
                     "%u/8 lotes, %lu muestras, %lu%% crecientes, bBad=%lu",
                     (unsigned)g_batchCount, (unsigned long)n,
                     n > 1 ? (unsigned long)(g_anMono * 100UL / (n - 1)) : 0UL,
                     (unsigned long)psoc.batchesBad());
    }

    /* C5 — la misma rampa pero por el FIR de hardware (DFB). Si C4 pasa y C5
     * no, el problema esta acotado al Filter y a su DMA, no al camino de
     * captura. La salida ya no es la rampa cruda: el FIR la suaviza y la
     * retrasa, pero tiene que seguir siendo creciente y con el mismo rango
     * aproximado. */
    psoc.selectStream(1);
    stPump(200);
    if (!stCapture(8, 0)) {
        stReportItem("C5", "Filtro FIR de hardware (DFB)", ST_V_FAIL,
                     "el PSoC no llego a ARMED");
    } else {
        uint32_t n = g_anSamples;
        bool grows = (n > 60) && (g_anMono * 100UL >= (n - 1) * 80UL);
        bool alive = (g_anMax != g_anMin);
        stReportItem("C5", "Filtro FIR de hardware (DFB)",
                     (g_batchCount >= 8 && grows && alive) ? ST_V_PASS : ST_V_FAIL,
                     "%u/8 lotes, %lu%% crecientes, rango %ld..%ld",
                     (unsigned)g_batchCount,
                     n > 1 ? (unsigned long)(g_anMono * 100UL / (n - 1)) : 0UL,
                     (long)g_anMin, (long)g_anMax);
    }
    psoc.selectStream(0);
    stPump(150);
    psoc.debugRamp(false);
    stPump(150);

    /* C6 — SD. Es el item que quedo explicitamente sin validar en la placa
     * nueva: los cuatro pines de SPIp quedaron repartidos en cuatro puertos
     * distintos y el CS pasa a manejarse por software. */
    g_sdStatusSeen = false;
    psoc.sdStatus(1);
    stPump(2500);
    psoc.sdTest();
    stPump(4000);
    stRxClear();
    psoc.stReport(ST_REP_SD);
    stAwait(1, 2000);
    const PsocSelfTestResult *sd = stFind(ST_ID_SD);
    if (sd) {
        uint8_t s = (uint8_t)sd->v0;
        bool present = (s & 0x01) != 0;
        bool selftest = (s & 0x08) != 0;
        bool fat = (s & 0x10) != 0;
        StVerdict v = (present && selftest && fat) ? ST_V_PASS
                    : (present ? ST_V_FAIL : ST_V_SKIP);
        stReportItem("C6", "SD FatFs (ruteo SPIp nuevo)", v,
                     present ? "estado=0x%02X presente=1 tipo=%u selftest=%u FAT=%u err=0x%02lX"
                             : "sin tarjeta detectada (estado=0x%02X)",
                     (unsigned)s, (unsigned)((s >> 1) & 0x03),
                     (unsigned)selftest, (unsigned)fat, (long)sd->v1);
    } else {
        stReportItem("C6", "SD FatFs (ruteo SPIp nuevo)", ST_V_FAIL, "sin respuesta");
    }

    /* C7 — pulsador del PSoC (P2[2]). En reposo tiene que leer un nivel
     * estable. Se muestrea varias veces porque un pulsador rebotando o un pin
     * flotante no sostienen el nivel. La pulsacion en si es interactiva:
     * comando USB `boton`. */
    {
        int lvl = -1; bool stable = true;
        for (int k = 0; k < 5; k++) {
            stRxClear();
            psoc.stReport(ST_REP_BUTTON);
            if (stAwait(1, 1200) >= 1) {
                const PsocSelfTestResult *r = stFind(ST_ID_BUTTON);
                if (r) {
                    if (lvl < 0) { lvl = (int)r->v0; }
                    else if ((int)r->v0 != lvl) { stable = false; }
                }
            }
            delay(40);
        }
        if (lvl < 0) {
            stReportItem("C7", "Pulsador del PSoC en reposo", ST_V_FAIL, "sin respuesta");
        } else {
            stReportItem("C7", "Pulsador del PSoC en reposo",
                         stable ? ST_V_PASS : ST_V_FAIL,
                         stable ? "nivel %d estable en 5 lecturas"
                                : "nivel inestable: pulsador rebotando o pin flotante",
                         lvl);
        }
    }
}

/* ==========================================================================
 * GRUPO D — analogico. El nucleo del autotest.
 * ========================================================================== */

/* Pendientes medidas en D2, en uV por codigo de IDAC. Filas = etapa que se
 * mueve, columnas = tap donde se mide. */
static float g_slope[4][4];
static bool  g_slopeOk[4][4];
/* Un extremo del barrido contra el riel comprime la pendiente. Se marca aparte
 * porque D4 (cociente de ganancia) tiene que negarse a dar un veredicto en vez
 * de informar un cociente que en realidad es basura. */
static bool  g_slopeSat[4][4];

/* Barre una etapa y mide todos los taps. Devuelve false si no se pudo. */
static bool d2SweepStage(uint8_t stage, uint8_t nTaps, int stepCodes)
{
    uint8_t base;
    /* Punto de partida: el codigo que tiene puesto ahora. Se centra el
     * escalon alrededor de el para no alejarse del punto de trabajo. */
    stRxClear();
    psoc.stReport(ST_REP_CAL);
    stAwait(nTaps, 2500);
    const PsocSelfTestResult *cur = stFind((uint8_t)(ST_ID_CAL_BASE | stage));
    base = cur ? (uint8_t)cur->v0 : 128;

    int lo = (int)base - stepCodes;
    int hi = (int)base + stepCodes;
    if (lo < 0)   { hi -= lo; lo = 0; }
    if (hi > 255) { lo -= (hi - 255); hi = 255; }
    if (lo < 0) { lo = 0; }
    int span = hi - lo;
    if (span < TH_D2_MIN_STEP) { return false; }

    int32_t vlo[4], vhi[4], pp;

    if (!stSetIdac(stage, (uint8_t)lo)) { return false; }
    for (uint8_t ch = 0; ch < nTaps; ch++) {
        if (!stMeasDc(ch, SEL_SETTLE_DC, vlo[ch], pp)) { vlo[ch] = 0; g_slopeOk[stage][ch] = false; }
    }
    if (!stSetIdac(stage, (uint8_t)hi)) { stSetIdac(stage, base); return false; }
    for (uint8_t ch = 0; ch < nTaps; ch++) {
        if (!stMeasDc(ch, SEL_SETTLE_DC, vhi[ch], pp)) { vhi[ch] = 0; g_slopeOk[stage][ch] = false; }
    }
    stSetIdac(stage, base);   /* siempre restaurar */

    /* Si algun tap saturo en cualquiera de los dos extremos, la pendiente
     * queda comprimida y no sirve. Se reintenta con la mitad del escalon. */
    for (uint8_t ch = 0; ch < nTaps; ch++) {
        if (isSaturated(vlo[ch]) || isSaturated(vhi[ch])) {
            if (stepCodes / 2 >= TH_D2_MIN_STEP) {
                return d2SweepStage(stage, nTaps, stepCodes / 2);
            }
        }
    }

    for (uint8_t ch = 0; ch < nTaps; ch++) {
        g_slope[stage][ch] = (float)(vhi[ch] - vlo[ch]) / (float)span;
        g_slopeOk[stage][ch] = true;
        g_slopeSat[stage][ch] = isSaturated(vlo[ch]) || isSaturated(vhi[ch]);
    }
    return true;
}

static void groupD()
{
    uint8_t nTaps = (g_psocStages > 0 && g_psocStages <= 4) ? g_psocStages : 4;

    /* D1 — reposo de las referencias. */
    {
        char det[96]; det[0] = '\0';
        bool railed = false;
        for (uint8_t ch = 0; ch < nTaps; ch++) {
            int32_t m, pp;
            if (stMeasDc(ch, SEL_SETTLE_DC, m, pp)) {
                char one[24];
                snprintf(one, sizeof(one), "%sch%u=%ldmV", ch ? " " : "",
                         (unsigned)ch, (long)(m / 1000));
                strncat(det, one, sizeof(det) - strlen(det) - 1);
                if (isSaturated(m)) { railed = true; }
            }
        }
        stReportItem("D1", "Reposo de los taps analogicos",
                     railed ? ST_V_FAIL : ST_V_PASS,
                     railed ? "%s  <- algun tap contra el riel" : "%s", det);

        /* El canal del capacitor tiene criterio propio: es un 100 nF a Vss,
         * o sea que legitimamente lee cerca de un riel. Reprobarlo por
         * "riel" marcaria en falta una placa sana. */
        if (g_amuxChannels > nTaps) {
            int32_t m, pp;
            if (stMeasDc((uint8_t)(g_amuxChannels - 1), SEL_SETTLE_DC, m, pp)) {
                stReportItem("D1b", "AMuxCapacitor (100 nF a Vss)", ST_V_INFO,
                             "%ld mV (cerca de Vss es lo esperado)", (long)(m / 1000));
            }
        }
    }

    /* D2 — matriz de transferencia DC.
     * El veredicto NO se juzga contra valores nominales sino contra la
     * ESTRUCTURA TRIANGULAR SUPERIOR, que es un invariante insensible a
     * tolerancias: una etapa mueve los taps de aguas abajo y no los de aguas
     * arriba. Eso localiza la falla en una etapa concreta sin pedirle
     * precision a nada. */
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            g_slope[i][j] = 0.0f; g_slopeOk[i][j] = false; g_slopeSat[i][j] = false;
        }
    }
    int sweeps = 0;
    for (uint8_t k = 0; k < nTaps; k++) {
        if (d2SweepStage(k, nTaps, TH_D2_STEP_CODES)) { sweeps++; }
    }

    {
        int badFwd = 0, badIso = 0;
        char worst[64]; worst[0] = '\0';
        for (uint8_t k = 0; k < nTaps; k++) {
            float diag = fabsf(g_slope[k][k]);
            /* Propagacion hacia adelante: la etapa tiene que mover su propio
             * tap. Si no lo mueve, esa etapa esta muerta. */
            if (!g_slopeOk[k][k] || diag < (float)TH_D2_MIN_SLOPE_UV_PER_CODE) {
                badFwd++;
                if (worst[0] == '\0') {
                    snprintf(worst, sizeof(worst), "etapa %u no mueve su tap (%.0f uV/cod)",
                             (unsigned)k, (double)diag);
                }
            }
            /* Aislamiento hacia atras. */
            for (uint8_t m = 0; m < k; m++) {
                if (g_slopeOk[k][m] && diag > 0.0f &&
                    fabsf(g_slope[k][m]) > TH_D2_ISOLATION_RATIO * diag) {
                    badIso++;
                    if (worst[0] == '\0') {
                        snprintf(worst, sizeof(worst),
                                 "etapa %u mueve el tap %u (aguas arriba)",
                                 (unsigned)k, (unsigned)m);
                    }
                }
            }
        }
        StVerdict v = (sweeps == 0) ? ST_V_FAIL
                    : (badFwd > 0) ? ST_V_FAIL
                    : (badIso > 0) ? ST_V_WARN : ST_V_PASS;
        stReportItem("D2", "Matriz DC IDAC->etapa", v,
                     (worst[0] != '\0') ? "%d barridos, %d sin respuesta, %d fugas: %s"
                                        : "%d barridos, triangular OK (%d/%d)",
                     sweeps, badFwd, badIso, worst);

        /* Las pendientes crudas se imprimen siempre: son el dato que sirve
         * para diagnosticar, mas alla del veredicto. */
        for (uint8_t k = 0; k < nTaps; k++) {
            char row[96]; row[0] = '\0';
            for (uint8_t m = 0; m < nTaps; m++) {
                char one[24];
                snprintf(one, sizeof(one), "%s%.1f", m ? " " : "",
                         g_slopeOk[k][m] ? (double)g_slope[k][m] : 0.0);
                strncat(row, one, sizeof(row) - strlen(row) - 1);
            }
            char code[8]; snprintf(code, sizeof(code), "D2.%u", (unsigned)k);
            stReportItem(code, "  pendientes uV/codigo", ST_V_INFO, "%s", row);
        }
    }

    /* D3 — asentamiento. Se mide el mismo tap con 5 ms y con 500 ms de
     * espera: la diferencia dice cuanto seguia moviendose. Un capacitor mal
     * puesto cambia esa diferencia. Es WARN, nunca FAIL: la cadena tiene un
     * 680 uF y los tiempos largos son legitimos. */
    {
        int32_t fast, slow, pp;
        uint8_t ch = (uint8_t)(nTaps - 1);
        if (stMeasDc(ch, SEL_SETTLE_FAST, fast, pp) &&
            stMeasDc(ch, SEL_SETTLE_DC, slow, pp)) {
            long drift = (long)labs((long)(slow - fast));
            stReportItem("D3", "Asentamiento del ultimo tap",
                         (drift > 50000) ? ST_V_WARN : ST_V_PASS,
                         "deriva 5ms->500ms = %ld uV", drift);
        } else {
            stReportItem("D3", "Asentamiento del ultimo tap", ST_V_SKIP, "sin medicion");
        }
    }

    /* D4 — cociente de ganancia de PGAout. Al ser un COCIENTE cancela toda
     * la incertidumbre de los resistores del sumador y del pasabajos, asi
     * que es el test analogico mas confiable de todos. */
    if (g_hwClass == 0 && nTaps >= 4) {
        float s1 = 0.0f, s4 = 0.0f;
        bool sat = false;
        psoc.setPgaout(0); stPump(400);
        if (d2SweepStage(2, nTaps, TH_D2_STEP_CODES)) {
            s1 = fabsf(g_slope[2][3]); sat |= g_slopeSat[2][3];
        }
        psoc.setPgaout(2); stPump(400);
        if (d2SweepStage(2, nTaps, TH_D2_STEP_CODES)) {
            s4 = fabsf(g_slope[2][3]); sat |= g_slopeSat[2][3];
        }
        psoc.setPgaout(0); stPump(400);
        if (sat) {
            /* A 4x el punto de trabajo se puede ir contra el riel en una placa
             * sin calibrar. Eso no prueba que la ganancia este mal, asi que no
             * corresponde un FAIL: se informa y se rehace despues de D8. */
            stReportItem("D4", "Ganancia PGAout 1x vs 4x", ST_V_SKIP,
                         "algun extremo saturo: cociente no medible sin calibrar");
        } else if (s1 > 1.0f) {
            float ratio = s4 / s1;
            bool ok = fabsf(ratio - TH_GAIN_RATIO_NOM) <=
                      TH_GAIN_RATIO_NOM * TH_GAIN_RATIO_TOL;
            stReportItem("D4", "Ganancia PGAout 1x vs 4x",
                         ok ? ST_V_PASS : ST_V_FAIL,
                         "cociente %.2f (nominal %.2f, tol %.0f%%)",
                         (double)ratio, (double)TH_GAIN_RATIO_NOM,
                         (double)(TH_GAIN_RATIO_TOL * 100.0f));
        } else {
            stReportItem("D4", "Ganancia PGAout 1x vs 4x", ST_V_FAIL,
                         "sin pendiente medible a ganancia 1x");
        }
    } else {
        stReportItem("D4", "Ganancia PGAout 1x vs 4x", ST_V_SKIP,
                     "solo aplica a nodos GEO");
    }

    /* D6 — piso de ruido. Solo reprueba por absurdo; el valor de 50 Hz se
     * informa siempre porque es el indicador de captacion de red. */
    for (uint8_t ch = 0; ch < nTaps; ch++) {
        stRxClear();
        psoc.stMeasAc(SEL_N_NOISE, ch);
        if (stAwait(2, 12000) >= 2) {
            const PsocSelfTestResult *a = stFind((uint8_t)(ST_ID_ACA_BASE | ch));
            const PsocSelfTestResult *b = stFind((uint8_t)(ST_ID_ACB_BASE | ch));
            if (a && b && a->status == ST_OK && b->status == ST_OK) {
                int32_t rms = a->v1, mains = b->v1;
                StVerdict v = (rms < TH_NOISE_RMS_DEAD_UV) ? ST_V_FAIL
                            : (rms > TH_NOISE_RMS_MAX_UV) ? ST_V_FAIL
                            : (mains > TH_NOISE_MAINS_WARN_UV) ? ST_V_WARN : ST_V_PASS;
                char code[8]; snprintf(code, sizeof(code), "D6.%u", (unsigned)ch);
                stReportItem(code, "Piso de ruido del tap", v,
                             "media %ld uV, RMS %ld uV, pp %ld uV, 50Hz %ld uV",
                             (long)a->v0, (long)rms, (long)b->v0, (long)mains);
                continue;
            }
        }
        char code[8]; snprintf(code, sizeof(code), "D6.%u", (unsigned)ch);
        stReportItem(code, "Piso de ruido del tap", ST_V_FAIL, "sin medicion");
    }

    /* D8 — auto-calibracion. Corre ACA, despues de D1..D6 (que necesitan la
     * placa sin calibrar) y antes de D5 (que la necesita calibrada). */
    g_evCalDone = false; g_evCalOk = 0;
    uint32_t tcal = millis();
    psoc.calibrate();
    while (!g_evCalDone && (millis() - tcal) < 180000UL) { psoc.poll(); delay(2); }
    uint32_t calMs = millis() - tcal;
    if (!g_evCalDone) {
        stReportItem("D8", "Auto-calibracion", ST_V_FAIL,
                     "no termino en 180 s");
    } else {
        stRxClear();
        psoc.stReport(ST_REP_CAL);
        stAwait(nTaps, 3000);
        int railed = 0, failed = 0;
        char det[96]; det[0] = '\0';
        for (uint8_t k = 0; k < nTaps; k++) {
            const PsocSelfTestResult *r = stFind((uint8_t)(ST_ID_CAL_BASE | k));
            if (!r) { failed++; continue; }
            if (r->status != ST_OK) { failed++; }
            /* Un codigo pegado a 0 o a 255 significa que el offset de esa
             * etapa no se pudo anular: hay algo mal en esa etapa. */
            if (r->v0 <= 0 || r->v0 >= 255) { railed++; }
            char one[20];
            snprintf(one, sizeof(one), "%s%ld", k ? "/" : "", (long)r->v0);
            strncat(det, one, sizeof(det) - strlen(det) - 1);
        }
        stReportItem("D8", "Auto-calibracion",
                     (failed || railed) ? ST_V_FAIL : ST_V_PASS,
                     "IDAC %s, %d etapa(s) al riel, %d fallada(s), %lu s",
                     det, railed, failed, (unsigned long)(calMs / 1000));
    }

    /* D5 — coherencia entre las 4 configs del ADC. Va DESPUES de D8 porque
     * necesita que el tap este cerca del nulo: cualquier cosa mas alla de
     * +-0,512 V recorta en la config 2 y la "coherencia" mediria basura o
     * reprobaria una placa sana. */
    {
        int32_t uv[5]; int got = 0; bool clipped = false;
        uint8_t ch = (uint8_t)(nTaps - 1);
        uint8_t stage = (uint8_t)(nTaps - 1);

        /* D8 deja el tap EN EL NULO (target = 0 counts). Comparar cuatro
         * medidas de ~0 uV no valida nada: cualquier escalado, correcto o no,
         * da cero. Hace falta una excursion conocida. Se inyecta con el IDAC
         * de la ultima etapa y se mantiene por debajo de +-0,45 V para que la
         * config 2 (+-0,512 V) no recorte. */
        stRxClear();
        psoc.stReport(ST_REP_CAL);
        stAwait(nTaps, 2500);
        const PsocSelfTestResult *cal = stFind((uint8_t)(ST_ID_CAL_BASE | stage));
        uint8_t baseCode = cal ? (uint8_t)cal->v0 : 128;
        int offCode = (int)baseCode + 40;
        if (offCode > 255) { offCode = (int)baseCode - 40; }
        if (offCode < 0)   { offCode = 0; }
        bool offsetOk = stSetIdac(stage, (uint8_t)offCode);

        for (uint8_t cfg = 1; cfg <= 4; cfg++) {
            psoc.setAdcConfig(cfg);
            stPump(500);
            int32_t m, pp;
            if (stMeasDc(ch, SEL_SETTLE_DC, m, pp)) {
                /* Si supera el fondo de escala de la config mas chica, la
                 * comparacion no significa nada. */
                if (labs((long)m) > 450000L) { clipped = true; }
                uv[got++] = m;
            }
        }
        psoc.setAdcConfig(1);
        stPump(500);
        stSetIdac(stage, baseCode);   /* siempre restaurar el punto calibrado */

        if (!offsetOk) {
            stReportItem("D5", "Coherencia de rangos del ADC", ST_V_SKIP,
                         "no se pudo inyectar la excursion de prueba");
        } else if (clipped) {
            stReportItem("D5", "Coherencia de rangos del ADC", ST_V_SKIP,
                         "el tap quedo fuera de +-0.45 V: la config 2 recortaria");
        } else if (got == 4) {
            int32_t mn = uv[0], mx = uv[0];
            for (int i = 1; i < 4; i++) { if (uv[i] < mn) mn = uv[i]; if (uv[i] > mx) mx = uv[i]; }
            long amp = labs((long)mx) > labs((long)mn) ? labs((long)mx) : labs((long)mn);
            /* Si pese a la inyeccion la excursion quedo despreciable, el test
             * no puede concluir nada: decirlo, no dar un PASS decorativo. */
            if (amp < 20000L) {
                stReportItem("D5", "Coherencia de rangos del ADC", ST_V_SKIP,
                             "excursion de solo %ld uV: insuficiente para comparar rangos",
                             amp);
            } else {
                float spread = (float)(mx - mn) / (float)amp;
                stReportItem("D5", "Coherencia de rangos del ADC",
                             (spread <= TH_ADCCFG_SPREAD_MAX) ? ST_V_PASS : ST_V_FAIL,
                             "dispersion %.1f%% sobre %ld uV (%ld..%ld uV)",
                             (double)(spread * 100.0f), amp, (long)mn, (long)mx);
            }
        } else {
            stReportItem("D5", "Coherencia de rangos del ADC", ST_V_FAIL,
                         "solo %d de 4 configs midieron", got);
        }
    }
}

/* ==========================================================================
 * FASE INTERACTIVA — lo que ningun test automatico puede cubrir.
 * Se dispara por comando, nunca sola: pide acciones al operador.
 * ========================================================================== */

/* D7 — golpe al geofono. Captura 128 lotes (1,47 s a 2604 Hz) mientras el
 * operador golpea al lado del geofono, y decide con dos cosas:
 *
 *   - que el pico supere claramente el ruido de fondo (el geofono responde);
 *   - el SIGNO de la primera excursion grande, que es la POLARIDAD.
 *
 * La polaridad importa: entre campañas quedo documentada una inversion en
 * varias posiciones, y hasta ahora no habia forma de verificarla en el nodo.
 * El SM-24 tiene fn = 10 Hz y zeta = 0,25, asi que a 2604 Hz un ciclo del
 * timbrado son ~260 muestras y la captura cubre unos 14 ciclos. */
static void testTap()
{
    Serial.println(F(""));
    Serial.println(F("[ST] D7 — GOLPE AL GEOFONO"));
    Serial.println(F("[ST] Golpea el suelo al lado del geofono cuando diga YA."));
    for (int i = 3; i > 0; i--) { Serial.printf("[ST] %d...\n", i); delay(1000); }
    Serial.println(F("[ST] YA!"));

    /* Umbral de excursion: 4x el ruido de fondo tipico. Se mide antes para no
     * inventar un numero: una captura corta sin golpe da la referencia. */
    if (!stCapture(16, 0)) {
        stReportItem("D7", "Golpe al geofono", ST_V_FAIL, "no se pudo capturar el fondo");
        return;
    }
    int32_t floorPp = g_anMax - g_anMin;
    int32_t trig = (floorPp > 0) ? (floorPp * 2) : 100;

    if (!stCapture(128, trig)) {
        stReportItem("D7", "Golpe al geofono", ST_V_FAIL, "no se pudo capturar");
        return;
    }
    int32_t peak = g_anPeakDev;
    bool hit = (peak > trig) && (peak > floorPp);
    const char *pol = (g_anFirstSign > 0) ? "POSITIVA"
                    : (g_anFirstSign < 0) ? "NEGATIVA" : "sin definir";
    stReportItem("D7", "Golpe al geofono (pico y polaridad)",
                 hit ? ST_V_PASS : ST_V_FAIL,
                 hit ? "pico %ld counts (fondo pp %ld), 1a excursion %s"
                     : "pico %ld counts no supera el fondo pp %ld: %s",
                 (long)peak, (long)floorPp, pol);
}

/* Pulsacion de los cuatro botones del ESP, de a uno. */
static void testButtonsEsp()
{
    const uint8_t pins[4] = { LOCAL_BTN_UP_PIN, LOCAL_BTN_DOWN_PIN,
                              LOCAL_BTN_OK_PIN, LOCAL_BTN_BACK_PIN };
    const char *names[4] = { "UP", "DOWN", "OK", "BACK" };
    for (int i = 0; i < 4; i++) {
        Serial.printf("[ST] Pulsa el boton %s (10 s)...\n", names[i]);
        uint32_t t0 = millis();
        bool seen = false;
        while ((millis() - t0) < 10000) {
            if (digitalRead(pins[i]) == LOW) { seen = true; break; }
            delay(5);
        }
        char code[8]; snprintf(code, sizeof(code), "E1.%d", i);
        char name[32]; snprintf(name, sizeof(name), "Boton %s del ESP", names[i]);
        stReportItem(code, name, seen ? ST_V_PASS : ST_V_FAIL,
                     seen ? "pulsacion detectada en GPIO%u" : "sin pulsacion en 10 s (GPIO%u)",
                     (unsigned)pins[i]);
        while (digitalRead(pins[i]) == LOW) { delay(5); }   /* esperar que suelte */
        delay(200);
    }
}

/* Pulsacion del boton del PSoC (P2[2], el onboard del CY8CKIT-059). */
static void testButtonPsoc()
{
    Serial.println(F("[ST] Pulsa el boton del PSoC (10 s)..."));
    int idle = -1; bool changed = false;
    uint32_t t0 = millis();
    while ((millis() - t0) < 10000) {
        stRxClear();
        psoc.stReport(ST_REP_BUTTON);
        if (stAwait(1, 800) >= 1) {
            const PsocSelfTestResult *r = stFind(ST_ID_BUTTON);
            if (r) {
                if (idle < 0) { idle = (int)r->v0; }
                else if ((int)r->v0 != idle) { changed = true; break; }
            }
        }
        delay(30);
    }
    stReportItem("E2", "Boton del PSoC (P2[2])",
                 changed ? ST_V_PASS : ST_V_FAIL,
                 changed ? "cambio de nivel detectado" : "sin cambio en 10 s");
}

/* ==========================================================================
 * Cierre: integridad de trama sobre TODA la corrida.
 * ========================================================================== */
static void groupBClose()
{
    /* Ojo: `drop`/_syncDrops NO es criterio. Cuenta bytes de texto del PSoC
     * descartados por el framer mientras busca el marcador, y crece de forma
     * benigna incluso en reposo (hallazgo F8 del banco). Los unicos que
     * indican trama corrupta son bBad y badLen. */
    /* Si nunca llego un byte, "cero tramas corruptas" es cierto pero enganoso:
     * seria un PASS en una placa sin enlace. Corresponde SKIP. */
    if (psoc.bytesRx() == 0) {
        stReportItem("B4", "Integridad de trama en toda la corrida", ST_V_SKIP,
                     "no llego ningun byte del PSoC: nada que verificar");
        return;
    }
    bool ok = (psoc.batchesBad() == 0 && psoc.badLen() == 0);
    stReportItem("B4", "Integridad de trama en toda la corrida",
                 ok ? ST_V_PASS : ST_V_FAIL,
                 "bBad=%lu badLen=%lu (drop=%lu informativo)",
                 (unsigned long)psoc.batchesBad(), (unsigned long)psoc.badLen(),
                 (unsigned long)psoc.syncDrops());
}

/* ── Corrida completa ────────────────────────────────────────────────────── */
static void runAutotest()
{
    char espBuild[48], psocBuild[48], mac[20];
    snprintf(espBuild, sizeof(espBuild), "slaveTest %s %s", __DATE__, __TIME__);
    snprintf(psocBuild, sizeof(psocBuild), "AcondAnalogTest (ver DIAG BOOT)");
    uint8_t m[6];
    WiFi.macAddress(m);
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             m[0], m[1], m[2], m[3], m[4], m[5]);

    stReportReset();
    stReportBegin(espBuild, psocBuild, mac, NODE_ID);

    groupA();
    if (groupB()) {
        groupC();
        groupD();
    } else {
        stReportItem("C*", "Tests del PSoC", ST_V_SKIP, "sin enlace con el PSoC");
        stReportItem("D*", "Tests analogicos", ST_V_SKIP, "sin enlace con el PSoC");
    }
    groupBClose();
    stReportSummary();

    localUiShowSelfTest(stReportCount(ST_V_PASS), stReportCount(ST_V_FAIL),
                        stReportCount(ST_V_WARN), stReportCount(ST_V_SKIP),
                        stReportFirstFail());
}

/* ── Consola USB ─────────────────────────────────────────────────────────── */
static void handleCmd(const char *cmd)
{
    if (!strcmp(cmd, "run") || !strcmp(cmd, "test")) {
        runAutotest();
    } else if (!strcmp(cmd, "a")) {
        stReportReset(); groupA(); stReportSummary();
    } else if (!strcmp(cmd, "b")) {
        stReportReset(); groupB(); groupBClose(); stReportSummary();
    } else if (!strcmp(cmd, "c")) {
        stReportReset(); groupC(); stReportSummary();
    } else if (!strcmp(cmd, "d")) {
        stReportReset(); groupD(); stReportSummary();
    } else if (!strcmp(cmd, "tap") || !strcmp(cmd, "golpe")) {
        stReportReset(); testTap(); stReportSummary();
    } else if (!strcmp(cmd, "e") || !strcmp(cmd, "inter")) {
        stReportReset();
        testButtonsEsp();
        testButtonPsoc();
        testTap();
        stReportSummary();
    } else if (!strcmp(cmd, "probe")) {
        bool ok = psoc.probe(400);
        Serial.printf("[ST] probe=%d bOK=%lu bBad=%lu ping=%lu diag=%lu ovr=%lu\n",
                      (int)ok, (unsigned long)psoc.batchesOK(),
                      (unsigned long)psoc.batchesBad(), (unsigned long)psoc.pingsRx(),
                      (unsigned long)psoc.diagEventsRx(),
                      (unsigned long)psoc.i2cOverruns());
    } else if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
        Serial.println(F("[ST] run|test  corrida completa"));
        Serial.println(F("[ST] a b c d   corre solo ese grupo"));
        Serial.println(F("[ST] tap       golpe al geofono: pico + POLARIDAD"));
        Serial.println(F("[ST] e|inter   fase interactiva (botones + golpe)"));
        Serial.println(F("[ST] probe     estado del enlace con el PSoC"));
    } else if (cmd[0] != '\0') {
        Serial.printf("[ST] comando desconocido '%s' (help)\n", cmd);
    }
}

static void serviceUsb()
{
    static char line[32];
    static uint8_t pos = 0;
    while (Serial.available() > 0) {
        char ch = (char)Serial.read();
        if (ch == '\r' || ch == '\n') {
            line[pos] = '\0';
            handleCmd(line);
            pos = 0;
        } else if (ch == 8 || ch == 127) {
            if (pos > 0) { pos--; }
        } else if (ch >= 32 && ch <= 126) {
            if (pos < sizeof(line) - 1) { line[pos++] = ch; }
        }
    }
}

/* ── setup / loop ────────────────────────────────────────────────────────── */
void setup()
{
    Serial.begin(115200);
    delay(300);

    pinMode(SYNC_TO_PSOC_PIN, OUTPUT);
    digitalWrite(SYNC_TO_PSOC_PIN, LOW);

    localUiBegin();

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);
    WiFi.disconnect();
    delay(100);

    transport.begin(g_masterMac);

    psoc.onDiag(onDiag);
    psoc.onSelfTest(onSelfTest);
    psoc.begin(onBatch);

    /* El PSoC tarda en arrancar y este firmware lo primero que hace es
     * medir: darle tiempo evita reportar un falso "sin enlace". */
    stPump(2000);
    (void)psoc.probe(600);

    runAutotest();

    Serial.println(F("[ST] Fase automatica terminada."));
    Serial.println(F("[ST] Falta la fase INTERACTIVA, que ningun test automatico"));
    Serial.println(F("[ST] puede cubrir: escribi 'e' para botones + golpe al"));
    Serial.println(F("[ST] geofono (da la POLARIDAD), o 'tap' para solo el golpe."));
    Serial.println(F("[ST] 'run' repite lo automatico, 'help' lista todo."));
}

void loop()
{
    psoc.poll();
    serviceUsb();
    delay(1);
}
