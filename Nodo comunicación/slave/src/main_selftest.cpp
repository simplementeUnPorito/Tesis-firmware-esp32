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
#include <Preferences.h>

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

/* D8 (auto-calibracion) DESACTIVADO a pedido de Elias, 2026-09-01.
 *
 * La maquina de calibracion viene de cuando las referencias eran VDAC8; con el
 * cambio a IDAC8 su modelo de planta, sus targets y sus ganancias PI quedaron
 * sin portar. Correrla hoy no dice nada util sobre la placa: falla siempre, y
 * ademas se come minutos de cada corrida. Se deja el codigo intacto y se
 * saltea el test, para poder reactivarlo con una linea cuando este portada. */
#define ST_D8_HABILITADO 0

/* Flancos que manda el ESP para probar la linea de SYNC. */
static const int TH_SYNC_EDGES = 20;

/* Selectores de asentamiento y de largo de serie (indices de las tablas de
 * psoc_selftest.h del PSoC). */
/* Copia de g_st_settle_ms[] del PSoC (psoc_selftest.h). Si se toca alla, hay
 * que tocarla aca: se usa para calcular plazos de espera realistas. */
static const uint32_t ST_SETTLE_MS[8] = { 5u, 30u, 120u, 500u, 1200u, 1200u, 1200u, 1200u };

static const uint8_t SEL_SETTLE_FAST = 0;   /* 5 ms   */
static const uint8_t SEL_SETTLE_DC   = 3;   /* 500 ms */
static const uint8_t SEL_N_NOISE     = 0;   /* 2604 muestras = 1 s justo; con N = fs
                                             * el bin del Goertzel cae exacto (k = f0) y
                                             * la continua se rechaza sola. */

/* ── Perfil de hardware presente ───────────────────────────────────────────
 * La placa se arma por partes: puede faltar el OLED, los botones, la SD o el
 * geofono, y eso NO es una falla. Sin esta declaracion el autotest no puede
 * distinguir "no esta soldado" de "esta soldado y roto", que es justo lo que
 * hay que saber.
 *
 * Se guarda en NVS, asi que se declara una vez y sobrevive a los reflasheos.
 * Por defecto todo esta en AUTO, que nunca reprueba: informa como WARN y
 * explica las dos lecturas posibles.
 * ------------------------------------------------------------------------- */
#define HW_AUSENTE  0u
#define HW_PRESENTE 1u
#define HW_AUTO     2u

struct HwProfile {
    uint8_t oled;
    uint8_t btn;
    uint8_t geo;
    uint8_t sd;
    uint8_t psoc;
};

static HwProfile g_hw = { HW_AUTO, HW_AUTO, HW_AUTO, HW_AUTO, HW_AUTO };

static const char *hwName(uint8_t v)
{
    switch (v) {
        case HW_AUSENTE:  return "ausente";
        case HW_PRESENTE: return "presente";
        default:          return "auto";
    }
}

/* Traduce "no respondio" al veredicto que corresponde segun lo declarado.
 * Es el corazon de "el test tiene que correr sin nada conectado": una parte
 * que no esta no puede reprobar la placa. */
static StVerdict verdictSinRespuesta(uint8_t decl)
{
    if (decl == HW_AUSENTE)  { return ST_V_SKIP; }   /* ya sabiamos que no esta */
    if (decl == HW_PRESENTE) { return ST_V_FAIL; }   /* dijiste que esta y no contesta */
    return ST_V_WARN;                                /* auto: puede no estar soldado */
}

static const char *sufijoSinRespuesta(uint8_t decl)
{
    if (decl == HW_AUSENTE)  { return "declarado ausente"; }
    if (decl == HW_PRESENTE) { return "declarado PRESENTE: revisar soldadura"; }
    return "en auto: si no lo soldaste es esperable";
}

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
static int64_t           g_anSumSq = 0;
static int32_t           g_anMin = 0, g_anMax = 0;
static uint32_t          g_anMono = 0;       /* muestras crecientes seguidas */
static int32_t           g_anPrev = 0;
static int32_t           g_anBaseline = 0;   /* media de las primeras muestras */
static bool              g_anBaselineSet = false;
static int32_t           g_anPeakDev = 0;    /* mayor |x - baseline| */
static int32_t           g_anFirstSign = 0;  /* signo de la 1a excursion grande */
static int32_t           g_anTrig = 0;       /* umbral de excursion, en counts */

/* RMS de AC (media ya restada). Se calcula al final, no en el lazo. */
static double anRms()
{
    if (g_anSamples == 0) { return 0.0; }
    double ex2 = (double)g_anSumSq / (double)g_anSamples;
    double ex  = (double)g_anSum   / (double)g_anSamples;
    double var = ex2 - ex * ex;
    return (var > 0.0) ? sqrt(var) : 0.0;
}

static void anReset(int32_t trigger)
{
    g_anSamples = 0; g_anSum = 0; g_anSumSq = 0; g_anMin = 0; g_anMax = 0; g_anMono = 0;
    g_anPrev = 0; g_anBaseline = 0; g_anBaselineSet = false;
    g_anPeakDev = 0; g_anFirstSign = 0; g_anTrig = trigger;
}

/* Datos del PSoC descubiertos en C1, que despues usan los tests analogicos. */
static uint8_t g_psocStages = 0;
static uint8_t g_amuxChannels = 0;
static uint8_t g_hwClass = 0xFF;

/* Resultado de B3. Si la linea de SYNC no anda, NINGUNA captura puede
 * arrancar, y peor: dejar al PSoC armado lo vuelve sordo. */
static bool g_syncOk = false;

static Preferences g_prefs;

static void hwLoad()
{
    /* Read-WRITE a proposito. En modo read-only, sobre un namespace que nunca
     * existio, el comportamiento de Preferences cambia entre versiones de
     * arduino-esp32: algunas devuelven false y otras dejan estados raros. Y el
     * namespace virgen es EXACTAMENTE el primer arranque de una placa nueva,
     * que es el caso que hay que sostener. En read-write se crea si no esta. */
    if (!g_prefs.begin("sthw", false)) { return; }   /* sin NVS: quedan en AUTO */
    g_hw.oled = g_prefs.getUChar("oled", HW_AUTO);
    g_hw.btn  = g_prefs.getUChar("btn",  HW_AUTO);
    g_hw.geo  = g_prefs.getUChar("geo",  HW_AUTO);
    g_hw.sd   = g_prefs.getUChar("sd",   HW_AUTO);
    g_hw.psoc = g_prefs.getUChar("psoc", HW_AUTO);
    g_prefs.end();
}

static void hwSave()
{
    if (!g_prefs.begin("sthw", false)) {
        Serial.println(F("[ST] no se pudo abrir NVS: el perfil no queda guardado"));
        return;
    }
    g_prefs.putUChar("oled", g_hw.oled);
    g_prefs.putUChar("btn",  g_hw.btn);
    g_prefs.putUChar("geo",  g_hw.geo);
    g_prefs.putUChar("sd",   g_hw.sd);
    g_prefs.putUChar("psoc", g_hw.psoc);
    g_prefs.end();
}

static void hwPrint()
{
    Serial.println(F("[ST] Perfil de hardware (0=ausente 1=presente 2=auto):"));
    Serial.printf("[ST]   oled = %u (%s)\n", g_hw.oled, hwName(g_hw.oled));
    Serial.printf("[ST]   btn  = %u (%s)   los 4 pulsadores del ESP\n", g_hw.btn, hwName(g_hw.btn));
    Serial.printf("[ST]   geo  = %u (%s)\n", g_hw.geo, hwName(g_hw.geo));
    Serial.printf("[ST]   sd   = %u (%s)\n", g_hw.sd, hwName(g_hw.sd));
    Serial.printf("[ST]   psoc = %u (%s)\n", g_hw.psoc, hwName(g_hw.psoc));
    Serial.println(F("[ST] Cambiar con: hw <parte> <0|1|2>   (queda guardado en NVS)"));
}

/* Resultado de la medicion electrica del bus I2C, hecha en setup() ANTES de
 * que Wire tome los pines. Ver medirBusI2C(). */
static int  g_busSdaAlta = -1;   /* -1 = no medido */
static int  g_busSclAlta = -1;
static int  g_busSdaN = 0, g_busSclN = 0;
static uint32_t g_busSclFlancos = 0;
static int  g_busSdaPu = 0, g_busSclPu = 0;  /* con pull-up interno */
static int  g_busSdaPd = 0, g_busSclPd = 0;  /* con pull-DOWN interno */

/* Contador de flancos en SCL para el test B0b. Va por interrupcion porque una
 * trama de 4 bytes a la velocidad del bus dura decenas de microsegundos: un
 * muestreo por software no la vería nunca. */
static volatile uint32_t g_sclEdges = 0;
static volatile uint32_t g_txEdges  = 0;   /* flancos en la UART hacia el PSoC */

static void IRAM_ATTR onSclEdge()
{
    g_sclEdges++;
}

static void IRAM_ATTR onTxEdge()
{
    g_txEdges++;
}

/* ── Callbacks ───────────────────────────────────────────────────────────── */
static void onSelfTest(const PsocSelfTestResult &r)
{
    if (g_stRxN < ST_RX_MAX) { g_stRx[g_stRxN++] = r; }
}

/* Nombre corto de los eventos que importan para el autotest. El resto sale en
 * hexadecimal: alcanza para seguir la pista. */
static const char *diagNombre(uint8_t ev)
{
    switch (ev) {
        case 0x01: return "BOOT";
        case 0x02: return "ANALOG_READY";
        case 0x10: return "CAL_START";
        case 0x11: return "CAL_DONE";
        case 0x12: return "CAL_BUSY";
        case 0x20: return "WAIT_ESP";
        case 0x21: return "ESP_SEEN";
        case 0x30: return "RX_CMD";
        case 0x31: return "SETN";
        case 0x32: return "ARMED";
        case 0x33: return "SYNC_RISE";
        case 0x34: return "SYNC_FALL";
        case 0x35: return "SAMPLING_START";
        case 0x36: return "CAPTURE_DONE";
        case 0x37: return "DUMP_START";
        case 0x38: return "DUMP_DONE";
        case 0x39: return "START_NOW";
        case 0x3A: return "DEBUG_MODE";
        case 0x3B: return "STATUS_REQ";
        case 0x3E: return "CAPTURE_CLAMPED";
        case 0x45: return "CAPTURE_WATCHDOG";
        case 0x46: return "TIMER_STORM";
        case 0x47: return "CHAIN_NEXT";
        case 0x48: return "SD_STATUS";
        case 0x49: return "SD_SESSION";
        case 0x4A: return "SD_ERROR";
        case 0x7E: return "IRQ_INESPERADA";
        case 0x7F: return "HARDFAULT";
        default:   return nullptr;
    }
}

/* La consola normal es para el operador. Los eventos internos ensuciaban las
 * instrucciones interactivas; siguen contandose y pueden verse con `diag on`. */
static bool g_diagEcho = false;   /* comando USB: 'diag on|off' */

static void onDiag(const PsocDiagEvent &e)
{
    g_diagCount++;
    if (g_diagEcho) {
        const char *n = diagNombre(e.event);
        if (n != nullptr) {
            Serial.printf("    <psoc %s val=%u estado=%u>\n", n,
                          (unsigned)e.value, (unsigned)e.psoc_state);
        } else {
            Serial.printf("    <psoc 0x%02X val=%u estado=%u>\n",
                          (unsigned)e.event, (unsigned)e.value,
                          (unsigned)e.psoc_state);
        }
    }
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
        g_anSumSq += (int64_t)x * (int64_t)x;
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

/* Busca un resultado por id EXIGIENDO status ST_OK.
 *
 * El PSoC contesta la misma trama con ST_ERR o ST_REJECTED cuando no pudo
 * hacer la medicion. Consumir v0/v1 de esas tramas equivale a creerle un
 * numero que el propio PSoC declaro invalido, y eso convierte una falla en un
 * PASS. Para inspeccionar una trama sin importar el status esta stFindAny(). */
static const PsocSelfTestResult *stFindAny(uint8_t id)
{
    for (uint8_t i = 0; i < g_stRxN; i++) {
        if (g_stRx[i].test_id == id) { return &g_stRx[i]; }
    }
    return nullptr;
}

static const PsocSelfTestResult *stFind(uint8_t id)
{
    const PsocSelfTestResult *r = stFindAny(id);
    return (r != nullptr && r->status == ST_OK) ? r : nullptr;
}

/* Una medicion DC de un tap, con reintento. El reintento acotado de 2
 * intentos es el mismo patron que usan todos los runners del banco: hay una
 * clase de transitorio (F6) que hace que el primer comando despues de un
 * cambio de configuracion se ignore en silencio. */
static bool stMeasDc(uint8_t ch, uint8_t settleSel, int32_t &meanUv, int32_t &ppUv)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt > 0) {
            /* Drenar antes de reintentar: la respuesta del intento vencido
             * puede seguir en vuelo y colarse como respuesta del nuevo. Con
             * canales distintos no importa (stFind empareja por test_id), pero
             * D5 mide cuatro veces el MISMO canal, uno por config del ADC, y
             * ahi una medida vieja se atribuiria a la config siguiente. */
            stPump(400);
        }
        stRxClear();
        psoc.stMeasDc(settleSel, ch);
        /* El timeout tiene que cubrir el asentamiento pedido mas las 32
         * conversiones del promedio. */
        /* El plazo se calcula con el asentamiento REAL, no con el indice del
         * selector: con el indice, el selector 3 (500 ms) esperaba 13 s por
         * intento, y D1/D2/D4 hacen decenas de mediciones. Una placa con una
         * etapa muda tardaba minutos en decirlo. */
        if (stAwait(1, ST_SETTLE_MS[settleSel & 0x07u] + 3000u) >= 1) {
            const PsocSelfTestResult *r = stFind((uint8_t)(ST_ID_DC_BASE | ch));
            if (r && r->status == ST_OK) { meanUv = r->v0; ppUv = r->v1; return true; }
        }
    }
    return false;
}

static bool stSetIdac(uint8_t stage, uint8_t code)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt > 0) { stPump(300); }
        stRxClear();
        psoc.stSetIdac(stage, code);
        if (stAwait(1, 1500) >= 1) {
            const PsocSelfTestResult *r = stFind((uint8_t)(ST_ID_IDAC_BASE | stage));
            if (r && r->status == ST_OK) { return true; }
        }
    }
    return false;
}

/* Espera a que el PSoC vuelva a IDLE.
 *
 * El PSoC corre auto-calibracion al arrancar y al cambiar de configuracion, y
 * mientras esta en PSOC_CALIBRATING rechaza las primitivas del autotest. Eso
 * dura decenas de segundos: si el autotest arranca antes, todo el grupo C y D
 * falla por un motivo que no tiene nada que ver con la placa. Es la trampa F6
 * que ya esta documentada en el banco ("tras ToggleReset esperar la auto-cal").
 *
 * Se sondea con un reporte inocuo: si vuelve ST_OK, esta en IDLE; si vuelve
 * ST_REJECTED, v1 trae el estado en el que esta. */
static bool stEsperarIdle(uint32_t timeoutMs)
{
    uint32_t t0 = millis();
    bool aviso = false;
    while ((millis() - t0) < timeoutMs) {
        stRxClear();
        psoc.stReport(ST_REP_ADCCFG);
        if (stAwait(1, 1500) >= 1) {
            const PsocSelfTestResult *r = stFindAny(ST_ID_ADCCFG);
            if (r != nullptr && r->status == ST_OK) {
                if (aviso) {
                    Serial.printf("    PSoC en IDLE despues de %lu s.\n",
                                  (unsigned long)((millis() - t0) / 1000u));
                }
                return true;
            }
            const PsocSelfTestResult *rj = stFindAny(ST_ID_IDENTITY);
            if (!aviso) {
                aviso = true;
                Serial.printf("    Esperando a que el PSoC salga de su estado %ld "
                              "(auto-calibracion al boot), hasta %lu s...\n",
                              rj ? (long)rj->v1 : -1L,
                              (unsigned long)(timeoutMs / 1000u));
            }
        }
        stPump(1000);
    }
    return false;
}

static bool isSaturated(int32_t uv) { return (uv >= TH_RAIL_UV) || (uv <= -TH_RAIL_UV); }

/* Cambia el rango del ADC y CONFIRMA con el ACK del PSoC.
 *
 * Sin confirmar, si los cuatro comandos se ignoraran (clase de transitorio F6,
 * o un PSoC ocupado) D5 mediria cuatro veces la MISMA configuracion, obtendria
 * dispersion cero y daria el PASS mas perfecto posible sin haber probado nada. */
static bool stSetAdcConfig(uint8_t cfg)
{
    for (int intento = 0; intento < 2; intento++) {
        uint8_t c, v;
        while (psoc.takeConfigAck(c, v)) { }   /* descartar acks viejos */
        psoc.setAdcConfig(cfg);
        uint32_t t0 = millis();
        while ((millis() - t0) < 1200) {
            psoc.poll();
            if (psoc.takeConfigAck(c, v)) {
                if (c == PSOC_CMD_ADC_CONFIG && v == cfg) { return true; }
            }
            delay(1);
        }
    }
    return false;
}

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

    /* A2 — OLED.
     *
     * IMPORTANTE: el SSD1306 por SPI **no se puede detectar**. El bus es de
     * ida solamente y la libreria devuelve false unicamente si le falla el
     * malloc del framebuffer, asi que localUiReady() da true con el OLED
     * soldado, sin soldar o quemado. Dar PASS con eso seria mentir.
     *
     * Lo unico honesto: decir que se dibujo el patron y que la confirmacion es
     * visual. Si el OLED esta declarado ausente, ni se intenta. */
    if (g_hw.oled == HW_AUSENTE) {
        stReportItem("A2", "OLED SSD1306 por SPI", ST_V_SKIP,
                     "declarado ausente (hw oled 0)");
    } else if (!localUiReady()) {
        /* Esto solo pasa si fallo el framebuffer: es un problema de memoria
         * del ESP, no del OLED. */
        stReportItem("A2", "OLED SSD1306 por SPI", ST_V_FAIL,
                     "fallo el framebuffer del driver: problema de RAM del ESP32");
    } else {
        stReportItem("A2", "OLED SSD1306 por SPI", ST_V_INFO,
                     "patron dibujado. El SPI no tiene lectura: confirmalo con la "
                     "vista y responde 'oled si' u 'oled no'");
    }

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
    if (g_hw.btn == HW_AUSENTE) {
        stReportItem("A3", "Pull-ups de botones", ST_V_SKIP,
                     "declarado ausente (hw btn 0)");
    } else if (nBad == 0) {
        /* Ojo: esto NO prueba que los pull-ups esten. Un pin de solo entrada
         * flotante puede sostener alto por carga residual. Prueba que ninguno
         * este pegado en bajo, que es la falla que si se detecta. */
        stReportItem("A3", "Pull-ups de botones", ST_V_PASS,
                     "4/4 en alto estable (no descarta pull-up ausente: ver con tester)");
    } else {
        /* Pegado en bajo si es concluyente: o falta el pull-up de 10 k, o el
         * pulsador esta en corto, o el pin esta puenteado a masa. */
        stReportItem("A3", "Pull-ups de botones",
                     (g_hw.btn == HW_AUTO) ? ST_V_WARN : ST_V_FAIL,
                     "%s en bajo -> falta R 10k a 3V3, pulsador en corto, o sin soldar (%s)",
                     btnBad, sufijoSinRespuesta(g_hw.btn));
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
    /* Asincrono con espera acotada: la version sincrona bloquea hasta que la
     * IDF decide terminar, y con una radio en mal estado eso parece un
     * cuelgue. Asi el autotest siempre sigue. */
    WiFi.scanNetworks(true, true);
    uint32_t tscan = millis();
    int n = WIFI_SCAN_RUNNING;
    while ((millis() - tscan) < 6000) {
        n = WiFi.scanComplete();
        if (n >= 0 || n == WIFI_SCAN_FAILED) { break; }
        delay(50);
    }
    if (n < 0) { WiFi.scanDelete(); n = 0; }
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
    /* B0 y B0b: se midieron en setup(), antes de levantar Wire. Ver
     * medirBusI2C() para el por que del orden. */
    if (g_busSdaAlta < 0) {
        stReportItem("B0", "Lineas del bus I2C en reposo", ST_V_SKIP,
                     "no se midio");
    } else if (g_busSdaAlta && g_busSclAlta) {
        stReportItem("B0", "Lineas del bus I2C en reposo", ST_V_PASS,
                     "SDA(%u) y SCL(%u) en alto: los pull-ups estan",
                     (unsigned)PSOC_I2C_SDA, (unsigned)PSOC_I2C_SCL);
    } else if (!g_busSdaAlta && !g_busSclAlta) {
        stReportItem("B0", "Lineas del bus I2C en reposo", ST_V_FAIL,
                     "SDA(%u) y SCL(%u) las DOS en bajo: faltan los pull-ups a 3V3, "
                     "o el otro extremo no tiene alimentacion",
                     (unsigned)PSOC_I2C_SDA, (unsigned)PSOC_I2C_SCL);
    } else {
        const char *cual = g_busSdaAlta ? "SCL" : "SDA";
        int puDeLaMala  = g_busSdaAlta ? g_busSclPu : g_busSdaPu;
        if (puDeLaMala >= 27) {
            stReportItem("B0", "Lineas del bus I2C en reposo", ST_V_FAIL,
                         "%s en bajo pero SUBE con el pull-up interno: falta el "
                         "pull-up EXTERNO de %s a 3V3 (SDA %d/50, SCL %d/50)",
                         cual, cual, g_busSdaN, g_busSclN);
        } else {
            stReportItem("B0", "Lineas del bus I2C en reposo", ST_V_FAIL,
                         "%s RETENIDA en bajo: no sube ni con el pull-up interno. "
                         "Corto a masa, o un dispositivo trabado sujetando la linea "
                         "(SDA %d/50, SCL %d/50)",
                         cual, g_busSdaN, g_busSclN);
        }
    }

    stReportItem("B0c", "Pull-ups externos, medidos", ST_V_INFO,
                 "SDA flotante %d/50 pu %d/30 pd %d/30 | SCL flotante %d/50 pu %d/30 pd %d/30",
                 g_busSdaN, g_busSdaPu, g_busSdaPd,
                 g_busSclN, g_busSclPu, g_busSclPd);

    /* El pull-down interno es la medicion que decide: con un pull-up externo de
     * 10 k, el divisor contra los 45 k internos deja la linea en 2,7 V (alto).
     * Sin pull-up externo, el pull-down gana. */
    if (g_busSdaPd >= 27 && g_busSclPd < 3) {
        stReportItem("B0d", "Pull-up externo de SCL", ST_V_FAIL,
                     "SDA aguanta el pull-down interno y SCL no: el PIN GPIO%u del ESP "
                     "NO esta viendo un pull-up externo. Si la resistencia esta puesta, "
                     "revisar continuidad DESDE EL PIN (soldadura, pista, o que el otro "
                     "extremo llegue a 3V3)", (unsigned)PSOC_I2C_SCL);
    } else if (g_busSclPd >= 27) {
        stReportItem("B0d", "Pull-up externo de SCL", ST_V_PASS,
                     "SCL aguanta el pull-down interno: el pull-up externo llega al pin");
    }

    /* B1 — subida por I2C. El PSoC es maestro y manda pings solo; si no
     * llega nada, o no hay pull-ups en SDA/SCL, o el PSoC no arranco. */
    /* El PSoC se queda en wait_for_esp() hasta ver un PONG o un STATUS. Si se
     * reinicio despues del ultimo sondeo del ESP (por ejemplo con ToggleReset),
     * nadie se lo vuelve a mandar y queda mudo para siempre. Se lo sondea de
     * nuevo antes de declarar el enlace muerto. */
    for (int k = 0; k < 3; k++) {
        psoc.sendPong();
        psoc.requestStatus();
        stPump(250);
    }

    uint32_t p0 = psoc.pingsRx(), d0 = psoc.diagEventsRx(), b0 = psoc.bytesRx();
    stPump(1500);
    uint32_t dPing = psoc.pingsRx() - p0;
    uint32_t dDiag = psoc.diagEventsRx() - d0;
    uint32_t dBytes = psoc.bytesRx() - b0;
    bool up = (dBytes > 0);
    if (up) {
        stReportItem("B1", "Subida I2C PSoC->ESP (0x42)",
                     (psoc.i2cOverruns() == 0) ? ST_V_PASS : ST_V_WARN,
                     "%lu B/1.5s, %lu pings, %lu diag, overruns=%lu",
                     (unsigned long)dBytes, (unsigned long)dPing,
                     (unsigned long)dDiag, (unsigned long)psoc.i2cOverruns());
    } else {
        /* Silencio total. Las causas posibles, de mas a menos probable:
         * PSoC sin programar o sin alimentar, faltan los pull-ups de SDA/SCL,
         * GND no comun, o SDA/SCL cruzados. El ESP es ESCLAVO I2C: no puede
         * sondear el bus, solo esperar. */
        stReportItem("B1", "Subida I2C PSoC->ESP (0x42)",
                     verdictSinRespuesta(g_hw.psoc),
                     "silencio total en 1.5 s. Revisar, en orden: PSoC programado y "
                     "alimentado / pull-ups de SDA(21) y SCL(22) a 3V3 / GND comun / "
                      "SDA-SCL cruzados. %s", sufijoSinRespuesta(g_hw.psoc));
    }

    /* La ventana de B0b ocurre una sola vez, antes de iniciar Wire. Si el PSoC
     * fue programado o reseteado despues del ESP, cero flancos en esa ventana
     * es historico y B1 es la medicion actual que manda. */
    if (g_busSclFlancos > 0u) {
        stReportItem("B0b", "Reloj del bus (SCL)", ST_V_PASS,
                     "%lu flancos observados al arranque",
                     (unsigned long)g_busSclFlancos);
    } else if (up) {
        stReportItem("B0b", "Reloj del bus (SCL)", ST_V_PASS,
                     "sin flancos en la ventana de boot, pero B1 confirma trafico I2C actual");
    } else {
        stReportItem("B0b", "Reloj del bus (SCL)", ST_V_FAIL,
                     "sin flancos al arranque ni trafico actual: el PSoC no transmite");
    }

    if (!up) {
        /* Sin enlace todo lo demas daria FAIL en cascada y taparia la causa
         * real, asi que se marca SKIP con el motivo explicito. El grupo A ya
         * corrio: si A pasa y B no, el problema esta del lado del PSoC o del
         * cableado entre los dos, no en el ESP. */
        stReportItem("B2", "Bajada UART ESP->PSoC", ST_V_SKIP,
                     "sin enlace de subida: no se puede confirmar la ida");
        stReportItem("B3", "Linea SYNC GPIO27 -> P0[4]", ST_V_SKIP,
                     "sin enlace de subida: el PSoC no puede reportar los flancos");
        return false;
    }

    /* B2a — el ESP, esta transmitiendo?
     *
     * Antes de acusar al cable hay que descartar el propio lado. Se cuentan
     * flancos en el pin de TX mientras se mandan comandos: si no hay flancos,
     * el problema es la UART del ESP y no la placa. Si hay flancos pero el PSoC
     * no reacciona (B2), entonces si esta entre el pin del ESP y el del PSoC.
     *
     * La interrupcion se puede enganchar a un pin que maneja un periferico: la
     * matriz de GPIO del ESP32 deja leerlo igual mientras la UART lo maneja. */
    {
        g_txEdges = 0;
        attachInterrupt(digitalPinToInterrupt(PSOC_UART_TX), onTxEdge, CHANGE);
        for (int k = 0; k < 5; k++) { psoc.requestStatus(); delay(40); }
        delay(50);
        detachInterrupt(digitalPinToInterrupt(PSOC_UART_TX));
        if (g_txEdges > 0u) {
            stReportItem("B2a", "El ESP transmite por la UART", ST_V_PASS,
                         "%lu flancos en GPIO%u al mandar 5 comandos",
                         (unsigned long)g_txEdges, (unsigned)PSOC_UART_TX);
        } else {
            stReportItem("B2a", "El ESP transmite por la UART", ST_V_FAIL,
                         "0 flancos en GPIO%u: la UART del ESP no esta saliendo. "
                         "El problema es del lado del ESP, no del cable",
                         (unsigned)PSOC_UART_TX);
        }
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

    /* B3 — linea de SYNC.
     *
     * El camino de captura actual sincroniza SYNC_IN en hardware y lo entrega
     * a superMaquina; ya no existe el viejo isr_SyncIn que incrementaba el
     * contador de psoc_selftest.h. Por eso una cuenta de cero NO demuestra una
     * falla del cable. La prueba robusta es electrica y directa: pedirle al
     * PSoC que lea P0[4] una vez con GPIO27 bajo y otra mientras sigue alto.
     * La cuenta se conserva solo como dato diagnostico para variantes viejas. */
    pinMode(SYNC_TO_PSOC_PIN, OUTPUT);
    digitalWrite(SYNC_TO_PSOC_PIN, LOW);
    delay(20);
    stRxClear();
    psoc.stSync(1);
    bool armed = (stAwait(1, 1200) >= 1);
    const PsocSelfTestResult *armResult = armed ? stFind(ST_ID_SYNC) : nullptr;
    bool sawLow = armResult && armResult->status == ST_OK && armResult->v1 == 0;
    if (!armed) {
        stReportItem("B3", "Linea SYNC GPIO27 -> P0[4]", ST_V_FAIL,
                     "el PSoC no contesto la lectura con GPIO27 en LOW");
    } else {
        for (int i = 0; i < TH_SYNC_EDGES / 2; i++) {
            digitalWrite(SYNC_TO_PSOC_PIN, HIGH); delay(6);
            digitalWrite(SYNC_TO_PSOC_PIN, LOW);  delay(6);
        }
        /* Dejar la linea alta durante la consulta: st_handle_sync(0) devuelve
         * en v1 el nivel fisico leido por SYNC_IN_Read(). */
        digitalWrite(SYNC_TO_PSOC_PIN, HIGH);
        delay(20);
        stRxClear();
        psoc.stSync(0);
        if (stAwait(1, 1200) >= 1) {
            const PsocSelfTestResult *r = stFind(ST_ID_SYNC);
            int got = r ? (int)r->v0 : -1;
            bool sawHigh = r && r->status == ST_OK && r->v1 != 0;
            digitalWrite(SYNC_TO_PSOC_PIN, LOW);
            g_syncOk = sawLow && sawHigh;
            if (g_syncOk) {
                stReportItem("B3", "Linea SYNC GPIO27 -> P0[4]", ST_V_PASS,
                             "P0[4] leyo LOW y HIGH correctamente (contador legacy=%d)",
                             got);
            } else {
                stReportItem("B3", "Linea SYNC GPIO27 -> P0[4]", ST_V_FAIL,
                             "P0[4] no sigue a GPIO27: LOW=%d HIGH=%d (contador legacy=%d)",
                             sawLow ? 1 : 0, sawHigh ? 1 : 0, got);
            }
        } else {
            digitalWrite(SYNC_TO_PSOC_PIN, LOW);
            stReportItem("B3", "Linea SYNC GPIO27 -> P0[4]", ST_V_FAIL,
                         "el PSoC no contesto la lectura con GPIO27 en HIGH");
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
    /* Sin linea de SYNC no se puede arrancar una captura, y ADEMAS armar el
     * PSoC lo deja MUDO: en PSOC_ARMED su service_runtime() corta UART, I2C y
     * pings a proposito (ventana critica), y solo sale con un flanco de bajada
     * de SYNC o un reset. Un solo cable roto encadenaba doce FAIL que no tenian
     * nada que ver entre si. */
    if (!g_syncOk) { return false; }

    /* Drenar lo que quedo de la captura anterior ANTES de poner el analizador
     * a cero: C4 y C5 corren pegadas y D7 hace dos capturas seguidas, asi que
     * un volcado que todavia esta llegando meteria sus muestras en el
     * acumulador de la nueva y el veredicto saldria de datos mezclados. */
    stPump(300);

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
    uint32_t plazo = nominal + 2000UL + (uint32_t)n * 4UL;
    uint32_t t1 = millis();
    while ((g_batchCount < n) && ((millis() - t1) < plazo)) {
        psoc.poll();
        delay(1);
    }
    digitalWrite(SYNC_TO_PSOC_PIN, LOW);
    stPump(400);

    /* Llegar a ARMED no es haber capturado. Antes se devolvia true por el solo
     * hecho de armarse, asi que una captura que no entrego un solo lote se
     * analizaba igual: C5 la convertia en SKIP y D7 media un fondo inexistente. */
    return (g_batchCount > 0u);
}

/* La orden DEBUG del firmware PSoC arranca la rampa inmediatamente; no debe
 * combinarse con stCapture(), que arma otra captura por PRESTART+SYNC. Hacerlo
 * generaba primero un volcado de 128 lotes, contaminaba C4 y dejaba al PSoC
 * ocupado para C5/C6/C7. Esta variante fija N antes de habilitar DEBUG y espera
 * exclusivamente esa captura inmediata. */
static bool stCaptureDebug(uint16_t n, int32_t trigger)
{
    stPump(300);
    g_batchCount = 0;
    g_firstBatchSeen = false;
    anReset(trigger);

    psoc.selectStream(0);
    stPump(150);
    psoc.setN(n);
    stPump(150);
    psoc.debugRamp(true);

    uint32_t nominal = ((uint32_t)n * 30UL * 1000UL) / 2604UL;
    uint32_t plazo = nominal + 2000UL + (uint32_t)n * 4UL;
    uint32_t t0 = millis();
    while ((g_batchCount < n) && ((millis() - t0) < plazo)) {
        psoc.poll();
        delay(1);
    }

    psoc.debugRamp(false);
    stPump(500);
    return (g_batchCount > 0u);
}

/* Diagnostico compacto que devuelve el driver SD del PSoC. Cada flag indica
 * que el nivel fisico fue observado al menos una vez mientras el SPI estaba
 * transfiriendo; ver ambos niveles confirma conmutacion en el propio pad. */
static const char *sdNiveles(uint8_t flags, uint8_t lowMask, uint8_t highMask)
{
    uint8_t seen = flags & (uint8_t)(lowMask | highMask);
    if (seen == (uint8_t)(lowMask | highMask)) { return "0/1"; }
    if (seen == lowMask)  { return "solo0"; }
    if (seen == highMask) { return "solo1"; }
    return "ninguno";
}

static const char *sdEtapaInit(uint8_t stage)
{
    switch (stage & 0x7Fu) {
        case 1: return "relojes iniciales";
        case 2: return "CMD0";
        case 3: return "CMD8";
        case 4: return "ACMD41/CMD1";
        case 5: return "CMD58";
        case 6: return "CMD16";
        case 7: return "lectura CSD";
        case 8: return "capacidad CSD";
        case 9: return "lista";
        default: return "sin ejecutar";
    }
}

/* ==========================================================================
 * GRUPO C — infraestructura del PSoC.
 * ========================================================================== */
static bool groupC()
{
    /* C1 — identidad */
    stRxClear();
    psoc.stReport(ST_REP_IDENTITY);
    stAwait(2, 2000);
    const PsocSelfTestResult *id = stFind(ST_ID_IDENTITY);
    const PsocSelfTestResult *sg = stFind(ST_ID_STAGES);
    /* Validar ANTES de castear: v0/v1 son int32 que vienen del PSoC, que es
     * justamente el que puede estar roto. Un valor absurdo envolvia al pasar a
     * uint8 y despues se usaba como cantidad de etapas y de canales. */
    bool idOk = (id != nullptr) && (sg != nullptr) &&
                (id->v0 == 0 || id->v0 == 1) &&
                (sg->v0 >= 1 && sg->v0 <= 4) &&
                (sg->v1 >= 1 && sg->v1 <= 16) &&
                (sg->v1 >= sg->v0);
    if (idOk) {
        g_hwClass = (uint8_t)id->v0;
        g_psocStages = (uint8_t)sg->v0;
        g_amuxChannels = (uint8_t)sg->v1;
        stReportItem("C1", "Identidad del PSoC", ST_V_PASS,
                     "clase=%s, Fs=%ld Hz, %u etapas, %u canales AMux",
                     (g_hwClass == 0) ? "GEO" : "HAMMER", (long)id->v1,
                     (unsigned)g_psocStages, (unsigned)g_amuxChannels);
    } else if (id != nullptr && sg != nullptr) {
        stReportItem("C1", "Identidad del PSoC", ST_V_FAIL,
                     "valores fuera de rango: clase=%ld etapas=%ld canales=%ld",
                     (long)id->v0, (long)sg->v0, (long)sg->v1);
        return false;
    } else {
        /* El enlace esta vivo (B1 y B2 pasaron) pero el PSoC no contesta el
         * reporte de identidad. La causa abrumadoramente mas probable no es
         * una falla de placa: es que el PSoC tiene grabado el firmware de
         * CAMPO, que no conoce los comandos 0xA0..0xA7 y los descarta en
         * silencio. Seguir con C y D daria diez FAIL por timeout, varios
         * minutos de espera y un diagnostico equivocado. */
        stReportItem("C1", "Identidad del PSoC", ST_V_FAIL,
                     "el enlace anda pero no contesta 0xA0: el PSoC casi seguro "
                     "tiene el firmware de CAMPO, no el de autotest");
        return false;
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
    /* El PSoC re-habilita la captura a SD por su cuenta al arrancar si detecta
     * la tarjeta montada (default field-safe, hallazgo OK-4 del banco). En ese
     * modo una captura no manda lotes durante el muestreo y el volcado hay que
     * pedirlo lote por lote con 0xBF: el autotest esperaria de gusto. Se fuerza
     * modo RAM antes de cualquier captura. */
    if (!g_syncOk) {
        stReportItem("C4", "Camino digital E2E (rampa cruda)", ST_V_SKIP,
                     "sin linea de SYNC (ver B3) no se puede disparar una captura");
        stReportItem("C5", "Filtro FIR de hardware (DFB)", ST_V_SKIP,
                     "sin linea de SYNC (ver B3) no se puede disparar una captura");
    } else {

    psoc.sdCapture(0);
    stPump(600);

    if (!stCaptureDebug(8, 0)) {
        stReportItem("C4", "Camino digital E2E (rampa cruda)", ST_V_FAIL,
                     "la captura inmediata de rampa no entrego lotes");
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

    /* C5 — filtro FIR de hardware (DFB).
     *
     * NO se puede probar con la rampa de debug: en superMaquina la rampa es
     * una FUENTE distinta (CE_CFG_SRC_DEBUG) de la del filtro
     * (CE_CFG_SRC_FILTER), asi que pedir rampa y stream FIR a la vez no hace
     * pasar la rampa por el DFB. Un test armado asi daria un veredicto que no
     * significa nada.
     *
     * Se prueba contra lo que el FIR tiene que hacer de verdad: es un pasa
     * bajos, asi que sobre el ruido de banda ancha de la entrada real el RMS
     * de la salida filtrada tiene que ser CLARAMENTE menor que el del camino
     * crudo. Si el DFB no arranca, o su DMA no entrega, o los coeficientes no
     * se cargaron, esa reduccion no aparece.
     *
     * 32 lotes (960 muestras) para que las 63 muestras de asentamiento del FIR
     * pesen poco, y RMS en vez de pico-pico porque un solo transitorio de
     * asentamiento domina el pico-pico. */
    double rmsRaw = 0.0, rmsFir = 0.0;
    bool capOk = true;

    psoc.selectStream(0);
    stPump(200);
    if (stCapture(32, 0)) { rmsRaw = anRms(); } else { capOk = false; }

    psoc.selectStream(1);
    stPump(200);
    if (stCapture(32, 0)) { rmsFir = anRms(); } else { capOk = false; }

    psoc.selectStream(0);
    stPump(150);

    if (!capOk) {
        stReportItem("C5", "Filtro FIR de hardware (DFB)", ST_V_FAIL,
                     "no se pudo capturar por alguno de los dos caminos");
    } else if (rmsRaw < 3.0) {
        /* Entrada demasiado quieta: sin ruido que filtrar la comparacion no
         * distingue un FIR sano de uno muerto. */
        stReportItem("C5", "Filtro FIR de hardware (DFB)", ST_V_SKIP,
                     "RMS crudo %.1f counts: muy poco ruido para evaluar el filtro",
                     rmsRaw);
    } else {
        double ratio = rmsFir / rmsRaw;
        /* Un pasa bajos sobre ruido de banda ancha tiene que bajar el RMS de
         * forma evidente. Se pide al menos un 20 % de reduccion, que es un
         * piso muy laxo a proposito: el objetivo es detectar un DFB que no
         * filtra nada (ratio ~1) o que no entrega (ratio ~0). */
        bool dead  = (rmsFir < 0.5);
        bool nofil = (ratio > 0.80);
        /* Tres llamadas y no un formato condicional: los formatos toman
         * distinta cantidad de argumentos y mezclarlos corre los varargs.
         * Ya paso una vez en B3. */
        if (dead) {
            stReportItem("C5", "Filtro FIR de hardware (DFB)", ST_V_FAIL,
                         "salida filtrada muerta: RMS %.1f contra crudo %.1f",
                         rmsFir, rmsRaw);
        } else if (nofil) {
            stReportItem("C5", "Filtro FIR de hardware (DFB)", ST_V_FAIL,
                         "no atenua: RMS FIR/crudo = %.2f (%.1f vs %.1f)",
                         ratio, rmsFir, rmsRaw);
        } else {
            stReportItem("C5", "Filtro FIR de hardware (DFB)", ST_V_PASS,
                         "RMS FIR/crudo = %.2f (%.1f vs %.1f)",
                         ratio, rmsFir, rmsRaw);
        }
    }
    }   /* fin del bloque que necesita la linea de SYNC */

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
    if (!sd) {
        stReportItem("C6", "SD FatFs (ruteo SPIp nuevo)", ST_V_FAIL,
                     "el PSoC no contesto el reporte de SD");
    } else {
        uint8_t s = (uint8_t)sd->v0;
        uint32_t diag = (uint32_t)sd->v1;
        uint8_t err   = (uint8_t)(diag & 0xFFu);
        uint8_t pads  = (uint8_t)((diag >> 8) & 0xFFu);
        uint8_t r1    = (uint8_t)((diag >> 16) & 0xFFu);
        uint8_t stage = (uint8_t)((diag >> 24) & 0xFFu);
        bool present  = (s & 0x01) != 0;
        bool selftest = (s & 0x08) != 0;
        bool fat      = (s & 0x10) != 0;

        stReportItem("C6.1", "Diagnostico de pads SPI SD", ST_V_INFO,
                     "init=%s%s R1=0x%02X; CS=%s SCK=%s MOSI=%s MISO=%s err=0x%02X",
                     sdEtapaInit(stage), (stage & 0x80u) ? "+RX_TIMEOUT" : "",
                     (unsigned)r1,
                     sdNiveles(pads, 0x40u, 0x80u),
                     sdNiveles(pads, 0x04u, 0x08u),
                     sdNiveles(pads, 0x10u, 0x20u),
                     sdNiveles(pads, 0x01u, 0x02u),
                     (unsigned)err);

        if (!present) {
            /* Sin tarjeta no se puede saber si el ruteo de SPIp esta bien.
             * Con la SD declarada presente eso si es una falla. */
            stReportItem("C6", "SD FatFs (ruteo SPIp nuevo)",
                         verdictSinRespuesta(g_hw.sd),
                         "init detenido en %s, R1=0x%02X (estado=0x%02X): %s",
                         sdEtapaInit(stage), (unsigned)r1, (unsigned)s,
                         sufijoSinRespuesta(g_hw.sd));
        } else if (!fat) {
            stReportItem("C6", "SD FatFs (ruteo SPIp nuevo)", ST_V_FAIL,
                         "tarjeta detectada pero FAT sin montar (0x%02X): "
                         "formatear en FAT32, o MISO/MOSI cruzados", (unsigned)s);
        } else if (!selftest) {
            stReportItem("C6", "SD FatFs (ruteo SPIp nuevo)", ST_V_FAIL,
                         "FAT montado pero escritura/lectura fallo (0x%02X, err=0x%02X): "
                         "ver diagnostico C6.1", (unsigned)s, (unsigned)err);
        } else {
            stReportItem("C6", "SD FatFs (ruteo SPIp nuevo)", ST_V_PASS,
                         "0x%02X tipo=%u FAT montado, escritura y lectura OK",
                         (unsigned)s, (unsigned)((s >> 1) & 0x03));
        }
    }

    /* C7 — pulsador del PSoC (P2[2]). En reposo tiene que leer un nivel
     * estable. Se muestrea varias veces porque un pulsador rebotando o un pin
     * flotante no sostienen el nivel. La pulsacion en si es interactiva:
     * comando USB `boton`. */
    {
        int lvl = -1; bool stable = true; int leidas = 0;
        for (int k = 0; k < 5; k++) {
            stRxClear();
            psoc.stReport(ST_REP_BUTTON);
            if (stAwait(1, 1200) >= 1) {
                const PsocSelfTestResult *r = stFind(ST_ID_BUTTON);
                if (r != nullptr && (r->v0 == 0 || r->v0 == 1)) {
                    leidas++;
                    if (lvl < 0) { lvl = (int)r->v0; }
                    else if ((int)r->v0 != lvl) { stable = false; }
                }
            }
            delay(40);
        }
        /* Se exigen las CINCO lecturas: antes bastaba una para declarar
         * "estable en 5 lecturas", que es directamente falso. */
        if (leidas < 5) {
            stReportItem("C7", "Pulsador del PSoC en reposo", ST_V_FAIL,
                         "solo %d de 5 lecturas validas", leidas);
        } else {
            stReportItem("C7", "Pulsador del PSoC en reposo",
                         stable ? ST_V_PASS : ST_V_FAIL,
                         stable ? "nivel %d estable en 5 lecturas"
                                : "nivel inestable: pulsador rebotando o pin flotante",
                         lvl);
        }
    }

    return true;
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
    bool okLo[4], okHi[4];

    if (!stSetIdac(stage, (uint8_t)lo)) { (void)stSetIdac(stage, base); return false; }
    for (uint8_t ch = 0; ch < nTaps; ch++) {
        okLo[ch] = stMeasDc(ch, SEL_SETTLE_DC, vlo[ch], pp);
        if (!okLo[ch]) { vlo[ch] = 0; }
    }
    if (!stSetIdac(stage, (uint8_t)hi)) { (void)stSetIdac(stage, base); return false; }
    for (uint8_t ch = 0; ch < nTaps; ch++) {
        okHi[ch] = stMeasDc(ch, SEL_SETTLE_DC, vhi[ch], pp);
        if (!okHi[ch]) { vhi[ch] = 0; }
    }
    (void)stSetIdac(stage, base);   /* siempre restaurar */

    /* Si algun tap saturo en cualquiera de los dos extremos, la pendiente
     * queda comprimida y no sirve. Se reintenta con la mitad del escalon. */
    for (uint8_t ch = 0; ch < nTaps; ch++) {
        if (!okLo[ch] || !okHi[ch]) { continue; }
        if (isSaturated(vlo[ch]) || isSaturated(vhi[ch])) {
            if (stepCodes / 2 >= TH_D2_MIN_STEP) {
                return d2SweepStage(stage, nTaps, stepCodes / 2);
            }
        }
    }

    for (uint8_t ch = 0; ch < nTaps; ch++) {
        /* La pendiente solo vale si midieron LOS DOS extremos. Antes se ponia
         * el extremo fallido en cero y despues se sobrescribia g_slopeOk=true:
         * un solo extremo perdido producia una pendiente enorme e inventada,
         * que hacia pasar una etapa muerta. */
        if (!okLo[ch] || !okHi[ch]) {
            g_slopeOk[stage][ch] = false;
            g_slope[stage][ch] = 0.0f;
            continue;
        }
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
        uint8_t medidos = 0;
        for (uint8_t ch = 0; ch < nTaps; ch++) {
            int32_t m, pp;
            if (stMeasDc(ch, SEL_SETTLE_DC, m, pp)) {
                medidos++;
                char one[24];
                snprintf(one, sizeof(one), "%sch%u=%ldmV", ch ? " " : "",
                         (unsigned)ch, (long)(m / 1000));
                strncat(det, one, sizeof(det) - strlen(det) - 1);
                if (isSaturated(m)) { railed = true; }
            }
        }
        /* Sin mediciones no hay veredicto. Antes, cero respuestas dejaban
         * railed=false y el item daba PASS: el aspecto de una placa perfecta
         * sobre una que no contesta una sola medida. */
        if (medidos < nTaps) {
            stReportItem("D1", "Reposo de los taps analogicos", ST_V_FAIL,
                         "solo %u de %u taps respondieron. %s",
                         (unsigned)medidos, (unsigned)nTaps, det);
        } else {
            stReportItem("D1", "Reposo de los taps analogicos",
                         railed ? ST_V_FAIL : ST_V_PASS,
                         railed ? "%s  <- algun tap contra el riel" : "%s", det);
        }

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
        Serial.printf("    D2: barriendo etapa %u de %u...\n",
                      (unsigned)(k + 1u), (unsigned)nTaps);
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
        if (worst[0] != '\0') {
            stReportItem("D2", "Matriz DC IDAC->etapa", v,
                         "%d/%u barridos, %d etapa(s) sin respuesta, %d fuga(s): %s",
                         sweeps, (unsigned)nTaps, badFwd, badIso, worst);
        } else {
            stReportItem("D2", "Matriz DC IDAC->etapa", v,
                         "%d/%u barridos, triangular superior OK, aislamiento < %.0f%%",
                         sweeps, (unsigned)nTaps,
                         (double)(TH_D2_ISOLATION_RATIO * 100.0f));
        }

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

    /* D6b — indicador de geofono conectado.
     *
     * El geofono no se puede detectar con un test digital, pero SI se puede
     * medir su efecto: la bobina son unos 375 ohm en paralelo con la red de
     * polarizacion de 50 k, asi que cargar la entrada baja de forma marcada el
     * ruido del primer tap. Con la entrada abierta ese mismo tap levanta mucho
     * mas ruido y mas captacion de red.
     *
     * No se inventa un umbral: se entrega el numero. Corriendo el autotest una
     * vez con el geofono y otra sin el, la diferencia queda a la vista, y ese
     * si es un discriminador confiable para ESTA placa. */
    {
        stRxClear();
        psoc.stMeasAc(SEL_N_NOISE, 0u);
        if (stAwait(2, 12000) >= 2) {
            const PsocSelfTestResult *ga = stFind((uint8_t)(ST_ID_ACA_BASE | 0u));
            const PsocSelfTestResult *gb = stFind((uint8_t)(ST_ID_ACB_BASE | 0u));
            if (ga != nullptr && gb != nullptr) {
                stReportItem("D6b", "Carga de la entrada (indicador de geofono)", ST_V_INFO,
                             "ch0: RMS %ld uV, 50 Hz %ld uV. Con geofono baja; con la "
                             "entrada abierta sube. Comparar las dos corridas",
                             (long)ga->v1, (long)gb->v1);
            } else {
                stReportItem("D6b", "Carga de la entrada (indicador de geofono)", ST_V_SKIP,
                             "el PSoC no devolvio las dos tramas");
            }
        } else {
            stReportItem("D6b", "Carga de la entrada (indicador de geofono)", ST_V_SKIP,
                         "sin medicion");
        }
    }

    /* D8 — auto-calibracion. Corre ACA, despues de D1..D6 (que necesitan la
     * placa sin calibrar) y antes de D5 (que la necesita calibrada). */
    g_evCalDone = false; g_evCalOk = 0;
    uint32_t calMs = 0;
#if ST_D8_HABILITADO
    Serial.println(F("    D8: calibrando (puede tardar ~20 s, tope 180 s)..."));
    uint32_t tcal = millis();
    psoc.calibrate();
    while (!g_evCalDone && (millis() - tcal) < 180000UL) { psoc.poll(); delay(2); }
    calMs = millis() - tcal;
#endif
    bool calOk = false;
#if !ST_D8_HABILITADO
    (void)calMs;
    stReportItem("D8", "Auto-calibracion", ST_V_SKIP,
                 "desactivada: la maquina de calibracion todavia no esta portada "
                 "de VDAC8 a IDAC8, su veredicto no seria valido");
#else
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
        /* g_evCalOk es el veredicto que da el propio PSoC en CAL_DONE; antes
         * se guardaba y no se miraba. Una calibracion que el PSoC declara
         * fallida no puede dar PASS por mas que los codigos parezcan sanos. */
        calOk = (failed == 0) && (railed == 0) && (g_evCalOk != 0u);
        stReportItem("D8", "Auto-calibracion", calOk ? ST_V_PASS : ST_V_FAIL,
                     "IDAC %s, %d al riel, %d fallada(s), ok=%u, %lu s",
                     det, railed, failed, (unsigned)g_evCalOk,
                     (unsigned long)(calMs / 1000));
    }
#endif

    /* D5 necesita partir de un punto calibrado. Si D8 no cerro bien, ese punto
     * no existe y cualquier veredicto de D5 seria sobre datos de otra cosa. */
#if ST_D8_HABILITADO
    if (!calOk) {
        stReportItem("D5", "Coherencia de rangos del ADC", ST_V_SKIP,
                     "la calibracion (D8) no cerro bien: falta el punto de partida");
        stRxClear();
        psoc.stReport(ST_REP_RESTORE);
        stAwait(1, 2000);
        return;
    }
#else
    (void)calOk;   /* sin D8, D5 corre igual y se autolimita si el tap no entra */
#endif

    /* D5 — coherencia entre las 4 configs del ADC. Va DESPUES de D8 porque
     * necesita partir de un punto conocido y acotado: con la placa sin
     * calibrar el tap puede estar en cualquier lado, y cualquier cosa mas alla
     * de +-0,512 V recorta en la config 2, con lo que la "coherencia" mediria
     * basura o reprobaria una placa sana. */
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

        uint8_t cfgAplicadas = 0;
        for (uint8_t cfg = 1; cfg <= 4; cfg++) {
            if (!stSetAdcConfig(cfg)) { continue; }   /* sin ACK no se mide */
            cfgAplicadas++;
            stPump(300);
            int32_t m, pp;
            if (stMeasDc(ch, SEL_SETTLE_DC, m, pp)) {
                /* Si supera el fondo de escala de la config mas chica, la
                 * comparacion no significa nada. */
                if (labs((long)m) > 450000L) { clipped = true; }
                uv[got++] = m;
            }
        }
        (void)stSetAdcConfig(1);
        stPump(300);
        (void)stSetIdac(stage, baseCode);   /* siempre restaurar el calibrado */

        if (cfgAplicadas < 4u) {
            stReportItem("D5", "Coherencia de rangos del ADC", ST_V_FAIL,
                         "solo %u de 4 rangos confirmaron el ACK: no se puede "
                         "comparar lo que no se aplico", (unsigned)cfgAplicadas);
        } else if (!offsetOk) {
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

    /* Devolver el AMux al canal de captura y parar el ADC: es el estado que el
     * resto del firmware del PSoC espera en IDLE. Sin esto queda seleccionado
     * el ultimo tap de prueba. */
    stRxClear();
    psoc.stReport(ST_REP_RESTORE);
    stAwait(1, 2000);
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
    if (g_hw.geo == HW_AUSENTE) {
        stReportItem("D7", "Golpe al geofono", ST_V_SKIP,
                     "declarado ausente (hw geo 0): no tiene sentido pedir un golpe");
        return;
    }

    Serial.println(F(""));
    Serial.println(F("[ST] D7 — GOLPE AL GEOFONO"));
    Serial.println(F("[ST] Golpea el suelo al lado del geofono cuando diga YA."));

    /* El fondo se mide ANTES de la cuenta regresiva, con el suelo quieto.
     * Medirlo despues del YA metia el propio golpe en la referencia y subia
     * el umbral que ese mismo golpe tenia que superar. */
    Serial.println(F("[ST] Midiendo el ruido de fondo: no toques nada todavia..."));
    if (!stCapture(16, 0)) {
        stReportItem("D7", "Golpe al geofono", ST_V_FAIL, "no se pudo capturar el fondo");
        return;
    }
    int32_t floorPp = g_anMax - g_anMin;
    int32_t trig = (floorPp > 0) ? (floorPp * 2) : 100;

    for (int i = 3; i > 0; i--) { Serial.printf("[ST] %d...\n", i); stPump(1000); }
    Serial.println(F("[ST] YA!"));

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
    if (g_hw.btn == HW_AUSENTE) {
        stReportItem("E1", "Botones del ESP", ST_V_SKIP,
                     "declarados ausentes (hw btn 0)");
        return;
    }
    const bool diagPrev = g_diagEcho;
    g_diagEcho = false;
    const uint8_t pins[4] = { LOCAL_BTN_UP_PIN, LOCAL_BTN_DOWN_PIN,
                              LOCAL_BTN_OK_PIN, LOCAL_BTN_BACK_PIN };
    const char *names[4] = { "UP", "DOWN", "OK", "BACK" };

    Serial.println();
    Serial.println(F("=================================================="));
    Serial.println(F("        PRUEBA DE LOS 4 BOTONES DEL ESP32"));
    Serial.println(F("=================================================="));
    Serial.println(F("Pulsa y suelta SOLO el boton que se indique."));
    Serial.println(F("Hay 10 segundos para cada boton."));

    for (int i = 0; i < 4; i++) {
        Serial.println();
        Serial.printf("[%d/4] BOTON %s  (GPIO%u)\n", i + 1, names[i],
                      (unsigned)pins[i]);

        /* No confundir un boton que ya estaba apretado con una pulsacion nueva. */
        if (digitalRead(pins[i]) == LOW) {
            Serial.println(F("  Sueltalo primero..."));
            uint32_t ts = millis();
            while (digitalRead(pins[i]) == LOW && (millis() - ts) < 3000u) {
                delay(5);
            }
        }

        Serial.println(F("  >>> PULSALO AHORA <<<"));
        uint32_t t0 = millis();
        bool seen = false;
        int lastSecond = -1;
        while ((millis() - t0) < 10000) {
            if (digitalRead(pins[i]) == LOW) { seen = true; break; }
            int second = 10 - (int)((millis() - t0) / 1000u);
            if (second != lastSecond) {
                Serial.printf("  Esperando %s... %d s\n", names[i], second);
                lastSecond = second;
            }
            psoc.poll();   /* no dejar de drenar el ring durante 10 s */
            delay(5);
        }
        if (seen) {
            Serial.printf("  [OK] %s DETECTADO. Ahora soltalo.\n", names[i]);
        } else {
            Serial.printf("  [ERROR] %s NO fue detectado.\n", names[i]);
        }

        char code[8]; snprintf(code, sizeof(code), "E1.%d", i);
        char name[32]; snprintf(name, sizeof(name), "Boton %s del ESP", names[i]);
        stReportItem(code, name, seen ? ST_V_PASS : ST_V_FAIL,
                     seen ? "pulsacion detectada en GPIO%u" : "sin pulsacion en 10 s (GPIO%u)",
                     (unsigned)pins[i]);

        /* Esperar a que suelte, PERO ACOTADO. Un pin en corto a masa, o sin su
         * pull-up de 10 k, se queda en LOW para siempre: un `while` a secas
         * congelaba el autotest justo en el caso que hay que diagnosticar. */
        uint32_t tr = millis();
        while (digitalRead(pins[i]) == LOW && (millis() - tr) < 3000) { delay(5); }
        if (digitalRead(pins[i]) == LOW) {
            Serial.printf("  [ERROR] %s sigue apretado/LOW despues de 3 s.\n", names[i]);
            char c2[8]; snprintf(c2, sizeof(c2), "E1.%db", i);
            stReportItem(c2, "  ^ el pin quedo pegado en bajo", ST_V_FAIL,
                         "GPIO%u sigue en LOW 3 s despues: corto a masa o falta el pull-up",
                         (unsigned)pins[i]);
        } else if (seen) {
            Serial.printf("  [OK] %s SOLTADO.\n", names[i]);
        }
        delay(200);
    }
    Serial.println();
    Serial.println(F("========== FIN PRUEBA BOTONES ESP32 =========="));
    g_diagEcho = diagPrev;
}

/* Pulsacion del boton del PSoC (P2[2], el onboard del CY8CKIT-059). */
static void testButtonPsoc()
{
    if (g_hw.psoc == HW_AUSENTE) {
        stReportItem("E2", "Boton del PSoC (P2[2])", ST_V_SKIP,
                     "PSoC declarado ausente (hw psoc 0)");
        return;
    }

    const bool diagPrev = g_diagEcho;
    g_diagEcho = false;

    Serial.println();
    Serial.println(F("=================================================="));
    Serial.println(F("            PRUEBA DEL BOTON DEL PSoC"));
    Serial.println(F("=================================================="));
    Serial.println(F("Boton conectado a P2[2]. No lo pulses todavia."));

    int idle = -1;
    stRxClear();
    psoc.stReport(ST_REP_BUTTON);
    if (stAwait(1, 1200) >= 1) {
        const PsocSelfTestResult *r = stFind(ST_ID_BUTTON);
        if (r != nullptr && (r->v0 == 0 || r->v0 == 1)) {
            idle = (int)r->v0;
        }
    }
    if (idle < 0) {
        Serial.println(F("[ERROR] El PSoC no respondio la lectura inicial."));
        stReportItem("E2", "Boton del PSoC (P2[2])", ST_V_FAIL,
                     "sin respuesta del PSoC");
        g_diagEcho = diagPrev;
        return;
    }

    Serial.printf("Nivel en reposo: %d\n", idle);
    Serial.println(F(">>> PULSA Y SUELTA AHORA EL BOTON DEL PSoC <<<"));

    bool changed = false;
    uint32_t t0 = millis();
    int lastSecond = -1;
    while ((millis() - t0) < 10000) {
        int second = 10 - (int)((millis() - t0) / 1000u);
        if (second != lastSecond) {
            Serial.printf("Esperando boton PSoC... %d s\n", second);
            lastSecond = second;
        }
        stRxClear();
        psoc.stReport(ST_REP_BUTTON);
        if (stAwait(1, 800) >= 1) {
            const PsocSelfTestResult *r = stFind(ST_ID_BUTTON);
            if (r) {
                if ((int)r->v0 != idle) { changed = true; break; }
            }
        }
        delay(30);
    }

    if (changed) {
        Serial.println(F("[OK] BOTON DEL PSoC DETECTADO."));
        Serial.println(F("Resultado: PASS"));
    } else {
        Serial.println(F("[ERROR] No se detecto ninguna pulsacion en 10 s."));
        Serial.println(F("Resultado: FAIL"));
    }
    stReportItem("E2", "Boton del PSoC (P2[2])",
                 changed ? ST_V_PASS : ST_V_FAIL,
                 changed ? "cambio de nivel detectado" : "sin cambio en 10 s");
    Serial.println(F("========== FIN PRUEBA BOTON PSoC ============="));
    g_diagEcho = diagPrev;
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

/* ==========================================================================
 * DIAGNOSTICO — de que item fallo a donde mirar en la placa.
 *
 * El checklist dice QUE fallo. Esto dice DONDE buscarlo, que es lo que hace
 * falta con el soldador en la mano. La tabla vive en flash y se recorre contra
 * los codigos que quedaron en FAIL o WARN.
 * ========================================================================== */
struct DiagHint {
    const char *code;      /* prefijo del codigo del item */
    const char *donde;     /* que revisar, en orden de probabilidad */
};

static const DiagHint DIAG_HINTS[] = {
    { "A1", "Alimentacion del ESP32. Un reset por BROWNOUT es regulador o "
            "condensador de desacople insuficiente." },
    { "A2", "OLED: CS=33 DC=16 RST=17 SCK=18 MOSI=23, y 3V3/GND del modulo. "
            "No es detectable por software: confirmar mirando la pantalla." },
    { "A3", "Pull-ups de 10 k a 3V3 en GPIO34/35/36/39. Esos pines son de solo "
            "entrada y NO tienen pull-up interno." },
    { "A4", "Radio del ESP32. Si A1 tambien fallo, mirar primero la alimentacion." },
    { "B1", "Enlace de subida: pull-ups de SDA(21)/SCL(22) a 3V3, GND comun, "
            "y que el PSoC este programado y alimentado." },
    { "B2", "Enlace de bajada: ESP GPIO26 -> PSoC Rx P15[0]. Si B1 pasa y B2 no, "
            "el problema esta en ESE cable, no en el I2C." },
    { "B3", "Linea de sincronismo: ESP GPIO27 -> PSoC SYNC_IN P0[4]." },
    { "B4", "Tramas corruptas: ruido en el bus I2C, pull-ups de valor muy alto, "
            "o cables largos. Ojo que `drop` NO cuenta como corrupcion." },
    { "C1", "El PSoC contesta pero no se identifica: firmware de PSoC viejo, "
            "sin los comandos de autotest." },
    { "C2", "Falla interna del PSoC: IRQ inesperada o HardFault. Anotar el "
            "numero de IRQ del detalle." },
    { "C4", "Camino digital de captura: superMaquina, DMA o el enlace. Si C4 "
            "falla no hay que creerle a ninguna medida analogica." },
    { "C5", "Filtro FIR: bloque DFB o su DMA. El resto de la captura funciona." },
    { "C6", "SD: CS=P1[6] SCK=P15[5] MOSI=P2[5] MISO=P15[4], mas alimentacion "
            "del zocalo. Son cuatro puertos distintos en la placa nueva." },
    { "C7", "Pulsador del PSoC en P2[2] (el de la placa CY8CKIT-059)." },
    { "D1", "Algun tap contra un riel. Mirar la fila D2 correspondiente para "
            "saber que etapa." },
    { "D2", "Etapa analogica sin respuesta: resistencia abierta, soldadura fria "
            "o amplificador operacional. El detalle dice que etapa." },
    { "D4", "Etapa PGAout o su red de realimentacion." },
    { "D5", "Referencia del ADC o el escalado de rangos." },
    { "D6", "Ruido: RMS casi nulo es ADC congelado; RMS enorme es una etapa "
            "oscilando. Mucho 50 Hz es masa o apantallamiento." },
    { "D7", "Geofono: bornera, continuidad de la bobina y polaridad del par." },
    { "D8", "Calibracion: un IDAC contra el riel significa que el offset de esa "
            "etapa no se puede anular. Mirar la resistencia de 30 k de esa "
            "referencia y su etapa." },
    { "E1", "Pulsadores del ESP y sus pull-ups." },
    { "E2", "Pulsador del PSoC." },
};

static void printDiagnostico()
{
    uint16_t nFail = stReportCount(ST_V_FAIL);
    uint16_t nWarn = stReportCount(ST_V_WARN);
    if (nFail == 0u && nWarn == 0u) { return; }

    Serial.println();
    Serial.println(F("DIAGNOSTICO — donde mirar:"));

    const size_t nHints = sizeof(DIAG_HINTS) / sizeof(DIAG_HINTS[0]);
    for (size_t i = 0; i < nHints; i++) {
        /* Se imprime la pista si hay algun item FAIL o WARN cuyo codigo empiece
         * con este prefijo. Asi D6.2 y D2.1 caen bajo D6 y D2. */
        if (!stReportHasIssueWithPrefix(DIAG_HINTS[i].code)) { continue; }
        Serial.printf("  [%s] %s\n", DIAG_HINTS[i].code, DIAG_HINTS[i].donde);
    }
    Serial.println(F("  (los WARN no reprueban la placa; los FAIL si)"));
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
    Serial.printf("HW   oled=%s btn=%s geo=%s sd=%s psoc=%s   ('hw' para cambiarlo)\n",
                  hwName(g_hw.oled), hwName(g_hw.btn), hwName(g_hw.geo),
                  hwName(g_hw.sd), hwName(g_hw.psoc));

    Serial.println(F("--- grupo A: ESP32 solo ---"));
    groupA();
    Serial.println(F("--- grupo B: enlace con el PSoC ---"));
    if (groupB()) {
        Serial.println(F("--- grupo C: infraestructura del PSoC ---"));
        if (groupC()) {
            Serial.println(F("--- grupo D: cadena analogica (el mas largo, ~2 min) ---"));
            groupD();
        } else {
            Serial.println(F("[ST] El PSoC no habla el protocolo de autotest."));
            Serial.println(F("[ST] Graba el proyecto AcondicionamientoAnalogicoTest y volve a probar."));
            stReportItem("D*", "Tests analogicos", ST_V_SKIP,
                         "el PSoC no tiene el firmware de autotest");
        }
    } else {
        stReportItem("C*", "Tests del PSoC", ST_V_SKIP, "sin enlace con el PSoC");
        stReportItem("D*", "Tests analogicos", ST_V_SKIP, "sin enlace con el PSoC");
    }
    groupBClose();
    stReportSummary();
    printDiagnostico();

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
        stReportReset(); (void)groupC(); stReportSummary();
    } else if (!strcmp(cmd, "d")) {
        stReportReset(); groupD(); stReportSummary();
    } else if (!strcmp(cmd, "botones") || !strcmp(cmd, "btn")) {
        stReportReset(); testButtonsEsp(); stReportSummary();
    } else if (!strcmp(cmd, "boton") || !strcmp(cmd, "botonpsoc")) {
        stReportReset(); testButtonPsoc(); stReportSummary();
    } else if (!strcmp(cmd, "tap") || !strcmp(cmd, "golpe")) {
        stReportReset(); testTap(); stReportSummary();
    } else if (!strcmp(cmd, "e") || !strcmp(cmd, "inter")) {
        stReportReset();
        testButtonsEsp();
        testButtonPsoc();
        testTap();
        stReportSummary();
    } else if (!strncmp(cmd, "hw", 2) && (cmd[2] == '\0' || cmd[2] == ' ')) {
        /* `hw` solo lista; `hw <parte> <0|1|2>` fija y guarda en NVS. */
        char parte[8] = {0};
        int val = -1;
        if (cmd[2] == '\0' || sscanf(cmd + 2, "%7s %d", parte, &val) < 2) {
            hwPrint();
        } else if (val < 0 || val > 2) {
            Serial.println(F("[ST] valor invalido: 0=ausente 1=presente 2=auto"));
        } else {
            uint8_t v = (uint8_t)val;
            bool ok = true;
            if      (!strcmp(parte, "oled")) { g_hw.oled = v; }
            else if (!strcmp(parte, "btn"))  { g_hw.btn  = v; }
            else if (!strcmp(parte, "geo"))  { g_hw.geo  = v; }
            else if (!strcmp(parte, "sd"))   { g_hw.sd   = v; }
            else if (!strcmp(parte, "psoc")) { g_hw.psoc = v; }
            else { ok = false; Serial.printf("[ST] parte desconocida '%s'\n", parte); }
            if (ok) { hwSave(); hwPrint(); }
        }
    } else if (!strcmp(cmd, "oled si") || !strcmp(cmd, "oled ok")) {
        g_hw.oled = HW_PRESENTE; hwSave();
        Serial.println(F("[ST] OLED marcado PRESENTE y funcionando (confirmado a ojo)."));
    } else if (!strcmp(cmd, "oled no")) {
        g_hw.oled = HW_AUSENTE; hwSave();
        Serial.println(F("[ST] OLED marcado AUSENTE: A2 va a dar SKIP de ahora en mas."));
    } else if (!strcmp(cmd, "diag on")) {
        g_diagEcho = true;  Serial.println(F("[ST] eco de DIAG encendido"));
    } else if (!strcmp(cmd, "diag off")) {
        g_diagEcho = false; Serial.println(F("[ST] eco de DIAG apagado"));
    } else if (!strncmp(cmd, "pin ", 4)) {
        /* Cuadrada lenta en un pin cualquiera, para ubicarlo con el tester y
         * medir continuidad hasta el otro extremo. Los tres cables del enlace
         * son GPIO26 (UART hacia el PSoC), GPIO27 (SYNC) y GPIO22 (SCL). */
        int p = atoi(cmd + 4);
        if (p < 0 || p > 39) {
            Serial.println(F("[ST] numero de pin invalido"));
        } else {
            Serial.printf("[ST] GPIO%d en cuadrada de 1 Hz por 20 s.\n", p);
            Serial.println(F("[ST] OJO: mientras dura, ese pin no cumple su funcion normal."));
            pinMode(p, OUTPUT);
            for (int i = 0; i < 20; i++) {
                digitalWrite(p, HIGH); Serial.println(F("[ST]  ALTO")); delay(500);
                digitalWrite(p, LOW);  Serial.println(F("[ST]  bajo")); delay(500);
            }
            Serial.println(F("[ST] listo. Reinicia el ESP para devolverle su funcion al pin."));
        }
    } else if (!strcmp(cmd, "sync")) {
        /* Para ubicar el pin con el tester: 20 s de cuadrada a 1 Hz. */
        Serial.printf("[ST] GPIO%u en cuadrada de 1 Hz por 20 s. Medi continuidad "
                      "contra P0[4] del PSoC.\n", (unsigned)SYNC_TO_PSOC_PIN);
        pinMode(SYNC_TO_PSOC_PIN, OUTPUT);
        for (int i = 0; i < 20; i++) {
            digitalWrite(SYNC_TO_PSOC_PIN, HIGH); Serial.println(F("[ST]  ALTO")); delay(500);
            digitalWrite(SYNC_TO_PSOC_PIN, LOW);  Serial.println(F("[ST]  bajo")); delay(500);
        }
        Serial.println(F("[ST] listo"));
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
        Serial.println(F("[ST] pin N     cuadrada de 1 Hz en GPIO N, para el tester"));
        Serial.println(F("[ST]            26=UART al PSoC  27=SYNC  22=SCL  21=SDA"));
        Serial.println(F("[ST] botones   pulsar los 4 botones del ESP, de a uno"));
        Serial.println(F("[ST] boton     pulsar el boton del PSoC"));
        Serial.println(F("[ST] tap       golpe al geofono: pico + POLARIDAD"));
        Serial.println(F("[ST] e|inter   fase interactiva (botones + golpe)"));
        Serial.println(F("[ST] probe     estado del enlace con el PSoC"));
        Serial.println(F("[ST] hw        ver el perfil de hardware presente"));
        Serial.println(F("[ST] hw X N    X=oled|btn|geo|sd|psoc  N=0 ausente 1 presente 2 auto"));
        Serial.println(F("[ST] oled si|no  confirmar a ojo si el OLED anda"));
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

/* Mide el bus I2C como GPIO, ANTES de inicializar Wire.
 *
 * TIENE que correr antes de psoc.begin(). TwoWire::begin() arranca con
 *   if (is_slave) { log_w("Bus already started in Slave Mode."); goto end; }
 * o sea que si el bus ya esta levantado, una segunda llamada NO reasigna los
 * pines. Hacer esta medicion despues le roba los pines al periferico y no hay
 * forma de devolverselos: el enlace queda muerto por culpa del propio test.
 * (Pasó: dos corridas dieron "silencio total" por esto y no por la placa.)
 *
 * Dos cosas se miden y responden preguntas distintas:
 *   - Nivel de reposo: un bus sano tiene SDA y SCL en ALTO por sus pull-ups.
 *     Se lee con INPUT pelado; con INPUT_PULLUP el pull-up interno del ESP
 *     (~45 k) taparia justo la falla que se busca.
 *   - Flancos en SCL: en I2C el reloj lo genera SIEMPRE el maestro, que aca es
 *     el PSoC. Si hay flancos, el PSoC esta transmitiendo y el problema es del
 *     lado del ESP; si no hay, el PSoC no habla. */
static void medirBusI2C()
{
    pinMode(PSOC_I2C_SDA, INPUT);
    pinMode(PSOC_I2C_SCL, INPUT);
    delayMicroseconds(200);

    g_busSdaN = 0;
    g_busSclN = 0;
    for (int k = 0; k < 50; k++) {
        if (digitalRead(PSOC_I2C_SDA) == HIGH) { g_busSdaN++; }
        if (digitalRead(PSOC_I2C_SCL) == HIGH) { g_busSclN++; }
        delay(1);
    }
    g_busSdaAlta = (g_busSdaN >= 45) ? 1 : 0;
    g_busSclAlta = (g_busSclN >= 45) ? 1 : 0;

    /* Segunda pasada con el pull-up INTERNO del ESP (~45 k). Distingue dos
     * causas que "la linea esta en bajo" confunde:
     *   - sube con el pull-up interno -> nadie la esta forzando; lo que falta
     *     es el pull-up EXTERNO de la placa.
     *   - sigue en bajo -> algo la esta reteniendo: corto a masa, o un
     *     dispositivo de colector abierto colgado (por ejemplo un maestro I2C
     *     trabado a mitad de transferencia).
     * Es seguro: un pull-up de 45 k no pelea con nada. */
    pinMode(PSOC_I2C_SDA, INPUT_PULLUP);
    pinMode(PSOC_I2C_SCL, INPUT_PULLUP);
    delay(5);
    g_busSdaPu = 0;
    g_busSclPu = 0;
    for (int k = 0; k < 30; k++) {
        if (digitalRead(PSOC_I2C_SDA) == HIGH) { g_busSdaPu++; }
        if (digitalRead(PSOC_I2C_SCL) == HIGH) { g_busSclPu++; }
        delay(1);
    }
    /* Tercera pasada, con el pull-DOWN interno (~45 k). Esta si MIDE si hay un
     * pull-up externo fuerte, en vez de inferirlo:
     *
     *   - Con un pull-up externo de 10 k a 3V3, el divisor contra los 45 k
     *     internos da 3,3 * 45/(45+10) = 2,7 V -> se lee ALTO.
     *   - Sin pull-up externo, el pull-down se impone -> se lee BAJO.
     *
     * SDA sirve de control: ahi el pull-up externo esta confirmado, asi que si
     * SDA lee alto y SCL lee bajo con el mismo pull-down, la diferencia es
     * real y esta en el cobre, no en el metodo. */
    pinMode(PSOC_I2C_SDA, INPUT_PULLDOWN);
    pinMode(PSOC_I2C_SCL, INPUT_PULLDOWN);
    delay(5);
    g_busSdaPd = 0;
    g_busSclPd = 0;
    for (int k = 0; k < 30; k++) {
        if (digitalRead(PSOC_I2C_SDA) == HIGH) { g_busSdaPd++; }
        if (digitalRead(PSOC_I2C_SCL) == HIGH) { g_busSclPd++; }
        delay(1);
    }

    pinMode(PSOC_I2C_SDA, INPUT);
    pinMode(PSOC_I2C_SCL, INPUT);

    /* El PSoC pinguea cada ~700 ms mientras espera al ESP: 3 s cubren varios. */
    g_sclEdges = 0;
    attachInterrupt(digitalPinToInterrupt(PSOC_I2C_SCL), onSclEdge, CHANGE);
    delay(3000);
    detachInterrupt(digitalPinToInterrupt(PSOC_I2C_SCL));
    g_busSclFlancos = g_sclEdges;
}

/* ── setup / loop ────────────────────────────────────────────────────────── */
void setup()
{
    Serial.begin(115200);
    delay(300);

    /* El perfil de hardware declarado tiene que estar cargado antes del primer
     * test: define si una parte que no contesta es SKIP, WARN o FAIL. */
    hwLoad();

    pinMode(SYNC_TO_PSOC_PIN, OUTPUT);
    digitalWrite(SYNC_TO_PSOC_PIN, LOW);

    localUiBegin();

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);
    WiFi.disconnect();
    delay(100);

    transport.begin(g_masterMac);

    /* ANTES de psoc.begin(): despues, Wire ya tiene los pines y medirlos como
     * GPIO rompe el enlace sin posibilidad de devolverlos. */
    medirBusI2C();

    psoc.onDiag(onDiag);
    psoc.onSelfTest(onSelfTest);
    psoc.begin(onBatch);

    /* El PSoC tarda en arrancar y este firmware lo primero que hace es
     * medir: darle tiempo evita reportar un falso "sin enlace". */
    stPump(2000);
    (void)psoc.probe(600);

    /* No disparar la corrida completa al boot: al programar o abrir el puerto
     * se puede resetear el ESP y eso ejecutaba tambien el grupo analogico sin
     * que el operador lo pidiera. `run` conserva la corrida completa; para el
     * banco digital se usan `b` y `c` de forma explicita. */
    Serial.println(F("[ST] Autotest listo. 'b' y 'c' corren solo digital;"));
    Serial.println(F("[ST] 'run' ejecuta la corrida completa, incluido analogico."));
}

void loop()
{
    psoc.poll();
    serviceUsb();

    /* Re-sondeo periodico mientras no haya trafico del PSoC, igual que hace el
     * firmware de campo. El PSoC espera un PONG o un STATUS para salir de
     * wait_for_esp(); si se reinicia (ToggleReset) despues del sondeo inicial,
     * sin esto queda mudo hasta que alguien corra el autotest de nuevo. */
    static uint32_t ultimoSondeo = 0;
    if (psoc.bytesRx() == 0u && (millis() - ultimoSondeo) > 2000u) {
        ultimoSondeo = millis();
        psoc.sendPong();
        psoc.requestStatus();
    }

    delay(1);
}
