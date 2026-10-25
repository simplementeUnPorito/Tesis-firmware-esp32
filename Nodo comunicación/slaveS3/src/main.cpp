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

#ifndef SLAVE_LAB_TOOLS_ENABLE
  #define SLAVE_LAB_TOOLS_ENABLE 0
#endif

#include <Arduino.h>
#if SLAVE_LAB_TOOLS_ENABLE
  #include <SD.h>
  #include <SPI.h>
#endif
#include <esp_wifi.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include "psoc_uart.h"
#include "espnow_transport.h"
#include "live_capture.h"
#include "ota_slave.h"
#include "ftm_slave.h"
#if defined(SIM_SIN_HW) && SIM_SIN_HW
#include "sim_psoc.h"
#endif

/* Pedido de lote del maestro, atendido en loop() (ver onDataRecv). */
static volatile MsgReqBatch g_reqBatchPending = {};
static volatile bool        g_reqBatchPendingFlag = false;
#if !defined(ESP8266)
static portMUX_TYPE         g_reqBatchMux = portMUX_INITIALIZER_UNLOCKED;
#define REQ_LOCK()   portENTER_CRITICAL(&g_reqBatchMux)
#define REQ_UNLOCK() portEXIT_CRITICAL(&g_reqBatchMux)
#else
#define REQ_LOCK()   noInterrupts()
#define REQ_UNLOCK() interrupts()
#endif

/* FIR del PSoC en transito (ver serviceFirUpload). */
static uint8_t  g_firBuf[FIR_HW_TAPS * 3];
static uint32_t g_firGot[(FIR_HW_TAPS + 31) / 32];
static volatile bool g_firCommitPending = false;
static volatile uint8_t g_firAction = 0;
static volatile uint16_t g_firSum = 0;
#include "sync_protocol.h"
#include "debug_log.h"
#include "sd_storage.h"
#include "local_ui.h"
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
  /* Tope RAM-only de lotes DECIMADOS por captura. El PSoC guarda hasta 512
   * lotes decimados y el ESP32 usa paginas chicas en vez de un malloc gigante,
   * asi el limite ya no depende del bloque contiguo maximo con WiFi arriba.
   * Duracion real = n*decim*30/2604. */
  #define PSOC_CAPTURE_MAX_BATCHES 512
#endif
#ifndef PSOC_SD_CAPTURE_MAX_BATCHES
  #define PSOC_SD_CAPTURE_MAX_BATCHES 60000u
#endif
/* Logging (humano + máquina) en debug_log.h, gateado por DBG_ENABLE.
 * Estos defines limpian el monitor USB: por defecto queda solo lo accionable
 * para diagnosticar calibracion/captura. */
#ifndef PSOC_DIAG_VERBOSE
  #define PSOC_DIAG_VERBOSE 0
#endif
#ifndef PSOC_CAL_LOG_POINTS
  #define PSOC_CAL_LOG_POINTS 1
#endif
#ifndef PSOC_CAL_LOG_CLAMPED_POINTS
  #define PSOC_CAL_LOG_CLAMPED_POINTS 0
#endif
#ifndef PSOC_CAL_LOG_PROGRESS
  #define PSOC_CAL_LOG_PROGRESS 0
#endif
#ifndef SLAVE_LOG_HELLO_TX
  #define SLAVE_LOG_HELLO_TX 0
#endif
#ifndef SLAVE_LOG_STATUS_PERIODIC
  #define SLAVE_LOG_STATUS_PERIODIC 0
#endif
#ifndef SLAVE_LOG_VIEW_UART
  #define SLAVE_LOG_VIEW_UART 0
#endif
#ifndef SLAVE_USB_CMD_ENABLE
  #define SLAVE_USB_CMD_ENABLE 1
#endif
/* Lineas de protocolo para un parser de host (dumpusb/rftest/sdstress): sin
 * el prefijo "[t][S2] " de SLAVE_LOG_PRINTF, igual que LOGM con "#M,...". */
#if DBG_ENABLE
  #define SLAVE_LOG_RAW(...) DBG_STREAM.printf(__VA_ARGS__)
#else
  #define SLAVE_LOG_RAW(...) do {} while (0)
#endif
#ifndef PSOC_AUTO_CAL_ON_READY
  /* Learning must only start after the operator confirms that the sensor is
   * still.  A newly detected PSoC therefore reports NEEDS_LEARNING but never
   * authorizes calibration on its own. */
  #define PSOC_AUTO_CAL_ON_READY 0
#endif
#ifndef PSOC_AUTO_CAL_DELAY_MS
  #define PSOC_AUTO_CAL_DELAY_MS 500u
#endif
#ifndef PSOC_AUTO_CAL_RETRY_MS
  #define PSOC_AUTO_CAL_RETRY_MS 3000u
#endif
/* Ganancia con la que tiene que ARRANCAR el nodo. Placa I2C GEO: PGA x4 y PGAout x24, las del perfil del lazo.
 * El PSoC arranca con el default del TopDesign (x1) y, en la placa I2C, recién
 * después de hablar con el ESP su lazo de control pasa el hardware a x4/x24
 * sin avisarlo. El ESP fija estos códigos tras cada arranque del PSoC (o del
 * propio ESP) y los confirma por el camino normal de config, así el maestro y
 * la web ven la ganancia real. Códigos: 0=x1 1=x2 2=x4 3=x8 4=x16 5=x24 6=x32
 * 7=x48 8=x50. 0xFF = no tocar. */
#ifndef SLAVE_BOOT_PGA_CODE
  #define SLAVE_BOOT_PGA_CODE 2u
#endif
#ifndef SLAVE_BOOT_PGAOUT_CODE
  #define SLAVE_BOOT_PGAOUT_CODE 5u   /* solo si el PSoC reporta GEO */
#endif
#ifndef SLAVE_BOOT_GAIN_DELAY_MS
  #define SLAVE_BOOT_GAIN_DELAY_MS 4000u   /* el PSoC termina su init (control_init) */
#endif
static uint8_t  g_boot_gain_step = 0u;      /* 0 nada, 1 PGA, 2 PGAout */
static uint8_t  g_boot_gain_tries = 0u;
static uint32_t g_boot_gain_due_ms = 0u;

static void scheduleBootGains()
{
    g_boot_gain_step = 1u;
    g_boot_gain_tries = 0u;
    g_boot_gain_due_ms = millis() + SLAVE_BOOT_GAIN_DELAY_MS;
}
#ifndef WIFI_TX_POWER_QDBM
  #define WIFI_TX_POWER_QDBM 84   /* maximo del ESP32: 21 dBm */
#endif

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
static const uint8_t MASTER_MAC_DEFAULT[6] = {
    MASTER_MAC0, MASTER_MAC1, MASTER_MAC2,
    MASTER_MAC3, MASTER_MAC4, MASTER_MAC5
};
static uint8_t g_masterTxMac[6] = {
    MASTER_MAC0, MASTER_MAC1, MASTER_MAC2,
    MASTER_MAC3, MASTER_MAC4, MASTER_MAC5
};
/* BSSID del AP del maestro, del escaneo de canal. El ESP-NOW del maestro sale
 * por su STA, que difiere del BSSID solo en el ultimo byte. Sin esto el esclavo
 * aprendia como maestro cualquier trama ESP-NOW del aire (2026-10-23: MACs
 * aleatorias de otro equipo; el ARM_ACK se iba a ellas y START se cancelaba). */
static uint8_t g_masterApBssid[6] = {0};
static bool    g_masterApKnown = false;
static bool    g_beaconMarks = true;

#if !defined(ESP8266)
/* Ultima baliza del AP del maestro: mientras se oiga, el maestro sigue en el
 * canal y no hace falta reescanear aunque no mande ESP-NOW (ver
 * masterChannelService). */
static volatile uint32_t g_lastMasterBeaconMs = 0;

static void beaconPromiscCb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (!g_beaconMarks || !g_masterApKnown || type != WIFI_PKT_MGMT || buf == nullptr) return;
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *frame = pkt->payload;
    if (pkt->rx_ctrl.sig_len < 22u || frame[0] != 0x80u ||
        memcmp(frame + 16, g_masterApBssid, 6) != 0) return;
    g_lastMasterBeaconMs = millis();
    const uint64_t now = (uint64_t)esp_timer_get_time();
    const uint64_t airtime = (uint64_t)pkt->rx_ctrl.sig_len * 8ULL;
    live::noteEvent(SAMPLE_FLAG_AP_BEACON, now - airtime, now);
}

static void applyBeaconMarks()
{
    wifi_promiscuous_filter_t filter = {};
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(beaconPromiscCb);
    esp_wifi_set_promiscuous(g_beaconMarks);
}
#else
static void applyBeaconMarks() {}
#endif

/* Pines GPIO hardware sync. platformio.ini es la fuente de verdad; estos
 * defaults solo evitan romper builds viejos que no definan las macros. */
/* (SYNC_IN_PIN eliminado 2026-08-03: no existe sync por cable desde el
 * maestro. El único pin de sync es la salida SYNC_TO_PSOC_PIN.) */
#ifndef SYNC_TO_PSOC_PIN
  #if defined(ESP8266)
    #define SYNC_TO_PSOC_PIN 16
  #else
    #define SYNC_TO_PSOC_PIN 27
  #endif
#endif

/* ── Estado ──────────────────────────────────────────────────────────────── */
/* ── Seguimiento del canal del maestro ───────────────────────────────────────
 * El maestro ya no vive solo en su AP: tambien se asocia a la red del cliente
 * (hotspot del celular) para que se puedan bajar las capturas. El ESP32 tiene UNA
 * sola radio, asi que al asociarse su AP es arrastrado al canal de esa red — y
 * ESP-NOW transmite en el canal vigente. Fijar el canal 1 aca funcionaba solo
 * mientras el maestro estuviera en canal 1: si el hotspot cae en 6 u 11, el
 * esclavo se queda sordo sin que nada lo indique.
 *
 * La solucion es no fijar nada: buscar el AP del maestro por SSID y adoptar su
 * canal, y volver a buscarlo si el trafico se corta. Nada de esto toca el
 * protocolo con el PSoC ni el protocolo ESP-NOW: solo cambia en que canal
 * escucha la radio. */
#ifndef MASTER_AP_SSID
#define MASTER_AP_SSID  "GeoNetwork"
#endif
#ifndef MASTER_AP_PASS
#define MASTER_AP_PASS  "geophone2026"   /* el mismo del AP del maestro; solo OTA */
#endif
#ifndef MASTER_CH_FALLBACK
#define MASTER_CH_FALLBACK  1
#endif
/* Silencio tolerado antes de sospechar que el maestro cambio de canal. Tiene que
 * ser MAYOR que la captura mas larga posible: durante SAMPLING el maestro no
 * habla, y rescanear ahi seria romper la captura. Igual se gatea por estado. */
#ifndef MASTER_SILENCE_MS
#define MASTER_SILENCE_MS  (45u * 1000u)
#endif

static volatile uint32_t g_lastMasterRxMs = 0;
static uint8_t  g_masterChannel = MASTER_CH_FALLBACK;

enum SlaveState { WAIT_ARM, ARMED, HOT_WAIT, SAMPLING, STOPPED };
static volatile SlaveState g_state       = WAIT_ARM;
static bool                g_radio_silencio = false;
static volatile bool       g_radio_silencio_pedido = false;
static uint16_t            g_radio_silencio_ms = 0;
/* Modo de sincronizacion de la captura en curso (SYNCMODE_*, llega en
 * PRESTART). RADIO_OFF reusa el mecanismo de radiosil para una sola captura;
 * FTM_PAUSED calla toda transmision mientras se muestrea. */
static uint8_t             g_syncMode = SYNCMODE_FTM_ACTIVE;
static uint32_t            g_syncPausedDrops = 0;
static const uint16_t      RADIO_OFF_SYNC_DELAY_MS = 300u;
static bool                g_psocConnected = false;  /* resultado de probe() en setup */
/* GPIO configurado en platformio.ini -> PSoC P1[4] (esp_reset).
 * Sin pull-up externo: el ESP entrega 3V3 en reposo y 0V al pedir reset.
 * P2[4] del PSoC, open-drain, es quien tira de RST/XRES. */
#ifndef PSOC_RESET_REQUEST_PIN
#error "Definir PSOC_RESET_REQUEST_PIN en platformio.ini"
#endif
#if PSOC_RESET_REQUEST_PIN == PSOC_UART_RX
#error "PSOC_RESET_REQUEST_PIN y PSOC_UART_RX no pueden usar el mismo GPIO"
#endif
#if defined(LOCAL_UI_ENABLE) && LOCAL_UI_ENABLE && \
    PSOC_RESET_REQUEST_PIN == LOCAL_OLED_MISO_PIN
#error "PSOC_RESET_REQUEST_PIN y LOCAL_OLED_MISO_PIN no pueden usar el mismo GPIO"
#endif
static constexpr uint32_t PSOC_RESET_PULSE_MS = 500u;
static constexpr uint32_t PSOC_RESET_STALE_MS = 15000u;
static constexpr uint32_t PSOC_RESET_RETRY_MS = 2000u;
static constexpr uint32_t PSOC_RESET_COOLDOWN_MS = 60000u;
static bool g_psocResetActive = false;
static bool g_psocEverSeen = false;
static uint8_t g_psocResetFailures = 0u;
static uint8_t g_psocAutoResetCount = 0u;
static uint32_t g_psocResetStartedMs = 0u;
static uint32_t g_psocResetLastProbeMs = 0u;
static uint32_t g_psocResetLastAutoMs = 0u;

static void drivePsocResetRequestPin(bool high)
{
    /* OUTPUT en Arduino-ESP32 es push-pull: fuerza tanto 3V3 como GND. */
    pinMode(PSOC_RESET_REQUEST_PIN, OUTPUT);
    digitalWrite(PSOC_RESET_REQUEST_PIN, high ? HIGH : LOW);
}

static void beginPsocResetRequest(const char *reason)
{
    if (g_psocResetActive) return;
    drivePsocResetRequestPin(false);
    g_psocResetActive = true;
    g_psocResetStartedMs = millis();
    g_psocConnected = false;
    SLAVE_LOG_PRINTF("[PSoC] reset solicitado (%s), GPIO%d nivel=%d esperado=0\n",
                     reason, PSOC_RESET_REQUEST_PIN,
                     digitalRead(PSOC_RESET_REQUEST_PIN));
}

static void servicePsocResetRequest()
{
    if (g_psocResetActive &&
        (uint32_t)(millis() - g_psocResetStartedMs) >= PSOC_RESET_PULSE_MS) {
        drivePsocResetRequestPin(true);
        g_psocResetActive = false;
        g_psocResetFailures = 0u;
        SLAVE_LOG_PRINTF("[PSoC] reset liberado, GPIO%d nivel=%d esperado=1\n",
                         PSOC_RESET_REQUEST_PIN,
                         digitalRead(PSOC_RESET_REQUEST_PIN));
    }
}
static volatile uint64_t   g_t_start_us  = 0;
static volatile bool       g_debug_mode  = false;
static          uint32_t   g_debug_count = 0;
static volatile uint32_t   g_debug_last_us = 0;
/* g_debugHwActive / g_debugHwFallUs viven en scope_pulse.h */
static          uint8_t    g_pga_code    = 0;
static          uint8_t    g_pgaout      = 0;   /* GEO: etapa PGAout del pipeline nuevo */
static          uint8_t    g_pgavdac     = 0;
static          uint8_t    g_vdac_byte   = 128;
static volatile bool       g_cfg_waiting = false;
static          uint8_t    g_cfg_sub_cmd = 0;
static          uint8_t    g_cfg_param   = 0;
static          uint32_t   g_cfg_start_ms = 0;
static          uint32_t   g_cfg_timeout_ms = 0;
static          uint32_t   g_cal_progress_ack_ms = 0; /* ultimo "calibrando..." (ok=2) enviado al maestro */
static          bool       g_auto_cal_requested = false;
static          uint32_t   g_auto_cal_due_ms = 0;

/* ── LED de identificación (CMD_BLINK_LED) ──────────────────────────────────
 * El LED de identificación es el del PSoC (el ESP no tiene un LED útil en el
 * pin 2). El esclavo solo reenvía el comando por UART con psoc.blinkLed(); el
 * PSoC titila su LED de forma NO bloqueante usando su Timer. */

/* ── Store-and-forward ───────────────────────────────────────────────────── */
static uint16_t    g_rec_n_batches = 0;          /* 0 = modo streaming clásico */
static uint16_t    g_store_fill    = 0;
static bool         g_psoc_sd_capture_enabled = false; /* estado REAL del PSoC (ack 0xBE / SD_STATUS) */
/* Preferencia del usuario (web o USB). Decide si una captura larga va a SD.
 * Se separa del estado real porque syncPsocSdForCapture() apaga la SD del
 * PSoC en las capturas que entran en RAM: su drenaje mete una espiga cada
 * 57,6 ms en la señal, y un PSoC sin la regla nueva la usaría igual. */
static bool         g_sd_user_enabled = false;
static bool         g_sd_sync_pending = false;   /* ack esperado por la sincronización */
static bool         g_sd_overridden = false;     /* PSoC distinto de la preferencia a propósito */
static bool         g_psoc_sd_store_active = false;    /* captura actual vive en GEOLAST.BIN */
static bool         g_psoc_sd_session_complete = false;
static bool         g_psoc_sd_capture_error = false;
static bool         g_psoc_sd_read_pending = false;
static bool         g_psoc_sd_read_usb = false;    /* banco: sdread N imprime por USB en vez de ESP-NOW */
static uint16_t     g_psoc_sd_read_seq = 0;
static bool         g_psoc_sd_read_compressed = false;
static uint32_t     g_psoc_sd_read_started_ms = 0;
#define PSOC_SD_READ_TIMEOUT_MS 450u  /* H3 auditoría 2026-07-11: ~3.6% de lecturas 0xBF
                                        * superan 250 ms (pico periódico); 450 ms absorbe casi
                                        * todas sin NACK y el maestro re-pide el mismo seq igual */

/* ── rftest (banco): trafico ESP-NOW controlado, EN PARALELO a una captura ──
 * Sin esto medir el ruido que mete la radio exige esperar a que transmita
 * por casualidad. Dispara una trama fija cada periodo_ms sin bloquear, para
 * correr junto con "cap N". Ver USB "rftest" y loop() -> serviceRfTest(). */
static uint32_t g_rftest_hasta_ms   = 0;   /* millis() de fin; 0 = sin trafico armado */
static uint32_t g_rftest_periodo_ms = 0;
static uint8_t  g_rftest_bytes      = 0;
static uint32_t g_rftest_enviados   = 0;
static uint32_t g_rftest_fallos     = 0;
static uint32_t g_rftest_prox_ms    = 0;
static bool     g_rftest_running    = false; /* para imprimir "#RFTEST fin" una sola vez */
static bool     g_rftest_broadcast  = false;
static const uint8_t RFTEST_BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* Instantes (micros) de cada evento de radio durante la captura. El PSoC
 * avisa por I2C en cada k*150 muestras (sdprobe modo 7, PSOC_EVT_LAB_PING) y
 * esos avisos, tambien en micros, son el reloj de referencia: el host
 * interpola cada evento entre dos avisos y obtiene su muestra exacta, sin la
 * deriva del IMO del PSoC (+-1 %) ni la latencia de la captura a RAM. */
#define LAB_POS_MAX 192
struct LabPos {
    volatile bool     enSampling;
    volatile uint32_t base;
    volatile uint16_t n;
    uint32_t          pos[LAB_POS_MAX];
};
static uint32_t          g_ping_t[LAB_POS_MAX];
static uint8_t           g_ping_v[LAB_POS_MAX];
static volatile uint16_t g_ping_n = 0u;
static bool              g_ping_en_sampling = false;
static volatile bool     g_rftest_disparo = false;   /* rftest periodo 0: transmitir ya */
static LabPos g_rftest_pos = {};
static LabPos g_rfrx_pos   = {};

static void labPosReset(LabPos &p) { p.enSampling = false; p.base = 0u; p.n = 0u; }

/* Llamar seguido (loop/serviceLabWaitSlice) para fijar la base de lotes. */
static void labPosTick(LabPos &p);
/* Registra un evento ahora; tambien desde el callback de recepcion. */
static void labPosMark(LabPos &p);
static void labPosPrint(const char *tag, LabPos &p);

/* rfrx: registra la llegada de las rafagas del maestro (tramas "RFB1"). */
static uint32_t g_rfrx_hasta_ms  = 0;
static bool     g_rfrx_running   = false;
static uint32_t g_rfrx_recibidas = 0;

/* Pulso para disparar el osciloscopio en cada transmision de rftest. */
#define RF_TRIG_PIN 4

#if defined(ESP8266)
static void onDataRecv(uint8_t *mac, uint8_t *data, uint8_t len);
#else
static void onDataRecv(const uint8_t *mac, const uint8_t *data, int len);
#if defined(ESP32)
#include <esp_arduino_version.h>
#if ESP_ARDUINO_VERSION_MAJOR >= 3
#include <esp_private/wifi.h>   /* reloj MAC para la baliza de tiempo FTM */
#endif
#if ESP_ARDUINO_VERSION_MAJOR >= 3
/* IDF 5: el callback de ESP-NOW recibe esp_now_recv_info_t. */
/* Instante de recepcion de la ultima trama (radio) y, en el mismo momento,
 * los dos relojes locales: sirve para anclar el START broadcast. */
static volatile uint32_t g_rx_ts = 0;
static volatile uint32_t g_rx_mac = 0;
static volatile int64_t  g_rx_esp = 0;
static void onDataRecvV3(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (info && info->rx_ctrl) {
        g_rx_ts = info->rx_ctrl->timestamp;
        g_rx_mac = esp_wifi_internal_get_mac_clock_time();
        g_rx_esp = esp_timer_get_time();
    }
    onDataRecv(info->src_addr, data, len);
}
#define ESPNOW_RECV_CB onDataRecvV3
#else
#define ESPNOW_RECV_CB onDataRecv
#endif
#endif
#endif

/* ── sdstress (banco): escrituras a la SD del PSoC EN PARALELO a una captura
 * corta que si entra en RAM. NO toca la regla de g_psoc_sd_store_active: es
 * un estresor aparte de la SD real de una captura larga. Reusa
 * PSOC_CMD_SD_TEST (self-test GEOTEST.BIN, el mismo camino que "sdtest")
 * disparado a un periodo elegido; 58 ms por defecto imita el drenaje real de
 * una captura a SD (un sector cada 57,6 ms). El ack 0xBD llega por
 * servicePsocConfigAck() -> applyConfirmedConfig(), que cuenta el fallo solo
 * mientras g_sdstress_running (asi no se mezcla con un "sdtest" suelto). */
static uint32_t g_sdstress_hasta_ms   = 0;
static uint32_t g_sdstress_periodo_ms = 0;
static uint32_t g_sdstress_escrituras = 0;
static uint32_t g_sdstress_fallos     = 0;
static uint32_t g_sdstress_prox_ms    = 0;
static bool     g_sdstress_running    = false;

/* ── sdprobe (banco): prueba parametrizada de ruido SD del PSoC, en paralelo
 * a una captura corta en RAM. El ack 0xAD se cuenta igual que sdstress. */
static uint32_t g_sdprobe_hasta_ms   = 0;
static uint32_t g_sdprobe_periodo_ms = 0;
static uint32_t g_sdprobe_envios     = 0;
static uint32_t g_sdprobe_fallos     = 0;
static uint32_t g_sdprobe_prox_ms    = 0;
static uint8_t  g_sdprobe_modo       = 0;
static uint8_t  g_sdprobe_div        = 0;
static uint8_t  g_sdprobe_slew       = 0;
static bool     g_sdprobe_running    = false;

#if SLAVE_LAB_TOOLS_ENABLE
/* E14: la SD cuelga del ESP32 por un HSPI exclusivo y escribe mientras el
 * PSoC captura solamente a RAM. */
static SPIClass spiHspi(LIVE_SD_SPI_HOST);
static uint32_t g_espsd_hasta_ms       = 0u;
static uint32_t g_espsd_cada_lotes     = 0u;
static uint32_t g_espsd_base           = 0u;
static uint32_t g_espsd_proximo        = 0u;
static uint32_t g_espsd_escrituras     = 0u;
static uint32_t g_espsd_fallos         = 0u;
static uint32_t g_espsd_sector         = 0u;
static uint32_t g_espsd_pos[128]       = {};
static bool     g_espsd_activo         = false;
static bool     g_espsd_en_sampling    = false;
static bool     g_espsd_tiempo_cumplido = false;
#endif

#ifndef STORE_PAGE_BATCHES
#define STORE_PAGE_BATCHES 32u
#endif
#define STORE_MAX_PAGES \
    ((PSOC_CAPTURE_MAX_BATCHES + STORE_PAGE_BATCHES - 1u) / STORE_PAGE_BATCHES)

static SampleBytes *g_store_pages[STORE_MAX_PAGES] = { nullptr };
static uint8_t      g_store_page_count = 0;
static uint16_t     g_store_capacity_batches = 0;
static uint32_t    *g_store_ts_us = nullptr;      /* timestamp relativo por batch */

/* ── Capturas store-then-dump: VER y START normal ────────────────────────── */
static volatile uint16_t g_view_remaining    = 0; /* legado live deshabilitado */
static volatile bool     g_view_store_active = false;
static volatile bool     g_start_store_active = false;
/* Modo de captura pedido (CAPMODE_*, ver sync_protocol.h). Lo fija el maestro
 * en PRESTART o el banco con "capmode". El asincronico con RAM del PSoC es el
 * camino clasico; todo lo demas pasa por live_capture. */
static uint8_t  g_capmode = 0u;
static bool     g_capmode_ramp = false;
static bool     g_live_notified = false;
static volatile uint8_t  g_psoc_live_ack = 0xFFu;
static volatile uint32_t g_psoc_live_ack_seq = 0u;
static int16_t  g_psoc_live_known = -1;
static bool capmodeIsLive(uint8_t mode)
{
    return (mode & CAPMODE_REALTIME) != 0u ||
           (mode & CAPMODE_STREAM_DUMP) != 0u ||
           (mode & CAPMODE_STORAGE_MASK) != CAPMODE_STORAGE_PSOC;
}
static volatile uint8_t  g_psoc_last_setn_value = 0;
static volatile uint32_t g_psoc_last_setn_ms = 0;
static volatile uint32_t g_psoc_setn_seq = 0;
static volatile bool     g_psoc_arm_ready = false;

#define PSOC_CFG_ACK_TIMEOUT_MS 750u
#define PSOC_SETN_ACK_TIMEOUT_MS 500u
#define PSOC_CAL_ACK_TIMEOUT_MS 450000u
#define PSOC_ADC_SNAPSHOT_ACK_TIMEOUT_MS 5000u
#define PSOC_CAL_PROGRESS_ACK_PERIOD_MS 3000u /* "sigue calibrando" (ok=2) hacia el maestro */
#define PSOC_CFG_READY_STALE_MS 5000u
#define COMPLETION_ACK_RETRY_PERIOD_MS 250u
#define COMPLETION_ACK_RETRY_COUNT 24u
/* Watchdog de HOT_WAIT: si el PSoC no confirma ARMED se reintenta preStart y,
 * si sigue mudo, se aborta con NACK en vez de quedar colgado hasta un power
 * cycle (bug de campo 2026-07-11: "START funciona una vez y nunca más"). */
#define HOTWAIT_READY_TIMEOUT_MS 2000u
#define HOTWAIT_REARM_MAX 2u
#define HOTWAIT_ABORT_MS 5000u
#define HOTWAIT_ARMED_ABORT_MS 300000u
#define HOTWAIT_STATUS_AFTER_MS 2000u
#ifndef PSOC_START_FALLBACK_EXTRA_MS
  #ifdef PSOC_VIEW_START_FALLBACK_EXTRA_MS
    #define PSOC_START_FALLBACK_EXTRA_MS PSOC_VIEW_START_FALLBACK_EXTRA_MS
  #else
    #define PSOC_START_FALLBACK_EXTRA_MS 2500u
  #endif
#endif

static uint32_t g_sampling_start_ms = 0;
static uint32_t g_sampling_start_psoc_bytes = 0;
static uint32_t g_sampling_start_batches_ok = 0;
static uint32_t g_hotwait_enter_ms = 0;
static uint8_t  g_hotwait_rearm_count = 0;
static bool     g_start_fallback_sent = false;
static uint8_t  g_completion_ack_sub_cmd = 0;
static uint8_t  g_completion_ack_ok = 0;
static uint8_t  g_completion_ack_retries_left = 0;
static uint32_t g_completion_ack_next_ms = 0;

static volatile uint32_t g_psoc_rx_edges = 0;
static volatile uint8_t  g_psoc_rx_last_level = 0;

#ifndef PSOC_RX_SCAN_ENABLE
  #define PSOC_RX_SCAN_ENABLE 0
#endif

#if PSOC_RX_SCAN_ENABLE
struct GpioScanSlot {
    uint8_t pin;
    volatile uint32_t edges;
    volatile uint8_t level;
};

/* Solo GPIO libres: fuera memoria, USB, consola, arranque y cableado. */
static GpioScanSlot g_rx_scan[] = {
    {1, 0, 0}, {2, 0, 0}, {4, 0, 0}, {47, 0, 0}, {48, 0, 0},
};

static void IRAM_ATTR onRxScan0() { g_rx_scan[0].edges++; g_rx_scan[0].level = (uint8_t)digitalRead(g_rx_scan[0].pin); }
static void IRAM_ATTR onRxScan1() { g_rx_scan[1].edges++; g_rx_scan[1].level = (uint8_t)digitalRead(g_rx_scan[1].pin); }
static void IRAM_ATTR onRxScan2() { g_rx_scan[2].edges++; g_rx_scan[2].level = (uint8_t)digitalRead(g_rx_scan[2].pin); }
static void IRAM_ATTR onRxScan3() { g_rx_scan[3].edges++; g_rx_scan[3].level = (uint8_t)digitalRead(g_rx_scan[3].pin); }
static void IRAM_ATTR onRxScan4() { g_rx_scan[4].edges++; g_rx_scan[4].level = (uint8_t)digitalRead(g_rx_scan[4].pin); }

static void (* const g_rx_scan_isr[])(void) = {
    onRxScan0, onRxScan1, onRxScan2, onRxScan3, onRxScan4,
};
static_assert(sizeof(g_rx_scan_isr) / sizeof(g_rx_scan_isr[0]) ==
              sizeof(g_rx_scan) / sizeof(g_rx_scan[0]), "un ISR por pin");

static bool rxScanIsReservedPin(uint8_t pin)
{
    if (pin == (uint8_t)PSOC_I2C_SDA ||
        pin == (uint8_t)PSOC_I2C_SCL ||
        pin == (uint8_t)PSOC_RESET_REQUEST_PIN ||
        pin == (uint8_t)PSOC_UART_RX ||
        pin == (uint8_t)PSOC_UART_TX ||
        pin == (uint8_t)SYNC_TO_PSOC_PIN ||
        pin == (uint8_t)DEBUG_HW_START_PIN) {
        return true;
    }
#if defined(LOCAL_UI_ENABLE) && LOCAL_UI_ENABLE
    if (pin == (uint8_t)LOCAL_OLED_SCK_PIN ||
        pin == (uint8_t)LOCAL_OLED_MOSI_PIN ||
        pin == (uint8_t)LOCAL_OLED_CS_PIN ||
        pin == (uint8_t)LOCAL_OLED_DC_PIN ||
        pin == (uint8_t)LOCAL_OLED_RESET_PIN ||
        pin == (uint8_t)LOCAL_BTN_UP_PIN ||
        pin == (uint8_t)LOCAL_BTN_DOWN_PIN ||
        pin == (uint8_t)LOCAL_BTN_OK_PIN ||
        pin == (uint8_t)LOCAL_BTN_BACK_PIN) {
        return true;
    }
#endif
    return false;
}

static void beginPsocRxScan()
{
    for (size_t i = 0; i < (sizeof(g_rx_scan) / sizeof(g_rx_scan[0])); i++) {
        const uint8_t pin = g_rx_scan[i].pin;
        if (rxScanIsReservedPin(pin)) {
            continue;
        }
        pinMode(pin, INPUT_PULLUP);
        g_rx_scan[i].level = (uint8_t)digitalRead(pin);
        attachInterrupt(digitalPinToInterrupt(pin), g_rx_scan_isr[i], CHANGE);
    }
    SLAVE_LOG_PRINTLN("[SLAVE] RX_SCAN enabled");
}

static void logPsocRxScan()
{
    noInterrupts();
    for (size_t i = 0; i < (sizeof(g_rx_scan) / sizeof(g_rx_scan[0])); i++) {
        SLAVE_LOG_PRINTF("[SLAVE] RX_SCAN p%u=%lu/%u\n",
                         (unsigned)g_rx_scan[i].pin,
                         (unsigned long)g_rx_scan[i].edges,
                         (unsigned)g_rx_scan[i].level);
    }
    interrupts();
}
#endif

/* ── Objetos ─────────────────────────────────────────────────────────────── */
static PsocUART       psoc;
static EspNowTransport transport;

/* Helpers de pulsos GPIO de osciloscopio (módulo aparte). */
#include "scope_pulse.h"

static bool sameMac(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, 6) == 0;
}

static bool usableUnicastMac(const uint8_t mac[6])
{
    static const uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
    static const uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    return !sameMac(mac, zero) && !sameMac(mac, broadcast);
}

static bool masterMacIsDefaultBroadcast()
{
    return !usableUnicastMac(MASTER_MAC_DEFAULT);
}

/* true si la trama puede venir del maestro: mismo equipo que el AP escaneado
 * (STA y softAP del ESP32 difieren en pocas unidades). Sin escaneo, se acepta. */
static bool macIsMaster(const uint8_t mac[6])
{
    if (!g_masterApKnown) return true;
    /* El S3 arma la MAC del AP prendiendo el bit de administrada localmente
     * sobre la de la STA (e2:.. contra e0:..), en vez de sumar uno al final. */
    if (((mac[0] ^ g_masterApBssid[0]) & ~0x02u) == 0u &&
        memcmp(&mac[1], &g_masterApBssid[1], 5) == 0) {
        return true;
    }
    uint64_t a = 0, b = 0;
    for (int i = 0; i < 6; i++) {
        a = (a << 8) | mac[i];
        b = (b << 8) | g_masterApBssid[i];
    }
    const uint64_t d = (a > b) ? (a - b) : (b - a);
    return d <= 3u;
}

static void learnMasterMac(const uint8_t mac[6])
{
    if (!usableUnicastMac(mac) || sameMac(g_masterTxMac, mac)) return;
    if (!transport.setMasterMac(mac)) {
        SLAVE_LOG_PRINTLN("[SLAVE] master MAC learn failed");
        return;
    }
    memcpy(g_masterTxMac, mac, 6);
    SLAVE_LOG_PRINTF("[SLAVE] master MAC learned %02X:%02X:%02X:%02X:%02X:%02X%s\n",
                     g_masterTxMac[0], g_masterTxMac[1], g_masterTxMac[2],
                     g_masterTxMac[3], g_masterTxMac[4], g_masterTxMac[5],
                     masterMacIsDefaultBroadcast() ? " (auto)" : "");
}

static void serviceRadioSilencio(bool desdeLoop);
static void iniciarTimerSyncRadioSilencio();
static void subirSyncConRadioSilencio();

static bool syncModeLive();
static esp_err_t sendToMaster(const uint8_t *data, size_t len)
{
    /* FTM pausado: nada sale al aire mientras se muestrea. */
    if (g_syncMode == SYNCMODE_FTM_PAUSED && g_state == SAMPLING && !syncModeLive()) {
        g_syncPausedDrops++;
        return ESP_OK;
    }
    /* El ACK de fin de captura sale apenas se deja SAMPLING, antes de que el
       loop vuelva a prender la radio callada por radiosil. */
    serviceRadioSilencio(false);
    return espnowSend(g_masterTxMac, data, len);
}

static void sendCfgAck(uint8_t sub_cmd, uint8_t ok);
static void sendCompletionAckWithRetry(uint8_t sub_cmd);
static void allocStore(uint16_t n_batches);
static void enterHotWait(uint16_t n_batches);
static uint16_t clampPsocCaptureBatches(uint16_t n);
static uint32_t expectedCaptureMs(uint16_t n_batches);
static bool storeReadyForHotWait();
static void serviceHotWaitWatchdog();
static void debugEspSetRamp(bool enable);
static SampleBytes *storeBatchPtr(uint16_t seq);

static void IRAM_ATTR onPsocRxEdge()
{
    g_psoc_rx_edges++;
    g_psoc_rx_last_level = (uint8_t)digitalRead(PSOC_UART_RX);
}

static void logPsocUartDiag(const char *tag)
{
    const unsigned long bytes = (unsigned long)psoc.bytesRx();
    const unsigned long mark  = (unsigned long)psoc.markersRx();
    const unsigned long drop  = (unsigned long)psoc.syncDrops();
    const unsigned long badLn = (unsigned long)psoc.badLen();
    const unsigned long ping  = (unsigned long)psoc.pingsRx();
    const unsigned long diag  = (unsigned long)psoc.diagEventsRx();
    const int rxLevel = digitalRead(PSOC_UART_RX);
    uint32_t rxEdges;
    uint8_t rxEdgeLevel;

    noInterrupts();
    rxEdges = g_psoc_rx_edges;
    rxEdgeLevel = g_psoc_rx_last_level;
    interrupts();

    if (psoc.hasLastByte()) {
        SLAVE_LOG_PRINTF(
            "[SLAVE %d] %s bOK=%lu bBad=%lu uartBytes=%lu mark=%lu drop=%lu badLen=%lu ping=%lu diag=%lu rx=%d edge=%lu/%u last=0x%02X age=%lu txOK=%lu txFail=%lu state=%d fill=%u/%u fs=%u\n",
            NODE_ID, tag,
            (unsigned long)psoc.batchesOK(), (unsigned long)psoc.batchesBad(),
            bytes, mark, drop, badLn, ping, diag, rxLevel,
            (unsigned long)rxEdges, (unsigned)rxEdgeLevel,
            psoc.lastByte(), (unsigned long)psoc.lastByteAgeMs(),
            (unsigned long)transport.sentOK(), (unsigned long)transport.sentFail(),
            (int)g_state, (unsigned)g_store_fill, (unsigned)g_rec_n_batches,
            (unsigned)psoc.sampleRate());
    } else {
        SLAVE_LOG_PRINTF(
            "[SLAVE %d] %s bOK=%lu bBad=%lu uartBytes=%lu mark=%lu drop=%lu badLen=%lu ping=%lu diag=%lu rx=%d edge=%lu/%u last=none txOK=%lu txFail=%lu state=%d fill=%u/%u fs=%u\n",
            NODE_ID, tag,
            (unsigned long)psoc.batchesOK(), (unsigned long)psoc.batchesBad(),
            bytes, mark, drop, badLn, ping, diag, rxLevel,
            (unsigned long)rxEdges, (unsigned)rxEdgeLevel,
            (unsigned long)transport.sentOK(), (unsigned long)transport.sentFail(),
            (int)g_state, (unsigned)g_store_fill, (unsigned)g_rec_n_batches,
            (unsigned)psoc.sampleRate());
    }
}

static const char *psocDiagName(uint8_t event)
{
    switch (event) {
        case PSOC_EVT_BOOT:           return "BOOT";
        case PSOC_EVT_ANALOG_READY:   return "ANALOG_READY";
        case PSOC_EVT_CAL_START:      return "CAL_START";
        case PSOC_EVT_CAL_DONE:       return "CAL_DONE";
        case PSOC_EVT_CAL_BUSY:       return "CAL_BUSY";
        case PSOC_EVT_CAL_STAGE_DAC:  return "CAL_STAGE_DAC";
        case PSOC_EVT_CAL_STAGE_MEAS: return "CAL_STAGE_MEAS";
        case PSOC_EVT_CAL_STAGE_BEGIN:return "CAL_STAGE_BEGIN";
        case PSOC_EVT_CAL_STAGE_OK:   return "CAL_STAGE_OK";
        case PSOC_EVT_CAL_VERIFY_BEGIN:return "CAL_VERIFY_BEGIN";
        case PSOC_EVT_CAL_VERIFY_OK:  return "CAL_VERIFY_OK";
        case PSOC_EVT_CAL_AMUX_IN:    return "CAL_AMUX_IN";
        case PSOC_EVT_CAL_PROGRESS:   return "CAL_PROGRESS";
        case PSOC_EVT_CAL_WATCHDOG:   return "CAL_WATCHDOG";
        case PSOC_EVT_CAL_LP_BAD:     return "CAL_LP_BAD";
        case PSOC_EVT_CAL_STAGE_MEAS32: return "CAL_STAGE_MEAS32";
        case PSOC_EVT_SERVO_STAGE:    return "SERVO_STAGE";
        case PSOC_EVT_SERVO_STEP:     return "SERVO_STEP";
        case PSOC_EVT_WAIT_ESP:       return "WAIT_ESP";
        case PSOC_EVT_ESP_SEEN:       return "ESP_SEEN";
        case PSOC_EVT_CAL_LOOP:       return "CAL_LOOP";
        case PSOC_EVT_CAL_STAGE_SAT:  return "CAL_STAGE_SAT";
        case PSOC_EVT_CAL_STAGE_SAT_ALL: return "CAL_STAGE_SAT_ALL";
        case PSOC_EVT_CAL_REALCHECK_BEGIN: return "CAL_REALCHECK_BEGIN";
        case PSOC_EVT_CAL_REALCHECK_DAC: return "CAL_REALCHECK_DAC";
        case PSOC_EVT_CAL_REALCHECK_MEAS32: return "CAL_REALCHECK_MEAS32";
        case PSOC_EVT_CAL_REALCHECK_NUDGE: return "CAL_REALCHECK_NUDGE";
        case PSOC_EVT_CAL_REALCHECK_OK: return "CAL_REALCHECK_OK";
        case PSOC_EVT_CAL_STAGE_TARGET32: return "CAL_STAGE_TARGET32";
        case PSOC_EVT_CAL_SWEEP_DAC: return "CAL_SWEEP_DAC";
        case PSOC_EVT_CAL_SWEEP_MEAS32: return "CAL_SWEEP_MEAS32";
        case PSOC_EVT_ADC_SNAPSHOT_BEGIN: return "ADC_SNAPSHOT_BEGIN";
        case PSOC_EVT_ADC_RAW32: return "ADC_RAW32";
        case PSOC_EVT_ADC_FILT32: return "ADC_FILT32";
        case PSOC_EVT_RX_CMD:         return "RX_CMD";
        case PSOC_EVT_SETN:           return "SETN";
        case PSOC_EVT_ARMED:          return "ARMED";
        case PSOC_EVT_SYNC_RISE:      return "SYNC_RISE";
        case PSOC_EVT_SYNC_FALL:      return "SYNC_FALL";
        case PSOC_EVT_SAMPLING_START: return "SAMPLING_START";
        case PSOC_EVT_CAPTURE_DONE:   return "CAPTURE_DONE";
        case PSOC_EVT_DUMP_START:     return "DUMP_START";
        case PSOC_EVT_DUMP_DONE:      return "DUMP_DONE";
        case PSOC_EVT_CHAIN_NEXT:     return "CHAIN_NEXT";
        case PSOC_EVT_START_NOW:      return "START_NOW";
        case PSOC_EVT_DEBUG_MODE:     return "DEBUG_MODE";
        case PSOC_EVT_STATUS_REQ:     return "STATUS_REQ";
        case PSOC_EVT_BUTTON:         return "BUTTON";
        case PSOC_EVT_BUTTON_IGNORED: return "BUTTON_IGNORED";
        case PSOC_EVT_CAPTURE_CLAMPED:return "CAPTURE_CLAMPED";
        case PSOC_EVT_CAL_PI_GAIN32:  return "CAL_PI_GAIN32";
        case PSOC_EVT_CAL_PI_DEADBAND:return "CAL_PI_DEADBAND";
        case PSOC_EVT_CAL_PI_ERROR32: return "CAL_PI_ERROR32";
        case PSOC_EVT_CAL_PI_BUCKET32:return "CAL_PI_BUCKET32";
        case PSOC_EVT_CAL_PI_STABLE:  return "CAL_PI_STABLE";
        case PSOC_EVT_CAL_AMUX_CAP:   return "CAL_AMUX_CAP";
        case PSOC_EVT_ARMED_TIMEOUT:  return "ARMED_TIMEOUT";
        case PSOC_EVT_SD_DIAG_STAGE:  return "SD_DIAG_STAGE";
        case PSOC_EVT_SD_DIAG_R1:     return "SD_DIAG_R1";
        case PSOC_EVT_SD_DIAG_PINS:   return "SD_DIAG_PINS";
        default:                      return "UNKNOWN";
    }
}

static const char *psocStateName(uint8_t state)
{
    switch (state) {
        case 0:  return "IDLE";
        case 1:  return "ARMED";
        case 2:  return "SAMPLING";
        case 3:  return "CALIBRATING";
        default: return "UNKNOWN";
    }
}

static uint8_t g_psoc_hw_class = 0xFFu;

/* Estado de la SD del PSoC (2026-07-11): la SD vive en el PSoC (SPI Master de
 * TopDesign), NO en el ESP32 — sd_storage.* queda obsoleto/stub. El PSoC
 * reporta PSOC_EVT_SD_STATUS en boot y ante PSOC_CMD_SD_STATUS; con firmware
 * PSoC viejo (sin 0xBC) no llega nunca → sd_present=0. */
static uint8_t g_psoc_sd_status = 0u;   /* +bit4 FAT montado, bit5 sesión COMPLETE */
static bool    g_psoc_sd_known  = false;

static uint8_t psocSdPresent() { return (uint8_t)(g_psoc_sd_status & 0x01u); }

static const char *psocHwName(uint8_t hwClass)
{
    switch (hwClass) {
        case 0:  return "GEO";
        case 1:  return "HAMMER";
        default: return "UNKNOWN";
    }
}

static const char *psocCalStageName(uint8_t stage)
{
    if (g_psoc_hw_class == 1u) {
        switch (stage) {
            case 0:  return "HAMMER_PGA";
            case 1:  return "HAMMER_LP";
            default: return "UNKNOWN";
        }
    } else {
        switch (stage) {
            case 0:  return "GEO_PGA";
            case 1:  return "GEO_BP";
            case 2:  return "GEO_ADDER";
            case 3:  return "GEO_LP";
            default: return "UNKNOWN";
        }
    }
}

static uint32_t absCounts32(int32_t value)
{
    return (value < 0) ? (uint32_t)(-value) : (uint32_t)value;
}

/* Cuentas del ADC centradas, o sea la DESVIACION que el lazo controla.
 *
 * Tiene que dar lo mismo que cal_pi_compare_counts() del PSoC
 * (calibration.c), porque las dos son la misma funcion escrita dos veces. No
 * daba: la del PSoC resta CAL_TARGET_1V_COUNTS para GEO y esta devolvia el
 * valor crudo. Consecuencia: una calibracion convergida a menos de 0,5 mV se
 * informaba como un error de -1000 mV, porque el log restaba el objetivo (0)
 * al nivel ABSOLUTO del tap en vez de a su desviacion.
 *
 * No es cosmetico: estos son los unicos numeros por los que se puede ver que
 * hizo la calibracion, y decian exactamente lo contrario de lo que paso.
 *
 * Los taps GEO descansan fisicamente sobre Vref ~= 1 V y el ADC entrega el
 * nivel absoluto; el cero que se calibra es la componente diferencial
 * alrededor de ese modo comun. En HAMMER no se centra, se toma el modulo,
 * igual que en el PSoC. */
#define PSOC_CAL_TARGET_1V_COUNTS 52429L   /* = CAL_TARGET_1V_COUNTS del PSoC */

static int32_t psocCalCompareCounts(int32_t raw)
{
    return (g_psoc_hw_class == 1u) ? (int32_t)absCounts32(raw)
                                   : (raw - PSOC_CAL_TARGET_1V_COUNTS);
}

static int32_t adcCountsToMv(int32_t counts)
{
    const int64_t scaled = (int64_t)counts * 1000LL;
    if (scaled >= 0) {
        return (int32_t)((scaled + 26214LL) / 52429LL);
    }
    return (int32_t)((scaled - 26214LL) / 52429LL);
}

static int16_t clampInt16(int32_t value)
{
    if (value > 32767L) return 32767;
    if (value < -32768L) return -32768;
    return (int16_t)value;
}

static void sendCfgAckS16(uint8_t hiBase, uint8_t loBase, uint8_t stage, int32_t value)
{
    const int16_t clipped = clampInt16(value);
    const uint16_t raw = (uint16_t)clipped;
    sendCfgAck((uint8_t)(hiBase + stage), (uint8_t)((raw >> 8) & 0xFFu));
    sendCfgAck((uint8_t)(loBase + stage), (uint8_t)(raw & 0xFFu));
}

static void labPingRecord(uint8_t v);
static void onPsocDiag(const PsocDiagEvent &event)
{
#if SLAVE_LAB_TOOLS_ENABLE
    if (event.event == PSOC_EVT_LAB_PING) { labPingRecord(event.value); return; }
#endif
    static uint8_t calStage = 0xFF;
    static uint8_t calDac = 0;
    static int16_t calMeas = 0;
    static bool calMeasHigh = false;
    static uint32_t calMeasRaw = 0;
    static int32_t calMeasRawSigned = 0;
    static uint8_t calMeasRawByte = 0;
    static uint32_t calTargetRaw = 0;
    static int32_t calTargetSigned = 0;
    static uint8_t calTargetRawByte = 0;
    static bool calTargetValid = false;
    static uint32_t calSweepRaw = 0;
    static uint8_t calSweepRawByte = 0;
    static uint32_t adcSnapshotRaw = 0;
    static uint8_t adcSnapshotRawByte = 0;
    static uint32_t calPiRaw = 0;
    static uint8_t calPiRawByte = 0;
    static uint8_t calPiRawEvent = 0;
    static uint16_t calPointIndex = 0;
    g_psocConnected = true;

    if (g_state == SAMPLING &&
        event.event != PSOC_EVT_DUMP_DONE &&
        event.event != PSOC_EVT_CAPTURE_DONE &&
        event.event != PSOC_EVT_SD_SESSION &&
        event.event != PSOC_EVT_SD_ERROR &&
        event.event != PSOC_EVT_ARMED_TIMEOUT &&
        event.event != PSOC_EVT_LIVE_DONE &&
        event.event != PSOC_EVT_LIVE_OVERRUN &&
        event.event != PSOC_EVT_LIVE_TOTAL1 &&
        event.event != PSOC_EVT_LIVE_TOTAL2) {
        return;
    }

    if (event.event == PSOC_EVT_LIVE_TOTAL1 || event.event == PSOC_EVT_LIVE_TOTAL2) {
        live::onPsocTotalByte(event.event == PSOC_EVT_LIVE_TOTAL1 ? 1u : 2u, event.value);
        return;
    }
    if (event.event == PSOC_EVT_LIVE_OVERRUN) {
        live::onPsocOverrun();
        SLAVE_LOG_PRINTLN("[LIVE] PSoC: anillo lleno, la captura paro");
        return;
    }
    if (event.event == PSOC_EVT_LIVE_DONE) {
        live::onPsocDone(event.value);
        digitalWrite(SYNC_TO_PSOC_PIN, LOW);
        if (g_state == SAMPLING) g_state = STOPPED;
        return;
    }

    if (event.event == PSOC_EVT_ARMED_TIMEOUT) {
        const bool hadCapture = g_view_store_active || g_start_store_active;
        const uint8_t failedSubCmd = g_view_store_active ? CMD_VIEW : CMD_START;
        digitalWrite(SYNC_TO_PSOC_PIN, LOW);
        g_view_store_active = false;
        g_start_store_active = false;
        g_psoc_sd_store_active = false;
        g_psoc_sd_session_complete = false;
        g_psoc_sd_capture_error = true;
        g_store_fill = 0u;
        g_state = STOPPED;
        if (hadCapture) {
            sendCfgAck(failedSubCmd, 0u);
        }
        SLAVE_LOG_PRINTLN("[SLAVE] PSoC ARMED timeout -> STOPPED");
        LOGM("PSOC_ARMED_TIMEOUT", "capture=%u,sub=0x%02X",
             (unsigned)hadCapture, (unsigned)failedSubCmd);
        return;
    }

    if (g_psoc_sd_store_active && g_state == SAMPLING) {
        if (event.event == PSOC_EVT_SD_ERROR) {
            g_psoc_sd_capture_error = true;
        } else if (event.event == PSOC_EVT_SD_SESSION) {
            g_psoc_sd_session_complete = (event.value == 1u);
            if (!g_psoc_sd_session_complete) g_psoc_sd_capture_error = true;
        } else if (event.event == PSOC_EVT_DUMP_DONE &&
                   (g_view_store_active || g_start_store_active)) {
            const bool wasView = g_view_store_active;
            const uint8_t doneSubCmd = wasView ? CMD_VIEW : CMD_START;
            const bool ok = g_psoc_sd_session_complete && !g_psoc_sd_capture_error;
            digitalWrite(SYNC_TO_PSOC_PIN, LOW);
            g_view_store_active = false;
            g_start_store_active = false;
            g_store_fill = ok ? g_rec_n_batches : 0u;
            g_state = STOPPED;
            if (ok) sendCompletionAckWithRetry(doneSubCmd);
            else sendCfgAck(doneSubCmd, 0u);
            SLAVE_LOG_PRINTF("[SLAVE] %s SD complete=%u err=%u fill=%u/%u\n",
                             wasView ? "VIEW" : "START",
                             (unsigned)g_psoc_sd_session_complete,
                             (unsigned)g_psoc_sd_capture_error,
                             (unsigned)g_store_fill, (unsigned)g_rec_n_batches);
            LOGM("CAPTURE_SD_DONE", "mode=%s,ok=%u,fill=%u,n=%u",
                 wasView ? "VIEW" : "START", (unsigned)ok,
                 (unsigned)g_store_fill, (unsigned)g_rec_n_batches);
            return;
        }
    }

    if (!g_psoc_sd_store_active && event.event == PSOC_EVT_DUMP_DONE &&
        (g_view_store_active || g_start_store_active) &&
        g_state == SAMPLING && g_store_fill < g_rec_n_batches) {
        const bool wasView = g_view_store_active;
        const uint8_t doneSubCmd = wasView ? CMD_VIEW : CMD_START;
        const char *doneName = wasView ? "VIEW" : "START";
        digitalWrite(SYNC_TO_PSOC_PIN, LOW);
        g_view_store_active = false;
        g_start_store_active = false;
        g_state = STOPPED;
        sendCfgAck(doneSubCmd, 0);
        SLAVE_LOG_PRINTF("[SLAVE] %s partial dump fill=%u/%u -> FAIL\n",
                         doneName, (unsigned)g_store_fill, (unsigned)g_rec_n_batches);
        LOGM("CAPTURE_PARTIAL", "mode=%s,fill=%u,n=%u,bOK=%lu,bBad=%lu,drop=%lu,bytes=%lu",
             doneName,
             (unsigned)g_store_fill, (unsigned)g_rec_n_batches,
             (unsigned long)psoc.batchesOK(),
             (unsigned long)psoc.batchesBad(),
             (unsigned long)psoc.syncDrops(),
             (unsigned long)psoc.bytesRx());
    }

    if (event.event == PSOC_EVT_BOOT) {
        g_psoc_hw_class = event.value;
        g_psoc_live_known = -1;
        scheduleBootGains();
        SLAVE_LOG_PRINTF("[PSoC] boot hw=%u/%s pstate=%u/%s\n",
                         event.value, psocHwName(event.value),
                         event.psoc_state, psocStateName(event.psoc_state));
        LOGM("PSOC_BOOT", "hw=%u,hwName=%s,pstate=%u,pstateName=%s",
             event.value, psocHwName(event.value),
             event.psoc_state, psocStateName(event.psoc_state));
    }

    if (event.event == PSOC_EVT_SD_DIAG_STAGE) {
        const uint8_t stage = event.value & 0x7Fu;
        const char *stageName = "UNKNOWN";
        switch (stage) {
            case 1u: stageName = "CLOCKS"; break;
            case 2u: stageName = "CMD0"; break;
            case 3u: stageName = "CMD8"; break;
            case 4u: stageName = "ACMD41/CMD1"; break;
            case 5u: stageName = "CMD58"; break;
            case 6u: stageName = "CMD16"; break;
            case 7u: stageName = "CSD"; break;
            case 8u: stageName = "CAPACITY"; break;
            case 9u: stageName = "READY"; break;
            default: break;
        }
        SLAVE_LOG_PRINTF("[PSoC] SD diag stage=%u/%s spi_timeout=%u\n",
                         (unsigned)stage, stageName,
                         (unsigned)((event.value >> 7) & 1u));
        LOGM("PSOC_SD_DIAG", "stage=%u,stageName=%s,spiTimeout=%u",
             (unsigned)stage, stageName,
             (unsigned)((event.value >> 7) & 1u));
    } else if (event.event == PSOC_EVT_SD_DIAG_R1) {
        SLAVE_LOG_PRINTF("[PSoC] SD diag last_r1=0x%02X%s\n",
                         event.value,
                         (event.value == 0xFFu) ? " (sin respuesta)" : "");
        LOGM("PSOC_SD_DIAG", "lastR1=0x%02X", event.value);
    } else if (event.event == PSOC_EVT_SD_DIAG_PINS) {
        SLAVE_LOG_PRINTF("[PSoC] SD diag pins=0x%02X MISO=%s SCK=%s MOSI=%s CS=%s\n",
                         event.value,
                         ((event.value & 0x03u) == 0x03u) ? "toggle" :
                         ((event.value & 0x02u) ? "high" : "low"),
                         ((event.value & 0x0Cu) == 0x0Cu) ? "toggle" :
                         ((event.value & 0x08u) ? "high" : "low"),
                         ((event.value & 0x30u) == 0x30u) ? "toggle" :
                         ((event.value & 0x20u) ? "high" : "low"),
                         ((event.value & 0xC0u) == 0xC0u) ? "toggle" :
                         ((event.value & 0x80u) ? "high" : "low"));
        LOGM("PSOC_SD_DIAG", "pins=0x%02X", event.value);
    } else if (event.event == PSOC_EVT_SD_STATUS) {
        g_psoc_sd_status = event.value;
        g_psoc_sd_known = true;
        g_psoc_sd_capture_enabled = (event.value & 0x40u) != 0u;
        if (!g_sd_overridden && !g_sd_sync_pending) {
            g_sd_user_enabled = g_psoc_sd_capture_enabled;
        }
        SLAVE_LOG_PRINTF("[PSoC] SD status=0x%02X present=%u type=%u selftest=%u\n",
                         event.value, (unsigned)(event.value & 1u),
                         (unsigned)((event.value >> 1) & 3u),
                         (unsigned)((event.value >> 3) & 1u));
        LOGM("PSOC_SD", "status=0x%02X,present=%u", event.value, (unsigned)(event.value & 1u));
    } else if (event.event == PSOC_EVT_SD_SESSION || event.event == PSOC_EVT_SD_ERROR) {
        SLAVE_LOG_PRINTF("[PSoC] %s val=0x%02X\n",
                         (event.event == PSOC_EVT_SD_SESSION) ? "SD_SESSION" : "SD_ERROR",
                         event.value);
        LOGM("PSOC_SD", "evt=0x%02X,val=0x%02X", event.event, event.value);
    }

    if (event.event == PSOC_EVT_SETN) {
        g_psoc_last_setn_value = event.value;
        g_psoc_last_setn_ms = millis();
        g_psoc_setn_seq++;
        SLAVE_LOG_PRINTF("[PSoC] SETN ack val=%u pstate=%u/%s\n",
                         event.value, event.psoc_state, psocStateName(event.psoc_state));
    } else if (event.event == PSOC_EVT_ARMED ||
               event.event == PSOC_EVT_SAMPLING_START ||
               event.event == PSOC_EVT_CHAIN_NEXT ||
               event.event == PSOC_EVT_CAPTURE_DONE ||
               event.event == PSOC_EVT_DUMP_START ||
               event.event == PSOC_EVT_DUMP_DONE ||
               event.event == PSOC_EVT_CAPTURE_WATCHDOG) {
        SLAVE_LOG_PRINTF("[PSoC] %s val=%u pstate=%u/%s fill=%u/%u\n",
                         psocDiagName(event.event), event.value,
                         event.psoc_state, psocStateName(event.psoc_state),
                         (unsigned)g_store_fill, (unsigned)g_rec_n_batches);
    }
    if (event.event == PSOC_EVT_ARMED) {
        g_psoc_arm_ready = true;
    }

    if (event.event == PSOC_EVT_CAL_START) {
        calStage = 0;
        calDac = 0;
        calMeas = 0;
        calMeasHigh = false;
        calMeasRaw = 0;
        calMeasRawSigned = 0;
        calMeasRawByte = 0;
        calTargetRaw = 0;
        calTargetSigned = 0;
        calTargetRawByte = 0;
        calTargetValid = false;
        adcSnapshotRaw = 0;
        adcSnapshotRawByte = 0;
        calPiRaw = 0;
        calPiRawByte = 0;
        calPiRawEvent = 0;
        calPointIndex = 0;
        g_cal_progress_ack_ms = millis();
        sendCfgAck(PSOC_CMD_CALIBRATE, 2);
        SLAVE_LOG_PRINTF("[CAL] start\n");
        LOGM("CAL_START", "pstate=%u", event.psoc_state);
    } else if (event.event == PSOC_EVT_CAL_DONE) {
        if (!(g_cfg_waiting && g_cfg_sub_cmd == PSOC_CMD_CALIBRATE)) {
            sendCfgAck(PSOC_CMD_CALIBRATE, event.value ? 1 : 0);
        }
        SLAVE_LOG_PRINTF("[CAL] done ok=%u\n", event.value);
        LOGM("CAL_DONE", "ok=%u,pstate=%u", event.value, event.psoc_state);
    } else if (event.event == PSOC_EVT_CAL_BUSY) {
        SLAVE_LOG_PRINTF("[CAL] busy\n");
        LOGM("CAL_BUSY", "pstate=%u", event.psoc_state);
    } else if (event.event == PSOC_EVT_CAL_STAGE_BEGIN) {
        calStage = event.value;
        calDac = 0;
        calMeas = 0;
        calMeasHigh = false;
        calMeasRaw = 0;
        calMeasRawSigned = 0;
        calMeasRawByte = 0;
        calTargetRaw = 0;
        calTargetSigned = 0;
        calTargetRawByte = 0;
        calTargetValid = false;
        adcSnapshotRaw = 0;
        adcSnapshotRawByte = 0;
        calPiRaw = 0;
        calPiRawByte = 0;
        calPiRawEvent = 0;
        calPointIndex = 0;
        g_cal_progress_ack_ms = millis();
        sendCfgAck(PSOC_CMD_CALIBRATE, (uint8_t)(3u + (event.value & 0x03u)));
        SLAVE_LOG_PRINTF("[CAL] begin stage=%u/%s\n",
                         event.value, psocCalStageName(event.value));
        LOGM("CAL_BEGIN", "stage=%u,name=%s", event.value,
             psocCalStageName(event.value));
    } else if (event.event == PSOC_EVT_SERVO_STAGE) {
        calStage = event.value;
        calDac = 0;
        calMeas = 0;
        calMeasHigh = false;
        calMeasRaw = 0;
        calMeasRawSigned = 0;
        calMeasRawByte = 0;
        calPointIndex = 0;
        SLAVE_LOG_PRINTF("[CAL] servo stage=%u/%s\n",
                         event.value, psocCalStageName(event.value));
        LOGM("SERVO_STAGE", "stage=%u,name=%s", event.value,
             psocCalStageName(event.value));
    } else if (event.event == PSOC_EVT_CAL_VERIFY_BEGIN) {
        calStage = event.value;
        calDac = 0;
        calMeas = 0;
        calMeasHigh = false;
        calMeasRaw = 0;
        calMeasRawSigned = 0;
        calMeasRawByte = 0;
        calPointIndex = 0;
        g_cal_progress_ack_ms = millis();
        sendCfgAck(PSOC_CMD_CALIBRATE, (uint8_t)(7u + (event.value & 0x03u)));
        SLAVE_LOG_PRINTF("[CAL] verify begin stage=%u/%s\n",
                         event.value, psocCalStageName(event.value));
        LOGM("CAL_VERIFY_BEGIN", "stage=%u,name=%s", event.value,
             psocCalStageName(event.value));
    } else if (event.event == PSOC_EVT_CAL_REALCHECK_BEGIN) {
        calStage = event.value;
        calDac = 0;
        calMeas = 0;
        calMeasHigh = false;
        calMeasRaw = 0;
        calMeasRawSigned = 0;
        calMeasRawByte = 0;
        calPointIndex = 0;
        g_cal_progress_ack_ms = millis();
        sendCfgAck(PSOC_CMD_CALIBRATE, (uint8_t)(11u + (event.value & 0x03u)));
        SLAVE_LOG_PRINTF("[CAL] realcheck begin stage=%u/%s\n",
                         event.value, psocCalStageName(event.value));
        LOGM("CAL_REALCHECK_BEGIN", "stage=%u,name=%s", event.value,
             psocCalStageName(event.value));
    } else if (event.event == PSOC_EVT_CAL_STAGE_DAC) {
        calDac = event.value;
    } else if (event.event == PSOC_EVT_CAL_SWEEP_DAC) {
        calDac = event.value;
        calSweepRaw = 0;
        calSweepRawByte = 0;
    } else if (event.event == PSOC_EVT_ADC_SNAPSHOT_BEGIN) {
        calStage = event.value;
        calDac = 0;
        calMeasRaw = 0;
        calMeasRawByte = 0;
        calTargetRaw = 0;
        calTargetRawByte = 0;
        calTargetValid = false;
        calSweepRaw = 0;
        calSweepRawByte = 0;
        adcSnapshotRaw = 0;
        adcSnapshotRawByte = 0;
        calPointIndex = 0;
        SLAVE_LOG_PRINTF("[ADC] snapshot begin stage=%u/%s\n",
                         calStage, psocCalStageName(calStage));
        LOGM("ADC_SNAPSHOT_BEGIN", "stage=%u,name=%s",
             calStage, psocCalStageName(calStage));
    } else if (event.event == PSOC_EVT_CAL_STAGE_TARGET32) {
        calTargetRaw = (calTargetRaw << 8) | event.value;
        calTargetRawByte++;
        if (calTargetRawByte >= 4) {
            calTargetRawByte = 0;
            calTargetSigned = (int32_t)calTargetRaw;
            calTargetRaw = 0;
            calTargetValid = true;
            SLAVE_LOG_PRINTF("[CAL] target stage=%u/%s counts=%ld mV=%ld\n",
                             calStage, psocCalStageName(calStage),
                             (long)calTargetSigned,
                             (long)adcCountsToMv(calTargetSigned));
            LOGM("CAL_TARGET", "stage=%u,name=%s,target=%ld,target_mV=%ld",
                 calStage, psocCalStageName(calStage),
                 (long)calTargetSigned, (long)adcCountsToMv(calTargetSigned));
        }
    } else if (event.event == PSOC_EVT_CAL_SWEEP_MEAS32) {
        calSweepRaw = (calSweepRaw << 8) | event.value;
        calSweepRawByte++;
        if (calSweepRawByte >= 4) {
            int32_t sweepSigned;
            uint32_t absRaw;
            int32_t cmpRaw;
            int32_t errRaw;
            calSweepRawByte = 0;
            sweepSigned = (int32_t)calSweepRaw;
            calSweepRaw = 0;
            absRaw = absCounts32(sweepSigned);
            cmpRaw = psocCalCompareCounts(sweepSigned);
            errRaw = calTargetValid ? (calTargetSigned - cmpRaw) : 0;
            SLAVE_LOG_PRINTF("[CAL] sweep stage=%u/%s dac=%u raw=%ld raw_mV=%ld abs=%lu cmp=%ld cmp_mV=%ld target=%ld target_mV=%ld err=%ld err_mV=%ld\n",
                             calStage, psocCalStageName(calStage), calDac,
                             (long)sweepSigned, (long)adcCountsToMv(sweepSigned),
                             (unsigned long)absRaw,
                             (long)cmpRaw, (long)adcCountsToMv(cmpRaw),
                             calTargetValid ? (long)calTargetSigned : 0L,
                             calTargetValid ? (long)adcCountsToMv(calTargetSigned) : 0L,
                             (long)errRaw, (long)adcCountsToMv(errRaw));
            LOGM("CAL_SWEEP", "stage=%u,name=%s,dac=%u,measRaw=%ld,raw_mV=%ld,abs=%lu,cmp=%ld,cmp_mV=%ld,target=%ld,target_mV=%ld,err=%ld,err_mV=%ld",
                 calStage, psocCalStageName(calStage), calDac,
                 (long)sweepSigned, (long)adcCountsToMv(sweepSigned),
                 (unsigned long)absRaw, (long)cmpRaw, (long)adcCountsToMv(cmpRaw),
                 calTargetValid ? (long)calTargetSigned : 0L,
                 calTargetValid ? (long)adcCountsToMv(calTargetSigned) : 0L,
                 (long)errRaw, (long)adcCountsToMv(errRaw));
        }
    } else if (event.event == PSOC_EVT_ADC_RAW32) {
        adcSnapshotRaw = (adcSnapshotRaw << 8) | event.value;
        adcSnapshotRawByte++;
        if (adcSnapshotRawByte >= 4) {
            int32_t adcSigned;
            uint32_t absRaw;
            int32_t cmpRaw;
            int32_t errRaw;
            adcSnapshotRawByte = 0;
            adcSigned = (int32_t)adcSnapshotRaw;
            adcSnapshotRaw = 0;
            absRaw = absCounts32(adcSigned);
            cmpRaw = psocCalCompareCounts(adcSigned);
            errRaw = calTargetValid ? (calTargetSigned - cmpRaw) : 0;
            SLAVE_LOG_PRINTF("[ADC] snapshot stage=%u/%s dac=%u raw=%ld raw_mV=%ld abs=%lu cmp=%ld cmp_mV=%ld target=%ld target_mV=%ld err=%ld err_mV=%ld\n",
                             calStage, psocCalStageName(calStage), calDac,
                             (long)adcSigned, (long)adcCountsToMv(adcSigned),
                             (unsigned long)absRaw, (long)cmpRaw,
                             (long)adcCountsToMv(cmpRaw),
                             calTargetValid ? (long)calTargetSigned : 0L,
                             calTargetValid ? (long)adcCountsToMv(calTargetSigned) : 0L,
                             (long)errRaw, (long)adcCountsToMv(errRaw));
            LOGM("ADC_SNAPSHOT", "stage=%u,name=%s,dac=%u,raw=%ld,raw_mV=%ld,abs=%lu,cmp=%ld,cmp_mV=%ld,target=%ld,target_mV=%ld,err=%ld,err_mV=%ld",
                 calStage, psocCalStageName(calStage), calDac,
                 (long)adcSigned, (long)adcCountsToMv(adcSigned),
                 (unsigned long)absRaw, (long)cmpRaw,
                 (long)adcCountsToMv(cmpRaw),
                 calTargetValid ? (long)calTargetSigned : 0L,
                 calTargetValid ? (long)adcCountsToMv(calTargetSigned) : 0L,
                 (long)errRaw, (long)adcCountsToMv(errRaw));
        }
    } else if (event.event == PSOC_EVT_CAL_PI_GAIN32 ||
               event.event == PSOC_EVT_CAL_PI_ERROR32 ||
               event.event == PSOC_EVT_CAL_PI_BUCKET32) {
        if (calPiRawByte == 0 || calPiRawEvent != event.event) {
            calPiRaw = 0;
            calPiRawByte = 0;
            calPiRawEvent = event.event;
        }
        calPiRaw = (calPiRaw << 8) | event.value;
        calPiRawByte++;
        if (calPiRawByte >= 4) {
            int32_t piSigned = (int32_t)calPiRaw;
            const char *kind =
                (event.event == PSOC_EVT_CAL_PI_GAIN32) ? "gain_x1000" :
                (event.event == PSOC_EVT_CAL_PI_ERROR32) ? "error_dac" :
                                                           "bucket";
            calPiRaw = 0;
            calPiRawByte = 0;
            calPiRawEvent = 0;
            SLAVE_LOG_PRINTF("[CAL] pi stage=%u/%s %s=%ld\n",
                             calStage, psocCalStageName(calStage),
                             kind, (long)piSigned);
            LOGM("CAL_PI", "stage=%u,name=%s,%s=%ld",
                 calStage, psocCalStageName(calStage),
                 kind, (long)piSigned);
        }
    } else if (event.event == PSOC_EVT_CAL_PI_DEADBAND) {
        SLAVE_LOG_PRINTF("[CAL] pi stage=%u/%s deadband_dac=%u\n",
                         calStage, psocCalStageName(calStage), event.value);
        LOGM("CAL_PI", "stage=%u,name=%s,deadband_dac=%u",
             calStage, psocCalStageName(calStage), event.value);
    } else if (event.event == PSOC_EVT_CAL_PI_STABLE) {
        SLAVE_LOG_PRINTF("[CAL] pi stage=%u/%s stable=%u\n",
                         calStage, psocCalStageName(calStage), event.value);
        LOGM("CAL_PI", "stage=%u,name=%s,stable=%u",
             calStage, psocCalStageName(calStage), event.value);
    } else if (event.event == PSOC_EVT_CAL_STAGE_MEAS) {
        if (!calMeasHigh) {
            calMeas = (int16_t)((uint16_t)event.value << 8);
            calMeasHigh = true;
        } else {
            calMeas = (int16_t)((uint16_t)calMeas | event.value);
            calMeasHigh = false;
#if PSOC_CAL_LOG_CLAMPED_POINTS
            SLAVE_LOG_PRINTF("[CAL] point16 stage=%u/%s dac=%u meas=%d\n",
                             calStage, psocCalStageName(calStage),
                             calDac, (int)calMeas);
            LOGM("CAL_POINT16", "stage=%u,name=%s,dac=%u,meas=%d",
                 calStage, psocCalStageName(calStage), calDac, (int)calMeas);
#endif
        }
    } else if (event.event == PSOC_EVT_CAL_STAGE_MEAS32) {
        calMeasRaw = (calMeasRaw << 8) | event.value;
        calMeasRawByte++;
        if (calMeasRawByte >= 4) {
            uint32_t absRaw;
            int32_t cmpRaw;
            int32_t errRaw;
            calMeasRawByte = 0;
            calMeasRawSigned = (int32_t)calMeasRaw;
            calMeasRaw = 0;
            absRaw = absCounts32(calMeasRawSigned);
            cmpRaw = psocCalCompareCounts(calMeasRawSigned);
            errRaw = calTargetValid ? (calTargetSigned - cmpRaw) : 0;
#if PSOC_CAL_LOG_POINTS
            SLAVE_LOG_PRINTF("[CAL] point stage=%u/%s i=%u dac=%u raw=%ld raw_mV=%ld abs=%lu cmp=%ld cmp_mV=%ld target=%ld target_mV=%ld err=%ld err_mV=%ld\n",
                             calStage, psocCalStageName(calStage),
                             (unsigned)calPointIndex, calDac,
                             (long)calMeasRawSigned, (long)adcCountsToMv(calMeasRawSigned),
                             (unsigned long)absRaw,
                             (long)cmpRaw, (long)adcCountsToMv(cmpRaw),
                             calTargetValid ? (long)calTargetSigned : 0L,
                             calTargetValid ? (long)adcCountsToMv(calTargetSigned) : 0L,
                             (long)errRaw, (long)adcCountsToMv(errRaw));
            LOGM("CAL_POINT32", "stage=%u,name=%s,i=%u,dac=%u,measRaw=%ld,raw_mV=%ld,abs=%lu,cmp=%ld,cmp_mV=%ld,target=%ld,target_mV=%ld,err=%ld,err_mV=%ld",
                 calStage, psocCalStageName(calStage),
                 (unsigned)calPointIndex, calDac, (long)calMeasRawSigned,
                 (long)adcCountsToMv(calMeasRawSigned),
                 (unsigned long)absRaw, (long)cmpRaw, (long)adcCountsToMv(cmpRaw),
                 calTargetValid ? (long)calTargetSigned : 0L,
                 calTargetValid ? (long)adcCountsToMv(calTargetSigned) : 0L,
                 (long)errRaw, (long)adcCountsToMv(errRaw));
#endif
            calPointIndex++;
        }
    } else if (event.event == PSOC_EVT_CAL_REALCHECK_DAC) {
        calDac = event.value;
        calMeasRaw = 0;
        calMeasRawByte = 0;
    } else if (event.event == PSOC_EVT_CAL_REALCHECK_MEAS32) {
        calMeasRaw = (calMeasRaw << 8) | event.value;
        calMeasRawByte++;
        if (calMeasRawByte >= 4) {
            uint32_t absRaw;
            calMeasRawByte = 0;
            calMeasRawSigned = (int32_t)calMeasRaw;
            absRaw = absCounts32(calMeasRawSigned);
#if PSOC_CAL_LOG_POINTS
            SLAVE_LOG_PRINTF("[CAL] realcheck point stage=%u/%s i=%u dac=%u raw=%ld abs=%lu\n",
                             calStage, psocCalStageName(calStage),
                             (unsigned)calPointIndex, calDac,
                             (long)calMeasRawSigned, (unsigned long)absRaw);
            LOGM("CAL_REALCHECK_POINT32", "stage=%u,name=%s,i=%u,dac=%u,measRaw=%ld,abs=%lu",
                 calStage, psocCalStageName(calStage),
                 (unsigned)calPointIndex, calDac, (long)calMeasRawSigned,
                 (unsigned long)absRaw);
#endif
            calPointIndex++;
        }
    } else if (event.event == PSOC_EVT_CAL_STAGE_OK) {
        uint32_t absRaw = absCounts32(calMeasRawSigned);
        int32_t cmpRaw = psocCalCompareCounts(calMeasRawSigned);
        int32_t errRaw = calTargetValid ? (calTargetSigned - cmpRaw) : 0;
        if (calStage < CMD_CAL_VDAC_STAGE_COUNT) {
            sendCfgAck((uint8_t)(CMD_CAL_VDAC_STAGE_BASE + calStage), calDac);
            sendCfgAckS16(CMD_CAL_TARGET_MV_HI_BASE, CMD_CAL_TARGET_MV_LO_BASE,
                          calStage, calTargetValid ? adcCountsToMv(calTargetSigned) : 0);
            sendCfgAckS16(CMD_CAL_ERROR_MV_HI_BASE, CMD_CAL_ERROR_MV_LO_BASE,
                          calStage, adcCountsToMv(errRaw));
        }
        SLAVE_LOG_PRINTF("[CAL] result stage=%u/%s ok=%u dac=%u raw=%ld raw_mV=%ld abs=%lu cmp=%ld cmp_mV=%ld target=%ld target_mV=%ld err=%ld err_mV=%ld meas16=%d\n",
                         calStage, psocCalStageName(calStage), event.value,
                         calDac, (long)calMeasRawSigned,
                         (long)adcCountsToMv(calMeasRawSigned),
                         (unsigned long)absRaw, (long)cmpRaw,
                         (long)adcCountsToMv(cmpRaw),
                         calTargetValid ? (long)calTargetSigned : 0L,
                         calTargetValid ? (long)adcCountsToMv(calTargetSigned) : 0L,
                         (long)errRaw, (long)adcCountsToMv(errRaw), (int)calMeas);
        LOGM("CAL_STAGE", "stage=%u,name=%s,ok=%u,dac=%u,measRaw=%ld,raw_mV=%ld,abs=%lu,cmp=%ld,cmp_mV=%ld,target=%ld,target_mV=%ld,err=%ld,err_mV=%ld,meas16=%d",
             calStage, psocCalStageName(calStage), event.value,
             calDac, (long)calMeasRawSigned,
             (long)adcCountsToMv(calMeasRawSigned),
             (unsigned long)absRaw,
             (long)cmpRaw, (long)adcCountsToMv(cmpRaw),
             calTargetValid ? (long)calTargetSigned : 0L,
             calTargetValid ? (long)adcCountsToMv(calTargetSigned) : 0L,
             (long)errRaw, (long)adcCountsToMv(errRaw),
             (int)calMeas);
    } else if (event.event == PSOC_EVT_SERVO_STEP) {
        SLAVE_LOG_PRINTF("[CAL] servo step stage=%u/%s dac=%u\n",
                         calStage, psocCalStageName(calStage), event.value);
        LOGM("SERVO_STEP", "stage=%u,name=%s,dac=%u",
             calStage, psocCalStageName(calStage), event.value);
    } else if (event.event == PSOC_EVT_CAL_LOOP) {
        SLAVE_LOG_PRINTF("[CAL] loop stage=%u/%s dac=%u -> best candidate\n",
                         calStage, psocCalStageName(calStage), event.value);
        LOGM("CAL_LOOP", "stage=%u,name=%s,dac=%u",
             calStage, psocCalStageName(calStage), event.value);
    } else if (event.event == PSOC_EVT_CAL_STAGE_SAT) {
        if (event.value) {
            SLAVE_LOG_PRINTF("[CAL] saturation stage=%u/%s dac=%u\n",
                             calStage, psocCalStageName(calStage), calDac);
            LOGM("CAL_SAT", "stage=%u,name=%s,dac=%u",
                 calStage, psocCalStageName(calStage), calDac);
        }
    } else if (event.event == PSOC_EVT_CAL_STAGE_SAT_ALL) {
        SLAVE_LOG_PRINTF("[CAL] all candidates saturated stage=%u/%s\n",
                         event.value, psocCalStageName(event.value));
        LOGM("CAL_SAT_ALL", "stage=%u,name=%s", event.value,
             psocCalStageName(event.value));
    } else if (event.event == PSOC_EVT_CAL_REALCHECK_NUDGE) {
        int8_t nudge = (int8_t)event.value;
        SLAVE_LOG_PRINTF("[CAL] realcheck nudge stage=%u/%s dac=%u step=%d\n",
                         calStage, psocCalStageName(calStage), calDac,
                         (int)nudge);
        LOGM("CAL_REALCHECK_NUDGE", "stage=%u,name=%s,dac=%u,step=%d",
             calStage, psocCalStageName(calStage), calDac, (int)nudge);
    } else if (event.event == PSOC_EVT_CAL_REALCHECK_OK) {
        uint32_t absRaw = absCounts32(calMeasRawSigned);
        SLAVE_LOG_PRINTF("[CAL] realcheck result stage=%u/%s ok=%u dac=%u raw=%ld abs=%lu\n",
                         calStage, psocCalStageName(calStage), event.value,
                         calDac, (long)calMeasRawSigned,
                         (unsigned long)absRaw);
        LOGM("CAL_REALCHECK", "stage=%u,name=%s,ok=%u,dac=%u,measRaw=%ld,abs=%lu",
             calStage, psocCalStageName(calStage), event.value,
             calDac, (long)calMeasRawSigned, (unsigned long)absRaw);
    } else if (event.event == PSOC_EVT_CAL_VERIFY_OK) {
        uint32_t absRaw = absCounts32(calMeasRawSigned);
        SLAVE_LOG_PRINTF("[CAL] verify stage=%u/%s ok=%u dac=%u raw=%ld abs=%lu meas16=%d\n",
                         calStage, psocCalStageName(calStage), event.value,
                         calDac, (long)calMeasRawSigned,
                         (unsigned long)absRaw, (int)calMeas);
        LOGM("CAL_VERIFY", "stage=%u,name=%s,ok=%u,dac=%u,measRaw=%ld,abs=%lu,meas16=%d",
             calStage, psocCalStageName(calStage), event.value,
             calDac, (long)calMeasRawSigned, (unsigned long)absRaw,
             (int)calMeas);
    } else if (event.event == PSOC_EVT_CAL_AMUX_IN) {
        SLAVE_LOG_PRINTF("[CAL] AMux signal channel=%u\n", event.value);
        LOGM("CAL_AMUX_IN", "channel=%u", event.value);
    } else if (event.event == PSOC_EVT_CAL_AMUX_CAP) {
        SLAVE_LOG_PRINTF("[CAL] AMux cap channel=%u\n", event.value);
        LOGM("CAL_AMUX_CAP", "channel=%u", event.value);
    } else if (event.event == PSOC_EVT_CAL_PROGRESS) {
        if (!(g_cfg_waiting && g_cfg_sub_cmd == PSOC_CMD_CALIBRATE)) {
            uint32_t now = millis();
            if ((uint32_t)(now - g_cal_progress_ack_ms) >=
                PSOC_CAL_PROGRESS_ACK_PERIOD_MS) {
                g_cal_progress_ack_ms = now;
                sendCfgAck(PSOC_CMD_CALIBRATE, 2);
            }
        }
#if PSOC_CAL_LOG_PROGRESS
        SLAVE_LOG_PRINTF("[CAL] progress stage=%u/%s\n",
                         event.value, psocCalStageName(event.value));
        LOGM("CAL_PROGRESS", "stage=%u,name=%s", event.value,
             psocCalStageName(event.value));
#endif
        /* Para calibracion manual, servicePsocConfigAck() emite el heartbeat
         * mientras g_cfg_waiting este activo. Para boot-cal autonoma no hay
         * pending ACK, asi que este evento mantiene vivo el estado del maestro. */
    } else if (event.event == PSOC_EVT_CAL_WATCHDOG) {
        SLAVE_LOG_PRINTF("[CAL] WATCHDOG timeout stage=%u/%s -> aborted\n",
                         event.value, psocCalStageName(event.value));
        LOGM("CAL_WATCHDOG", "stage=%u,name=%s", event.value,
             psocCalStageName(event.value));
    } else if (event.event == PSOC_EVT_CAL_LP_BAD) {
        SLAVE_LOG_PRINTF("[CAL] CRITICAL stage=%u/%s LP out of range\n",
                         event.value, psocCalStageName(event.value));
        LOGM("CAL_LP_BAD", "stage=%u,name=%s", event.value,
             psocCalStageName(event.value));
    } else if (event.event == PSOC_EVT_BUTTON) {
        SLAVE_LOG_PRINTF("[BUTTON] pressed pstate=%u/%s\n",
                         event.value, psocStateName(event.value));
        LOGM("BUTTON", "pstate=%u,pstateName=%s",
             event.value, psocStateName(event.value));
    } else if (event.event == PSOC_EVT_BUTTON_IGNORED) {
        SLAVE_LOG_PRINTF("[BUTTON] ignored pstate=%u/%s\n",
                         event.value, psocStateName(event.value));
        LOGM("BUTTON_IGNORED", "pstate=%u,pstateName=%s",
             event.value, psocStateName(event.value));
    } else if (event.event == PSOC_EVT_CAPTURE_CLAMPED) {
        SLAVE_LOG_PRINTF("[CAPTURE] clamped requested_sat=%u max=%u\n",
                         event.value, PSOC_CAPTURE_MAX_BATCHES);
        LOGM("CAPTURE_CLAMPED", "requested_sat=%u,max=%u",
             event.value, PSOC_CAPTURE_MAX_BATCHES);
    }

#if PSOC_DIAG_VERBOSE
    const char *eventName = psocDiagName(event.event);
    const char *stateName = psocStateName(event.psoc_state);
    SLAVE_LOG_PRINTF("[SLAVE] PSOC_EVT %s event=0x%02X val=%u pstate=%u/%s estate=%d bytes=%lu bOK=%lu fill=%u/%u\n",
                     eventName, event.event, event.value,
                     event.psoc_state, stateName, (int)g_state,
                     (unsigned long)psoc.bytesRx(),
                     (unsigned long)psoc.batchesOK(),
                     (unsigned)g_store_fill, (unsigned)g_rec_n_batches);
    LOGM("PSOC_EVT", "event=0x%02X,name=%s,val=%u,pstate=%u,pstateName=%s,estate=%d,bytes=%lu,bOK=%lu,fill=%u/%u",
         event.event, eventName, event.value,
         event.psoc_state, stateName, (int)g_state,
         (unsigned long)psoc.bytesRx(),
         (unsigned long)psoc.batchesOK(),
         (unsigned)g_store_fill, (unsigned)g_rec_n_batches);
#endif
}

/* ── Sync (2026-08-03) ───────────────────────────────────────────────────────
 * No hay entrada de sync por cable desde el maestro: el arranque llega SIEMPRE
 * por ESP-NOW (CMD_START / HOT_WAIT), y este esclavo es el que genera el flanco
 * hacia el PSoC por SYNC_TO_PSOC_PIN. La vieja ISR onSyncEdge() sobre
 * SYNC_IN_PIN quedó eliminada junto con el pin: nunca estuvo cableada en campo
 * y sólo agregaba una entrada más para ruido.
 * ────────────────────────────────────────────────────────────────────────── */

/* ── Forward declarations ────────────────────────────────────────────────── */
static void allocStore(uint16_t n_batches);
static void handleReqBatch(const MsgReqBatch *msg);
static void sendHotWaitAck();
static uint8_t g_hotwait_why = HOTWAIT_WHY_NONE;   /* motivo del ultimo NACK de HOT_WAIT */
static void radioSilencioAlEntrar();
static uint16_t effectivePsocSampleRateHz();

/* ── Callbacks ESP-NOW ───────────────────────────────────────────────────── */

static void sendCfgAck(uint8_t sub_cmd, uint8_t ok)
{
    MsgCfgAck ack = { CMD_CFG_ACK, NODE_ID, sub_cmd, ok };
    sendToMaster((const uint8_t *)&ack, sizeof(ack));
}

static void clearCompletionAckRetry()
{
    g_completion_ack_sub_cmd = 0;
    g_completion_ack_ok = 0;
    g_completion_ack_retries_left = 0;
    g_completion_ack_next_ms = 0;
}

static void sendCompletionAckWithRetry(uint8_t sub_cmd)
{
    sendCfgAck(sub_cmd, 2);
    g_completion_ack_sub_cmd = sub_cmd;
    g_completion_ack_ok = 2;
    g_completion_ack_retries_left = COMPLETION_ACK_RETRY_COUNT;
    g_completion_ack_next_ms = millis() + COMPLETION_ACK_RETRY_PERIOD_MS;
}

static void serviceCompletionAckRetry()
{
    if (g_completion_ack_sub_cmd == 0 || g_completion_ack_retries_left == 0) {
        return;
    }
    if ((int32_t)(millis() - g_completion_ack_next_ms) < 0) {
        return;
    }
    sendCfgAck(g_completion_ack_sub_cmd, g_completion_ack_ok);
    g_completion_ack_retries_left--;
    g_completion_ack_next_ms = millis() + COMPLETION_ACK_RETRY_PERIOD_MS;
}

static bool isGainCode(uint8_t code)
{
    return code <= 8u;
}

static bool isAdcConfigCode(uint8_t code)
{
    return code >= 1u && code <= 4u;
}

static bool isDecimationFactor(uint8_t factor)
{
    return factor >= 1u && factor <= 100u;
}

static void applyConfirmedConfig(uint8_t sub_cmd, uint8_t value)
{
    switch (sub_cmd) {
        case 0xA6:
            g_pga_code = value;
            break;
        case PSOC_CMD_PGAOUT:
            g_pgaout = value;
            break;
        case 0xA9:
            g_pgavdac = value;
            break;
        /* 0xAA ya no es un VDAC global de un byte. Su ACK devuelve p1
         * (signo|etapa), no una magnitud que tenga sentido en heartbeat. */
        case PSOC_CMD_SD_CAPTURE:
            if (value > 1u) break;            /* 0xEE = rechazo, no cambió nada */
            g_psoc_sd_capture_enabled = (value != 0u);
            if (g_sd_sync_pending) {
                g_sd_sync_pending = false;
                g_sd_overridden = (g_psoc_sd_capture_enabled != g_sd_user_enabled);
            } else {
                g_sd_user_enabled = g_psoc_sd_capture_enabled;
                g_sd_overridden = false;
            }
            break;
        case PSOC_CMD_SD_TEST:
            /* sdstress (banco): contar el fallo solo mientras el estresor esta
             * activo, para no mezclarlo con un "sdtest" manual suelto. */
            if (value == 0u && g_sdstress_running) {
                g_sdstress_fallos++;
            }
            break;
        case PSOC_CMD_SD_PROBE:
            if (value == 0u && g_sdprobe_running) {
                g_sdprobe_fallos++;
            }
            break;
        case PSOC_CMD_LIVE_MODE:
            g_psoc_live_ack = value;
            g_psoc_live_ack_seq++;
            break;
        default:
            break;
    }
}

static uint8_t expectedPsocAckValue(uint8_t sub_cmd, uint8_t param)
{
    /* SAVE_EEPROM se envia con parametro 0, pero el PSoC responde 1 si grabo bien. */
    return (sub_cmd == PSOC_CMD_SAVE_EEPROM) ? 1u : param;
}

static void waitForPsocConfigAck(uint8_t sub_cmd, uint8_t param)
{
    g_cfg_sub_cmd = sub_cmd;
    g_cfg_param = param;
    g_cfg_start_ms = millis();
    g_cfg_timeout_ms = (sub_cmd == PSOC_CMD_CALIBRATE)
                     ? PSOC_CAL_ACK_TIMEOUT_MS
                     : ((sub_cmd == PSOC_CMD_ADC_SNAPSHOT)
                        ? PSOC_ADC_SNAPSHOT_ACK_TIMEOUT_MS
                        : PSOC_CFG_ACK_TIMEOUT_MS);
    g_cfg_waiting = true;
    if (sub_cmd == PSOC_CMD_CALIBRATE) {
        /* Que el primer "calibrando..." salga ya en el proximo loop(),
         * sin esperar PSOC_CAL_PROGRESS_ACK_PERIOD_MS. */
        g_cal_progress_ack_ms = g_cfg_start_ms - PSOC_CAL_PROGRESS_ACK_PERIOD_MS;
    }
}

static bool ensurePsocReadyForConfig()
{
    psoc.poll();
    if (g_psocConnected && psoc.hasLastByte() &&
        psoc.lastByteAgeMs() <= PSOC_CFG_READY_STALE_MS) {
        return true;
    }

    const bool ready = psoc.probe(250);
    g_psocConnected = ready;
    if (!ready) {
        SLAVE_LOG_PRINTF("[SLAVE] PSoC cfg probe failed age=%lu bytes=%lu\n",
                         (unsigned long)psoc.lastByteAgeMs(),
                         (unsigned long)psoc.bytesRx());
        LOGM("CFG_PSOC_MISS", "age=%lu,bytes=%lu",
             (unsigned long)psoc.lastByteAgeMs(),
             (unsigned long)psoc.bytesRx());
    }
    return ready;
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
        if (ackCmd == PSOC_CMD_SD_READ_BATCH && g_psoc_sd_read_pending) {
            MsgCfgAck nack = { CMD_CFG_ACK, NODE_ID, CMD_REQ_BATCH, 0 };
            sendToMaster((const uint8_t *)&nack, sizeof(nack));
            g_psoc_sd_read_pending = false;
            g_psoc_sd_read_usb = false;
            SLAVE_LOG_PRINTF("[SLAVE] PSoC SD_READ NACK seq=%u val=%u\n",
                             (unsigned)g_psoc_sd_read_seq, (unsigned)ackVal);
            continue;
        }
        if (g_cfg_waiting && ackCmd == g_cfg_sub_cmd) {
            const uint8_t expected = expectedPsocAckValue(g_cfg_sub_cmd, g_cfg_param);
            const uint8_t ok = (ackVal == expected) ? 1u : 0u;
            if (ok) {
                applyConfirmedConfig(ackCmd, ackVal);
            }
            sendCfgAck(g_cfg_sub_cmd, ok);
            SLAVE_LOG_PRINTF("[SLAVE] PSoC cfg ack sub=0x%02X val=%u expected=%u ok=%u\n",
                             ackCmd, ackVal, expected, ok);
            LOGM("CFG_ACK", "sub=0x%02X,val=%u,expected=%u,ok=%u",
                 ackCmd, ackVal, expected, ok);
            g_cfg_waiting = false;
        } else {
            applyConfirmedConfig(ackCmd, ackVal);
            SLAVE_LOG_PRINTF("[SLAVE] PSoC cfg ack unsolicited sub=0x%02X val=%u\n",
                             ackCmd, ackVal);
            LOGM("CFG_ACK_UNSOL", "sub=0x%02X,val=%u", ackCmd, ackVal);
        }
    }

    if (g_cfg_waiting &&
        (uint32_t)(millis() - g_cfg_start_ms) >= g_cfg_timeout_ms) {
        const uint8_t expected = expectedPsocAckValue(g_cfg_sub_cmd, g_cfg_param);
        sendCfgAck(g_cfg_sub_cmd, 0);
        SLAVE_LOG_PRINTF("[SLAVE] PSoC cfg ack timeout sub=0x%02X expected=%u elapsed=%lu/%lu\n",
                         g_cfg_sub_cmd, expected,
                         (unsigned long)(millis() - g_cfg_start_ms),
                         (unsigned long)g_cfg_timeout_ms);
        LOGM("CFG_TIMEOUT", "sub=0x%02X,expected=%u,elapsed=%lu,timeout=%lu",
             g_cfg_sub_cmd, expected,
             (unsigned long)(millis() - g_cfg_start_ms),
             (unsigned long)g_cfg_timeout_ms);
        g_cfg_waiting = false;
    }

    /* "Calibrando..." (ok=2) hacia el maestro: por temporizador propio del
     * ESP, independiente de que el stream de diagnostico del PSoC llegue.
     * Mientras g_cfg_waiting este activo para CALIBRATE (hasta
     * PSOC_CAL_ACK_TIMEOUT_MS=450s o el ack final), repetir cada
     * PSOC_CAL_PROGRESS_ACK_PERIOD_MS para que el panel muestre el estado
     * "Calibrando..." de forma confiable. */
    if (g_cfg_waiting && g_cfg_sub_cmd == PSOC_CMD_CALIBRATE) {
        uint32_t now = millis();
        if ((uint32_t)(now - g_cal_progress_ack_ms) >= PSOC_CAL_PROGRESS_ACK_PERIOD_MS) {
            g_cal_progress_ack_ms = now;
            sendCfgAck(PSOC_CMD_CALIBRATE, 2);
        }
    }
}

/* Manda SETN al PSoC y espera su ACK, CON UN REINTENTO.
 *
 * El reintento no es precaucion teorica: se cazo el fallo con las dos puntas
 * abiertas a la vez. Cada tanto una captura vuelve vacia, y la unica diferencia
 * entre la vuelta que anda y la que no es esta:
 *
 *   anda:  [PSoC] SETN ack val=5 pstate=0/IDLE  -> ARMED -> CAPTURE_DONE
 *   falla: [SLAVE] PSoC SETN ack timeout ...    -> HOT_WAIT aborted
 *                                               -> START ignored state=0
 *
 * O sea que el ACK del PSoC a este comando no llega dentro del plazo, el
 * esclavo aborta el armado -bien: mejor eso que capturar basura- y el START
 * siguiente se ignora. Medido, pasa en el orden del 4 % de las capturas, y el
 * resto del camino no falla: 0 de 60 fallos en 0xAA, SET_RECLEN y ARM.
 *
 * Mandarlo una sola vez era la excepcion en este firmware: todos los demas
 * caminos de comando al PSoC ya usan el mismo reintento acotado de dos
 * intentos, justamente porque hay una clase de transitorio que se traga el
 * primer comando en silencio. */
static bool sendAndConfirmPsocSetN(uint16_t n_batches)
{
    const uint8_t expected = (n_batches > 255u) ? 255u : (uint8_t)n_batches;

    /* 2026-10-23: en modo vivo se perdieron los dos intentos seguidos y la
     * captura se aborto; cuatro intentos (~2 s) cuestan menos que eso. */
    for (int intento = 0; intento < 4; intento++) {
        const uint32_t startMs = millis();
        const uint32_t startSeq = g_psoc_setn_seq;

        psoc.setN(n_batches);
        while ((uint32_t)(millis() - startMs) < PSOC_SETN_ACK_TIMEOUT_MS) {
            psoc.poll();
            servicePsocConfigAck();
            if (g_psoc_setn_seq != startSeq &&
                g_psoc_last_setn_value == expected &&
                (int32_t)(g_psoc_last_setn_ms - startMs) >= 0) {
                if (intento > 0) {
                    SLAVE_LOG_PRINTF("[SLAVE] SETN ack en el reintento n=%u\n",
                                     (unsigned)n_batches);
                }
                return true;
            }
            delay(1);
        }
        if (intento < 3) {
            SLAVE_LOG_PRINTF("[SLAVE] SETN sin ack n=%u; reintento %d\n",
                             (unsigned)n_batches, intento + 1);
        }
    }

    SLAVE_LOG_PRINTF("[SLAVE] PSoC SETN ack timeout n=%u expected=%u last=%u age=%lu\n",
                     (unsigned)n_batches, (unsigned)expected,
                     (unsigned)g_psoc_last_setn_value,
                     (unsigned long)(millis() - g_psoc_last_setn_ms));
    LOGM("SETN_TIMEOUT", "n=%u,expected=%u,last=%u,age=%lu",
         (unsigned)n_batches, (unsigned)expected,
         (unsigned)g_psoc_last_setn_value,
         (unsigned long)(millis() - g_psoc_last_setn_ms));
    return false;
}

/* Deja el PSoC en el modo de captura de ESTA captura: 0 clasico, 1 en vivo,
 * 2 en vivo con la rampa de banco. Se cachea y solo viaja si cambia; un boot
 * del PSoC invalida la cache. */
static bool syncPsocLiveMode(uint8_t mode)
{
    if (g_psoc_live_known == (int16_t)mode) return true;
    for (int intento = 0; intento < 2; intento++) {
        const uint32_t startMs = millis();
        const uint32_t startSeq = g_psoc_live_ack_seq;
        psoc.liveMode(mode);
        while ((uint32_t)(millis() - startMs) < PSOC_SETN_ACK_TIMEOUT_MS) {
            psoc.poll();
            servicePsocConfigAck();
            if (g_psoc_live_ack_seq != startSeq) {
                if (g_psoc_live_ack == mode) {
                    g_psoc_live_known = (int16_t)mode;
                    return true;
                }
                break;
            }
            delay(1);
        }
    }
    g_psoc_live_known = -1;
    SLAVE_LOG_PRINTF("[SLAVE] PSoC LIVE_MODE %u sin confirmar (ack=%u)\n",
                     (unsigned)mode, (unsigned)g_psoc_live_ack);
    return false;
}

/* Deja la SD del PSoC como la necesita ESTA captura: prendida solo si no
 * entra en RAM. Funciona con cualquier firmware de PSoC (con o sin la regla
 * de psoc_arm), porque el PSoC usa la SD siempre que tenga el flag. */
static bool syncPsocSdForCapture(bool want)
{
    /* El PSoC ya no tiene SD (se saco del TopDesign; la SD es del ESP): 0xBE
     * contesta 0xEE. Apagada es el unico estado posible. */
    if (!want) return true;
    if (!want && g_psoc_sd_known && !psocSdPresent()) return true;
    if (g_psoc_sd_known && g_psoc_sd_capture_enabled == want && !g_sd_sync_pending) return true;

    for (int intento = 0; intento < 2; intento++) {
        const uint32_t startMs = millis();
        g_sd_sync_pending = true;
        psoc.sdCapture(want ? 1u : 0u);
        while ((uint32_t)(millis() - startMs) < PSOC_SETN_ACK_TIMEOUT_MS) {
            psoc.poll();
            servicePsocConfigAck();
            if (!g_sd_sync_pending) {
                if (g_psoc_sd_capture_enabled == want) {
                    g_psoc_sd_known = true;
                    return true;
                }
                break;
            }
            delay(1);
        }
    }
    g_sd_sync_pending = false;
    SLAVE_LOG_PRINTF("[SLAVE] PSoC SD %s sin confirmar\n", want ? "ON" : "OFF");
    LOGM("SD_SYNC_FAIL", "want=%u,actual=%u",
         (unsigned)want, (unsigned)g_psoc_sd_capture_enabled);
    return false;
}

static bool captureOrDumpBusy()
{
    return (g_state == SAMPLING || g_state == HOT_WAIT ||
            g_view_remaining != 0 ||
            (g_state == STOPPED && g_rec_n_batches > 0));
}

static bool requestPsocCalibration(const char *source)
{
    if (captureOrDumpBusy()) {
        SLAVE_LOG_PRINTF("[%s] cal ignored: capture/dump busy state=%d\n",
                         source, (int)g_state);
        LOGM("CAL_REQ", "source=%s,ok=0,reason=capture_busy,state=%d",
             source, (int)g_state);
        return false;
    }

    servicePsocConfigAck();
    if (g_cfg_waiting) {
        SLAVE_LOG_PRINTF("[%s] cal ignored: pending sub=0x%02X\n",
                         source, g_cfg_sub_cmd);
        LOGM("CAL_REQ", "source=%s,ok=0,reason=cfg_busy,pending=0x%02X",
             source, g_cfg_sub_cmd);
        return false;
    }

    if (!ensurePsocReadyForConfig()) {
        SLAVE_LOG_PRINTF("[%s] cal failed: PSoC not ready\n", source);
        LOGM("CAL_REQ", "source=%s,ok=0,reason=psoc_not_ready", source);
        return false;
    }

    psoc.calibrate();
    waitForPsocConfigAck(PSOC_CMD_CALIBRATE, 1);
    SLAVE_LOG_PRINTF("[%s] cal -> PSoC CMD 0x%02X\n", source, PSOC_CMD_CALIBRATE);
    LOGM("CAL_REQ", "source=%s,ok=1,sub=0x%02X", source, PSOC_CMD_CALIBRATE);
    return true;
}

static bool requestPsocAdcSnapshot(const char *source)
{
    if (captureOrDumpBusy()) {
        SLAVE_LOG_PRINTF("[%s] adc ignored: capture/dump busy state=%d\n",
                         source, (int)g_state);
        LOGM("ADC_REQ", "source=%s,ok=0,reason=capture_busy,state=%d",
             source, (int)g_state);
        return false;
    }

    servicePsocConfigAck();
    if (g_cfg_waiting) {
        SLAVE_LOG_PRINTF("[%s] adc ignored: pending sub=0x%02X\n",
                         source, g_cfg_sub_cmd);
        LOGM("ADC_REQ", "source=%s,ok=0,reason=cfg_busy,pending=0x%02X",
             source, g_cfg_sub_cmd);
        return false;
    }

    if (!ensurePsocReadyForConfig()) {
        SLAVE_LOG_PRINTF("[%s] adc failed: PSoC not ready\n", source);
        LOGM("ADC_REQ", "source=%s,ok=0,reason=psoc_not_ready", source);
        return false;
    }

    psoc.adcSnapshot();
    waitForPsocConfigAck(PSOC_CMD_ADC_SNAPSHOT, 1);
    SLAVE_LOG_PRINTF("[%s] adc -> PSoC CMD 0x%02X\n", source, PSOC_CMD_ADC_SNAPSHOT);
    LOGM("ADC_REQ", "source=%s,ok=1,sub=0x%02X", source, PSOC_CMD_ADC_SNAPSHOT);
    return true;
}

#if PSOC_AUTO_CAL_ON_READY
static void scheduleAutoCalibration(uint32_t delayMs)
{
    if (!g_auto_cal_requested) {
        g_auto_cal_due_ms = millis() + delayMs;
        SLAVE_LOG_PRINTF("[AUTO_CAL] scheduled in %lu ms (due=%lu)\n",
                         (unsigned long)delayMs, (unsigned long)g_auto_cal_due_ms);
        LOGM("AUTO_CAL", "ev=scheduled,delay_ms=%lu", (unsigned long)delayMs);
    }
}

static void serviceAutoCalibration()
{
    if (g_auto_cal_requested || g_auto_cal_due_ms == 0u) {
        return;
    }
    if ((int32_t)(millis() - g_auto_cal_due_ms) < 0) {
        return;
    }
    if (!g_psocConnected || captureOrDumpBusy() || g_cfg_waiting) {
        SLAVE_LOG_PRINTF("[AUTO_CAL] deferred (psocConnected=%d busy=%d cfg_waiting=%d) retry in %lu ms\n",
                         (int)g_psocConnected, (int)captureOrDumpBusy(), (int)g_cfg_waiting,
                         (unsigned long)PSOC_AUTO_CAL_RETRY_MS);
        LOGM("AUTO_CAL", "ev=deferred,psoc=%d,busy=%d,cfg_waiting=%d",
             (int)g_psocConnected, (int)captureOrDumpBusy(), (int)g_cfg_waiting);
        g_auto_cal_due_ms = millis() + PSOC_AUTO_CAL_RETRY_MS;
        return;
    }
    SLAVE_LOG_PRINTF("[AUTO_CAL] due -> requesting calibration\n");
    if (requestPsocCalibration("AUTO")) {
        g_auto_cal_requested = true;
        g_auto_cal_due_ms = 0u;
        LOGM("AUTO_CAL", "ev=requested,ok=1");
    } else {
        SLAVE_LOG_PRINTF("[AUTO_CAL] request failed, retry in %lu ms\n",
                         (unsigned long)PSOC_AUTO_CAL_RETRY_MS);
        LOGM("AUTO_CAL", "ev=requested,ok=0");
        g_auto_cal_due_ms = millis() + PSOC_AUTO_CAL_RETRY_MS;
    }
}
#else
static void scheduleAutoCalibration(uint32_t) {}
static void serviceAutoCalibration() {}
#endif

/* ── Acciones de nodo compartidas por la UI local y los comandos USB ──────
 * Estas viven FUERA de #if SLAVE_USB_CMD_ENABLE a propósito. El firmware de
 * campo compila con los flags de banco de pruebas en 0, y la UI local (OLED
 * + 4 botones) es justamente la interfaz de campo: si quedara dentro del
 * bloque de USB, la placa se quedaría sin ninguna forma de manejar la
 * máquina de estados sin la web. El prefijo "FromUsb" es histórico.
 * ────────────────────────────────────────────────────────────────────── */

static void serviceBootGains()
{
    if (g_boot_gain_step == 0u || (int32_t)(millis() - g_boot_gain_due_ms) < 0) {
        return;
    }
    if (!g_psocConnected || captureOrDumpBusy() || g_cfg_waiting) {
        g_boot_gain_due_ms = millis() + 500u;
        return;
    }
    if (++g_boot_gain_tries > 6u) {
        SLAVE_LOG_PRINTF("[BOOT_GAIN] abandonado en paso %u\n", (unsigned)g_boot_gain_step);
        LOGM("BOOT_GAIN", "ev=giveup,step=%u", (unsigned)g_boot_gain_step);
        g_boot_gain_step = 0u;
        return;
    }
    g_boot_gain_due_ms = millis() + 1000u;
    if (g_boot_gain_step == 1u) {
        /* g_pga_code es lo que el PSoC confirmó por ack: si ya coincide, listo. */
        if (SLAVE_BOOT_PGA_CODE > 8u || g_pga_code == SLAVE_BOOT_PGA_CODE) {
            g_boot_gain_step = 2u;
            g_boot_gain_tries = 0u;
            g_boot_gain_due_ms = millis();
            return;
        }
        if (!ensurePsocReadyForConfig()) return;
        psoc.setPga(SLAVE_BOOT_PGA_CODE);
        waitForPsocConfigAck(0xA6u, SLAVE_BOOT_PGA_CODE);
        SLAVE_LOG_PRINTF("[BOOT_GAIN] PGA -> codigo %u\n", (unsigned)SLAVE_BOOT_PGA_CODE);
        LOGM("BOOT_GAIN", "ev=pga,code=%u", (unsigned)SLAVE_BOOT_PGA_CODE);
        return;
    }
    if (g_boot_gain_step == 2u) {
        if (SLAVE_BOOT_PGAOUT_CODE > 8u || g_psoc_hw_class != 0u ||
            g_pgaout == SLAVE_BOOT_PGAOUT_CODE) {
            g_boot_gain_step = 0u;
            return;
        }
        if (!ensurePsocReadyForConfig()) return;
        psoc.setPgaout(SLAVE_BOOT_PGAOUT_CODE);
        waitForPsocConfigAck(PSOC_CMD_PGAOUT, SLAVE_BOOT_PGAOUT_CODE);
        SLAVE_LOG_PRINTF("[BOOT_GAIN] PGAout -> codigo %u\n", (unsigned)SLAVE_BOOT_PGAOUT_CODE);
        LOGM("BOOT_GAIN", "ev=pgaout,code=%u", (unsigned)SLAVE_BOOT_PGAOUT_CODE);
        return;
    }
    g_boot_gain_step = 0u;
}

/* Fin de una captura en vivo: el asincronico clasico manda START_DONE; los
 * caminos stream mandan STREAM_END con el total. */
static void serviceLiveEnd()
{
    if (!live::active()) return;
    if (live::wantsStop() && digitalRead(SYNC_TO_PSOC_PIN) == HIGH) {
        digitalWrite(SYNC_TO_PSOC_PIN, LOW);
        SLAVE_LOG_PRINTLN("[LIVE] SD llena o con error: corto la captura");
    }
    /* Maestro desaparecido (reiniciado, sin enlace): sin ningun STREAM_ACK en
     * 30 s con la captura ya cerrada, se abandona la sesion. Antes el esclavo
     * quedaba esperando para siempre, sin HELLO, y no se podia volver a armar
     * desde la web (2026-10-23). La SD conserva la captura. */
    if ((live::config().realtime || live::config().streamDump) && live::psocDone() &&
        live::msSinceMasterAck() > 30000u) {
        SLAVE_LOG_PRINTF("[LIVE] maestro sin confirmar hace %lu ms: abandono la sesion\n",
                         (unsigned long)live::msSinceMasterAck());
        digitalWrite(SYNC_TO_PSOC_PIN, LOW);
        live::close();
        g_live_notified = false;
        g_state = WAIT_ARM;
        return;
    }
    /* STREAM_END se repite: si se pierde, el maestro no cierra nunca. */
    static uint8_t endRetries = 0;
    static uint32_t endMs = 0;
    const bool streams = live::config().realtime || live::config().streamDump;
    if (g_live_notified) {
        if (streams && endRetries < 20u &&
            (uint32_t)(millis() - endMs) >= 250u) {
            endRetries++;
            endMs = millis();
            MsgStreamEnd e = { CMD_STREAM_END, NODE_ID, live::total(), live::endReason() };
            sendToMaster((const uint8_t *)&e, sizeof(e));
        }
        return;
    }
    if (!live::finished()) return;
    g_live_notified = true;
    endRetries = 0;
    endMs = millis();
    digitalWrite(SYNC_TO_PSOC_PIN, LOW);
    if (g_state == SAMPLING) g_state = STOPPED;
    const uint32_t total = live::total();
    g_store_fill = (total > 0xFFFFu) ? 0xFFFFu : (uint16_t)total;
    if (streams) {
        MsgStreamEnd e = { CMD_STREAM_END, NODE_ID, total, live::endReason() };
        sendToMaster((const uint8_t *)&e, sizeof(e));
    }
    /* En los caminos stream no: START_DONE dispara el volcado clasico. */
    if (g_start_store_active && !streams) {
        sendCompletionAckWithRetry(CMD_START);
    }
    g_start_store_active = false;
    SLAVE_LOG_RAW("#LIVE_END total=%lu reason=%u\n", (unsigned long)total,
                  (unsigned)live::endReason());
    live::printStatus();
}

static void requestPrestartFromUsb(uint16_t n)
{
    if (captureOrDumpBusy()) {
        SLAVE_LOG_PRINTF("[USB] pre ignored: busy state=%d fill=%u/%u\n",
                         (int)g_state, (unsigned)g_store_fill, (unsigned)g_rec_n_batches);
        return;
    }
    enterHotWait(n);
    g_view_store_active = false;
    g_start_store_active = false;
    SLAVE_LOG_PRINTF("[USB] pre n=%u state=%d ready=%u\n",
                     (unsigned)g_rec_n_batches, (int)g_state,
                     (unsigned)storeReadyForHotWait());
    LOGM("USB_CMD", "cmd=pre,n=%u,ready=%u",
         (unsigned)g_rec_n_batches, (unsigned)storeReadyForHotWait());
}

static void requestUsbStartFromHotWait()
{
    if (g_state != HOT_WAIT || !storeReadyForHotWait()) {
        SLAVE_LOG_PRINTF("[USB] sync ignored state=%d ready=%u\n",
                         (int)g_state, (unsigned)storeReadyForHotWait());
        return;
    }
    uint32_t nowUs = (uint32_t)micros();
    int syncBefore = digitalRead(SYNC_TO_PSOC_PIN);
    subirSyncConRadioSilencio();
    int syncAfter = digitalRead(SYNC_TO_PSOC_PIN);
    g_t_start_us = (uint64_t)nowUs;
    g_store_fill = 0;
    g_debug_count = 0;
    g_debug_last_us = nowUs;
    g_sampling_start_ms = millis();
    g_sampling_start_psoc_bytes = psoc.bytesRx();
    g_sampling_start_batches_ok = psoc.batchesOK();
    g_view_store_active = false;
    g_start_store_active = (g_rec_n_batches > 0 && storeReadyForHotWait());
    g_start_fallback_sent = false;
    g_state = SAMPLING;
    if (live::active()) live::start(g_t_start_us, effectivePsocSampleRateHz());
    radioSilencioAlEntrar();
    SLAVE_LOG_PRINTF("[USB] sync start n=%u sync=%d->%d bytes=%lu\n",
                     (unsigned)g_rec_n_batches, syncBefore, syncAfter,
                     (unsigned long)g_sampling_start_psoc_bytes);
    LOGM("USB_CMD", "cmd=sync,n=%u,syncBefore=%d,syncAfter=%d",
         (unsigned)g_rec_n_batches, syncBefore, syncAfter);
}

static void requestCaptureFromUsb(uint16_t n)
{
    requestPrestartFromUsb(n);
    /* El PSoC confirma ARMED (PSOC_EVT_ARMED -> g_psoc_arm_ready) de forma
     * asíncrona ~100-300 ms después del preStart. Sin esta espera el sync se
     * disparaba antes de ARMED y quedaba ignorado (ready=0), dejando la captura
     * colgada en HOT_WAIT con fill=0/N. Esto era la falsa "regresión de banco"
     * no-monotónica (cap chico ✓, cap grande ✗): dependía de la latencia del
     * ACK ARMED, no de pérdida de frames UART. El path real maestro->esclavo no
     * sufre esto porque PRESTART y START llegan separados por radio. */
    if (g_state == HOT_WAIT) {
        const uint32_t waitStart = millis();
        while (g_state == HOT_WAIT && !storeReadyForHotWait() &&
               (uint32_t)(millis() - waitStart) < HOTWAIT_ABORT_MS) {
            psoc.poll();
            servicePsocConfigAck();
            serviceHotWaitWatchdog();   /* re-arma preStart si ARMED no llega */
            delay(1);
        }
    }
    requestUsbStartFromHotWait();
}

static void requestStopFromUsb()
{
    if (live::active()) live::requestStop();
    digitalWrite(SYNC_TO_PSOC_PIN, LOW);
    g_debug_mode = false;
    debugEspSetRamp(false);
    psoc.debugRamp(false);
    g_view_store_active = false;
    g_start_store_active = false;
    g_start_fallback_sent = false;
    g_view_remaining = 0;
    allocStore(0);
    g_state = STOPPED;
    SLAVE_LOG_PRINTLN("[USB] stop");
    LOGM("USB_CMD", "cmd=stop,ok=1");
}

static void requestClearFromUsb()
{
    g_view_store_active = false;
    g_start_store_active = false;
    g_start_fallback_sent = false;
    g_view_remaining = 0;
    allocStore(0);
    if (g_state != SAMPLING && g_state != HOT_WAIT) {
        g_state = STOPPED;
    }
    SLAVE_LOG_PRINTLN("[USB] clear");
    LOGM("USB_CMD", "cmd=clear,ok=1");
}

/* Avanza al siguiente codigo de ganancia (0-8, con vuelta) y lo manda al PSoC.
 * El valor local NO se actualiza aca: se confirma con el ACK del PSoC en
 * applyConfirmedConfig(), igual que cuando el cambio viene de la web. */
static bool requestLocalGainStep(uint8_t subCmd, uint8_t current, const char *name)
{
    char notice[22];
    const uint8_t next = (uint8_t)((current + 1u) % 9u);

    servicePsocConfigAck();
    if (!g_psocConnected || captureOrDumpBusy() || g_cfg_waiting) {
        snprintf(notice, sizeof(notice), "%s ocupado", name);
        localUiNotify(notice, false);
        return false;
    }
    if (subCmd == PSOC_CMD_PGAOUT) {
        psoc.setPgaout(next);
    } else {
        psoc.setPga(next);
    }
    waitForPsocConfigAck(subCmd, next);
    snprintf(notice, sizeof(notice), "%s -> codigo %u", name, (unsigned)next);
    localUiNotify(notice, true);
    return true;
}

static void serviceLocalUi()
{
    const bool critical = (g_state == SAMPLING || g_state == HOT_WAIT);
    LocalUiStatus status = {
        NODE_ID,
        (uint8_t)g_state,
        g_psocConnected,
        g_cfg_waiting,
        psocSdPresent() != 0u,
        g_masterChannel,
        psoc.sampleRate(),
        g_store_fill,
        g_rec_n_batches,
        g_pga_code,
        g_pgaout,
        /* PGAout solo existe en el pipeline GEO de la placa nueva. Mientras el
         * PSoC no diga que es GEO se oculta, para no ofrecer un ajuste que el
         * nodo va a rechazar. */
        (g_psoc_hw_class == 0u)
    };

    const LocalUiAction action = localUiService(status, critical);
    switch (action) {
        case LOCAL_UI_ACTION_CAPTURE:
            if (!g_psocConnected || captureOrDumpBusy() || g_cfg_waiting) {
                localUiNotify("Captura no disponible", false);
                break;
            }
            localUiNotify("Armando captura", true);
            requestCaptureFromUsb(LOCAL_CAPTURE_BATCHES);
            if (g_state != SAMPLING) {
                localUiNotify("No se pudo iniciar", false);
            }
            break;
        case LOCAL_UI_ACTION_CALIBRATE: {
            const bool ok = requestPsocCalibration("LOCAL_UI");
            if (ok) {
                g_auto_cal_requested = true;
                g_auto_cal_due_ms = 0u;
            }
            localUiNotify(ok ? "Calibracion pedida" : "Calibracion ocupada", ok);
            break;
        }
        case LOCAL_UI_ACTION_ADC_SNAPSHOT: {
            const bool ok = requestPsocAdcSnapshot("LOCAL_UI");
            localUiNotify(ok ? "Snapshot solicitado" : "Snapshot no disponible", ok);
            break;
        }
        case LOCAL_UI_ACTION_IDENTIFY:
            if (!g_psocConnected || captureOrDumpBusy()) {
                localUiNotify("Nodo ocupado", false);
            } else {
                psoc.blinkLed();
                localUiNotify("LED PSoC activo", true);
            }
            break;
        case LOCAL_UI_ACTION_CLEAR:
            if (critical) {
                localUiNotify("Captura activa", false);
            } else {
                requestClearFromUsb();
                localUiNotify("Captura limpiada", true);
            }
            break;
        case LOCAL_UI_ACTION_STOP:
            requestStopFromUsb();
            localUiNotify("Captura detenida", true);
            break;
        case LOCAL_UI_ACTION_PGA_NEXT:
            (void)requestLocalGainStep(PSOC_CMD_PGA, g_pga_code, "PGA");
            break;
        case LOCAL_UI_ACTION_PGAOUT_NEXT:
            (void)requestLocalGainStep(PSOC_CMD_PGAOUT, g_pgaout, "PGAout");
            break;
        case LOCAL_UI_ACTION_NONE:
        default:
            break;
    }
}

#if SLAVE_USB_CMD_ENABLE
static bool usbCommandEquals(const char *cmd, const char *word)
{
    while (*cmd != '\0' && *word != '\0') {
        char a = *cmd++;
        char b = *word++;
        if (a >= 'A' && a <= 'Z') { a = (char)(a - 'A' + 'a'); }
        if (b >= 'A' && b <= 'Z') { b = (char)(b - 'A' + 'a'); }
        if (a != b) { return false; }
    }
    return *cmd == '\0' && *word == '\0';
}

static bool usbCommandStartsWithWord(const char *cmd, const char *word)
{
    while (*cmd != '\0' && *word != '\0') {
        char a = *cmd++;
        char b = *word++;
        if (a >= 'A' && a <= 'Z') { a = (char)(a - 'A' + 'a'); }
        if (b >= 'A' && b <= 'Z') { b = (char)(b - 'A' + 'a'); }
        if (a != b) { return false; }
    }
    return *word == '\0' && (*cmd == ' ' || *cmd == '=' || *cmd == ':');
}

static bool usbParseGainParam(const char *cmd, const char *word, uint8_t &value)
{
    if (!usbCommandStartsWithWord(cmd, word)) {
        return false;
    }
    while (*cmd != '\0' && *cmd != ' ' && *cmd != '=' && *cmd != ':') {
        cmd++;
    }
    while (*cmd == ' ' || *cmd == '=' || *cmd == ':') {
        cmd++;
    }
    if (*cmd < '0' || *cmd > '8') {
        return false;
    }
    value = (uint8_t)(*cmd - '0');
    cmd++;
    while (*cmd == ' ') {
        cmd++;
    }
    return *cmd == '\0';
}

static bool usbParseU16Param(const char *cmd, const char *word, uint16_t &value)
{
    uint32_t acc = 0;
    if (!usbCommandStartsWithWord(cmd, word)) {
        return false;
    }
    while (*cmd != '\0' && *cmd != ' ' && *cmd != '=' && *cmd != ':') {
        cmd++;
    }
    while (*cmd == ' ' || *cmd == '=' || *cmd == ':') {
        cmd++;
    }
    if (*cmd < '0' || *cmd > '9') {
        return false;
    }
    while (*cmd >= '0' && *cmd <= '9') {
        acc = (acc * 10u) + (uint32_t)(*cmd - '0');
        if (acc > 65535u) {
            return false;
        }
        cmd++;
    }
    while (*cmd == ' ') {
        cmd++;
    }
    if (*cmd != '\0') {
        return false;
    }
    value = (uint16_t)acc;
    return true;
}

#if SLAVE_LAB_TOOLS_ENABLE
static void requestPrestartFromUsb(uint16_t n);
static void requestStartNowFromUsb(uint16_t n);
static void requestCaptureFromUsb(uint16_t n);

static bool usbParseTwoU16Params(const char *cmd, const char *word,
                                 uint16_t &first, uint16_t &second)
{
    uint32_t values[2] = {0u, 0u};
    uint8_t index = 0u;

    if (!usbCommandStartsWithWord(cmd, word)) {
        return false;
    }
    while (*cmd != '\0' && *cmd != ' ' && *cmd != '=' && *cmd != ':') {
        cmd++;
    }
    while (*cmd != '\0' && index < 2u) {
        while (*cmd == ' ' || *cmd == '=' || *cmd == ':') {
            cmd++;
        }
        if (*cmd < '0' || *cmd > '9') {
            return false;
        }
        uint32_t acc = 0u;
        while (*cmd >= '0' && *cmd <= '9') {
            acc = (acc * 10u) + (uint32_t)(*cmd - '0');
            if (acc > 65535u) {
                return false;
            }
            cmd++;
        }
        values[index++] = acc;
        while (*cmd == ' ') {
            cmd++;
        }
    }
    if (index != 2u || *cmd != '\0') {
        return false;
    }
    first = (uint16_t)values[0];
    second = (uint16_t)values[1];
    return true;
}

static uint32_t usbCaptureTimeoutMs(uint16_t n)
{
    uint32_t timeoutMs = expectedCaptureMs(n) + 2000u;
    return (timeoutMs < 3000u) ? 3000u : timeoutMs;
}

#if SLAVE_LAB_TOOLS_ENABLE
static void serviceRfTest();
static void serviceRfRx();
static void serviceLabPing();
static void serviceSdStress();
static void serviceSdProbe();
static void serviceEspSd();
#endif

static void serviceLabWaitSlice()
{
    psoc.poll();
    servicePsocConfigAck();
#if SLAVE_LAB_TOOLS_ENABLE
    /* capwait/startwait bloquean loop(): sin esto los estresores paralelos
     * (rftest/sdstress/sdprobe/espsd) no corrian DURANTE la captura sino antes y
     * despues, y la captura salia limpia. Medido 2026-10-19: 0 marcas. */
    serviceLabPing();
    serviceRfTest();
    serviceRfRx();
    serviceSdStress();
    serviceSdProbe();
    serviceEspSd();
#endif
    serviceRadioSilencio(true);   /* radiosil: el apagado se difiere al loop */
    delay(1);
}

/* Banco: captura en vivo completa por USB, con el modo de "capmode". Con
 * secs > 0 corta con STOP a los secs segundos (para N = 0, sin limite). Espera
 * el fin (en tiempo real, que el maestro -o el simulado- confirme todo). */
static void requestLiveCapFromUsb(uint32_t n, uint32_t secs)
{
    if (!capmodeIsLive(g_capmode)) {
        SLAVE_LOG_RAW("#LIVECAP rechazado: capmode es el clasico\n");
        return;
    }
    /* Lo que haria un ARM: soltar la captura anterior, que queda retenida
     * esperando la descarga. */
    live::close();
    requestClearFromUsb();
    requestCaptureFromUsb((uint16_t)(n > 0xFFFFu ? 0xFFFFu : n));
    if (!live::active() || g_state != SAMPLING) {
        SLAVE_LOG_RAW("#LIVECAP no arranco state=%d\n", (int)g_state);
        live::printStatus();
        return;
    }
    uint32_t fs = effectivePsocSampleRateHz();
    if (fs == 0u) fs = PSOC_NATIVE_SAMPLE_RATE_HZ;
    const uint32_t captureMs = (n > 0u)
        ? (uint32_t)(((uint64_t)n * SPI_BATCH_SAMPLES * 1000u) / fs) : secs * 1000u;
    const uint32_t limitMs = captureMs + secs * 1000u + 30000u;
    const uint32_t t0 = millis();
    uint32_t lastStatMs = t0;
    bool stopped = false;
    while ((uint32_t)(millis() - t0) < limitMs && !g_live_notified) {
        serviceLabWaitSlice();
        live::service();
        serviceLiveEnd();
        if (!stopped && secs > 0u && (uint32_t)(millis() - t0) >= secs * 1000u) {
            stopped = true;
            live::requestStop();
            digitalWrite(SYNC_TO_PSOC_PIN, LOW);
        }
        if ((uint32_t)(millis() - lastStatMs) >= 2000u) {
            lastStatMs = millis();
            live::printStatus();
        }
    }
    SLAVE_LOG_RAW("#LIVECAP fin=%u ms=%lu\n", (unsigned)g_live_notified,
                  (unsigned long)(millis() - t0));
    if (!g_live_notified) live::printStatus();
}

static void waitForLabCaptureDone(const char *tag, uint16_t requestedBatches)
{
    const uint32_t timeoutMs = usbCaptureTimeoutMs(requestedBatches);
    const uint32_t startMs = millis();
    const uint32_t startOk = psoc.batchesOK();
    const uint32_t startBad = psoc.batchesBad();
    const uint32_t startBytes = psoc.bytesRx();

    while (g_state == SAMPLING &&
           (uint32_t)(millis() - startMs) < timeoutMs) {
        serviceLabWaitSlice();
    }

    const uint32_t elapsedMs = millis() - startMs;
    const bool full = (g_rec_n_batches > 0u &&
                       g_store_fill >= g_rec_n_batches &&
                       g_state == STOPPED);
    const bool timeout = (g_state == SAMPLING);
    SLAVE_LOG_PRINTF("[LAB] %s done=%u timeout=%u ms=%lu/%lu state=%d fill=%u/%u dOK=%lu dBad=%lu dBytes=%lu sync=%d\n",
                     tag, (unsigned)full, (unsigned)timeout,
                     (unsigned long)elapsedMs, (unsigned long)timeoutMs,
                     (int)g_state, (unsigned)g_store_fill,
                     (unsigned)g_rec_n_batches,
                     (unsigned long)(psoc.batchesOK() - startOk),
                     (unsigned long)(psoc.batchesBad() - startBad),
                     (unsigned long)(psoc.bytesRx() - startBytes),
                     digitalRead(SYNC_TO_PSOC_PIN));
    logPsocUartDiag(tag);
}

static void requestLabDiagFromUsb()
{
    SLAVE_LOG_PRINTF("[LAB] diag state=%d psoc=%u streamPga=%u pgaout=%u pgavdac=%u vdac=%u syncOut=%d i2cOvr=%lu\n",
                     (int)g_state, (unsigned)g_psocConnected,
                     (unsigned)g_pga_code, (unsigned)g_pgaout, (unsigned)g_pgavdac,
                     (unsigned)g_vdac_byte, digitalRead(SYNC_TO_PSOC_PIN),
                     (unsigned long)psoc.i2cOverruns());
    logPsocUartDiag("LAB_DIAG");
}

static void requestLabPinsFromUsb()
{
    uint32_t rxEdges;
    uint8_t rxEdgeLevel;
    noInterrupts();
    rxEdges = g_psoc_rx_edges;
    rxEdgeLevel = g_psoc_rx_last_level;
    interrupts();
    SLAVE_LOG_PRINTF("[LAB] pins syncOut=%d psocRx=%d rxEdges=%lu rxEdgeLevel=%u i2cSda=%d i2cScl=%d\n",
                     digitalRead(SYNC_TO_PSOC_PIN),
                     digitalRead(PSOC_UART_RX), (unsigned long)rxEdges,
                      (unsigned)rxEdgeLevel,
                      digitalRead(PSOC_I2C_SDA), digitalRead(PSOC_I2C_SCL));
}

static void requestQuietWindowFromUsb(uint16_t n, uint16_t ms)
{
    const uint32_t preBytes = psoc.bytesRx();
    const uint32_t prePing = psoc.pingsRx();
    const uint32_t preDiag = psoc.diagEventsRx();
    const uint32_t preOk = psoc.batchesOK();
    const uint32_t preBad = psoc.batchesBad();

    requestPrestartFromUsb(n);

    const uint32_t settleMs = millis();
    while (g_state == HOT_WAIT &&
           (uint32_t)(millis() - settleMs) < 25u) {
        psoc.poll();
        delay(1);
    }

    const uint32_t startBytes = psoc.bytesRx();
    const uint32_t startPing = psoc.pingsRx();
    const uint32_t startDiag = psoc.diagEventsRx();
    const uint32_t startOk = psoc.batchesOK();
    const uint32_t startBad = psoc.batchesBad();
    const uint32_t startMs = millis();

    while (g_state == HOT_WAIT &&
           (uint32_t)(millis() - startMs) < (uint32_t)ms) {
        psoc.poll();
        delay(1);
    }

    SLAVE_LOG_PRINTF("[LAB] quiet n=%u ms=%u state=%d settleBytes=%lu settlePing=%lu settleDiag=%lu winBytes=%lu winPing=%lu winDiag=%lu winOK=%lu winBad=%lu sync=%d\n",
                     (unsigned)n, (unsigned)ms, (int)g_state,
                     (unsigned long)(startBytes - preBytes),
                     (unsigned long)(startPing - prePing),
                     (unsigned long)(startDiag - preDiag),
                     (unsigned long)(psoc.bytesRx() - startBytes),
                     (unsigned long)(psoc.pingsRx() - startPing),
                     (unsigned long)(psoc.diagEventsRx() - startDiag),
                     (unsigned long)(psoc.batchesOK() - startOk),
                     (unsigned long)(psoc.batchesBad() - startBad),
                     digitalRead(SYNC_TO_PSOC_PIN));
    logPsocUartDiag("LAB_QUIET");
}

static void waitForLabStreamAck(uint8_t streamMode)
{
    const uint32_t startMs = millis();
    servicePsocConfigAck();
    if (g_cfg_waiting) {
        SLAVE_LOG_PRINTF("[LAB] stream ack skipped pending=0x%02X\n", g_cfg_sub_cmd);
        return;
    }
    psoc.selectStream(streamMode);
    waitForPsocConfigAck(PSOC_CMD_SELECT_STREAM, streamMode);
    while (g_cfg_waiting && (uint32_t)(millis() - startMs) < 1000u) {
        serviceLabWaitSlice();
    }
    if (g_cfg_waiting) {
        SLAVE_LOG_PRINTF("[LAB] stream %u ack timeout guard\n", (unsigned)streamMode);
        g_cfg_waiting = false;
    }
}

static void requestLabCaptureFromUsb(uint8_t streamMode, uint16_t n, const char *tag)
{
    waitForLabStreamAck(streamMode);
    requestCaptureFromUsb(n);
    waitForLabCaptureDone(tag, n);
}

/* Banco: leer un lote de GEOLAST.BIN (SD del PSoC) por 0xBF y volcarlo por USB
 * — mismo camino que usa handleReqBatch() para el maestro, pero verificable
 * sin radio. Requiere una captura SD previa completa (g_psoc_sd_store_active). */
static void requestSdReadFromUsb(uint16_t seq)
{
    if (!g_psoc_sd_store_active || g_store_fill == 0u) {
        SLAVE_LOG_PRINTF("[USB] sdread ignored: sin captura SD (fill=%u sd=%u)\n",
                         (unsigned)g_store_fill, (unsigned)g_psoc_sd_store_active);
        return;
    }
    if (seq >= g_store_fill) {
        SLAVE_LOG_PRINTF("[USB] sdread ignored: seq=%u fuera de rango (fill=%u)\n",
                         (unsigned)seq, (unsigned)g_store_fill);
        return;
    }
    if (g_psoc_sd_read_pending) {
        SLAVE_LOG_PRINTF("[USB] sdread ignored: lectura pendiente seq=%u\n",
                         (unsigned)g_psoc_sd_read_seq);
        return;
    }
    g_psoc_sd_read_pending = true;
    g_psoc_sd_read_usb = true;
    g_psoc_sd_read_seq = seq;
    g_psoc_sd_read_started_ms = millis();
    psoc.sdReadBatch(seq);
    LOGM("USB_CMD", "cmd=sdread,seq=%u", (unsigned)seq);
}

static_assert(sizeof(SampleBytes) == 10u,
              "dumpusb asume 10 bytes por SampleBytes (raw_lo..flags)");

static uint8_t crc8Dump(const uint8_t *data, size_t len)
{
    /* CRC-8 simple (poly 0x07, init 0x00): solo para detectar una linea
     * cortada en el volcado por USB, no es criptografico. */
    uint8_t crc = 0x00u;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0u; bit < 8u; bit++) {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x07u) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

/* Banco: volcar por USB los lotes que ya quedaron en RAM, DESPUES de que la
 * captura termino. Existe porque hoy las muestras solo salen del nodo por
 * ESP-NOW y leerlas ya prende la radio: eso mide el ruido de la LECTURA, no
 * el de la captura. La captura tiene que ser limpia; el volcado, que ocurre
 * despues, puede ser sucio. Formato pensado para un parser de host trivial:
 * una linea "#DUMP <seq> <600 hex> <crc8 hex>" por lote (ver crc8Dump). */
static void requestDumpUsbFromUsb(bool hasDesde, uint16_t desde,
                                   bool hasHasta, uint16_t hasta)
{
    if (g_state == SAMPLING) {
        SLAVE_LOG_PRINTF("[USB] dumpusb rechazado: captura en curso state=%d\n",
                         (int)g_state);
        return;
    }
    if (g_store_fill == 0u) {
        SLAVE_LOG_PRINTF("[USB] dumpusb rechazado: sin captura (fill=0)\n");
        return;
    }

    uint16_t rangeDesde = hasDesde ? desde : 0u;
    uint16_t rangeHasta = hasHasta ? hasta : (uint16_t)(g_store_fill - 1u);
    if (rangeHasta >= g_store_fill) {
        rangeHasta = (uint16_t)(g_store_fill - 1u);
    }
    if (rangeDesde > rangeHasta) {
        SLAVE_LOG_PRINTF("[USB] dumpusb rango invalido desde=%u hasta=%u fill=%u\n",
                         (unsigned)rangeDesde, (unsigned)rangeHasta,
                         (unsigned)g_store_fill);
        return;
    }

    uint16_t fs = psoc.sampleRate();
    if (fs == 0u) { fs = PSOC_NATIVE_SAMPLE_RATE_HZ; }

    SLAVE_LOG_RAW("#DUMP_BEGIN n=%u fs=%u spb=%u bpp=%u pga=%u pgaout=%u\n",
                  (unsigned)(rangeHasta - rangeDesde + 1u), (unsigned)fs,
                  (unsigned)SPI_BATCH_SAMPLES, (unsigned)sizeof(SampleBytes),
                  (unsigned)g_pga_code, (unsigned)g_pgaout);

    static const char hexDigits[] = "0123456789abcdef";
    const size_t nBytes = (size_t)SPI_BATCH_SAMPLES * sizeof(SampleBytes);
    char line[32 + SPI_BATCH_SAMPLES * (int)sizeof(SampleBytes) * 2];
    uint16_t volcados = 0u;

    for (uint16_t seq = rangeDesde; seq <= rangeHasta; seq++) {
        SampleBytes *batch = storeBatchPtr(seq);
        if (batch == nullptr) {
            SLAVE_LOG_RAW("#DUMP_MISS %u\n", (unsigned)seq);
            yield();
            continue;
        }
        const uint8_t *bytes = (const uint8_t *)batch;
        const uint8_t crc = crc8Dump(bytes, nBytes);

        int pos = snprintf(line, sizeof(line), "#DUMP %u ", (unsigned)seq);
        for (size_t i = 0; i < nBytes; i++) {
            line[pos++] = hexDigits[(bytes[i] >> 4) & 0x0Fu];
            line[pos++] = hexDigits[bytes[i] & 0x0Fu];
        }
        pos += snprintf(line + pos, sizeof(line) - (size_t)pos, " %02x\n", crc);
        SLAVE_LOG_RAW("%s", line);
        volcados++;
        yield();   /* miles de lotes: ceder CPU para no disparar el watchdog */
    }
    SLAVE_LOG_RAW("#DUMP_END %u\n", (unsigned)volcados);
    LOGM("USB_CMD", "cmd=dumpusb,desde=%u,hasta=%u,n=%u",
         (unsigned)rangeDesde, (unsigned)rangeHasta, (unsigned)volcados);
}

/* Banco: trafico ESP-NOW controlado y EN PARALELO a una captura, para medir
 * cuanto ruido agrega transmitir (en vez de esperar a que la radio hable por
 * casualidad). NO bloquea: arma el estado global y loop() -> serviceRfTest()
 * dispara una trama por periodo mientras millis() < g_rftest_hasta_ms. La
 * validacion de rango ya la hizo el parser en handleUsbCommand. */
static void labPosTick(LabPos &p)
{
    if (!p.enSampling && g_state == SAMPLING) {
        p.base = psoc.batchesOK();
        p.enSampling = true;
    }
}

static void labPosMark(LabPos &p)
{
    if (!p.enSampling || g_state != SAMPLING) return;
    const uint16_t k = p.n;
    if (k >= LAB_POS_MAX) return;
    p.pos[k] = micros();
    p.n = (uint16_t)(k + 1u);
}

static void labPosPrint(const char *tag, LabPos &p)
{
    SLAVE_LOG_RAW("#%s t %u", tag, (unsigned)p.n);
    for (uint16_t i = 0; i < p.n; i++) SLAVE_LOG_RAW(" %lu", (unsigned long)p.pos[i]);
    SLAVE_LOG_RAW("\n");
    SLAVE_LOG_RAW("#PING t %u", (unsigned)g_ping_n);
    for (uint16_t i = 0; i < g_ping_n; i++) {
        SLAVE_LOG_RAW(" %u:%lu", (unsigned)g_ping_v[i], (unsigned long)g_ping_t[i]);
    }
    SLAVE_LOG_RAW("\n");
}

/* Reinicia el registro de pings al empezar cada captura. */
static void serviceLabPing()
{
    if (g_state == SAMPLING) {
        if (!g_ping_en_sampling) { g_ping_n = 0u; g_ping_en_sampling = true; }
    } else {
        g_ping_en_sampling = false;
    }
}

static void labPingRecord(uint8_t v)
{
    const uint32_t t = micros();
    const uint16_t k = g_ping_n;
    if (k < LAB_POS_MAX) { g_ping_t[k] = t; g_ping_v[k] = v; g_ping_n = (uint16_t)(k + 1u); }
    if (g_rftest_running && g_rftest_periodo_ms == 0u) g_rftest_disparo = true;
}

static void rftestFin(const char *evento)
{
    g_rftest_hasta_ms = 0u;
    if (!g_rftest_running) return;
    g_rftest_running = false;
    labPosPrint("RFTEST", g_rftest_pos);
    SLAVE_LOG_RAW("#RFTEST fin enviados=%lu fallos=%lu\n",
                  (unsigned long)g_rftest_enviados, (unsigned long)g_rftest_fallos);
    LOGM("USB_CMD", "cmd=rftest,event=%s,enviados=%lu,fallos=%lu", evento,
         (unsigned long)g_rftest_enviados, (unsigned long)g_rftest_fallos);
}

static bool ensureRfTestBroadcastPeer()
{
#if defined(ESP8266)
    return espnowAddPeer(RFTEST_BROADCAST_MAC);
#else
    if (esp_now_is_peer_exist(RFTEST_BROADCAST_MAC)) return true;
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, RFTEST_BROADCAST_MAC, sizeof(peer.peer_addr));
    peer.channel = espnowCurrentChannel();
    peer.encrypt = false;
    return esp_now_add_peer(&peer) == ESP_OK;
#endif
}

static void requestRfTestFromUsb(uint32_t ms, uint32_t periodoMs, uint32_t bytesN,
                                 bool broadcast)
{
    if (ms == 0u) {
        if (!g_rftest_running) {
            SLAVE_LOG_RAW("#RFTEST fin enviados=%lu fallos=%lu\n",
                          (unsigned long)g_rftest_enviados, (unsigned long)g_rftest_fallos);
        }
        rftestFin("cancel");
        return;
    }
    if (broadcast && !ensureRfTestBroadcastPeer()) {
        SLAVE_LOG_PRINTF("[USB] rftest: no se pudo registrar peer broadcast\n");
        return;
    }
    g_rftest_periodo_ms = periodoMs;
    g_rftest_bytes      = (uint8_t)bytesN;
    g_rftest_broadcast  = broadcast;
    g_rftest_enviados   = 0u;
    g_rftest_fallos     = 0u;
    g_rftest_prox_ms    = millis();
    g_rftest_disparo    = false;
    labPosReset(g_rftest_pos);
    pinMode(RF_TRIG_PIN, OUTPUT);
    digitalWrite(RF_TRIG_PIN, LOW);
    g_rftest_running    = true;
    g_rftest_hasta_ms   = millis() + ms;
    SLAVE_LOG_RAW("#RFTEST inicio ms=%lu periodo=%lu bytes=%lu\n",
                  (unsigned long)ms, (unsigned long)periodoMs, (unsigned long)bytesN);
    LOGM("USB_CMD", "cmd=rftest,ok=1,ms=%lu,periodo=%lu,bytes=%lu",
         (unsigned long)ms, (unsigned long)periodoMs, (unsigned long)bytesN);
}

/* loop(): una trama fija por periodo, sin bloquear. Payload de patron fijo,
 * el contenido no importa para medir ruido inducido por la radio. Cada envio
 * queda registrado (lote de la captura) y levanta RF_TRIG_PIN ~200 us para
 * disparar el osciloscopio. */
static void serviceRfTest()
{
    if (g_rftest_hasta_ms == 0u) return;
    labPosTick(g_rftest_pos);
    const uint32_t now = millis();
    if (now >= g_rftest_hasta_ms) {
        rftestFin("fin");
        return;
    }
    if (g_rftest_periodo_ms == 0u) {
        if (!g_rftest_disparo) return;
        g_rftest_disparo = false;
    } else {
        if (now < g_rftest_prox_ms) return;
        g_rftest_prox_ms = now + g_rftest_periodo_ms;
    }

    static uint8_t payload[240];
    static bool payloadReady = false;
    if (!payloadReady) {
        for (uint16_t i = 0; i < sizeof(payload); i++) {
            payload[i] = (uint8_t)(0xA5u ^ (uint8_t)i);   /* patron fijo, reconocible */
        }
        payloadReady = true;
    }

    labPosMark(g_rftest_pos);
    digitalWrite(RF_TRIG_PIN, HIGH);
    const esp_err_t err = g_rftest_broadcast
        ? espnowSend(RFTEST_BROADCAST_MAC, payload, g_rftest_bytes)
        : sendToMaster(payload, g_rftest_bytes);
    delayMicroseconds(200);
    digitalWrite(RF_TRIG_PIN, LOW);
    g_rftest_enviados++;
    if (err != ESP_OK) { g_rftest_fallos++; }
}

/* rfrx: mientras esta armado, cada trama "RFB1" del maestro (rafaga 0xC1)
 * queda registrada por onDataRecv; al terminar se informa #RFRX pos. */
static void rfrxFin()
{
    g_rfrx_hasta_ms = 0u;
    if (!g_rfrx_running) return;
    g_rfrx_running = false;
    labPosPrint("RFRX", g_rfrx_pos);
    SLAVE_LOG_RAW("#RFRX fin recibidas=%lu\n", (unsigned long)g_rfrx_recibidas);
}

static void requestRfRxFromUsb(uint32_t ms)
{
    if (ms == 0u) {
        if (!g_rfrx_running) SLAVE_LOG_RAW("#RFRX fin recibidas=%lu\n", (unsigned long)g_rfrx_recibidas);
        rfrxFin();
        return;
    }
    labPosReset(g_rfrx_pos);
    g_rfrx_recibidas = 0u;
    g_rfrx_running = true;
    g_rfrx_hasta_ms = millis() + ms;
    SLAVE_LOG_RAW("#RFRX inicio ms=%lu\n", (unsigned long)ms);
}

static void serviceRfRx()
{
    if (g_rfrx_hasta_ms == 0u) return;
    labPosTick(g_rfrx_pos);
    if (millis() >= g_rfrx_hasta_ms) rfrxFin();
}

/* Llamado al principio de onDataRecv: true si era una trama de rafaga. */
static bool rfrxOnFrame(const uint8_t *data, int len)
{
    if (len < 4 || data[0] != 'R' || data[1] != 'F' || data[2] != 'B' || data[3] != '1') return false;
    if (g_rfrx_running) {
        g_rfrx_recibidas++;
        labPosMark(g_rfrx_pos);
    }
    return true;
}

/* txpower: potencia maxima de transmision en dBm (2..21). */
static void requestTxPowerFromUsb(long dbm)
{
    if (dbm < 2 || dbm > 21) {
        SLAVE_LOG_PRINTF("[USB] uso: txpower <dBm 2-21>\n");
        return;
    }
    const esp_err_t err = esp_wifi_set_max_tx_power((int8_t)(dbm * 4));
    int8_t real = 0;
    esp_wifi_get_max_tx_power(&real);
    SLAVE_LOG_RAW("#TXPOWER pedido=%ld dBm real=%d qdBm err=%d\n", dbm, (int)real, (int)err);
}

/* radio off|on: apaga y vuelve a prender WiFi + ESP-NOW sin reiniciar el ESP
 * (reiniciarlo resetea el PSoC y obliga al lazo a volver a aprender). */
static bool g_radio_on = true;
static bool g_radio_apagada_por_silencio = false;
static esp_timer_handle_t g_radio_silencio_sync_timer = nullptr;

static void radioSilencioSyncTimerCb(void *)
{
    digitalWrite(SYNC_TO_PSOC_PIN, HIGH);
}

/* SYNC programado por FTM: el esp_timer despierta unos ms antes y se espera
 * activamente hasta el microsegundo del plazo. */
static esp_timer_handle_t g_sched_sync_timer = nullptr;
static volatile int64_t   g_sched_deadline = 0;
static volatile int64_t   g_sched_actual = 0;
static volatile bool      g_sched_fired = false;
static uint32_t           g_sched_master_us = 0;
static uint32_t           g_sched_uncert_ns = 0;
static constexpr int64_t  SCHED_EARLY_US = 10000;   /* con 3 ms el timer igual llegaba ~20 us tarde */

static portMUX_TYPE g_sched_mux = portMUX_INITIALIZER_UNLOCKED;

static void schedSyncTimerCb(void *)
{
    const int64_t dl = g_sched_deadline;
    /* Hasta 300 us antes, espera normal; el tramo final sin interrupciones (una
     * del WiFi en el ultimo tramo atrasaba el flanco 15-30 us). */
    while (esp_timer_get_time() < dl - 300) { }
    portENTER_CRITICAL(&g_sched_mux);
    while (esp_timer_get_time() < dl) { }
    gpio_set_level((gpio_num_t)SYNC_TO_PSOC_PIN, 1);
    const int64_t real = esp_timer_get_time();
    portEXIT_CRITICAL(&g_sched_mux);
    g_sched_actual = real;
    g_sched_fired = true;
}

static bool programarSyncFtm(uint32_t startMasterUs)
{
    int64_t deadline = 0;
    uint32_t unc = 0;
    if (!ftm::masterMacToLocalEsp(startMasterUs, deadline, unc)) return false;
    if (g_sched_sync_timer == nullptr) {
        const esp_timer_create_args_t args = {
            .callback = &schedSyncTimerCb,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "ftm_sync",
            .skip_unhandled_events = false,
        };
        if (esp_timer_create(&args, &g_sched_sync_timer) != ESP_OK) {
            g_sched_sync_timer = nullptr;
            return false;
        }
    }
    esp_timer_stop(g_sched_sync_timer);
    g_sched_deadline = deadline;
    g_sched_fired = false;
    g_sched_master_us = startMasterUs;
    g_sched_uncert_ns = unc;
    const int64_t wait = deadline - SCHED_EARLY_US - esp_timer_get_time();
    if (esp_timer_start_once(g_sched_sync_timer, (uint64_t)(wait > 0 ? wait : 1)) != ESP_OK) return false;
    return true;
}

static volatile int64_t g_bcast_sync_local = 0;
static volatile bool    g_bcast_sync_new = false;

#if defined(ESP32) && ESP_ARDUINO_VERSION_MAJOR >= 3
#ifndef START_RX_DELAY_US
#define START_RX_DELAY_US 20000   /* > SCHED_EARLY_US */
#endif
static uint8_t  g_sched_kind = 0;      /* 0 = FTM, 1 = recepcion del broadcast */
static uint32_t g_sched_rx_age = 0;
static char     g_sched_rx_dom = '-';

/* Plazo local = recepcion del START + START_RX_DELAY_US. El timestamp de
 * rx_ctrl esta en el reloj MAC o en el esp_timer (32 bits, us): se usa el que
 * de una edad razonable. */
static bool programarSyncRx()
{
    const uint32_t ageMac = g_rx_mac - g_rx_ts;
    const uint32_t ageEsp = (uint32_t)g_rx_esp - g_rx_ts;
    uint32_t age;
    if (ageMac < 20000u) { age = ageMac; g_sched_rx_dom = 'M'; }
    else if (ageEsp < 20000u) { age = ageEsp; g_sched_rx_dom = 'E'; }
    else return false;
    const int64_t deadline = g_rx_esp - (int64_t)age + START_RX_DELAY_US;
    if (deadline - esp_timer_get_time() < (int64_t)SCHED_EARLY_US + 500) return false;
    if (g_sched_sync_timer == nullptr) {
        const esp_timer_create_args_t args = {
            .callback = &schedSyncTimerCb,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "ftm_sync",
            .skip_unhandled_events = false,
        };
        if (esp_timer_create(&args, &g_sched_sync_timer) != ESP_OK) {
            g_sched_sync_timer = nullptr;
            return false;
        }
    }
    esp_timer_stop(g_sched_sync_timer);
    g_sched_kind = 1;
    g_sched_rx_age = age;
    g_sched_deadline = deadline;
    g_sched_fired = false;
    g_sched_master_us = 0;
    g_sched_uncert_ns = 0;
    const int64_t wait = deadline - SCHED_EARLY_US - esp_timer_get_time();
    return esp_timer_start_once(g_sched_sync_timer, (uint64_t)(wait > 0 ? wait : 1)) == ESP_OK;
}
#endif

/* Informe del SYNC programado (desde loop): a que hora del maestro se subio. */
static void serviceSchedSyncReport()
{
    if (g_bcast_sync_new) {
        g_bcast_sync_new = false;
        uint32_t m = 0;
        const bool ok = ftm::localEspToMasterMac(g_bcast_sync_local, m);
        SLAVE_LOG_PRINTF("[SYNCBC] broadcast real_maestro=%lu conv=%d modelo=%d\n",
                         (unsigned long)m, (int)ok, (int)ftm::modelOk());
    }
    if (!g_sched_fired) return;
    g_sched_fired = false;
    uint32_t realMaster = 0;
    const bool okConv = ftm::localEspToMasterMac(g_sched_actual, realMaster);
#if defined(ESP32) && ESP_ARDUINO_VERSION_MAJOR >= 3
    if (g_sched_kind == 1) {
        g_sched_kind = 0;
        SLAVE_LOG_PRINTF("[SYNCRX] real_maestro=%lu conv=%d edad_rx_us=%lu reloj=%c tarde_local_us=%lld\n",
                         (unsigned long)realMaster, (int)okConv, (unsigned long)g_sched_rx_age,
                         g_sched_rx_dom, (long long)(g_sched_actual - g_sched_deadline));
        return;
    }
#endif
    SLAVE_LOG_PRINTF("[SYNCFTM] objetivo_maestro=%lu real_maestro=%lu err_us=%ld tarde_local_us=%lld incert_ns=%lu conv=%d\n",
                     (unsigned long)g_sched_master_us, (unsigned long)realMaster,
                     (long)(int32_t)(realMaster - g_sched_master_us),
                     (long long)(g_sched_actual - g_sched_deadline),
                     (unsigned long)g_sched_uncert_ns, (int)okConv);
}

/* Coredump del ultimo panico: los esclavos no tienen USB, asi que el resumen
 * (tarea, PC, backtrace) sale por el log remoto a los 20, 40 y 60 s del
 * arranque y despues se borra. Decodificar con xtensa-esp32s3-elf-addr2line
 * contra el firmware.elf que estaba grabado. */
#if defined(ESP32) && ESP_ARDUINO_VERSION_MAJOR >= 3
#include <esp_core_dump.h>
static void serviceCoreDumpReport()
{
    static uint8_t sent = 0;
    if (sent >= 4u || millis() < 20000u * (uint32_t)(sent + 1u)) return;
    sent++;
    if (esp_core_dump_image_check() != ESP_OK) {
        if (sent == 1u) SLAVE_LOG_PRINTF("[COREDUMP] no hay (reset=%d)\n", (int)esp_reset_reason());
        sent = 4u;
        return;
    }
    if (sent == 4u) {
        SLAVE_LOG_PRINTF("[COREDUMP] borrado err=%d\n", (int)esp_core_dump_image_erase());
        return;
    }
    esp_core_dump_summary_t s;
    if (esp_core_dump_get_summary(&s) != ESP_OK) {
        SLAVE_LOG_PRINTF("[COREDUMP] presente pero sin resumen\n");
        return;
    }
    char bt[16 * 11 + 1];
    size_t o = 0;
    for (uint32_t i = 0; i < s.exc_bt_info.depth && i < 16u && o + 12 < sizeof(bt); i++)
        o += snprintf(bt + o, sizeof(bt) - o, " 0x%08lx", (unsigned long)s.exc_bt_info.bt[i]);
    bt[o] = 0;
    SLAVE_LOG_PRINTF("[COREDUMP] tarea=%.16s pc=0x%08lx causa=%lu vaddr=0x%08lx corrupto=%d bt:%s\n",
                     s.exc_task, (unsigned long)s.exc_pc, (unsigned long)s.ex_info.exc_cause,
                     (unsigned long)s.ex_info.exc_vaddr, (int)s.exc_bt_info.corrupted, bt);
}
#else
static void serviceCoreDumpReport() {}
#endif

static void iniciarTimerSyncRadioSilencio()
{
    if (g_radio_silencio_sync_timer != nullptr) return;
    const esp_timer_create_args_t args = {
        .callback = &radioSilencioSyncTimerCb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "radiosil_sync",
        .skip_unhandled_events = false,
    };
    if (esp_timer_create(&args, &g_radio_silencio_sync_timer) != ESP_OK) {
        g_radio_silencio_sync_timer = nullptr;
        SLAVE_LOG_RAW("#RADIOSIL timer FAIL\n");
    }
}

/* El tiempo real necesita la radio: anula los modos silenciosos. */
static bool syncModeLive()
{
    return live::active() && live::config().realtime;
}

/* radiosil (banco, por USB) o el modo "radio apagada" de esta captura. */
static bool radioSilActiva()
{
    return g_radio_silencio || (g_syncMode == SYNCMODE_RADIO_OFF && !syncModeLive());
}

static uint16_t radioSilDelayMs()
{
    if (g_radio_silencio) return g_radio_silencio_ms;
    return RADIO_OFF_SYNC_DELAY_MS;
}

static void subirSyncConRadioSilencio()
{
    if (!radioSilActiva() || radioSilDelayMs() == 0u || syncModeLive()) {
        digitalWrite(SYNC_TO_PSOC_PIN, HIGH);
        return;
    }
    g_radio_silencio_pedido = true;
    if (g_radio_silencio_sync_timer == nullptr) {
        digitalWrite(SYNC_TO_PSOC_PIN, HIGH);
        return;
    }
    esp_timer_stop(g_radio_silencio_sync_timer);
    if (esp_timer_start_once(g_radio_silencio_sync_timer,
                             (uint64_t)radioSilDelayMs() * 1000u) != ESP_OK) {
        digitalWrite(SYNC_TO_PSOC_PIN, HIGH);
    }
}

static void radioApagar()
{
    if (g_radio_on) {
        live::setRadioReady(false);
#if !defined(ESP8266)
        esp_wifi_set_promiscuous(false);
#endif
        esp_now_deinit();
        esp_wifi_stop();
        g_radio_on = false;
    }
}

static void radioPrender()
{
    if (!g_radio_on) {
        esp_wifi_start();
        esp_wifi_set_ps(WIFI_PS_NONE);
        esp_wifi_set_max_tx_power(WIFI_TX_POWER_QDBM);
        esp_wifi_set_channel(g_masterChannel, WIFI_SECOND_CHAN_NONE);
        const bool ok = transport.begin(g_masterTxMac);
        esp_now_register_recv_cb(ESPNOW_RECV_CB);
        applyBeaconMarks();
        g_radio_on = true;
        live::setRadioReady(true);
        if (!ok) SLAVE_LOG_RAW("#RADIO on: ESP-NOW FAIL\n");
    }
}

static void radioSilencioAlEntrar()
{
    /* En tiempo real la radio tiene que quedar prendida. */
    if (syncModeLive()) return;
    if (radioSilActiva()) g_radio_silencio_pedido = true;
}

static void serviceRadioSilencio(bool desdeLoop)
{
    if (desdeLoop && g_radio_silencio_pedido) {
        if (radioSilActiva() && g_radio_on &&
            (g_state == HOT_WAIT || g_state == SAMPLING)) {
            radioApagar();
            g_radio_apagada_por_silencio = true;
        }
        g_radio_silencio_pedido = false;
    }
    if (g_radio_apagada_por_silencio &&
        g_state != SAMPLING && g_state != HOT_WAIT) {
        radioPrender();
        g_radio_apagada_por_silencio = false;
        SLAVE_LOG_RAW("#RADIOSIL vuelve\n");
    }
}

static void requestRadioFromUsb(bool on)
{
    if (on) radioPrender();
    else radioApagar();
    g_radio_apagada_por_silencio = false;
    SLAVE_LOG_RAW("#RADIO %s\n", g_radio_on ? "on" : "off");
}

static void requestRadioSilencioFromUsb(bool on, uint16_t ms = 0u)
{
    g_radio_silencio = on;
    g_radio_silencio_ms = on ? ms : 0u;
    if (!on) g_radio_silencio_pedido = false;
    if (!on && g_radio_apagada_por_silencio) {
        radioPrender();
        g_radio_apagada_por_silencio = false;
    }
    SLAVE_LOG_RAW("#RADIOSIL %u %u\n", (unsigned)g_radio_silencio,
                  (unsigned)g_radio_silencio_ms);
}

/* Banco: escrituras a la SD del PSoC EN PARALELO a una captura corta que SI
 * entra en RAM (no cambia la regla de g_psoc_sd_store_active: es un estresor
 * aparte). Reusa PSOC_CMD_SD_TEST (0xBD, self-test GEOTEST.BIN, el mismo
 * camino que "sdtest"); _sendCmd1() es un unico write() de 4 bytes por la
 * UART del PSoC, no bloquea. El ack 0xBD llega por servicePsocConfigAck() ->
 * applyConfirmedConfig(), que cuenta el fallo (ver case PSOC_CMD_SD_TEST). */
static void requestSdStressFromUsb(uint32_t ms, uint32_t periodoMs)
{
    if (ms == 0u) {
        g_sdstress_hasta_ms = 0u;
        g_sdstress_running = false;
        SLAVE_LOG_RAW("#SDSTRESS fin escrituras=%lu fallos=%lu\n",
                      (unsigned long)g_sdstress_escrituras, (unsigned long)g_sdstress_fallos);
        LOGM("USB_CMD", "cmd=sdstress,ok=1,cancel=1");
        return;
    }
    if (!psocSdPresent()) {
        SLAVE_LOG_RAW("#SDSTRESS sin sd\n");
        LOGM("USB_CMD", "cmd=sdstress,ok=0,reason=sin_sd");
        return;
    }
    g_sdstress_periodo_ms = periodoMs;
    g_sdstress_escrituras = 0u;
    g_sdstress_fallos     = 0u;
    g_sdstress_prox_ms    = millis();
    g_sdstress_running    = true;
    g_sdstress_hasta_ms   = millis() + ms;
    SLAVE_LOG_RAW("#SDSTRESS inicio ms=%lu periodo=%lu\n",
                  (unsigned long)ms, (unsigned long)periodoMs);
    LOGM("USB_CMD", "cmd=sdstress,ok=1,ms=%lu,periodo=%lu",
         (unsigned long)ms, (unsigned long)periodoMs);
}

/* loop(): una escritura por periodo, sin bloquear (ver nota de arriba). */
static void serviceSdStress()
{
    if (g_sdstress_hasta_ms == 0u) return;
    const uint32_t now = millis();
    if (now >= g_sdstress_hasta_ms) {
        g_sdstress_hasta_ms = 0u;
        if (g_sdstress_running) {
            g_sdstress_running = false;
            SLAVE_LOG_RAW("#SDSTRESS fin escrituras=%lu fallos=%lu\n",
                          (unsigned long)g_sdstress_escrituras, (unsigned long)g_sdstress_fallos);
            LOGM("USB_CMD", "cmd=sdstress,event=fin,escrituras=%lu,fallos=%lu",
                 (unsigned long)g_sdstress_escrituras, (unsigned long)g_sdstress_fallos);
        }
        return;
    }
    if (now < g_sdstress_prox_ms) return;
    g_sdstress_prox_ms = now + g_sdstress_periodo_ms;
    psoc.sdTest();
    g_sdstress_escrituras++;
}

static void requestSdProbeFromUsb(uint32_t ms, uint32_t periodoMs,
                                  uint8_t modo, uint8_t div, uint8_t slew)
{
    if (ms == 0u) {
        g_sdprobe_hasta_ms = 0u;
        g_sdprobe_running = false;
        /* El PSoC arma la sonda en IDLE y la ejecuta en la captura: hay que
         * desarmarla explicitamente (modo 6) o queda activa para la proxima. */
        psoc.sdProbe(6u, 0u, 0u);
        SLAVE_LOG_RAW("#SDPROBE fin envios=%lu fallos=%lu\n",
                      (unsigned long)g_sdprobe_envios, (unsigned long)g_sdprobe_fallos);
        LOGM("USB_CMD", "cmd=sdprobe,ok=1,cancel=1");
        return;
    }
    /* Los modos 0-3 no tocan la tarjeta (solo CS/bus): corren aun sin SD. */
    if (modo >= 4u && modo <= 5u && !psocSdPresent()) {
        SLAVE_LOG_RAW("#SDPROBE sin sd\n");
        LOGM("USB_CMD", "cmd=sdprobe,ok=0,reason=sin_sd");
        return;
    }
    g_sdprobe_periodo_ms = periodoMs;
    g_sdprobe_envios     = 0u;
    g_sdprobe_fallos     = 0u;
    g_sdprobe_prox_ms    = millis();
    g_sdprobe_modo       = modo;
    g_sdprobe_div        = div;
    g_sdprobe_slew       = slew;
    g_sdprobe_running    = true;
    g_sdprobe_hasta_ms   = millis() + ms;
    SLAVE_LOG_RAW("#SDPROBE inicio modo=%u periodo=%lu div=%u slew=%u\n",
                  (unsigned)modo, (unsigned long)periodoMs,
                  (unsigned)div, (unsigned)slew);
    LOGM("USB_CMD", "cmd=sdprobe,ok=1,ms=%lu,periodo=%lu,modo=%u,div=%u,slew=%u",
         (unsigned long)ms, (unsigned long)periodoMs, (unsigned)modo,
         (unsigned)div, (unsigned)slew);
}

static void serviceSdProbe()
{
    if (g_sdprobe_hasta_ms == 0u) return;
    const uint32_t now = millis();
    if (now >= g_sdprobe_hasta_ms) {
        g_sdprobe_hasta_ms = 0u;
        if (g_sdprobe_running) {
            g_sdprobe_running = false;
            SLAVE_LOG_RAW("#SDPROBE fin envios=%lu fallos=%lu\n",
                          (unsigned long)g_sdprobe_envios, (unsigned long)g_sdprobe_fallos);
            LOGM("USB_CMD", "cmd=sdprobe,event=fin,envios=%lu,fallos=%lu",
                 (unsigned long)g_sdprobe_envios, (unsigned long)g_sdprobe_fallos);
        }
        return;
    }
    if (now < g_sdprobe_prox_ms) return;
    g_sdprobe_prox_ms = now + g_sdprobe_periodo_ms;

    psoc.sdProbe(g_sdprobe_modo, g_sdprobe_div, g_sdprobe_slew);
    g_sdprobe_envios++;
}

static void finishEspSd()
{
    SLAVE_LOG_RAW("#ESPSD pos %lu", (unsigned long)g_espsd_escrituras);
    for (uint32_t i = 0u; i < g_espsd_escrituras; i++) {
        SLAVE_LOG_RAW(" %lu", (unsigned long)g_espsd_pos[i]);
    }
    SLAVE_LOG_RAW("\n");
    SLAVE_LOG_RAW("#ESPSD fin escrituras=%lu fallos=%lu\n",
                  (unsigned long)g_espsd_escrituras,
                  (unsigned long)g_espsd_fallos);
    SD.end();
    g_espsd_activo = false;
    g_espsd_en_sampling = false;
    g_espsd_tiempo_cumplido = false;
}

static void cancelEspSd()
{
    if (g_espsd_activo) {
        SD.end();
    }
    g_espsd_activo = false;
    g_espsd_en_sampling = false;
    g_espsd_tiempo_cumplido = false;
    g_espsd_hasta_ms = 0u;
}

/* sddiag: diagnóstico de bajo nivel de la SD del E14 (HSPI: SCK 14, MISO 32,
 * MOSI 13, CS 25), sin la librería SD. Imprime el estado eléctrico de MISO con
 * CS alto/bajo y cada respuesta del protocolo byte por byte. */
/* SDDIAG_* vienen de board_pins_s3.h. */
static uint8_t sdDiagXfer(uint8_t b) { return spiHspi.transfer(b); }

static void sdDiagDump(const char *tag, uint8_t n)
{
    SLAVE_LOG_RAW("#SDDIAG %s:", tag);
    for (uint8_t i = 0; i < n; i++) SLAVE_LOG_RAW(" %02X", sdDiagXfer(0xFF));
    SLAVE_LOG_RAW("\n");
}

static uint8_t sdDiagCmd(uint8_t cmd, uint32_t arg, uint8_t crc)
{
    sdDiagXfer(0xFF);
    sdDiagXfer(0x40 | cmd);
    sdDiagXfer(arg >> 24); sdDiagXfer(arg >> 16); sdDiagXfer(arg >> 8); sdDiagXfer(arg);
    sdDiagXfer(crc);
    uint8_t r1 = 0xFF;
    for (uint8_t i = 0; i < 16; i++) { r1 = sdDiagXfer(0xFF); if (!(r1 & 0x80)) break; }
    return r1;
}

/* Nivel de MISO con pull-up y con pull-down internos: si sigue al pull, la
 * línea flota (nadie la maneja); si no, alguien la maneja a ese nivel. */
static void sdDiagMisoLevel(const char *tag)
{
    spiHspi.end();
    pinMode(SDDIAG_MISO, INPUT_PULLUP);   delayMicroseconds(200);
    const int up = digitalRead(SDDIAG_MISO);
    pinMode(SDDIAG_MISO, INPUT_PULLDOWN); delayMicroseconds(200);
    const int dn = digitalRead(SDDIAG_MISO);
    pinMode(SDDIAG_MISO, INPUT);
    SLAVE_LOG_RAW("#SDDIAG miso %s: pullup=%d pulldown=%d -> %s\n", tag, up, dn,
                  (up && !dn) ? "FLOTA" : (up && dn) ? "MANEJADA ALTO" :
                  (!up && !dn) ? "MANEJADA BAJO" : "RARO");
    spiHspi.begin(SDDIAG_SCK, SDDIAG_MISO, SDDIAG_MOSI, -1);
}

static void sdDiagnostico()
{
    SD.end();
    pinMode(SDDIAG_CS, OUTPUT);
    digitalWrite(SDDIAG_CS, HIGH);
    spiHspi.begin(SDDIAG_SCK, SDDIAG_MISO, SDDIAG_MOSI, -1);
    sdDiagMisoLevel("CS_alto");
    digitalWrite(SDDIAG_CS, LOW);
    sdDiagMisoLevel("CS_bajo_sin_init");
    digitalWrite(SDDIAG_CS, HIGH);

    spiHspi.beginTransaction(SPISettings(250000, MSBFIRST, SPI_MODE0));
    for (uint8_t i = 0; i < 12; i++) sdDiagXfer(0xFF);   /* >=74 clocks, CS alto */
    digitalWrite(SDDIAG_CS, LOW);
    sdDiagDump("idle_CS_bajo", 8);

    uint8_t r1 = 0xFF;
    for (uint8_t i = 0; i < 10 && r1 != 0x01; i++) r1 = sdDiagCmd(0, 0, 0x95);
    SLAVE_LOG_RAW("#SDDIAG CMD0 r1=%02X (esperado 01)\n", r1);
    r1 = sdDiagCmd(8, 0x1AA, 0x87);
    SLAVE_LOG_RAW("#SDDIAG CMD8 r1=%02X\n", r1);
    sdDiagDump("CMD8_R7(esperado 00 00 01 AA)", 4);

    uint16_t n = 0;
    for (; n < 1000; n++) {
        sdDiagCmd(55, 0, 0xFF);
        r1 = sdDiagCmd(41, 0x40000000, 0xFF);
        if (r1 == 0x00 || (r1 & 0x80)) break;
        delay(1);
    }
    SLAVE_LOG_RAW("#SDDIAG ACMD41 r1=%02X intentos=%u\n", r1, n + 1);
    sdDiagDump("tras_ACMD41", 8);

    r1 = sdDiagCmd(58, 0, 0xFF);
    SLAVE_LOG_RAW("#SDDIAG CMD58 r1=%02X\n", r1);
    sdDiagDump("OCR(esperado C0 FF 80 00 o similar)", 4);

    r1 = sdDiagCmd(9, 0, 0xFF);
    SLAVE_LOG_RAW("#SDDIAG CMD9 r1=%02X\n", r1);
    sdDiagDump("CMD9_bytes(esperado FF.. FE + 16 CSD)", 40);
    sdDiagDump("idle_tras_CMD9", 8);

    r1 = sdDiagCmd(10, 0, 0xFF);
    SLAVE_LOG_RAW("#SDDIAG CMD10 r1=%02X\n", r1);
    sdDiagDump("CMD10_bytes(CID)", 40);

    r1 = sdDiagCmd(17, 0, 0xFF);
    SLAVE_LOG_RAW("#SDDIAG CMD17 r1=%02X\n", r1);
    sdDiagDump("CMD17_bytes(sector 0)", 40);

    spiHspi.endTransaction();
    sdDiagMisoLevel("CS_bajo_tras_init");
    digitalWrite(SDDIAG_CS, HIGH);
    sdDiagMisoLevel("CS_alto_final");
    SLAVE_LOG_RAW("#SDDIAG fin\n");
}

/* sddiag2: tension ANALOGICA de MISO (GPIO32 = ADC1_CH4) segun CS y pulls,
 * espera larga de busy, CMD13 y modos/velocidades alternativos. */
static void sdDiagMv(const char *tag)
{
    spiHspi.end();
    const int modos[3] = {INPUT, INPUT_PULLUP, INPUT_PULLDOWN};
    const char *nom[3] = {"sin_pull", "pullup", "pulldown"};
    SLAVE_LOG_RAW("#SDDIAG2 mv %s:", tag);
    for (int m = 0; m < 3; m++) {
        pinMode(SDDIAG_MISO, modos[m]); delay(2);
        uint32_t acc = 0;
        for (int k = 0; k < 16; k++) acc += analogReadMilliVolts(SDDIAG_MISO);
        SLAVE_LOG_RAW(" %s=%lu", nom[m], (unsigned long)(acc / 16));
    }
    SLAVE_LOG_RAW("\n");
    pinMode(SDDIAG_MISO, INPUT);
    spiHspi.begin(SDDIAG_SCK, SDDIAG_MISO, SDDIAG_MOSI, -1);
}

static void sdDiagInit(uint32_t hz, uint8_t modo, const char *tag)
{
    digitalWrite(SDDIAG_CS, HIGH);
    spiHspi.beginTransaction(SPISettings(hz, MSBFIRST, modo));
    for (uint8_t i = 0; i < 12; i++) sdDiagXfer(0xFF);
    digitalWrite(SDDIAG_CS, LOW);
    uint8_t r0 = 0xFF;
    for (uint8_t i = 0; i < 10 && r0 != 0x01; i++) r0 = sdDiagCmd(0, 0, 0x95);
    const uint8_t r8 = sdDiagCmd(8, 0x1AA, 0x87);
    uint8_t r7[4]; for (int i = 0; i < 4; i++) r7[i] = sdDiagXfer(0xFF);
    /* 2000 bytes de reposo: cuantos no son 0x00 y cuantos son 0xFF */
    uint16_t no0 = 0, ff = 0;
    for (uint16_t i = 0; i < 2000; i++) { uint8_t b = sdDiagXfer(0xFF); if (b) no0++; if (b == 0xFF) ff++; }
    const uint8_t r13 = sdDiagCmd(13, 0, 0xFF);
    const uint8_t r13b = sdDiagXfer(0xFF);
    SLAVE_LOG_RAW("#SDDIAG2 %s: CMD0=%02X CMD8=%02X R7=%02X%02X%02X%02X reposo2000: no_cero=%u ff=%u CMD13=%02X %02X\n",
                  tag, r0, r8, r7[0], r7[1], r7[2], r7[3], no0, ff, r13, r13b);
    spiHspi.endTransaction();
    digitalWrite(SDDIAG_CS, HIGH);
}

/* sddiag3: el camino de sddiag2 (que anda) + ACMD41/CMD58/CMD9, midiendo el
 * reposo despues de cada paso para ver en que paso MISO pasa a 00. */
static void sdDiagReposo(const char *tag)
{
    uint16_t ff = 0, cero = 0;
    for (uint16_t i = 0; i < 200; i++) { uint8_t b = sdDiagXfer(0xFF); if (b == 0xFF) ff++; if (!b) cero++; }
    SLAVE_LOG_RAW("#SDDIAG3 reposo %s: ff=%u cero=%u de 200\n", tag, ff, cero);
}

static void sdDiagnostico3()
{
    SD.end();
    pinMode(SDDIAG_CS, OUTPUT);
    digitalWrite(SDDIAG_CS, HIGH);
    spiHspi.begin(SDDIAG_SCK, SDDIAG_MISO, SDDIAG_MOSI, -1);
    spiHspi.beginTransaction(SPISettings(250000, MSBFIRST, SPI_MODE0));
    for (uint8_t i = 0; i < 12; i++) sdDiagXfer(0xFF);
    digitalWrite(SDDIAG_CS, LOW);
    uint8_t r = 0xFF;
    for (uint8_t i = 0; i < 10 && r != 0x01; i++) r = sdDiagCmd(0, 0, 0x95);
    SLAVE_LOG_RAW("#SDDIAG3 CMD0=%02X\n", r);
    sdDiagReposo("tras_CMD0");
    r = sdDiagCmd(8, 0x1AA, 0x87);
    SLAVE_LOG_RAW("#SDDIAG3 CMD8=%02X\n", r); sdDiagDump("R7", 4);
    sdDiagReposo("tras_CMD8");
    r = sdDiagCmd(55, 0, 0xFF);
    SLAVE_LOG_RAW("#SDDIAG3 CMD55=%02X\n", r);
    sdDiagReposo("tras_CMD55");
    uint16_t n = 0;
    for (; n < 2000; n++) {
        if (n) sdDiagCmd(55, 0, 0xFF);
        r = sdDiagCmd(41, 0x40000000, 0xFF);
        if (r == 0x00) break;
        if (n < 3 || (n % 100) == 0) { SLAVE_LOG_RAW("#SDDIAG3 ACMD41[%u]=%02X\n", n, r); sdDiagReposo("tras_ACMD41"); }
        delay(1);
    }
    SLAVE_LOG_RAW("#SDDIAG3 ACMD41 final=%02X intentos=%u\n", r, n + 1);
    sdDiagReposo("tras_ACMD41_ok");
    r = sdDiagCmd(58, 0, 0xFF);
    SLAVE_LOG_RAW("#SDDIAG3 CMD58=%02X\n", r); sdDiagDump("OCR", 4);
    sdDiagReposo("tras_CMD58");
    r = sdDiagCmd(9, 0, 0xFF);
    SLAVE_LOG_RAW("#SDDIAG3 CMD9=%02X\n", r); sdDiagDump("CMD9", 40);
    spiHspi.endTransaction();
    digitalWrite(SDDIAG_CS, HIGH);
    SLAVE_LOG_RAW("#SDDIAG3 fin\n");
}

/* sddiag4: tras ACMD41 OK, quien sostiene MISO en bajo: con CS alto la
 * tarjeta suelta DO (si sigue 00 es el modulo); espera larga de busy; CMD0. */
static void sdDiagnostico4()
{
    SD.end();
    pinMode(SDDIAG_CS, OUTPUT);
    digitalWrite(SDDIAG_CS, HIGH);
    spiHspi.begin(SDDIAG_SCK, SDDIAG_MISO, SDDIAG_MOSI, -1);
    spiHspi.beginTransaction(SPISettings(250000, MSBFIRST, SPI_MODE0));
    for (uint8_t i = 0; i < 12; i++) sdDiagXfer(0xFF);
    digitalWrite(SDDIAG_CS, LOW);
    uint8_t r = 0xFF;
    for (uint8_t i = 0; i < 10 && r != 0x01; i++) r = sdDiagCmd(0, 0, 0x95);
    sdDiagCmd(8, 0x1AA, 0x87); for (int i = 0; i < 4; i++) sdDiagXfer(0xFF);
    uint16_t n = 0;
    for (; n < 2000; n++) { sdDiagCmd(55, 0, 0xFF); r = sdDiagCmd(41, 0x40000000, 0xFF); if (r == 0x00) break; delay(1); }
    SLAVE_LOG_RAW("#SDDIAG4 ACMD41=%02X intentos=%u\n", r, n + 1);
    sdDiagReposo("CS_bajo");
    digitalWrite(SDDIAG_CS, HIGH);
    sdDiagReposo("CS_ALTO");
    delay(100);
    sdDiagReposo("CS_ALTO_100ms");
    digitalWrite(SDDIAG_CS, LOW);
    const uint32_t t0 = millis();
    uint32_t bytes = 0; uint8_t b = 0;
    while (millis() - t0 < 2000) { b = sdDiagXfer(0xFF); bytes++; if (b != 0x00) break; }
    SLAVE_LOG_RAW("#SDDIAG4 espera busy: primer_no_cero=%02X tras %lu bytes, %lu ms\n",
                  b, (unsigned long)bytes, (unsigned long)(millis() - t0));
    r = sdDiagCmd(13, 0, 0xFF);
    SLAVE_LOG_RAW("#SDDIAG4 CMD13=%02X %02X\n", r, sdDiagXfer(0xFF));
    digitalWrite(SDDIAG_CS, HIGH);
    for (uint8_t i = 0; i < 12; i++) sdDiagXfer(0xFF);
    digitalWrite(SDDIAG_CS, LOW);
    r = 0xFF;
    for (uint8_t i = 0; i < 10 && r != 0x01; i++) r = sdDiagCmd(0, 0, 0x95);
    SLAVE_LOG_RAW("#SDDIAG4 CMD0_reset=%02X\n", r);
    sdDiagReposo("tras_CMD0_reset");
    spiHspi.endTransaction();
    digitalWrite(SDDIAG_CS, HIGH);
    SLAVE_LOG_RAW("#SDDIAG4 fin\n");
}

/* sddiag5: variantes de inicializacion; tras cada una, reposo con CS bajo. */
static void sdDiagVariante(const char *tag, uint8_t tipo, uint32_t arg)
{
    digitalWrite(SDDIAG_CS, HIGH);
    for (uint8_t i = 0; i < 12; i++) sdDiagXfer(0xFF);
    digitalWrite(SDDIAG_CS, LOW);
    uint8_t r = 0xFF;
    for (uint8_t i = 0; i < 10 && r != 0x01; i++) r = sdDiagCmd(0, 0, 0x95);
    sdDiagCmd(8, 0x1AA, 0x87); for (int i = 0; i < 4; i++) sdDiagXfer(0xFF);
    uint16_t n = 0;
    for (; n < 1500; n++) {
        if (tipo == 0) { sdDiagCmd(55, 0, 0xFF); r = sdDiagCmd(41, arg, 0xFF); }
        else           { r = sdDiagCmd(1, arg, 0xFF); }
        if (r == 0x00 || (r & 0x80)) break;
        delay(1);
    }
    uint16_t ff = 0, cero = 0;
    for (uint16_t i = 0; i < 200; i++) { uint8_t b = sdDiagXfer(0xFF); if (b == 0xFF) ff++; if (!b) cero++; }
    SLAVE_LOG_RAW("#SDDIAG5 %s: r=%02X intentos=%u reposo ff=%u cero=%u\n", tag, r, n + 1, ff, cero);
}

static void sdDiagnostico5()
{
    SD.end();
    pinMode(SDDIAG_CS, OUTPUT);
    digitalWrite(SDDIAG_CS, HIGH);
    spiHspi.begin(SDDIAG_SCK, SDDIAG_MISO, SDDIAG_MOSI, -1);
    spiHspi.beginTransaction(SPISettings(250000, MSBFIRST, SPI_MODE0));
    sdDiagVariante("ACMD41_HCS1", 0, 0x40000000);
    sdDiagVariante("ACMD41_HCS0", 0, 0x00000000);
    sdDiagVariante("ACMD41_HCS1_ventana", 0, 0x40FF8000);
    sdDiagVariante("ACMD41_XPC", 0, 0x50000000);
    sdDiagVariante("CMD1", 1, 0x00000000);
    spiHspi.endTransaction();
    digitalWrite(SDDIAG_CS, HIGH);
    SLAVE_LOG_RAW("#SDDIAG5 fin\n");
}

/* sddiag6: discrimina alimentacion vs tarjeta.
 *  a) con la radio apagada (menos carga en el 5 V compartido);
 *  b) ACMD41 espaciados con CS ALTO entre intentos (sin clocks: menos consumo)
 *     y deteccion del primer byte de reposo que no es FF tras cada ACMD41. */
static void sdDiag6Init(const char *tag, uint16_t pausaMs)
{
    digitalWrite(SDDIAG_CS, HIGH);
    for (uint8_t i = 0; i < 12; i++) sdDiagXfer(0xFF);
    digitalWrite(SDDIAG_CS, LOW);
    uint8_t r = 0xFF;
    for (uint8_t i = 0; i < 10 && r != 0x01; i++) r = sdDiagCmd(0, 0, 0x95);
    sdDiagCmd(8, 0x1AA, 0x87); for (int i = 0; i < 4; i++) sdDiagXfer(0xFF);
    uint16_t n = 0; uint16_t ffAntes = 0;
    for (; n < 400; n++) {
        sdDiagCmd(55, 0, 0xFF);
        r = sdDiagCmd(41, 0x40000000, 0xFF);
        ffAntes = 0;
        for (uint8_t i = 0; i < 32; i++) { if (sdDiagXfer(0xFF) == 0xFF) ffAntes++; }
        if (r == 0x00 || ffAntes < 32) break;     /* lista, o la linea se cayo */
        digitalWrite(SDDIAG_CS, HIGH); sdDiagXfer(0xFF);
        delay(pausaMs);
        digitalWrite(SDDIAG_CS, LOW);
    }
    uint16_t ff = 0;
    for (uint16_t i = 0; i < 200; i++) if (sdDiagXfer(0xFF) == 0xFF) ff++;
    const uint8_t r58 = sdDiagCmd(58, 0, 0xFF);
    uint8_t ocr[4]; for (int i = 0; i < 4; i++) ocr[i] = sdDiagXfer(0xFF);
    SLAVE_LOG_RAW("#SDDIAG6 %s: ACMD41 r=%02X en intento %u, ff_tras_r1=%u/32, reposo_ff=%u/200, CMD58=%02X OCR=%02X%02X%02X%02X\n",
                  tag, r, n + 1, ffAntes, ff, r58, ocr[0], ocr[1], ocr[2], ocr[3]);
    digitalWrite(SDDIAG_CS, HIGH);
}

static void sdDiagnostico6()
{
    SD.end();
    pinMode(SDDIAG_CS, OUTPUT);
    digitalWrite(SDDIAG_CS, HIGH);
    spiHspi.begin(SDDIAG_SCK, SDDIAG_MISO, SDDIAG_MOSI, -1);
    spiHspi.beginTransaction(SPISettings(250000, MSBFIRST, SPI_MODE0));
    sdDiag6Init("radio_ON_pausa1ms", 1);
    sdDiag6Init("radio_ON_pausa50ms", 50);
    spiHspi.endTransaction();
    WiFi.mode(WIFI_OFF);
    delay(300);
    spiHspi.beginTransaction(SPISettings(250000, MSBFIRST, SPI_MODE0));
    sdDiag6Init("radio_OFF_pausa1ms", 1);
    sdDiag6Init("radio_OFF_pausa50ms", 50);
    spiHspi.endTransaction();
    SLAVE_LOG_RAW("#SDDIAG6 fin (radio quedo APAGADA: reiniciar el ESP)\n");
}

/* sddiag7: SPI por bit-banging para leer MISO con el ADC entre bytes. El
 * nivel ALTO de MISO es el VCC del buffer del modulo (= riel 3,3 V de la
 * tarjeta): si el riel se hunde, se ve bajar antes de la caida a 0. */
/* Pines SD definidos por la placa. */
static uint8_t bbXfer(uint8_t out)
{
    uint8_t in = 0;
    for (int8_t b = 7; b >= 0; b--) {
        digitalWrite(SDDIAG_MOSI, (out >> b) & 1);
        delayMicroseconds(4);
        digitalWrite(SDDIAG_SCK, HIGH);
        in = (uint8_t)((in << 1) | (digitalRead(SDDIAG_MISO) ? 1 : 0));
        delayMicroseconds(4);
        digitalWrite(SDDIAG_SCK, LOW);
    }
    return in;
}

static uint8_t bbCmd(uint8_t cmd, uint32_t arg, uint8_t crc)
{
    bbXfer(0xFF); bbXfer(0x40 | cmd);
    bbXfer(arg >> 24); bbXfer(arg >> 16); bbXfer(arg >> 8); bbXfer(arg); bbXfer(crc);
    uint8_t r = 0xFF;
    for (uint8_t i = 0; i < 16; i++) { r = bbXfer(0xFF); if (!(r & 0x80)) break; }
    return r;
}

/* n bytes de reposo; imprime byte y mV de MISO tras cada byte */
static void bbSerie(const char *tag, uint8_t n)
{
    SLAVE_LOG_RAW("#SDDIAG7 %s:", tag);
    for (uint8_t i = 0; i < n; i++) {
        const uint8_t b = bbXfer(0xFF);
        const uint32_t mv = analogReadMilliVolts(SDDIAG_MISO);
        pinMode(SDDIAG_MISO, INPUT);   /* analogRead deja el pin analogico */
        SLAVE_LOG_RAW(" %02X/%lu", b, (unsigned long)mv);
    }
    SLAVE_LOG_RAW("\n");
}

static void sdDiagnostico7()
{
    SD.end();
    spiHspi.end();
    pinMode(SDDIAG_CS, OUTPUT);   digitalWrite(SDDIAG_CS, HIGH);
    pinMode(SDDIAG_SCK, OUTPUT);  digitalWrite(SDDIAG_SCK, LOW);
    pinMode(SDDIAG_MOSI, OUTPUT); digitalWrite(SDDIAG_MOSI, HIGH);
    pinMode(SDDIAG_MISO, INPUT);
    for (uint8_t i = 0; i < 12; i++) bbXfer(0xFF);
    digitalWrite(SDDIAG_CS, LOW);
    uint8_t r = 0xFF;
    for (uint8_t i = 0; i < 10 && r != 0x01; i++) r = bbCmd(0, 0, 0x95);
    SLAVE_LOG_RAW("#SDDIAG7 CMD0=%02X\n", r);
    bbSerie("reposo_idle", 12);
    r = bbCmd(8, 0x1AA, 0x87);
    uint8_t r7[4]; for (int i = 0; i < 4; i++) r7[i] = bbXfer(0xFF);
    SLAVE_LOG_RAW("#SDDIAG7 CMD8=%02X R7=%02X%02X%02X%02X\n", r, r7[0], r7[1], r7[2], r7[3]);
    bbSerie("reposo_tras_CMD8", 12);
    bbCmd(55, 0, 0xFF);
    r = bbCmd(41, 0x40000000, 0xFF);
    SLAVE_LOG_RAW("#SDDIAG7 ACMD41#1=%02X\n", r);
    bbSerie("tras_ACMD41_a", 40);
    bbSerie("tras_ACMD41_b", 40);
    bbCmd(55, 0, 0xFF);
    r = bbCmd(41, 0x40000000, 0xFF);
    SLAVE_LOG_RAW("#SDDIAG7 ACMD41#2=%02X\n", r);
    bbSerie("tras_ACMD41_2", 40);
    digitalWrite(SDDIAG_CS, HIGH);
    bbSerie("CS_alto", 12);
    /* Ioff: 74LVC125A sin VCC deja la salida en alta Z -> MISO sigue al pull */
    for (int k = 0; k < 3; k++) {
        pinMode(SDDIAG_MISO, INPUT_PULLUP);   delay(2); const int up = digitalRead(SDDIAG_MISO);
        pinMode(SDDIAG_MISO, INPUT_PULLDOWN); delay(2); const int dn = digitalRead(SDDIAG_MISO);
        pinMode(SDDIAG_MISO, INPUT);
        SLAVE_LOG_RAW("#SDDIAG7 trabado CS_alto t=%dms: pullup=%d pulldown=%d -> %s\n", k * 200, up, dn,
                      (up && !dn) ? "ALTA_Z (riel caido / buffer sin VCC)" :
                      (!up && !dn) ? "MANEJADA_BAJO (riel vivo)" : (up && dn) ? "MANEJADA_ALTO" : "RARO");
        delay(200);
    }
    digitalWrite(SDDIAG_CS, LOW);
    r = 0xFF;
    for (uint8_t i = 0; i < 10 && r != 0x01; i++) r = bbCmd(0, 0, 0x95);
    SLAVE_LOG_RAW("#SDDIAG7 CMD0_tras_trabado=%02X\n", r);
    bbSerie("tras_CMD0", 6);
    digitalWrite(SDDIAG_CS, HIGH);
    pinMode(SDDIAG_MISO, INPUT_PULLUP);   delay(2); const int up2 = digitalRead(SDDIAG_MISO);
    pinMode(SDDIAG_MISO, INPUT_PULLDOWN); delay(2); const int dn2 = digitalRead(SDDIAG_MISO);
    pinMode(SDDIAG_MISO, INPUT);
    SLAVE_LOG_RAW("#SDDIAG7 sano CS_alto: pullup=%d pulldown=%d\n", up2, dn2);
    SLAVE_LOG_RAW("#SDDIAG7 fin\n");
    pinMode(SDDIAG_MISO, INPUT);
}

/* sddiag8: VDD real vs alimentacion parasita por las entradas. Se deja la
 * tarjeta en idle SPI, se ponen CS/CLK/DI en 0 V (sin camino parasito) y se
 * pregunta CMD13 SIN CMD0: si responde 01 conservo el estado (VDD bueno). */
static void sdDiag8Ayuno(const char *tag, uint8_t csNivel, uint16_t ms)
{
    digitalWrite(SDDIAG_SCK, LOW);
    digitalWrite(SDDIAG_MOSI, LOW);
    digitalWrite(SDDIAG_CS, csNivel);
    delay(ms);
    digitalWrite(SDDIAG_MOSI, HIGH);
    digitalWrite(SDDIAG_CS, LOW);
    const uint8_t r = bbCmd(13, 0, 0xFF);
    const uint8_t r2 = bbXfer(0xFF);
    SLAVE_LOG_RAW("#SDDIAG8 %s: CMD13 sin CMD0 = %02X %02X -> %s\n", tag, r, r2,
                  (r == 0x01) ? "conservo estado SPI (alimentada)" :
                  (r == 0xFF) ? "sin respuesta (perdio estado)" : "otro");
}

static void sdDiagnostico8()
{
    SD.end();
    spiHspi.end();
    pinMode(SDDIAG_CS, OUTPUT);   digitalWrite(SDDIAG_CS, HIGH);
    pinMode(SDDIAG_SCK, OUTPUT);  digitalWrite(SDDIAG_SCK, LOW);
    pinMode(SDDIAG_MOSI, OUTPUT); digitalWrite(SDDIAG_MOSI, HIGH);
    pinMode(SDDIAG_MISO, INPUT);
    for (int rep = 0; rep < 2; rep++) {
        digitalWrite(SDDIAG_CS, HIGH);
        for (uint8_t i = 0; i < 12; i++) bbXfer(0xFF);
        digitalWrite(SDDIAG_CS, LOW);
        uint8_t r = 0xFF;
        for (uint8_t i = 0; i < 10 && r != 0x01; i++) r = bbCmd(0, 0, 0x95);
        r = bbCmd(8, 0x1AA, 0x87); for (int i = 0; i < 4; i++) bbXfer(0xFF);
        const uint8_t r13 = bbCmd(13, 0, 0xFF); bbXfer(0xFF);
        SLAVE_LOG_RAW("#SDDIAG8 rep%d base: CMD8=%02X CMD13=%02X\n", rep, r, r13);
        sdDiag8Ayuno(rep ? "ayuno_CS_ALTO_300ms" : "ayuno_TODO_0V_300ms", rep ? HIGH : LOW, 300);
        sdDiag8Ayuno(rep ? "ayuno_CS_ALTO_2s" : "ayuno_TODO_0V_2s", rep ? HIGH : LOW, 2000);
    }
    /* trabado: tras ACMD41 y 2 s con todo en 0 V, ¿la linea vuelve sola? */
    digitalWrite(SDDIAG_CS, HIGH);
    for (uint8_t i = 0; i < 12; i++) bbXfer(0xFF);
    digitalWrite(SDDIAG_CS, LOW);
    uint8_t r = 0xFF;
    for (uint8_t i = 0; i < 10 && r != 0x01; i++) r = bbCmd(0, 0, 0x95);
    bbCmd(8, 0x1AA, 0x87); for (int i = 0; i < 4; i++) bbXfer(0xFF);
    bbCmd(55, 0, 0xFF); r = bbCmd(41, 0x40000000, 0xFF);
    uint8_t c = 0; for (int i = 0; i < 40; i++) if (bbXfer(0xFF) == 0) c++;
    SLAVE_LOG_RAW("#SDDIAG8 trabada: ACMD41=%02X ceros=%u/40\n", r, c);
    digitalWrite(SDDIAG_SCK, LOW); digitalWrite(SDDIAG_MOSI, LOW); digitalWrite(SDDIAG_CS, LOW);
    delay(2000);
    digitalWrite(SDDIAG_MOSI, HIGH);
    c = 0; for (int i = 0; i < 40; i++) if (bbXfer(0xFF) == 0) c++;
    SLAVE_LOG_RAW("#SDDIAG8 trabada tras 2s a 0V: ceros=%u/40\n", c);
    digitalWrite(SDDIAG_CS, HIGH);
    SLAVE_LOG_RAW("#SDDIAG8 fin\n");
}

/* sddiag9 (sin tarjeta): MISO analogico para cada combinacion de CS/SCK/MOSI
 * y con/sin pull-up interno -> a que linea sigue MISO y cuanto la carga. */
static uint32_t sdDiag9Mv(int modo)
{
    pinMode(SDDIAG_MISO, modo); delay(3);
    uint32_t acc = 0;
    for (int k = 0; k < 16; k++) acc += analogReadMilliVolts(SDDIAG_MISO);
    pinMode(SDDIAG_MISO, INPUT);
    return acc / 16;
}

static void sdDiagnostico9()
{
    SD.end();
    spiHspi.end();
    pinMode(SDDIAG_CS, OUTPUT); pinMode(SDDIAG_SCK, OUTPUT); pinMode(SDDIAG_MOSI, OUTPUT);
    for (int m = 0; m < 8; m++) {
        const int cs = (m >> 2) & 1, sck = (m >> 1) & 1, mosi = m & 1;
        digitalWrite(SDDIAG_CS, cs); digitalWrite(SDDIAG_SCK, sck); digitalWrite(SDDIAG_MOSI, mosi);
        delay(5);
        const uint32_t a = sdDiag9Mv(INPUT), b = sdDiag9Mv(INPUT_PULLUP), c = sdDiag9Mv(INPUT_PULLDOWN);
        SLAVE_LOG_RAW("#SDDIAG9 CS=%d SCK=%d MOSI=%d : miso_mV sin_pull=%lu pullup=%lu pulldown=%lu\n",
                      cs, sck, mosi, (unsigned long)a, (unsigned long)b, (unsigned long)c);
    }
    digitalWrite(SDDIAG_CS, HIGH);
    SLAVE_LOG_RAW("#SDDIAG9 fin\n");
}

static void sdDiagnostico2()
{
    SD.end();
    pinMode(SDDIAG_CS, OUTPUT);
    digitalWrite(SDDIAG_CS, HIGH);
    spiHspi.begin(SDDIAG_SCK, SDDIAG_MISO, SDDIAG_MOSI, -1);
    sdDiagMv("CS_alto");
    digitalWrite(SDDIAG_CS, LOW);  delay(5);
    sdDiagMv("CS_bajo");
    digitalWrite(SDDIAG_CS, HIGH); delay(5);
    sdDiagInit(250000, SPI_MODE0, "250k_modo0");
    sdDiagInit(100000, SPI_MODE0, "100k_modo0");
    sdDiagInit(250000, SPI_MODE3, "250k_modo3");
    sdDiagInit(1000000, SPI_MODE0, "1M_modo0");
    /* tras init: CS bajo sin clocks y CS alto */
    digitalWrite(SDDIAG_CS, LOW);  delay(5);
    sdDiagMv("CS_bajo_tras_init");
    digitalWrite(SDDIAG_CS, HIGH); delay(5);
    sdDiagMv("CS_alto_tras_init");
    /* CS (GPIO25) realmente conmuta? leer el propio pin */
    digitalWrite(SDDIAG_CS, LOW);  delay(1); const int csl = digitalRead(SDDIAG_CS);
    digitalWrite(SDDIAG_CS, HIGH); delay(1); const int csh = digitalRead(SDDIAG_CS);
    SLAVE_LOG_RAW("#SDDIAG2 CS lectura bajo=%d alto=%d\n#SDDIAG2 fin\n", csl, csh);
}

/* Banco: monta la SD del ESP (HSPI) y, si no tiene sistema de archivos, la
 * formatea en FAT (BORRA la tarjeta). Para tarjetas nuevas o en exFAT, que
 * SD.begin() no monta y dejan a espsd en "sin_sd". */
static void requestEspSdFormatFromUsb()
{
    cancelEspSd();
    spiHspi.begin(LIVE_SD_SCK, LIVE_SD_MISO, LIVE_SD_MOSI, LIVE_SD_CS);
    SLAVE_LOG_RAW("#ESPSDFMT inicio (puede tardar minutos)\n");
    const uint32_t t0 = millis();
    const bool ok = SD.begin(LIVE_SD_CS, spiHspi, 4000000, "/sd", 5, true);
    SLAVE_LOG_RAW("#ESPSDFMT ok=%d tipo=%u sectores=%lu MB=%llu ms=%lu\n",
                  ok ? 1 : 0, (unsigned)SD.cardType(),
                  (unsigned long)SD.numSectors(),
                  (unsigned long long)(SD.cardSize() / (1024ull * 1024ull)),
                  (unsigned long)(millis() - t0));
    SD.end();
}

/* Banco: escribe /foo.txt en la SD del ESP, lo lee de vuelta y lista la raiz.
 * Prueba visible de que la tarjeta monta y acepta escrituras con archivo. */
static void requestEspSdFooFromUsb()
{
    cancelEspSd();
    spiHspi.begin(LIVE_SD_SCK, LIVE_SD_MISO, LIVE_SD_MOSI, LIVE_SD_CS);
    if (!SD.begin(LIVE_SD_CS, spiHspi, 4000000)) {
        SLAVE_LOG_RAW("#ESPSDFOO sin_sd\n");
        SD.end();
        return;
    }
    File f = SD.open("/foo.txt", FILE_APPEND);
    const size_t n = f ? f.printf("foo nodo=%u ms=%lu\n", (unsigned)NODE_ID,
                                  (unsigned long)millis()) : 0u;
    if (f) f.close();
    SLAVE_LOG_RAW("#ESPSDFOO escritos=%u MB=%llu\n", (unsigned)n,
                  (unsigned long long)(SD.cardSize() / (1024ull * 1024ull)));
    f = SD.open("/foo.txt", FILE_READ);
    if (f) {
        SLAVE_LOG_RAW("#ESPSDFOO contenido (%u B):\n", (unsigned)f.size());
        while (f.available()) {
            const String linea = f.readStringUntil('\n');
            SLAVE_LOG_RAW("#ESPSDFOO | %s\n", linea.c_str());
        }
        f.close();
    }
    File raiz = SD.open("/");
    for (File e = raiz.openNextFile(); e; e = raiz.openNextFile()) {
        SLAVE_LOG_RAW("#ESPSDFOO ls %s %u\n", e.name(), (unsigned)e.size());
        e.close();
    }
    raiz.close();
    SD.end();
    SLAVE_LOG_RAW("#ESPSDFOO fin\n");
}

static void requestEspSdFromUsb(uint32_t ms, uint32_t cadaLotes)
{
    cancelEspSd();
    if (ms == 0u) return;

    spiHspi.begin(LIVE_SD_SCK, LIVE_SD_MISO, LIVE_SD_MOSI, LIVE_SD_CS);
    if (!SD.begin(LIVE_SD_CS, spiHspi, 4000000) || SD.numSectors() < 2u) {
        SLAVE_LOG_RAW("#ESPSD sin_sd\n");
        SD.end();
        return;
    }

    g_espsd_hasta_ms = millis() + ms;
    g_espsd_cada_lotes = cadaLotes;
    g_espsd_base = 0u;
    g_espsd_proximo = cadaLotes;
    g_espsd_escrituras = 0u;
    g_espsd_fallos = 0u;
    g_espsd_sector = (uint32_t)SD.numSectors() - 2u;
    g_espsd_en_sampling = false;
    g_espsd_tiempo_cumplido = false;
    g_espsd_activo = true;
}

static void serviceEspSd()
{
    if (!g_espsd_activo) return;

    const uint32_t now = millis();
    if ((int32_t)(now - g_espsd_hasta_ms) >= 0) {
        g_espsd_tiempo_cumplido = true;
    }

    if (g_state != SAMPLING) {
        if (g_espsd_en_sampling) {
            g_espsd_en_sampling = false;
            if (g_espsd_escrituras > 0u) {
                finishEspSd();
                return;
            }
        }
        if (g_espsd_tiempo_cumplido) {
            finishEspSd();
        }
        return;
    }

    /* El reporte se difiere hasta salir de SAMPLING para no contaminar la
     * captura con trafico USB. */
    if (g_espsd_tiempo_cumplido) return;

    if (!g_espsd_en_sampling) {
        g_espsd_base = psoc.batchesOK();
        g_espsd_proximo = g_espsd_cada_lotes;
        g_espsd_en_sampling = true;
        return;
    }

    if (g_espsd_escrituras >= 128u) return;
    const uint32_t pos = psoc.batchesOK() - g_espsd_base;
    if (pos < g_espsd_proximo) return;

    static uint8_t sectorBuf[512];
    memset(sectorBuf, (g_espsd_escrituras & 1u) ? 0xAA : 0x55,
           sizeof(sectorBuf));
    g_espsd_pos[g_espsd_escrituras] = pos;
    if (!SD.writeRAW(sectorBuf, g_espsd_sector)) {
        g_espsd_fallos++;
    }
    g_espsd_escrituras++;
    g_espsd_proximo += g_espsd_cada_lotes;
}
#endif

static void requestCalibrationFromUsb()
{
    const bool sent = requestPsocCalibration("USB");
    if (sent) {
        g_auto_cal_requested = true;
        g_auto_cal_due_ms = 0u;
    }
    LOGM("USB_CMD", "cmd=cal,ok=%u,sub=0x%02X",
         (unsigned)sent, PSOC_CMD_CALIBRATE);
}

static void requestAdcSnapshotFromUsb()
{
    const bool sent = requestPsocAdcSnapshot("USB");
    LOGM("USB_CMD", "cmd=adc,ok=%u,sub=0x%02X",
         (unsigned)sent, PSOC_CMD_ADC_SNAPSHOT);
}

static void requestBlinkFromUsb()
{
    /* Titilar el LED del PSoC (mismo camino que CMD_BLINK_LED de la web). */
    psoc.blinkLed();
    SLAVE_LOG_PRINTF("[USB] blink -> PSoC CMD 0x%02X\n", PSOC_CMD_BLINK_LED);
    LOGM("USB_CMD", "cmd=blink,ok=1,sub=0x%02X", PSOC_CMD_BLINK_LED);
}

static void requestProbeFromUsb()
{
    g_psocConnected = psoc.probe(400);
    SLAVE_LOG_PRINTF("[USB] probe psoc=%u\n", (unsigned)g_psocConnected);
    logPsocUartDiag("USB_PROBE");
    LOGM("USB_CMD", "cmd=probe,ok=%u", (unsigned)g_psocConnected);
}

static void requestStatusFromUsb()
{
    psoc.requestStatus();
    delay(40);
    psoc.poll();
    servicePsocConfigAck();
    logPsocUartDiag("USB_STATUS");
    LOGM("USB_CMD", "cmd=status,ok=1");
}

static void requestStreamFromUsb(uint16_t mode)
{
    const uint8_t streamMode = (mode != 0u) ? 1u : 0u;
    psoc.selectStream(streamMode);
    SLAVE_LOG_PRINTF("[USB] stream %u -> PSoC CMD 0x%02X\n",
                     (unsigned)streamMode, PSOC_CMD_SELECT_STREAM);
    LOGM("USB_CMD", "cmd=stream,mode=%u", (unsigned)streamMode);
}

static void requestDebugPsocFromUsb(uint16_t enable)
{
    psoc.debugRamp(enable != 0u);
    SLAVE_LOG_PRINTF("[USB] debugpsoc %u -> PSoC CMD 0x%02X\n",
                     (unsigned)(enable != 0u), PSOC_CMD_DEBUG);
    LOGM("USB_CMD", "cmd=debugpsoc,en=%u", (unsigned)(enable != 0u));
}

static void requestStartNowFromUsb(uint16_t n)
{
    uint32_t nowUs;
    n = clampPsocCaptureBatches(n);
    if (captureOrDumpBusy()) {
        SLAVE_LOG_PRINTF("[USB] startnow ignored: busy state=%d fill=%u/%u\n",
                         (int)g_state, (unsigned)g_store_fill, (unsigned)g_rec_n_batches);
        return;
    }
    allocStore(n);
    digitalWrite(SYNC_TO_PSOC_PIN, LOW);
    g_view_store_active = false;
    g_start_store_active = (g_rec_n_batches > 0 && storeReadyForHotWait());
    g_start_fallback_sent = false;
    g_store_fill = 0;
    nowUs = (uint32_t)micros();
    g_t_start_us = (uint64_t)nowUs;
    g_debug_count = 0;
    g_debug_last_us = nowUs;
    g_sampling_start_ms = millis();
    g_sampling_start_psoc_bytes = psoc.bytesRx();
    g_sampling_start_batches_ok = psoc.batchesOK();
    g_state = SAMPLING;
    radioSilencioAlEntrar();
    /* Camino USB (contexto de loop): apagar ya y, con radiosil on <ms>, dejar
       que el transitorio del apagado se asiente antes de arrancar el PSoC. */
    serviceRadioSilencio(true);
    if (g_radio_silencio && g_radio_silencio_ms > 0u) delay(g_radio_silencio_ms);
    psoc.setN(g_rec_n_batches);
    psoc.startNow();
    SLAVE_LOG_PRINTF("[USB] startnow n=%u bytes=%lu store=%u\n",
                     (unsigned)g_rec_n_batches,
                     (unsigned long)g_sampling_start_psoc_bytes,
                     (unsigned)g_start_store_active);
    LOGM("USB_CMD", "cmd=startnow,n=%u,store=%u",
         (unsigned)g_rec_n_batches, (unsigned)g_start_store_active);
}

static bool requestPsocGainFromUsb(uint8_t subCmd, uint8_t param, const char *name)
{
    bool sent = false;
    if (!isGainCode(param)) {
        LOGM("USB_CMD", "cmd=%s,ok=0,reason=bad_gain,value=%u", name, (unsigned)param);
        return false;
    }
    servicePsocConfigAck();
    if (g_cfg_waiting) {
        LOGM("USB_CMD", "cmd=%s,ok=0,reason=cfg_busy,pending=0x%02X",
             name, g_cfg_sub_cmd);
        return false;
    }
    if (ensurePsocReadyForConfig()) {
        /* Los TRES subcomandos son distintos.  El else tapaba PGAOUT y mandaba
         * PGAVDAC en su lugar, asi que `pgaout N` por USB no cambiaba nunca la
         * ganancia de PGAout: cambiaba la referencia del VDAC y despues se
         * quedaba esperando un ACK de un comando que jamas se habia enviado.
         * Descubierto el 2026-09-19 barriendo las 81 combinaciones: el
         * firmware reportaba PGAOUT=0 en todas, con LPo identico a ganancia 1
         * y a ganancia 1152.  La ruta web/ESP-NOW siempre estuvo bien; el bug
         * era solo de la consola USB, que es justo la del banco. */
        if (subCmd == PSOC_CMD_PGA) {
            psoc.setPga(param);
        } else if (subCmd == PSOC_CMD_PGAOUT) {
            psoc.setPgaout(param);
        } else {
            psoc.setPgavdac(param);
        }
        waitForPsocConfigAck(subCmd, param);
        sent = true;
    }
    SLAVE_LOG_PRINTF("[USB] %s %u -> PSoC CMD 0x%02X\n",
                     name, (unsigned)param, subCmd);
    LOGM("USB_CMD", "cmd=%s,ok=%u,sub=0x%02X,value=%u",
         name, (unsigned)sent, subCmd, (unsigned)param);
    return sent;
}

/* "range N" — banco de pruebas: selecciona la config de rango del ADC
 * (1=CF_2V5, 2=CF_0V512, 3=CF_1V024, 4=CF_0V625) sin pasar por el maestro. */
static bool requestAdcConfigFromUsb(uint8_t cfg)
{
    bool sent = false;
    if (!isAdcConfigCode(cfg)) {
        LOGM("USB_CMD", "cmd=range,ok=0,reason=bad_range,value=%u", (unsigned)cfg);
        return false;
    }
    servicePsocConfigAck();
    if (g_cfg_waiting) {
        LOGM("USB_CMD", "cmd=range,ok=0,reason=cfg_busy,pending=0x%02X", g_cfg_sub_cmd);
        return false;
    }
    if (ensurePsocReadyForConfig()) {
        psoc.setAdcConfig(cfg);
        waitForPsocConfigAck(PSOC_CMD_ADC_CONFIG, cfg);
        sent = true;
    }
    SLAVE_LOG_PRINTF("[USB] range %u -> PSoC CMD 0x%02X\n", (unsigned)cfg, PSOC_CMD_ADC_CONFIG);
    LOGM("USB_CMD", "cmd=range,ok=%u,sub=0x%02X,value=%u",
         (unsigned)sent, PSOC_CMD_ADC_CONFIG, (unsigned)cfg);
    return sent;
}

/* "decim N" — banco de pruebas: factor de decimación con promedio (1..100,
 * 1=sin decimar) sin pasar por el maestro. */
static bool requestDecimationFromUsb(uint8_t factor)
{
    bool sent = false;
    if (factor < 1u || factor > 100u) {
        LOGM("USB_CMD", "cmd=decim,ok=0,reason=bad_factor,value=%u", (unsigned)factor);
        return false;
    }
    servicePsocConfigAck();
    if (g_cfg_waiting) {
        LOGM("USB_CMD", "cmd=decim,ok=0,reason=cfg_busy,pending=0x%02X", g_cfg_sub_cmd);
        return false;
    }
    if (ensurePsocReadyForConfig()) {
        psoc.setDecimation(factor);
        waitForPsocConfigAck(PSOC_CMD_SET_DECIMATION, factor);
        sent = true;
    }
    SLAVE_LOG_PRINTF("[USB] decim %u -> PSoC CMD 0x%02X\n", (unsigned)factor, PSOC_CMD_SET_DECIMATION);
    LOGM("USB_CMD", "cmd=decim,ok=%u,sub=0x%02X,value=%u",
         (unsigned)sent, PSOC_CMD_SET_DECIMATION, (unsigned)factor);
    return sent;
}

/* Banco/campo: mover una referencia firmada sin usar el protocolo de autotest
 * 0xA2. El ACK esperado es p1 (signo|etapa), exactamente como lo define el
 * firmware PSoC de campo para 0xAA. */
static bool requestManualIdacFromUsb(uint8_t stage, int16_t code)
{
    if (stage >= 4u || code < -255 || code > 255) {
        return false;
    }
    servicePsocConfigAck();
    if (g_cfg_waiting || !ensurePsocReadyForConfig()) {
        return false;
    }

    uint8_t p1 = stage;
    int16_t magnitude = code;
    if (magnitude < 0) {
        p1 |= 0x80u;
        magnitude = (int16_t)-magnitude;
    }
    psoc.setStageDac(p1, (uint8_t)magnitude);
    waitForPsocConfigAck(PSOC_CMD_VDAC, p1);
    SLAVE_LOG_PRINTF("[USB] idac stage=%u code=%d -> PSoC 0xAA p1=%u p2=%u\n",
                     (unsigned)stage, (int)code, (unsigned)p1,
                     (unsigned)magnitude);
    LOGM("USB_CMD", "cmd=idac,stage=%u,code=%d,p1=%u,p2=%u",
         (unsigned)stage, (int)code, (unsigned)p1, (unsigned)magnitude);
    return true;
}

static void handleUsbCommand(const char *cmd)
{
    if(psoc.controlUsb(cmd))return;
    uint8_t value = 0u;
    uint16_t value16 = 0u;
    uint16_t value16b = 0u;
    if (cmd[0] == '\0') {
        return;
    }
#if defined(SIM_SIN_HW) && SIM_SIN_HW
    if (usbCommandEquals(cmd, "sim")) {
        simPsocPrintState();
    } else if (usbCommandEquals(cmd, "sim off")) {
        simPsocSetEnabled(false);
        g_psocConnected = false;
        g_psoc_hw_class = SLAVE_HW_UNKNOWN;
        g_psoc_sd_status = 0u;
        g_psoc_sd_known = false;
        SLAVE_LOG_PRINTLN("[SIM] PSoC off");
    } else if (usbCommandEquals(cmd, "sim on")) {
        simPsocSetEnabled(true);
        psoc.poll();
        g_psocConnected = true;
        SLAVE_LOG_PRINTLN("[SIM] PSoC on");
    } else
#endif
    if (usbCommandEquals(cmd, "resetlow")) {
        g_psocResetActive = false;
        drivePsocResetRequestPin(false);
        SLAVE_LOG_PRINTF("[USB] resetlow GPIO%d nivel=%d esperado=0\n",
                         PSOC_RESET_REQUEST_PIN,
                         digitalRead(PSOC_RESET_REQUEST_PIN));
    } else if (usbCommandEquals(cmd, "resethigh")) {
        g_psocResetActive = false;
        drivePsocResetRequestPin(true);
        SLAVE_LOG_PRINTF("[USB] resethigh GPIO%d nivel=%d esperado=1\n",
                         PSOC_RESET_REQUEST_PIN,
                         digitalRead(PSOC_RESET_REQUEST_PIN));
    } else if (usbCommandEquals(cmd, "resetstate")) {
        SLAVE_LOG_PRINTF("[USB] resetstate GPIO%d nivel=%d\n",
                         PSOC_RESET_REQUEST_PIN,
                         digitalRead(PSOC_RESET_REQUEST_PIN));
    } else if (usbCommandEquals(cmd, "psocreset")) {
        if (g_state == SAMPLING || g_state == HOT_WAIT || captureOrDumpBusy()) {
            SLAVE_LOG_PRINTF("[USB] psocreset rechazado: captura/transferencia activa\n");
        } else {
            beginPsocResetRequest("USB");
        }
    } else if (usbCommandEquals(cmd, "probe")) {
        requestProbeFromUsb();
    } else if (usbCommandEquals(cmd, "status") || usbCommandEquals(cmd, "s")) {
        requestStatusFromUsb();
    } else if (usbParseU16Param(cmd, "stream", value16)) {
        requestStreamFromUsb(value16);
    } else if (usbParseU16Param(cmd, "debugpsoc", value16) ||
               usbParseU16Param(cmd, "dpsoc", value16)) {
        requestDebugPsocFromUsb(value16);
    } else if (usbParseU16Param(cmd, "pre", value16) ||
               usbParseU16Param(cmd, "prestart", value16)) {
        requestPrestartFromUsb(value16);
    } else if (usbCommandEquals(cmd, "sync") || usbCommandEquals(cmd, "go")) {
        requestUsbStartFromHotWait();
    } else if (usbParseU16Param(cmd, "startnow", value16) ||
               usbParseU16Param(cmd, "now", value16)) {
        requestStartNowFromUsb(value16);
    } else if (!strncmp(cmd, "capmode", 7) && (cmd[7] == ' ' || cmd[7] == '\0')) {
        unsigned st = 0, rt = 0, ramp = 0, streamDump = 0;
        const int n = sscanf(cmd + 7, "%u %u %u %u", &st, &rt, &ramp, &streamDump);
        if (n >= 1 && st <= CAPMODE_STORAGE_SD) {
            g_capmode = (uint8_t)(st | (rt ? CAPMODE_REALTIME : 0u) |
                                  (streamDump ? CAPMODE_STREAM_DUMP : 0u));
            g_capmode_ramp = (ramp != 0u);
        }
        SLAVE_LOG_RAW("#CAPMODE storage=%u rt=%u ramp=%u streamdump=%u\n",
                      (unsigned)(g_capmode & CAPMODE_STORAGE_MASK),
                      (unsigned)((g_capmode & CAPMODE_REALTIME) != 0u),
                      (unsigned)g_capmode_ramp,
                      (unsigned)((g_capmode & CAPMODE_STREAM_DUMP) != 0u));
    } else if (!strncmp(cmd, "sdhz", 4) && (cmd[4] == ' ' || cmd[4] == '\0')) {
        unsigned long hz = 0;
        if (sscanf(cmd + 4, "%lu", &hz) == 1 && hz >= 400000ul && hz <= 20000000ul) {
            live::setSdHz((uint32_t)hz);
        }
        SLAVE_LOG_RAW("#SDHZ %lu\n", (unsigned long)live::sdHz());
    } else if (!strncmp(cmd, "rtsim", 5) && (cmd[5] == ' ' || cmd[5] == '\0')) {
        unsigned loss = 0;
        if (strstr(cmd, "off")) {
            live::simMaster(false, 0);
        } else {
            sscanf(cmd + 5, "%u", &loss);
            live::simMaster(true, (uint8_t)(loss > 90u ? 90u : loss));
        }
        SLAVE_LOG_RAW("#RTSIM on=%u loss=%u\n", (unsigned)live::simEnabled(), loss);
    } else if (usbCommandEquals(cmd, "livestat")) {
        live::printStatus();
    } else if (usbCommandEquals(cmd, "ota")) {
        ota::printStatus();
    } else if (usbCommandEquals(cmd, "marcas")) {
        live::printStatus();
        SLAVE_LOG_RAW("#MARCAS beacon=%s\n", g_beaconMarks ? "on" : "off");
    } else if (usbCommandEquals(cmd, "marcas beacon on")) {
        g_beaconMarks = true;
        applyBeaconMarks();
        SLAVE_LOG_RAW("#MARCAS beacon=on\n");
    } else if (usbCommandEquals(cmd, "marcas beacon off")) {
        g_beaconMarks = false;
        applyBeaconMarks();
        SLAVE_LOG_RAW("#MARCAS beacon=off\n");
    } else if (!strncmp(cmd, "livedump", 8) && (cmd[8] == ' ' || cmd[8] == '\0')) {
        live::dumpUsb(strstr(cmd, "hex") != nullptr);
    } else if (!strncmp(cmd, "livecap", 7) && (cmd[7] == ' ' || cmd[7] == '\0')) {
        unsigned long n = 0, secs = 0;
        sscanf(cmd + 7, "%lu %lu", &n, &secs);
        requestLiveCapFromUsb((uint32_t)n, (uint32_t)secs);
    } else if (usbParseU16Param(cmd, "cap", value16) ||
               usbParseU16Param(cmd, "capture", value16) ||
               usbParseU16Param(cmd, "start", value16)) {
        requestCaptureFromUsb(value16);
    } else if (usbCommandEquals(cmd, "clear") || usbCommandEquals(cmd, "flush")) {
        requestClearFromUsb();
    } else if (usbCommandEquals(cmd, "stop")) {
        requestStopFromUsb();
    } else if (usbCommandEquals(cmd, "c") ||
        usbCommandEquals(cmd, "cal") ||
        usbCommandEquals(cmd, "calibrate")) {
        requestCalibrationFromUsb();
    } else if (usbCommandEquals(cmd, "a") ||
               usbCommandEquals(cmd, "adc") ||
               usbCommandEquals(cmd, "snapshot")) {
        requestAdcSnapshotFromUsb();
    } else if (usbCommandEquals(cmd, "b") ||
               usbCommandEquals(cmd, "blink") ||
               usbCommandEquals(cmd, "titilar")) {
        requestBlinkFromUsb();
    } else if (usbParseGainParam(cmd, "pga", value)) {
        (void)requestPsocGainFromUsb(PSOC_CMD_PGA, value, "pga");
    } else if (usbParseGainParam(cmd, "pgaout", value)) {
        (void)requestPsocGainFromUsb(PSOC_CMD_PGAOUT, value, "pgaout");
    } else if (usbParseGainParam(cmd, "pgavdac", value)) {
        (void)requestPsocGainFromUsb(PSOC_CMD_PGAVDAC, value, "pgavdac");
    } else if (!strncmp(cmd, "view ", 5)) {
        /* Banco: tap del AMux que digitaliza la captura (0xAC, solo IDLE).
         * 0 PGA/SEo, 1 BP, 2 OPA_SUM, 3 SUMo, 4 LPo; 255 vuelve al de fabrica. */
        int ch = -1;
        servicePsocConfigAck();
        if (sscanf(cmd + 5, "%d", &ch) == 1 &&
            ((ch >= 0 && ch <= 4) || ch == 255) && !g_cfg_waiting &&
            ensurePsocReadyForConfig()) {
            psoc.setViewChannel((uint8_t)ch);
            waitForPsocConfigAck(PSOC_CMD_VIEW_CHANNEL, (uint8_t)ch);
            SLAVE_LOG_PRINTF("[USB] view %d -> PSoC CMD 0xAC\n", ch);
        } else {
            SLAVE_LOG_PRINTF("[USB] uso: view <0-4|255> (PSoC en IDLE, sin config pendiente)\n");
        }
    } else if (!strncmp(cmd, "idac ", 5)) {
        int stage = -1;
        int code = 0;
        if (sscanf(cmd + 5, "%d %d", &stage, &code) == 2 &&
            stage >= 0 && stage < 4 && code >= -255 && code <= 255) {
            if (!requestManualIdacFromUsb((uint8_t)stage, (int16_t)code)) {
                SLAVE_LOG_PRINTF("[USB] idac rechazado: PSoC/config ocupado\n");
            }
        } else {
            SLAVE_LOG_PRINTF("[USB] uso: idac <etapa 0-3> <codigo -255..255>\n");
        }
    } else if (usbParseGainParam(cmd, "range", value)) {
        (void)requestAdcConfigFromUsb(value);
    } else if (usbParseGainParam(cmd, "decim", value)) {
        (void)requestDecimationFromUsb(value);
    } else if (usbCommandEquals(cmd, "sdinfo")) {
        /* Re-init + estado de la SD del PSoC. Respuesta por evento SD_STATUS
         * (log [PSoC] SD status=...) y ack 0xBC. */
        psoc.sdStatus(1);
        LOGM("USB_CMD", "cmd=sdinfo,ok=1");
    } else if (usbCommandEquals(cmd, "sdtest")) {
        psoc.sdTest();
        LOGM("USB_CMD", "cmd=sdtest,ok=1");
    } else if (usbParseGainParam(cmd, "sdcap", value)) {
        psoc.sdCapture((uint8_t)value);
        LOGM("USB_CMD", "cmd=sdcap,value=%u", (unsigned)value);
#if SLAVE_LAB_TOOLS_ENABLE
    } else if (usbCommandEquals(cmd, "diag")) {
        requestLabDiagFromUsb();
    } else if (usbCommandEquals(cmd, "pins")) {
        requestLabPinsFromUsb();
    } else if (usbParseTwoU16Params(cmd, "quiet", value16, value16b)) {
        requestQuietWindowFromUsb(value16, value16b);
    } else if (usbParseU16Param(cmd, "capwait", value16)) {
        requestCaptureFromUsb(value16);
        waitForLabCaptureDone("LAB_CAPWAIT", value16);
    } else if (usbParseU16Param(cmd, "startwait", value16)) {
        requestStartNowFromUsb(value16);
        waitForLabCaptureDone("LAB_STARTWAIT", value16);
    } else if (usbParseU16Param(cmd, "rawcap", value16)) {
        requestLabCaptureFromUsb(0u, value16, "LAB_RAWCAP");
    } else if (usbParseU16Param(cmd, "fircap", value16)) {
        requestLabCaptureFromUsb(1u, value16, "LAB_FIRCAP");
    } else if (usbParseU16Param(cmd, "sdread", value16)) {
        requestSdReadFromUsb(value16);
    } else if (usbCommandEquals(cmd, "dumpusb")) {
        requestDumpUsbFromUsb(false, 0u, false, 0u);
    } else if (!strncmp(cmd, "dumpusb ", 8)) {
        long a = -1, b = -1;
        const int n = sscanf(cmd + 8, "%ld %ld", &a, &b);
        if (n == 1 && a >= 0 && a <= 0xFFFF) {
            requestDumpUsbFromUsb(true, (uint16_t)a, false, 0u);
        } else if (n == 2 && a >= 0 && a <= 0xFFFF && b >= 0 && b <= 0xFFFF) {
            requestDumpUsbFromUsb(true, (uint16_t)a, true, (uint16_t)b);
        } else {
            SLAVE_LOG_PRINTF("[USB] uso: dumpusb [desde] [hasta]\n");
        }
    } else if (!strncmp(cmd, "rfrx", 4) && (cmd[4] == '\0' || cmd[4] == ' ')) {
        long ms = -1;
        if (sscanf(cmd + 4, "%ld", &ms) == 1 && ms >= 0 && ms <= 600000) requestRfRxFromUsb((uint32_t)ms);
        else SLAVE_LOG_PRINTF("[USB] uso: rfrx <ms 0-600000>\n");
    } else if (!strncmp(cmd, "txpower", 7) && (cmd[7] == '\0' || cmd[7] == ' ')) {
        long dbm = -1;
        sscanf(cmd + 7, "%ld", &dbm);
        requestTxPowerFromUsb(dbm);
    } else if (usbCommandEquals(cmd, "radio off")) {
        requestRadioFromUsb(false);
    } else if (usbCommandEquals(cmd, "radio on")) {
        requestRadioFromUsb(true);
    } else if (usbCommandEquals(cmd, "ftm")) {
        ftm::printStatus(false);
        SLAVE_LOG_RAW("#SYNC modo=%u drops_pausado=%lu\n", (unsigned)g_syncMode,
                      (unsigned long)g_syncPausedDrops);
    } else if (usbCommandEquals(cmd, "ftm raw")) {
        ftm::printStatus(true);
    } else if (!strncmp(cmd, "ftm periodo ", 12)) {
        ftm::setPeriodMs((uint32_t)strtoul(cmd + 12, nullptr, 10));
        ftm::printStatus(false);
    } else if (!strncmp(cmd, "syncmode ", 9)) {
        const unsigned long m = strtoul(cmd + 9, nullptr, 10);
        if (m <= SYNCMODE_RADIO_OFF) g_syncMode = (uint8_t)m;
        SLAVE_LOG_RAW("#SYNC modo=%u\n", (unsigned)g_syncMode);
    } else if (usbCommandEquals(cmd, "radiosil off")) {
        requestRadioSilencioFromUsb(false);
    } else if (!strncmp(cmd, "radiosil on", 11) &&
               (cmd[11] == '\0' || cmd[11] == ' ')) {
        long ms = 0;
        char extra = '\0';
        const int n = sscanf(cmd + 11, "%ld %c", &ms, &extra);
        if (n <= 1 && ms >= 0 && ms <= 1000) {   /* n = -1: sin numero */
            requestRadioSilencioFromUsb(true, (uint16_t)ms);
        } else {
            SLAVE_LOG_PRINTF("[USB] uso: radiosil on [ms 0-1000]\n");
        }
    } else if (!strncmp(cmd, "rftest", 6) &&
               (cmd[6] == '\0' || cmd[6] == ' ')) {
        long ms = -1, periodo = -1, bytesN = -1;
        char modo = '\0', extra = '\0';
        const int n = sscanf(cmd + 6, "%ld %ld %ld %c %c",
                             &ms, &periodo, &bytesN, &modo, &extra);
        if (n >= 1 && ms == 0) {
            requestRfTestFromUsb(0u, 0u, 0u, false);
        } else if ((n == 3 || (n == 4 && (modo == 'b' || modo == 'B'))) &&
                   ms > 0 && ms <= 600000 &&
                   periodo >= 0 && periodo <= 10000 &&
                   bytesN >= 1 && bytesN <= 240) {
            requestRfTestFromUsb((uint32_t)ms, (uint32_t)periodo, (uint32_t)bytesN,
                                 n == 4);
        } else {
            SLAVE_LOG_PRINTF("[USB] uso: rftest <ms 0-600000> <periodo_ms 0-10000 (0 = en cada ping del PSoC)> <bytes 1-240> [b]\n");
        }
    } else if (!strncmp(cmd, "sdstress", 8) &&
               (cmd[8] == '\0' || cmd[8] == ' ')) {
        long ms = -1, periodo = -1;
        const int n = sscanf(cmd + 8, "%ld %ld", &ms, &periodo);
        if (n >= 1 && ms == 0) {
            requestSdStressFromUsb(0u, 0u);
        } else if (n == 2 && ms > 0 && ms <= 600000 &&
                   periodo >= 5 && periodo <= 10000) {
            requestSdStressFromUsb((uint32_t)ms, (uint32_t)periodo);
        } else {
            SLAVE_LOG_PRINTF("[USB] uso: sdstress <ms 0-600000> <periodo_ms 5-10000>\n");
        }
    } else if (usbCommandEquals(cmd, "sddiag")) {
        sdDiagnostico();
    } else if (usbCommandEquals(cmd, "sddiag2")) {
        sdDiagnostico2();
    } else if (usbCommandEquals(cmd, "sddiag3")) {
        sdDiagnostico3();
    } else if (usbCommandEquals(cmd, "sddiag4")) {
        sdDiagnostico4();
    } else if (usbCommandEquals(cmd, "sddiag5")) {
        sdDiagnostico5();
    } else if (usbCommandEquals(cmd, "sddiag6")) {
        sdDiagnostico6();
    } else if (usbCommandEquals(cmd, "sddiag7")) {
        sdDiagnostico7();
    } else if (usbCommandEquals(cmd, "sddiag8")) {
        sdDiagnostico8();
    } else if (usbCommandEquals(cmd, "sddiag9")) {
        sdDiagnostico9();
    } else if (usbCommandEquals(cmd, "espsdfmt")) {
        requestEspSdFormatFromUsb();
    } else if (usbCommandEquals(cmd, "espsdfoo")) {
        requestEspSdFooFromUsb();
    } else if (!strncmp(cmd, "espsd", 5) &&
               (cmd[5] == '\0' || cmd[5] == ' ')) {
        long ms = -1, cadaLotes = -1;
        const int n = sscanf(cmd + 5, "%ld %ld", &ms, &cadaLotes);
        if (n >= 1 && ms == 0) {
            requestEspSdFromUsb(0u, 0u);
        } else if (n == 2 && ms > 0 && ms <= 600000 &&
                   cadaLotes >= 1 && cadaLotes <= 65535) {
            requestEspSdFromUsb((uint32_t)ms, (uint32_t)cadaLotes);
        } else {
            SLAVE_LOG_PRINTF("[USB] uso: espsd <ms 0-600000> <cada_lotes 1-65535>\n");
        }
    } else if (!strncmp(cmd, "sdprobe", 7) &&
               (cmd[7] == '\0' || cmd[7] == ' ')) {
        long ms = -1, periodo = -1, modo = -1, div = 0, slew = 0;
        const int n = sscanf(cmd + 7, "%ld %ld %ld %ld %ld",
                             &ms, &periodo, &modo, &div, &slew);
        if (n >= 1 && ms == 0) {
            requestSdProbeFromUsb(0u, 0u, 0u, 0u, 0u);
        } else if (n >= 3 && ms > 0 && ms <= 600000 &&
                   periodo >= 5 && periodo <= 10000 &&
                   ((modo >= 0 && modo <= 5) || modo == 7) && div >= 0 && div <= 255 &&
                   slew >= 0 && slew <= 2) {
            requestSdProbeFromUsb((uint32_t)ms, (uint32_t)periodo,
                                  (uint8_t)modo, (uint8_t)div, (uint8_t)slew);
        } else {
            SLAVE_LOG_PRINTF("[USB] uso: sdprobe <ms 0-600000> <periodo_ms 5-10000> <modo 0-5> [div 0-255] [slew 0-2]\n");
        }
#endif
    } else if (usbCommandEquals(cmd, "?") || usbCommandEquals(cmd, "help")) {
#if defined(SIM_SIN_HW) && SIM_SIN_HW
        SLAVE_LOG_PRINTF("[USB] simulator: sim, sim on, sim off\n");
#endif
#if SLAVE_LAB_TOOLS_ENABLE
        SLAVE_LOG_PRINTF("[USB] commands: resetlow, resethigh, resetstate, psocreset, probe, status, stream N, debugpsoc N, pre N, sync, startnow N, cap N, clear, stop, cal, adc, blink, pga N, pgaout N, pgavdac N, idac E C, range N, decim N, sdinfo, sdtest, sdcap N, diag, pins, quiet N MS, capwait N, startwait N, rawcap N, fircap N, sdread N, dumpusb [desde] [hasta], rftest ms periodo_ms bytes [b], sdstress ms periodo_ms, espsd ms cada_lotes, sdprobe ms periodo_ms modo [div] [slew], ctl punto P C0 C1 C2 C3, ctl puntos, ctl punto dump P, ctl punto clear, ctl punto clear P, ctl punto policy N\n");
#else
        SLAVE_LOG_PRINTF("[USB] commands: resetlow, resethigh, resetstate, psocreset, probe, status, stream N, debugpsoc N, pre N, sync, startnow N, cap N, clear, stop, cal, adc, blink, pga N, pgaout N, pgavdac N, idac E C, range N, decim N, ctl punto P C0 C1 C2 C3, ctl puntos, ctl punto dump P, ctl punto clear, ctl punto clear P, ctl punto policy N\n");
#endif
        LOGM("USB_CMD", "cmd=help,ok=1");
    } else {
        SLAVE_LOG_PRINTF("[USB] unknown command '%s' (use: help)\n", cmd);
        LOGM("USB_CMD", "cmd=unknown,ok=0,text=%s", cmd);
    }
}

static void serviceUsbCommands()
{
    static char line[32];
    static uint8_t pos = 0;

    while (Serial.available() > 0) {
        char ch = (char)Serial.read();
        if (ch == '\r' || ch == '\n') {
            line[pos] = '\0';
            handleUsbCommand(line);
            pos = 0;
        } else if (ch == 8 || ch == 127) {
            if (pos > 0) { pos--; }
        } else if (ch >= 32 && ch <= 126) {
            if (pos < sizeof(line) - 1) {
                line[pos++] = ch;
            }
        }
    }
}
#else
static void serviceUsbCommands() {}
#endif

static void sendStartAck(uint8_t status, uint32_t startToken, uint32_t rxUs)
{
    MsgStartAck ack = { CMD_START_ACK, NODE_ID, status, startToken, rxUs };
    sendToMaster((const uint8_t *)&ack, sizeof(ack));
}

static void freeStoreRam()
{
    for (uint8_t i = 0; i < g_store_page_count && i < STORE_MAX_PAGES; i++) {
        free(g_store_pages[i]);
        g_store_pages[i] = nullptr;
    }
    for (uint8_t i = g_store_page_count; i < STORE_MAX_PAGES; i++) {
        g_store_pages[i] = nullptr;
    }
    g_store_page_count = 0;
    g_store_capacity_batches = 0;
    free(g_store_ts_us);
    g_store_ts_us = nullptr;
}

static bool storeRamReady()
{
    return (g_store_page_count > 0 &&
            g_store_capacity_batches >= g_rec_n_batches &&
            g_store_ts_us != nullptr);
}

static SampleBytes *storeBatchPtr(uint16_t seq)
{
    if (seq >= g_store_capacity_batches) return nullptr;
    const uint16_t page = seq / STORE_PAGE_BATCHES;
    const uint16_t inPage = seq % STORE_PAGE_BATCHES;
    if (page >= g_store_page_count || page >= STORE_MAX_PAGES) return nullptr;
    return g_store_pages[page]
        ? &g_store_pages[page][(size_t)inPage * SPI_BATCH_SAMPLES]
        : nullptr;
}

static bool storeReadyForHotWait()
{
    if (live::active()) return g_psoc_arm_ready;
    if (g_rec_n_batches == 0) return true;
    if (!g_psoc_arm_ready) return false;
    if (g_psoc_sd_store_active) return true;
    return storeRamReady();
}

static bool storeAllocated()
{
    if (live::active()) return true;
    if (g_rec_n_batches == 0) return true;
    return g_psoc_sd_store_active || storeRamReady();
}

/* Veredicto del lazo del PSoC para la web: en banda, estado y perfil.
 * Viaja en el HELLO (cada 2 s fuera de captura), sin tráfico extra. */
static uint8_t controlStatusForHello()
{
    if (!psoc.controlLiveSeen()) return SLAVE_CTL_UNKNOWN;
    const int32_t state = psoc.controlMetadata(0x100);
    uint8_t status = 0u;
    if (psoc.controlMetadata(0x102) != 0) status |= SLAVE_CTL_BAND_BIT;
    if (state >= 0 && state <= 4) {
        status |= (uint8_t)(state << SLAVE_CTL_STATE_SHIFT);
    }
    if (psoc.controlMetadata(0x101) != 0) status |= SLAVE_CTL_PROFILE_BIT;
    return status;
}

static uint16_t effectivePsocSampleRateHz()
{
    return psoc.sampleRate();
}

static uint16_t clampPsocCaptureBatches(uint16_t n)
{
    if (n > PSOC_SD_CAPTURE_MAX_BATCHES) {
        n = PSOC_SD_CAPTURE_MAX_BATCHES;
    }
    if (n > PSOC_CAPTURE_MAX_BATCHES) {
        if (g_sd_user_enabled && psocSdPresent()) {
            return n;
        }
        return (uint16_t)PSOC_CAPTURE_MAX_BATCHES;
    }
    return n;
}

static uint32_t expectedCaptureMs(uint16_t n_batches)
{
    uint32_t fs = effectivePsocSampleRateHz();
    if (fs == 0) fs = PSOC_NATIVE_SAMPLE_RATE_HZ;
    uint32_t ms = ((uint32_t)n_batches * (uint32_t)SPI_BATCH_SAMPLES * 1000u + fs - 1u) / fs;
    return ms + PSOC_START_FALLBACK_EXTRA_MS;
}

/* Armado de una captura en vivo (todo lo que no es asincronico con la RAM
 * del PSoC sola). La RAM clasica se libera para que el anillo la use. */
static void enterHotWaitLive(uint16_t n_batches)
{
    allocStore(0);
    digitalWrite(SYNC_TO_PSOC_PIN, LOW);
    g_t_start_us     = 0;
    g_store_fill     = 0;
    g_view_remaining = 0;
    g_view_store_active = false;
    g_start_store_active = false;
    g_debug_count    = 0;
    g_debug_last_us  = (uint32_t)micros();
    g_start_fallback_sent = false;
    g_psoc_arm_ready = false;
    g_live_notified  = false;

    live::Config c;
    c.storage  = (uint8_t)(g_capmode & CAPMODE_STORAGE_MASK);
    c.realtime = (g_capmode & CAPMODE_REALTIME) != 0u;
    c.streamDump = (g_capmode & CAPMODE_STREAM_DUMP) != 0u;
    c.nBatches = n_batches;
    c.ramp     = g_capmode_ramp;
    const int32_t eff = live::prepare(c);
    if (eff < 0) {
        g_rec_n_batches = 0;
        g_state = STOPPED;
        g_hotwait_why = HOTWAIT_WHY_LIVE_STORE;
        sendHotWaitAck();
        SLAVE_LOG_PRINTF("[SLAVE] HOT_WAIT live aborted n=%u: sin almacenamiento\n",
                         (unsigned)n_batches);
        return;
    }
    /* 0xFFFF en el HOTWAIT_ACK = sin limite. */
    g_rec_n_batches = (eff == 0) ? 0xFFFFu : (uint16_t)eff;
    /* Un intento mas antes de abortar: un ack perdido (o el PSoC todavia con
     * el SETN anterior, p. ej. tras SET_RECLEN de la web) dejaba al nodo fuera
     * de la captura. Se registra que paso fallo. */
    bool psocReady = false;
    for (int intento = 0; intento < 2 && !psocReady; intento++) {
        const bool sdOk = syncPsocSdForCapture(false);
        const bool liveOk = sdOk && syncPsocLiveMode(g_capmode_ramp ? 2u : 1u);
        const bool setnOk = liveOk && sendAndConfirmPsocSetN((uint16_t)eff);
        psocReady = setnOk;
        if (!psocReady) {
            SLAVE_LOG_PRINTF("[SLAVE] HOT_WAIT live intento %d: sd=%d live=%d setn=%d\n",
                             intento + 1, (int)sdOk, (int)liveOk, (int)setnOk);
        }
    }
    if (!psocReady) {
        live::close();
        g_state = STOPPED;
        g_hotwait_why = HOTWAIT_WHY_LIVE_PSOC;
        sendHotWaitAck();
        SLAVE_LOG_PRINTF("[SLAVE] HOT_WAIT live aborted n=%ld: PSoC sin confirmar\n",
                         (long)eff);
        return;
    }
    psoc.preStart();
    g_hotwait_enter_ms = millis();
    g_hotwait_rearm_count = 0;
    g_state = HOT_WAIT;
    SLAVE_LOG_PRINTF("[SLAVE] HOT_WAIT live mode=0x%02X n=%ld\n",
                     (unsigned)g_capmode, (long)eff);
}

static void enterHotWait(uint16_t n_batches)
{
    bool psocReady;

    if (capmodeIsLive(g_capmode)) {
        enterHotWaitLive(n_batches);
        return;
    }
    live::close();
    n_batches = clampPsocCaptureBatches(n_batches);
    allocStore(n_batches);
    digitalWrite(SYNC_TO_PSOC_PIN, LOW);
    g_t_start_us     = 0;
    g_store_fill     = 0;
    g_view_remaining = 0;
    g_view_store_active = false;
    g_start_store_active = false;
    g_debug_count    = 0;
    g_debug_last_us  = (uint32_t)micros();
    g_start_fallback_sent = false;
    g_psoc_arm_ready = false;
    /* Sin donde guardar NO se arma el PSoC: armado queda sordo por UART hasta
     * un reset. allocStore puede haber recortado N a la RAM disponible. */
    if (n_batches > 0 && !g_psoc_sd_store_active && !storeRamReady()) {
        g_state = STOPPED;
        g_hotwait_why = HOTWAIT_WHY_NO_RAM;
        sendHotWaitAck();
        SLAVE_LOG_PRINTF("[SLAVE] HOT_WAIT aborted n=%u: sin RAM en el ESP\n",
                         (unsigned)n_batches);
        return;
    }
    n_batches = g_rec_n_batches;
    /* Armar el PSoC por UART: set N y pre-start (queda esperando flanco SYNC). */
    psocReady = syncPsocLiveMode(0u) &&
                syncPsocSdForCapture(g_psoc_sd_store_active) &&
                sendAndConfirmPsocSetN(n_batches);
    if (!psocReady) {
        allocStore(0);
        g_state = STOPPED;
        g_hotwait_why = HOTWAIT_WHY_SETN;
        sendHotWaitAck();   /* NACK explícito: el maestro no debe esperar READY */
        SLAVE_LOG_PRINTF("[SLAVE] HOT_WAIT aborted n=%u: PSoC SETN no confirmado\n",
                         (unsigned)n_batches);
        LOGM("HOTWAIT_ABORT", "n=%u,reason=setn_timeout", (unsigned)n_batches);
        return;
    }
    psoc.preStart();
    g_hotwait_enter_ms = millis();
    g_hotwait_rearm_count = 0;
    g_state = HOT_WAIT;
    SLAVE_LOG_PRINTF("[SLAVE] HOT_WAIT n=%u ready=%u\n",
                     n_batches, (unsigned)storeReadyForHotWait());
    LOGM("HOTWAIT", "n=%u,ready=%u,sync=%d,bytes=%lu",
         n_batches, (unsigned)storeReadyForHotWait(),
         digitalRead(SYNC_TO_PSOC_PIN), (unsigned long)psoc.bytesRx());
}

static void sendHotWaitAck()
{
    uint8_t ok = (g_state == HOT_WAIT && storeReadyForHotWait()) ? 1 : 0;
    MsgHotWaitAck ack = {
        CMD_HOTWAIT_ACK, NODE_ID, ok, (uint8_t)g_state, g_rec_n_batches,
        ok ? (uint8_t)HOTWAIT_WHY_NONE : g_hotwait_why
    };
    sendToMaster((const uint8_t *)&ack, sizeof(ack));
    SLAVE_LOG_PRINTF("[SLAVE] HOTWAIT_ACK ok=%u state=%u n=%u\n",
                     ok, (unsigned)g_state, g_rec_n_batches);
}

static void abortHotWait(const char *reason)
{
    digitalWrite(SYNC_TO_PSOC_PIN, LOW);
    g_view_store_active = false;
    g_start_store_active = false;
    g_start_fallback_sent = false;
    allocStore(0);
    g_state = STOPPED;
    sendHotWaitAck();   /* ok=0 con state: NACK explícito hacia el maestro */
    SLAVE_LOG_PRINTF("[SLAVE] HOT_WAIT watchdog abort: %s\n", reason);
    LOGM("HOTWAIT_ABORT", "reason=%s", reason);
}

/* Watchdog de HOT_WAIT (desde loop()): si el PSoC nunca confirma ARMED
 * (p.ej. sd_session_begin falló y psoc_arm() salió sin armar), reintenta
 * preStart() hasta HOTWAIT_REARM_MAX veces y después aborta con NACK. Un
 * HOT_WAIT armado que el maestro nunca arranca también expira (caso maestro
 * reiniciado). Antes de esto el esclavo quedaba mudo hasta un power cycle. */
static void serviceHotWaitWatchdog()
{
    if (g_state != HOT_WAIT) {
        g_hotwait_rearm_count = 0;
        return;
    }
    uint32_t elapsed = millis() - g_hotwait_enter_ms;
    if (!g_psoc_arm_ready) {
        if (elapsed >= HOTWAIT_ABORT_MS) {
            abortHotWait("psoc_no_armed");
        } else if (g_hotwait_rearm_count < HOTWAIT_REARM_MAX &&
                   elapsed >= HOTWAIT_READY_TIMEOUT_MS *
                              (uint32_t)(g_hotwait_rearm_count + 1u)) {
            g_hotwait_rearm_count++;
            psoc.preStart();
            SLAVE_LOG_PRINTF("[SLAVE] HOT_WAIT re-arm #%u (sin PSOC_EVT_ARMED tras %lu ms)\n",
                             (unsigned)g_hotwait_rearm_count, (unsigned long)elapsed);
            LOGM("HOTWAIT_REARM", "try=%u,ms=%lu",
                 (unsigned)g_hotwait_rearm_count, (unsigned long)elapsed);
        }
        return;
    }
    if (elapsed >= HOTWAIT_ARMED_ABORT_MS) {
        abortHotWait("start_never_came");
    }
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
            radioSilencioAlEntrar();
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

    const uint8_t subCmd = cfg->sub_cmd;
    const uint8_t param = cfg->param;
    const uint8_t param2 = cfg->param2;
    uint8_t ok = 1;
    bool waitAck = false;
    switch (cfg->sub_cmd) {
        case 0xA6:                       /* PGA */
            if (!isGainCode(param)) {
                ok = 0;
            } else {
                waitAck = true;
            }
            break;
        case PSOC_CMD_PGAOUT:            /* PGAout (etapa de salida GEO) */
            if (!isGainCode(param)) {
                ok = 0;
            } else {
                waitAck = true;
            }
            break;
        case 0xA9:                       /* PGAvdac */
            if (!isGainCode(param)) {
                ok = 0;
            } else {
                waitAck = true;
            }
            break;
        case 0xAA: {                     /* IDAC manual firmado de campo */
            const uint8_t stage = (uint8_t)(param & 0x0Fu);
            if (stage >= 4u || (param & 0x70u) != 0u) {
                ok = 0;
            } else {
                waitAck = true;
            }
            break;
        }
        case PSOC_CMD_CALIBRATE:
            waitAck = true;
            break;
        case PSOC_CMD_SAVE_EEPROM:          /* 0xB6: guardar calibración en EEPROM */
            waitAck = true;
            break;
        case PSOC_CMD_SELECT_STREAM:        /* 0xB7: 0=crudo, 1=FIR hardware */
            waitAck = true;
            break;
        case PSOC_CMD_ADC_CONFIG:           /* 0xBA: 1=±2.5V, 2=±0.512V, 3=±1.024V, 4=±0.625V */
            if (!isAdcConfigCode(param)) {
                ok = 0;
            } else {
                waitAck = true;
            }
            break;
        case PSOC_CMD_SET_DECIMATION:       /* 0xBB: factor 1..100 (1=sin decimar) */
            if (!isDecimationFactor(param)) {
                ok = 0;
            } else {
                waitAck = true;
            }
            break;
        case PSOC_CMD_FORCE_PI:             /* 0xAB: forzar el lazo N segundos */
            waitAck = true;
            break;
        case PSOC_CMD_VIEW_CHANNEL:         /* 0xAC: canal del AMux a mirar (vista) */
            /* 0xFF = volver al canal normal; el resto lo valida el PSoC
             * contra los canales que existen en SU placa, que no son los
             * mismos en la I2C y en la SERIAL. */
            waitAck = true;
            break;
        case PSOC_CMD_SD_CAPTURE:           /* 0xBE: 0/1 — modo captura a SD del PSoC */
            if (param > 1u) {
                ok = 0;
            } else {
                waitAck = true;
            }
            break;
        case SLAVE_CMD_SET_SD_ENABLE:       /* 0xC0: OBSOLETO (era SD-en-ESP32). La SD vive en el
                                             * PSoC; usar 0xBE. Se rechaza siempre. */
            ok = 0;
            break;                          /* waitAck queda false: ack inmediato abajo */
        default:
            ok = 0;
            break;
    }
    if (ok && waitAck) {
        if (!ensurePsocReadyForConfig()) {
            ok = 0;
            sendCfgAck(subCmd, ok);
        } else {
            switch (subCmd) {
                case 0xA6:
                    psoc.setPga(param);
                    break;
                case PSOC_CMD_PGAOUT:
                    psoc.setPgaout(param);
                    break;
                case 0xA9:
                    psoc.setPgavdac(param);
                    break;
                case 0xAA:
                    psoc.setStageDac(param, param2);
                    break;
                case PSOC_CMD_CALIBRATE:
                    psoc.calibrate();
                    break;
                case PSOC_CMD_SAVE_EEPROM:
                    psoc.saveEeprom();
                    break;
                case PSOC_CMD_SELECT_STREAM:
                    psoc.selectStream(param);
                    break;
                case PSOC_CMD_ADC_CONFIG:
                    psoc.setAdcConfig(param);
                    break;
                case PSOC_CMD_SET_DECIMATION:
                    psoc.setDecimation(param);
                    break;
                case PSOC_CMD_FORCE_PI:
                    psoc.forcePi(param);
                    break;
                case PSOC_CMD_VIEW_CHANNEL:
                    psoc.setViewChannel(param);
                    break;
                case PSOC_CMD_SD_CAPTURE:
                    psoc.sdCapture(param);
                    break;
                default:
                    ok = 0;
                    break;
            }
            if (ok) {
                waitForPsocConfigAck(subCmd, param);
            } else {
                sendCfgAck(subCmd, ok);
            }
        }
    } else {
        sendCfgAck(subCmd, ok);
    }
    SLAVE_LOG_PRINTF("[SLAVE] cfg sub=0x%02X p1=%u p2=%u ok=%u\n",
                     subCmd, param, param2, ok);
    LOGM("CFG", "sub=0x%02X,p1=%u,p2=%u,ok=%u,wait=%u",
         subCmd, param, param2, ok, (unsigned)waitAck);
}

static void handleDebugNode(const MsgDebugNode *dbg)
{
    if (dbg->node_id != NODE_ID) return;
    bool enable = (dbg->enable != 0);
    debugEspSetRamp(enable);
    sendCfgAck(0xA7, 1);
    SLAVE_LOG_PRINTF("[SLAVE] debug_node=%d\n", (int)g_debug_mode);
}

/* ARM y PRESTART sueltan y reservan la memoria de la captura (allocStore,
 * live::close/prepare). Atendidos en el callback de ESP-NOW (tarea WiFi)
 * liberaban el anillo mientras loop() retransmitia de el: panico
 * LoadProhibited en live::sendBatch (nodo 3, 2026-10-25, por coredump). Se
 * encolan tal como llegaron y loop() los procesa en orden con el mismo
 * camino. */
#if defined(ESP32)
struct DeferredRx { uint8_t mac[6]; uint8_t len; uint8_t data[64]; };
static constexpr uint8_t DEFERRED_RX_N = 8;
static DeferredRx g_deferredRx[DEFERRED_RX_N];
static volatile uint8_t g_deferredHead = 0, g_deferredTail = 0;
static uint32_t g_deferredDrops = 0;
static bool g_inDeferredRx = false;
static portMUX_TYPE g_deferredMux = portMUX_INITIALIZER_UNLOCKED;

static bool deferMasterRx(const uint8_t *mac, const uint8_t *data, int len)
{
    if (len > (int)sizeof(g_deferredRx[0].data)) return false;   /* se atiende como antes */
    portENTER_CRITICAL(&g_deferredMux);
    const uint8_t next = (uint8_t)((g_deferredHead + 1u) % DEFERRED_RX_N);
    if (next != g_deferredTail) {
        DeferredRx &e = g_deferredRx[g_deferredHead];
        memcpy(e.mac, mac, 6);
        memcpy(e.data, data, (size_t)len);
        e.len = (uint8_t)len;
        g_deferredHead = next;
    } else {
        g_deferredDrops++;
    }
    portEXIT_CRITICAL(&g_deferredMux);
    return true;   /* lleno: se descarta; el maestro reintenta ARM/PRESTART */
}

static void serviceDeferredRx()
{
    for (;;) {
        DeferredRx e;
        portENTER_CRITICAL(&g_deferredMux);
        const bool have = g_deferredTail != g_deferredHead;
        if (have) {
            e = g_deferredRx[g_deferredTail];
            g_deferredTail = (uint8_t)((g_deferredTail + 1u) % DEFERRED_RX_N);
        }
        portEXIT_CRITICAL(&g_deferredMux);
        if (!have) return;
        g_inDeferredRx = true;
        onDataRecv(e.mac, e.data, (int)e.len);
        g_inDeferredRx = false;
    }
}
#else
static void serviceDeferredRx() {}
#endif

#if defined(ESP8266)
static void onDataRecv(uint8_t *mac, uint8_t *data, uint8_t len)
#else
static void onDataRecv(const uint8_t *mac, const uint8_t *data, int len)
#endif
{
    if (len < 1) return;
    const int radioLen = len;
#if SLAVE_LAB_TOOLS_ENABLE
    if (rfrxOnFrame(data, len)) return;   /* rafaga de banco: no es protocolo */
#endif
    if (!geonetTagged(data, len)) {
        static uint32_t s_untaggedLogMs = 0;
        if (millis() - s_untaggedLogMs > 10000u) {
            s_untaggedLogMs = millis();
            SLAVE_LOG_PRINTF("[SLAVE] ESP-NOW sin marca de red ignorado %02X:%02X:%02X:%02X:%02X:%02X len=%d\n",
                             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], (int)len);
        }
        return;
    }
    len -= GEONET_TAG_LEN;
    /* Lo que solo manda un esclavo no puede venir del maestro: sin esto, un
     * esclavo que arranco sin ver el AP (acepta cualquier origen) tomaba a otro
     * esclavo como maestro, le mandaba su HELLO a el y, como se oian entre si,
     * ninguno reescaneaba: la red quedaba muda (2026-10-25). */
    switch (data[0]) {
        case CMD_ARM_ACK: case CMD_START_ACK: case CMD_CFG_ACK: case CMD_STATUS:
        case CMD_HELLO: case CMD_HOTWAIT_ACK: case CMD_STREAM_END: case CMD_OTA_STATUS:
        case CMD_FTM_REPORT: case CMD_SLAVE_LOG:
        case DATA_PKT_MARKER: case DATA32_PKT_MARKER: case DATAZ_PKT_MARKER:
            return;
        default: break;
    }
#if defined(ESP32)
    if (!g_inDeferredRx && (data[0] == CMD_ARM || data[0] == CMD_PRESTART) &&
        deferMasterRx(mac, data, len + GEONET_TAG_LEN)) {
        return;
    }
#endif
    if (!macIsMaster(mac)) {
        static uint32_t s_foreignLogMs = 0;
        if (millis() - s_foreignLogMs > 10000u) {
            s_foreignLogMs = millis();
            SLAVE_LOG_PRINTF("[SLAVE] ESP-NOW ajeno ignorado %02X:%02X:%02X:%02X:%02X:%02X cmd=0x%02X len=%d\n",
                             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                             data[0], (int)len);
        }
        return;
    }
#if defined(ESP32)
    if (!g_inDeferredRx)
#endif
    {
        const uint64_t now = (uint64_t)esp_timer_get_time();
        const uint64_t airtime = (uint64_t)(radioLen + 40) * 8ULL;
        live::noteEvent(SAMPLE_FLAG_MASTER_RX, now - airtime, now);
    }
    learnMasterMac(mac);
    ota::onMasterAlive();   /* confirma una imagen OTA pendiente */
    g_lastMasterRxMs = millis();   /* para el seguimiento de canal (ver abajo) */
    uint8_t cmd = data[0];
    /* La rampa ESP es una prueba de radio: admitir DEBUG=0 para que
     * s3_link_test.py pueda detenerla sin un STOP global. */
    const bool stopDebug = g_debug_mode &&
        ((cmd == CMD_DEBUG && len == (int)sizeof(MsgDebug) &&
          ((const MsgDebug *)data)->enable == 0) ||
         (cmd == CMD_DEBUG_NODE && len == (int)sizeof(MsgDebugNode) &&
          ((const MsgDebugNode *)data)->node_id == NODE_ID &&
          ((const MsgDebugNode *)data)->enable == 0));
    if (g_state == SAMPLING && cmd != CMD_STOP && cmd != CMD_ARM &&
        cmd != CMD_STREAM_ACK && !stopDebug) {
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
        g_psoc_arm_ready = false;
        g_psoc_sd_read_pending = false;
        g_psoc_sd_read_usb = false;
        /* Pizarra limpia: ARM es el fin explícito de cualquier captura/dump
         * anterior. Sin esto, flags viejos (store activo, retry de ACK de
         * completado, arena con n_batches previo) contaminaban el ciclo
         * siguiente y START dejaba de funcionar hasta un power cycle. */
        g_view_store_active = false;
        g_start_store_active = false;
        g_start_fallback_sent = false;
        allocStore(0);
        live::close();
        g_live_notified = false;
        digitalWrite(SYNC_TO_PSOC_PIN, LOW);
        g_state = ARMED;
        scheduleAutoCalibration(0u);
        MsgArmAck ack = { CMD_ARM_ACK, NODE_ID, 0 };
        sendToMaster((const uint8_t *)&ack, sizeof(ack));
        SLAVE_LOG_PRINTLN("[SLAVE] ARMED");
    }
    else if (cmd == CMD_PRESTART && len >= 3 && g_state != SAMPLING) {
        /* Un maestro viejo manda 3 bytes: queda el modo que estaba (el banco
         * puede fijarlo con "capmode"). */
        if (len >= (int)offsetof(MsgPrestart, sync_mode)) {
            g_capmode = ((const MsgPrestart *)data)->mode;
        }
        {
            const uint8_t sm = (len >= (int)sizeof(MsgPrestart))
                ? ((const MsgPrestart *)data)->sync_mode : (uint8_t)SYNCMODE_FTM_ACTIVE;
            if (sm != g_syncMode) {
                SLAVE_LOG_PRINTF("[SYNC] modo %u -> %u\n", (unsigned)g_syncMode, (unsigned)sm);
            }
            g_syncMode = (sm <= SYNCMODE_RADIO_OFF) ? sm : (uint8_t)SYNCMODE_FTM_ACTIVE;
            g_syncPausedDrops = 0;
        }
        if (!g_auto_cal_requested && g_auto_cal_due_ms != 0u) {
            serviceAutoCalibration();
        }
        if (g_cfg_waiting && g_cfg_sub_cmd == PSOC_CMD_CALIBRATE) {
            SLAVE_LOG_PRINTLN("[SLAVE] PRESTART deferred: calibration busy");
            LOGM("PRESTART_DEFER", "reason=cal_busy,state=%d", (int)g_state);
            return;
        }
        if (!g_auto_cal_requested && g_auto_cal_due_ms != 0u) {
            /* START takes priority over a scheduled-but-not-started auto-cal.
               Cancel it so we don't block capture. User can manually calibrate. */
            SLAVE_LOG_PRINTLN("[SLAVE] PRESTART: override pending auto-cal");
            LOGM("PRESTART_OVERRIDE_AUTOCAL", "state=%d", (int)g_state);
            g_auto_cal_due_ms = 0u;
        }
        const MsgPrestart *msg = (const MsgPrestart *)data;
        const uint16_t requested = clampPsocCaptureBatches(msg->n_batches);
        if (g_state == HOT_WAIT && requested == g_rec_n_batches &&
            storeReadyForHotWait()) {
            /* PRESTART duplicado/redundante (retry del maestro, reenvio ESP-NOW):
             * este nodo ya esta armado y listo para el mismo N. Responder el ACK
             * directo en vez de repetir enterHotWait(): una vez que el PSoC llega
             * a PSOC_ARMED se queda mudo por UART a proposito (ventana de
             * muestreo), asi que un segundo SETN nunca se ackea, vence a los
             * 500 ms y el aborto por timeout libera el store dejando al PSoC
             * fisicamente armado pero sordo — roto hasta un reset fisico. */
            sendHotWaitAck();
            SLAVE_LOG_PRINTF("[SLAVE] PRESTART duplicado n=%u ignorado (ya HOT_WAIT ready)\n",
                              (unsigned)requested);
            LOGM("PRESTART_DUP", "n=%u", (unsigned)requested);
        } else {
            enterHotWait(msg->n_batches);
        }
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
    else if (cmd == CMD_START && len >= (int)offsetof(MsgStart, sched)) {
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
            SLAVE_LOG_PRINTF("[SLAVE] START already sampling token=%u (no RF ack)\n",
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
        int syncBefore = digitalRead(SYNC_TO_PSOC_PIN);
        /* START programado por FTM: el flanco sale en el instante del maestro,
         * no al llegar el paquete. Sin modelo valido, como siempre. */
        const bool sched = len >= (int)sizeof(MsgStart) && msg->sched == 1u;
        bool programado = false;
        if (sched) {
            programado = programarSyncFtm(msg->start_mac_us);
            SLAVE_LOG_PRINTF("[SYNCFTM] START programado=%d objetivo_maestro=%lu modelo=%d\n",
                             (int)programado, (unsigned long)msg->start_mac_us, (int)ftm::modelOk());
            if (programado && radioSilActiva()) g_radio_silencio_pedido = true;
        }
#if defined(ESP32) && ESP_ARDUINO_VERSION_MAJOR >= 3
        /* Sin plan FTM: anclar al instante de recepcion del broadcast. */
        if (!programado) {
            programado = programarSyncRx();
            if (programado && radioSilActiva()) g_radio_silencio_pedido = true;
        }
#endif
        if (!programado) {
            subirSyncConRadioSilencio();
            /* Para medir la dispersion del START por broadcast entre nodos:
             * a que hora del maestro subio SYNC, segun el modelo FTM. */
            g_bcast_sync_local = esp_timer_get_time();
            g_bcast_sync_new = true;
        }
        int syncAfter = digitalRead(SYNC_TO_PSOC_PIN);
        /* No START_ACK here: once SYNC rises the PSoC is measuring, so RF stays
           silent until the store is full and CMD_START ok=2 can be sent. */
        g_t_start_us    = msg->t_start_us;
        g_store_fill    = 0;   /* reset store index para nueva grabación */
        g_debug_count   = 0;
        g_debug_last_us = nowUs;
        g_sampling_start_ms = millis();
        g_sampling_start_psoc_bytes = psoc.bytesRx();
        g_sampling_start_batches_ok = psoc.batchesOK();
        g_start_store_active = (g_rec_n_batches > 0 && storeReadyForHotWait() && !g_view_store_active);
        g_start_fallback_sent = false;
        g_state = SAMPLING;
        if (live::active()) live::start(g_t_start_us, effectivePsocSampleRateHz());
        radioSilencioAlEntrar();
        SLAVE_LOG_PRINTF("[SLAVE] START_OK target=%u token=%u sync=%d->%d bytes=%lu fill=%u/%u view=%u start=%u\n",
                         (unsigned)targetNode, (unsigned)startToken,
                         syncBefore, syncAfter,
                         (unsigned long)g_sampling_start_psoc_bytes,
                         (unsigned)g_store_fill, (unsigned)g_rec_n_batches,
                         (unsigned)g_view_store_active, (unsigned)g_start_store_active);
        LOGM("START_OK", "token=%u,target=%u,n_batches=%u,store=%d,sync=%d->%d,view=%u,start=%u",
             (unsigned)startToken, (unsigned)targetNode,
             (unsigned)g_rec_n_batches, (int)storeReadyForHotWait(),
             syncBefore, syncAfter, (unsigned)g_view_store_active,
             (unsigned)g_start_store_active);
    }
    else if (cmd == CMD_TIME_BEACON && len >= (int)sizeof(MsgTimeBeacon)) {
#if defined(ESP32) && ESP_ARDUINO_VERSION_MAJOR >= 3
        ftm::noteMasterBeacon(((const MsgTimeBeacon *)data)->master_mac_us,
                              esp_wifi_internal_get_mac_clock_time());
#endif
    }
    else if (cmd == CMD_STREAM_ACK && len >= (int)sizeof(MsgStreamAck)) {
        const MsgStreamAck *a = (const MsgStreamAck *)data;
        if (a->node_id == NODE_ID) live::onMasterAck(a->next_expected, a->sack);
    }
    else if (cmd == CMD_FIR_COEFFS && len >= 4) {
        const MsgFirCoeffs *m = (const MsgFirCoeffs *)data;
        if (m->node_id == NODE_ID && m->count <= FIR_HW_CHUNK &&
            (int)m->first + m->count <= FIR_HW_TAPS && len >= 4 + 3 * (int)m->count) {
            memcpy(&g_firBuf[(size_t)m->first * 3u], m->coef, (size_t)m->count * 3u);
            for (int i = 0; i < m->count; i++) g_firGot[(m->first + i) / 32] |= 1u << ((m->first + i) % 32);
        }
    }
    else if (cmd == CMD_FIR_COMMIT && len >= (int)sizeof(MsgFirCommit)) {
        const MsgFirCommit *m = (const MsgFirCommit *)data;
        if (m->node_id == NODE_ID) {
            g_firAction = m->action;
            g_firSum = m->sum;
            g_firCommitPending = true;   /* lo manda loop(): la UART no va en la tarea WiFi */
        }
    }
    else if (cmd == CMD_OTA_OFFER && len >= (int)sizeof(MsgOtaOffer)) {
        ota::onOffer(*(const MsgOtaOffer *)data,
                     g_state == SAMPLING || g_state == HOT_WAIT || live::active());
    }
    else if (cmd == CMD_OTA_CONTROL && len >= (int)sizeof(MsgOtaControl)) {
        ota::onControl(*(const MsgOtaControl *)data);
    }
    else if (cmd == CMD_STOP) {
        /* En vivo el STOP no tira nada: el PSoC cierra con la bajada de SYNC
         * y lo pendiente se sigue pasando hasta el aviso de fin. */
        if (live::active()) live::requestStop();
        digitalWrite(SYNC_TO_PSOC_PIN, LOW);
        g_debug_mode = false;
        if (g_view_remaining > 0) {
            g_view_remaining = 0;
        }
        psoc.debugRamp(false);      /* por seguridad: cortar cualquier debug PSoC */
        g_view_store_active = false;
        g_start_store_active = false;
        g_start_fallback_sent = false;
        g_psoc_arm_ready = false;
        g_psoc_sd_read_pending = false;
        g_psoc_sd_read_usb = false;
        g_state = STOPPED;
        SLAVE_LOG_PRINTLN("[SLAVE] STOP");
    }
    else if (cmd == CMD_DEBUG && len >= (int)sizeof(MsgDebug)) {
        const MsgDebug *d = (const MsgDebug *)data;
        bool enable = (d->enable != 0);
        debugEspSetRamp(enable);
        MsgArmAck ack = { CMD_ARM_ACK, NODE_ID, (uint8_t)(g_debug_mode ? 0xDD : 0x00) };
        sendToMaster((const uint8_t *)&ack, sizeof(ack));
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
        g_start_store_active = false;
        uint8_t ok = (g_state == HOT_WAIT && storeAllocated()) ? 1u : 0u;
        if (ok) {
            g_view_store_active = true;
        } else {
            g_view_store_active = false;
            g_start_store_active = false;
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
                          (uint8_t)((msg->n_batches == 0 || storeAllocated()) ? 1 : 0) };
        sendToMaster((const uint8_t *)&ack, sizeof(ack));
    }
    else if (cmd == CMD_REQ_BATCH && len >= (int)MSG_REQ_BATCH_LEGACY_BYTES) {
        /* Se atiende en loop(): mandar las dos tramas desde la tarea WiFi la
         * dejaba ocupada cuando llegaba el pedido siguiente, y en el S3 se
         * perdia uno de cada dos (descarga a ~3 lotes/s por reintentos). */
        REQ_LOCK();
        MsgReqBatch req = {};
        const size_t copyLen = ((size_t)len < sizeof(req)) ? (size_t)len : sizeof(req);
        memcpy(&req, data, copyLen);
        memcpy((void *)&g_reqBatchPending, &req, sizeof(req));
        g_reqBatchPendingFlag = true;
        REQ_UNLOCK();
    }
    else if (cmd == CMD_BLINK_LED && len >= (int)sizeof(MsgBlinkLed)) {
        const MsgBlinkLed *msg = (const MsgBlinkLed *)data;
        if (msg->node_id != 0u && msg->node_id != NODE_ID) return;
        /* El LED de identificación vive en el PSoC: reenviamos el comando por
         * UART. El PSoC titila su LED (no bloqueante, vía su Timer). Igual
         * confirmamos al maestro para que la web muestre el ACK. */
        psoc.blinkLed();
        sendCfgAck(CMD_BLINK_LED, 1);
        SLAVE_LOG_PRINTF("[SLAVE] BLINK_LED -> PSoC node=%u\n", NODE_ID);
        LOGM("BLINK_LED", "node=%u,target=psoc", NODE_ID);
    }
}
/* SaveEEPROM (0xB6) y SelectStream (0xB7) llegan por CMD_SET_CONFIG → handleSetConfig */

/* ── Store-and-forward helpers ───────────────────────────────────────────── */

static void allocStore(uint16_t n_batches)
{
    n_batches = clampPsocCaptureBatches(n_batches);
    clearCompletionAckRetry();
    freeStoreRam();
    g_store_fill   = 0;
    g_rec_n_batches= n_batches;
    /* Misma regla que psoc_arm(): la SD solo si la captura no entra en la RAM
     * del PSoC. Su drenaje escribe un sector cada 57,6 ms durante el muestreo
     * y cada escritura mete una espiga en la señal. */
    g_psoc_sd_store_active =
        (n_batches > (uint16_t)PSOC_CAPTURE_MAX_BATCHES &&
         g_sd_user_enabled && psocSdPresent());
    g_psoc_sd_session_complete = false;
    g_psoc_sd_capture_error = false;
    g_psoc_sd_read_pending = false;
    g_psoc_sd_read_usb = false;
    if (n_batches == 0) return;
    if (g_psoc_sd_store_active) {
        SLAVE_LOG_PRINTF("[SLAVE] store PSoC-SD n=%u\n", n_batches);
        return;
    }

    g_store_ts_us = (uint32_t *)malloc((size_t)n_batches * sizeof(uint32_t));
    if (g_store_ts_us) {
        uint16_t remaining = n_batches;
        uint16_t capacity = 0;
        while (remaining > 0 && g_store_page_count < STORE_MAX_PAGES) {
            const uint16_t batchesThisPage =
                (remaining > STORE_PAGE_BATCHES) ? STORE_PAGE_BATCHES : remaining;
            SampleBytes *page = (SampleBytes *)malloc(
                (size_t)batchesThisPage * SPI_BATCH_SAMPLES * sizeof(SampleBytes));
            if (!page) {
                break;
            }
            g_store_pages[g_store_page_count++] = page;
            capacity += batchesThisPage;
            remaining -= batchesThisPage;
        }
        g_store_capacity_batches = capacity;
        /* La RAM del ESP alcanza para menos que la del PSoC (medido 2026-10-16:
         * 448 lotes entran, 480 no, con WiFi arriba). Antes se liberaba todo y
         * se seguia armando el PSoC sin donde guardar: quedaba ARMED, sordo por
         * UART, y el esclavo no volvia a capturar sin reset ("Max RAM lo
         * traba"). Ahora se recorta a lo que entra y el HOTWAIT_ACK le informa
         * al maestro el N efectivo. */
        if (capacity > 0 && capacity < n_batches) {
            SLAVE_LOG_PRINTF("[SLAVE] RAM ESP alcanza para %u de %u lotes: recorto\n",
                             (unsigned)capacity, (unsigned)n_batches);
            g_rec_n_batches = capacity;
        }
    }
    if (!storeRamReady()) {
        freeStoreRam();
    }
    SLAVE_LOG_PRINTF("[SLAVE] store arena n=%u pages=%u pageB=%u buf=%s heap=%u maxAlloc=%u\n",
                     n_batches, (unsigned)g_store_page_count,
                     (unsigned)STORE_PAGE_BATCHES,
                     storeReadyForHotWait() ? "OK" : "FAIL",
                     (unsigned)ESP.getFreeHeap(),
                     (unsigned)ESP.getMaxAllocHeap());
}

static void handleReqBatch(const MsgReqBatch *msg)
{
    if (msg->node_id != NODE_ID) return;
    uint16_t seq = msg->batch_seq;
    const bool compressed = (msg->flags & REQ_BATCH_COMPRESSED) != 0u;
#if SIM_SIN_HW
    SLAVE_LOG_PRINTF("[REQ] seq=%u state=%d live=%d\n", seq, (int)g_state, (int)live::active());
#endif
    if (g_state == SAMPLING) {
        return;   /* No TX ESP-NOW mientras el ADC está capturando. */
    }
    if (live::active()) {
        if (!live::serveReq(seq, compressed)) {
            MsgCfgAck nack = { CMD_CFG_ACK, NODE_ID, CMD_REQ_BATCH, 0 };
            sendToMaster((const uint8_t *)&nack, sizeof(nack));
        }
        return;
    }
    if ((g_view_store_active || g_start_store_active) && g_store_fill < g_rec_n_batches) {
        SLAVE_LOG_PRINTF("[SLAVE] REQ_BATCH ignored during capture seq=%u fill=%u/%u state=%d\n",
                         seq, g_store_fill, g_rec_n_batches, (int)g_state);
        return;
    }
    if (seq >= g_store_fill || (!storeRamReady() && !g_psoc_sd_store_active)) {
        /* Batch fuera de rango: notificar al master para que avance sin esperar timeout */
        MsgCfgAck nack = { CMD_CFG_ACK, NODE_ID, CMD_REQ_BATCH, 0 };
        sendToMaster((const uint8_t *)&nack, sizeof(nack));
        SLAVE_LOG_PRINTF("[SLAVE] REQ_BATCH NACK seq=%u fill=%u\n", seq, g_store_fill);
        return;
    }
    if (g_psoc_sd_store_active) {
        if (g_psoc_sd_read_pending) {
            if (seq == g_psoc_sd_read_seq) {
                return; /* el PSoC ya está atendiendo exactamente este retry */
            }
            MsgCfgAck nack = { CMD_CFG_ACK, NODE_ID, CMD_REQ_BATCH, 0 };
            sendToMaster((const uint8_t *)&nack, sizeof(nack));
            return;
        }
        g_psoc_sd_read_pending = true;
        g_psoc_sd_read_seq = seq;
        g_psoc_sd_read_compressed = compressed;
        g_psoc_sd_read_started_ms = millis();
        psoc.sdReadBatch(seq);
        return;
    }
    const SampleBytes *s = storeBatchPtr(seq);
    if (!s) {
        MsgCfgAck nack = { CMD_CFG_ACK, NODE_ID, CMD_REQ_BATCH, 0 };
        sendToMaster((const uint8_t *)&nack, sizeof(nack));
        SLAVE_LOG_PRINTF("[SLAVE] REQ_BATCH arena miss seq=%u fill=%u\n", seq, g_store_fill);
        return;
    }
    uint64_t ts = g_t_start_us + (uint64_t)g_store_ts_us[seq];
    transport.sendStoredBatch(s, seq, ts, NODE_ID, compressed);
}

static void servicePsocSdReadTimeout()
{
    if (!g_psoc_sd_read_pending ||
        (uint32_t)(millis() - g_psoc_sd_read_started_ms) < PSOC_SD_READ_TIMEOUT_MS) {
        return;
    }
    if (!g_psoc_sd_read_usb) {
        MsgCfgAck nack = { CMD_CFG_ACK, NODE_ID, CMD_REQ_BATCH, 0 };
        sendToMaster((const uint8_t *)&nack, sizeof(nack));
    }
    SLAVE_LOG_PRINTF("[SLAVE] PSoC SD_READ timeout seq=%u usb=%u\n",
                     (unsigned)g_psoc_sd_read_seq, (unsigned)g_psoc_sd_read_usb);
    g_psoc_sd_read_pending = false;
    g_psoc_sd_read_usb = false;
}

/* ── Callback PSoC — batch recibido ─────────────────────────────────────── */

/* Lotes leídos de la SD del PSoC: las marcas de escritura vienen en los bits
 * 21-22 del int24 (XOR contra el signo). Se restaura el valor y la marca pasa
 * al byte flags. Solo se aplica a lotes de SD: la rampa debug usa los 24 bits. */
static void decodeSdMarks(SampleBytes *s)
{
    for (int i = 0; i < SPI_BATCH_SAMPLES; i++) {
        const uint8_t rep = (s[i].digi2 & 0x80u) ? 0x03u : 0x00u;
        const uint8_t mark = (uint8_t)(((s[i].digi2 >> 5) & 0x03u) ^ rep);
        if (mark != 0u) {
            s[i].digi2 ^= (uint8_t)(mark << 5);
            s[i].flags |= mark;
        }
    }
}

static void psocBatchToStored(const PsocBatch &batch, SampleBytes *dst)
{
    for (int i = 0; i < SPI_BATCH_SAMPLES; i++) {
        const PsocSample &s = batch.samples[i];
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

static void onBatch(const PsocBatch &batch)
{
    g_psocConnected = true;

    if (live::active()) {
        live::onPsocBatch(batch);
        return;
    }

    if (g_psoc_sd_read_pending) {
        if (batch.seq != g_psoc_sd_read_seq) {
            SLAVE_LOG_PRINTF("[SLAVE] SD batch seq=%u, esperado=%u: ignorado\n",
                             (unsigned)batch.seq, (unsigned)g_psoc_sd_read_seq);
            return;
        }
        SampleBytes stored[SPI_BATCH_SAMPLES];
        const uint16_t seq = g_psoc_sd_read_seq;
        uint32_t fs = effectivePsocSampleRateHz();
        if (fs == 0u) fs = PSOC_NATIVE_SAMPLE_RATE_HZ;
        psocBatchToStored(batch, stored);
        decodeSdMarks(stored);
        g_psoc_sd_read_pending = false;
        if (g_psoc_sd_read_usb) {
            /* Banco (sdread N): volcar resumen por USB en vez de ESP-NOW. */
            g_psoc_sd_read_usb = false;
            int32_t sum = 0;
            for (int i = 0; i < SPI_BATCH_SAMPLES; i++) {
                sum += (int32_t)batch.samples[i].raw_input;
            }
            SLAVE_LOG_PRINTF("[USB] SDREAD seq=%u raw[0]=%d raw[%d]=%d sumRaw=%ld "
                             "dig[0]=%ld dig[%d]=%ld flags[0]=0x%02X\n",
                             (unsigned)seq,
                             (int)batch.samples[0].raw_input,
                             SPI_BATCH_SAMPLES - 1,
                             (int)batch.samples[SPI_BATCH_SAMPLES - 1].raw_input,
                             (long)sum,
                             (long)batch.samples[0].post_digital,
                             SPI_BATCH_SAMPLES - 1,
                             (long)batch.samples[SPI_BATCH_SAMPLES - 1].post_digital,
                             (unsigned)batch.samples[0].sample_flags);
            /* El resumen prueba que 0xBF respondio, pero no alcanza para
             * medir ruido: perderia 28 de las 30 muestras. Despues de que
             * la captura termino, el banco recibe el mismo SampleBytes (10 B
             * por muestra) que entrega dumpusb desde RAM. `stored` conserva
             * las marcas SD ya decodificadas por decodeSdMarks(). */
            SLAVE_LOG_PRINTF("#SDREAD %u ", (unsigned)seq);
            for (int i = 0; i < SPI_BATCH_SAMPLES; i++) {
                const SampleBytes &s = stored[i];
                SLAVE_LOG_PRINTF("%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
                                 (unsigned)s.raw_lo, (unsigned)s.raw_hi,
                                 (unsigned)s.alog0, (unsigned)s.alog1,
                                 (unsigned)s.alog2, (unsigned)s.digi0,
                                 (unsigned)s.digi1, (unsigned)s.digi2,
                                 (unsigned)s.gain, (unsigned)s.flags);
            }
            SLAVE_LOG_PRINTLN("");
            return;
        }
        const uint64_t ts = g_t_start_us +
            ((uint64_t)seq * SPI_BATCH_SAMPLES * 1000000ULL) / fs;
        transport.sendStoredBatch(stored, seq, ts, NODE_ID, g_psoc_sd_read_compressed);
        return;
    }

    if (g_state != SAMPLING || g_psoc_sd_store_active) return;

    if (g_view_remaining > 0) {
        g_view_remaining = 0;   /* fail-safe: VIEW live no debe transmitir mientras muestrea */
    }

    /* Store-and-forward RAM: el camino SD no llega aquí; se lee por 0xBF. */
    if (storeRamReady() && g_store_fill < g_rec_n_batches) {
        SampleBytes *dst = storeBatchPtr(g_store_fill);
        if (!dst) {
            return;
        }
        psocBatchToStored(batch, dst);
        uint32_t tsRel = (uint32_t)batch.timestamp_us;
        g_store_ts_us[g_store_fill] = tsRel;
        g_store_fill++;
        if (g_store_fill >= g_rec_n_batches) {
            g_state = STOPPED;
            digitalWrite(SYNC_TO_PSOC_PIN, LOW);
            SLAVE_LOG_PRINTF("[SLAVE] FULL -> STOPPED (%u batches)\n", g_store_fill);
            if (g_view_store_active) {
                g_view_store_active = false;
                g_start_store_active = false;
                sendCompletionAckWithRetry(CMD_VIEW);   /* VIEW_DONE: ahora sí puede empezar el dump */
                SLAVE_LOG_PRINTF("[SLAVE] VIEW capture done -> ACK\n");
                LOGM("VIEWDONE", "store=1,n=%u", g_store_fill);
            } else if (g_start_store_active) {
                g_start_store_active = false;
                sendCompletionAckWithRetry(CMD_START);  /* START_DONE: master puede dumpear */
                SLAVE_LOG_PRINTF("[SLAVE] START capture done -> ACK\n");
                LOGM("STARTDONE", "store=1,n=%u", g_store_fill);
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

/* ── Seguimiento del canal del maestro ───────────────────────────────────── */

#if !defined(ESP8266)
/* Busca el AP del maestro por SSID y devuelve su canal. Bloquea ~2 s (el escaneo
 * pasa por los 13 canales); solo se llama en boot o cuando ya no hay trafico, o
 * sea nunca durante una captura. */
static uint8_t findMasterChannel(uint8_t fallback)
{
    int n = WiFi.scanNetworks(false /*async*/, true /*hidden*/);
    uint8_t ch = 0;
    int32_t best = -1000;
    for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i) != MASTER_AP_SSID) continue;
        /* Puede haber más de un maestro al alcance: quedarse con el más fuerte. */
        if (WiFi.RSSI(i) > best) {
            best = WiFi.RSSI(i);
            ch = WiFi.channel(i);
            memcpy(g_masterApBssid, WiFi.BSSID(i), 6);
        }
    }
    WiFi.scanDelete();
    if (ch == 0) {
        SLAVE_LOG_PRINTF("[SLAVE] AP '%s' no encontrado en %d redes; canal %u\n",
                         MASTER_AP_SSID, n, fallback);
        return fallback;
    }
    g_masterApKnown = true;
    SLAVE_LOG_PRINTF("[SLAVE] AP '%s' en canal %u (%d dBm) BSSID %02X:%02X:%02X:%02X:%02X:%02X\n",
                     MASTER_AP_SSID, ch, (int)best,
                     g_masterApBssid[0], g_masterApBssid[1], g_masterApBssid[2],
                     g_masterApBssid[3], g_masterApBssid[4], g_masterApBssid[5]);
    return ch;
}

/* Si el maestro deja de hablar, puede haberse movido de canal (se asocio a la red
 * del cliente y arrastro su AP). Rescanear y reengancharse.
 * Se gatea por estado: durante SAMPLING el silencio es normal —el protocolo es
 * maestro-iniciado— y un escaneo ahi arruinaria la captura. */
static void masterChannelService()
{
    if (g_state == SAMPLING || g_state == HOT_WAIT) return;
    /* Si el maestro nunca hablo desde el boot, el escaneo del boot puede haber
     * caido en el canal de respaldo (maestro apagado, o todavia en el canal 1
     * antes de asociarse al hotspot): sin rescanear, el esclavo quedaba sordo
     * para siempre. Se cuenta el silencio desde el boot. */
    /* Sin el AP del maestro identificado (arranco con el maestro apagado) se
     * reescanea seguido hasta encontrarlo. */
    const uint32_t silencio = g_masterApKnown ? MASTER_SILENCE_MS : 10000u;
    if ((millis() - g_lastMasterRxMs) < silencio) return;
    /* Las balizas del AP del maestro tambien cuentan: un maestro en reposo no
     * manda ESP-NOW, y el reescaneo (segundos sordo) se comia el ARM/PRESTART
     * de la captura siguiente (2026-10-24). */
    if (g_lastMasterBeaconMs != 0u && (millis() - g_lastMasterBeaconMs) < 5000u) return;

    uint8_t ch = findMasterChannel(g_masterChannel);
    if (ch != g_masterChannel) {
        SLAVE_LOG_PRINTF("[SLAVE] maestro cambio de canal %u -> %u\n",
                         g_masterChannel, ch);
        g_masterChannel = ch;
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    }
    /* Reiniciar el reloj aunque el canal no haya cambiado: si no, se rescanea en
     * bucle cada pasada de loop() mientras el maestro siga callado. */
    g_lastMasterRxMs = millis();
}
#else
static uint8_t findMasterChannel(uint8_t fallback) { return fallback; }
static void masterChannelService() {}
#endif

/* ── Setup ───────────────────────────────────────────────────────────────── */

void setup()
{
    /* Arduino: precargar HIGH y habilitar salida; reafirmar tras pinMode. */
    drivePsocResetRequestPin(true);
    dbgLogBegin(DBG_LOG_BAUD);   /* Serial USB del esclavo = canal de log */
    SLAVE_LOG_PRINTF("[slaveS3 %d] boot chip=%s flash=%u psram=%u\n", NODE_ID,
                     ESP.getChipModel(), ESP.getFlashChipSize(), ESP.getPsramSize());
    SLAVE_LOG_PRINTF("[PSoC] reset pin GPIO%d OUTPUT, nivel=%d esperado=1\n",
                     PSOC_RESET_REQUEST_PIN,
                     digitalRead(PSOC_RESET_REQUEST_PIN));
    /* SD-en-ESP32 purgada (2026-07-11): la SD vive en el PSoC. sd_storage.*
     * queda como stub sin inicializar; sd_present sale de PSOC_EVT_SD_STATUS. */
    /* Heap: el store usa paginas de STORE_PAGE_BATCHES lotes, asi el limite
     * RAM-only lo da la suma de heap util y el buffer PSoC, no un bloque gigante. */
    SLAVE_LOG_PRINTF("[SLAVE %d] heap free=%u maxAlloc=%u (~%u lotes contiguos antiguos)\n",
                     NODE_ID, (unsigned)ESP.getFreeHeap(),
                     (unsigned)ESP.getMaxAllocHeap(),
                     (unsigned)(ESP.getMaxAllocHeap() / 304u));
    samplePulseBegin();
    debugEspHardwareBegin();
    localUiBegin();
    /* SPI.begin() configura sus pads; reclamar reset despues de todo el SPI. */
    drivePsocResetRequestPin(true);
    SLAVE_LOG_PRINTF("[PSoC] reset pin post-UI GPIO%d nivel=%d esperado=1\n",
                     PSOC_RESET_REQUEST_PIN,
                     digitalRead(PSOC_RESET_REQUEST_PIN));
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
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.setOutputPower(20.5f);
    WiFi.softAP("GeoSlave", "", 1, 1);   /* ssid, pass vacío, canal 1, oculto */
    delay(100);
    SLAVE_LOG_PRINTF("[SLAVE] MAC: %s  ch=%d (AP-fixed)\n",
                     WiFi.macAddress().c_str(), wifi_get_channel());
#else
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_max_tx_power(WIFI_TX_POWER_QDBM);
    WiFi.disconnect();
    delay(100);
    /* Adoptar el canal del AP del maestro en vez de fijar el 1: el maestro puede
     * estar arrastrado al canal de la red del cliente. Si no se lo encuentra se
     * usa el 1, que es donde arranca su AP. Ver el comentario de arriba. */
    g_masterChannel = findMasterChannel(MASTER_CH_FALLBACK);
    esp_wifi_set_channel(g_masterChannel, WIFI_SECOND_CHAN_NONE);
    SLAVE_LOG_PRINTF("[SLAVE] MAC: %s  ch=%d (maestro)\n",
                     WiFi.macAddress().c_str(), WiFi.channel());
    SLAVE_LOG_PRINTF("[SLAVE] WiFi ps=off tx=%d qdBm\n", WIFI_TX_POWER_QDBM);
#endif

    /* ESP-NOW */
    if (!transport.begin(g_masterTxMac)) {
        SLAVE_LOG_PRINTLN("[SLAVE] ESP-NOW FAIL - halt");
        while (true) { delay(1000); }
    }
    esp_now_register_recv_cb(ESPNOW_RECV_CB);
    applyBeaconMarks();

    /* (El LED de identificación es el del PSoC; el ESP ya no maneja ningún LED.) */

    /* Salida de sync hacia el PSoC. Es el único pin de sync que queda: el
     * arranque llega por ESP-NOW y este flanco lo propaga al PSoC. */
    pinMode(SYNC_TO_PSOC_PIN, OUTPUT);
    digitalWrite(SYNC_TO_PSOC_PIN, LOW);
    iniciarTimerSyncRadioSilencio();

    /* UART → PSoC */
    psoc.begin(onBatch);
    psoc.onDiag(onPsocDiag);
    live::begin(&psoc, sendToMaster, NODE_ID);
    ota::begin(sendToMaster, NODE_ID, MASTER_AP_SSID, MASTER_AP_PASS);
    ftm::begin();
    ftm::setTurn(NODE_ID);
    g_psoc_rx_last_level = (uint8_t)digitalRead(PSOC_UART_RX);
    attachInterrupt(digitalPinToInterrupt(PSOC_UART_RX), onPsocRxEdge, CHANGE);
#if PSOC_RX_SCAN_ENABLE
    beginPsocRxScan();
#endif

    /* Arrancar siempre desde un estado conocido. El pulso se completa de forma
     * no bloqueante en loop(); el probe periódico corre recién después de que
     * servicePsocResetRequest() libere la solicitud de reset. Esto evita probar
     * la UART/I2C mientras el PSoC todavía está retenido en XRES. */
    beginPsocResetRequest("arranque del esclavo");
    LOGM("PSOC_RESET", "reason=boot");

    SLAVE_LOG_PRINTF("[SLAVE %d] listo, reseteando PSoC antes de esperar ARM\n", NODE_ID);
    LOGM("BOOT", "node=%d,psoc=%d", NODE_ID, (int)g_psocConnected);
}

/* ── Loop ────────────────────────────────────────────────────────────────── */

/* FIR del PSoC recibido de la web: se pasa por UART fuera de captura y el ACK
 * del PSoC (0xCB) vuelve al maestro como cualquier configuracion. */
static void serviceFirUpload()
{
    if (!g_firCommitPending) return;
    if (g_state == SAMPLING || g_state == HOT_WAIT || g_cfg_waiting) return;
    g_firCommitPending = false;
    if (g_firAction != 0u) {
        uint16_t sum = 0;
        bool complete = true;
        for (int i = 0; i < FIR_HW_TAPS; i++) {
            if (!(g_firGot[i / 32] & (1u << (i % 32)))) complete = false;
            sum = (uint16_t)(sum + g_firBuf[i * 3] + g_firBuf[i * 3 + 1] + g_firBuf[i * 3 + 2]);
        }
        if (!complete || sum != g_firSum) {
            SLAVE_LOG_PRINTF("[FIR] incompleto o suma distinta (%u vs %u)\n", sum, g_firSum);
            sendCfgAck(0xCBu, 0u);
            return;
        }
        for (int i = 0; i < FIR_HW_TAPS; i++) {
            psoc.firTap((uint8_t)i, g_firBuf[i * 3], g_firBuf[i * 3 + 1], g_firBuf[i * 3 + 2]);
            if ((i & 15) == 15) delay(2);   /* no desbordar la RX del PSoC */
        }
    }
    psoc.firCommit(g_firAction, g_firSum);
    waitForPsocConfigAck(0xCBu, 1u);
    memset(g_firGot, 0, sizeof(g_firGot));
    SLAVE_LOG_PRINTF("[FIR] enviado al PSoC (accion %u)\n", (unsigned)g_firAction);
}

/* Sesiones FTM contra el AP del maestro y su resumen cada 2 s. Durante la
 * captura solo en FTM activo; nunca con la radio apagada ni durante una OTA. */
static void serviceFtm()
{
    const bool capturing = (g_state == SAMPLING || g_state == HOT_WAIT);
    const bool allowed = g_radio_on && g_masterApKnown && !ota::busy() &&
                         (!capturing || g_syncMode == SYNCMODE_FTM_ACTIVE);
    ftm::service(allowed, g_masterApBssid, g_masterChannel);
    static uint32_t lastReportMs = 0;
    const uint32_t now = millis();
    if (now - lastReportMs < 2000u) return;
    if (capturing && g_syncMode != SYNCMODE_FTM_ACTIVE) return;
    lastReportMs = now;
    MsgFtmReport r;
    if (ftm::takeReport(r, NODE_ID, g_syncMode)) {
        sendToMaster((const uint8_t *)&r, sizeof(r));
    }
}

/* Log replicado al maestro (remote_log.h): una trama cada 50 ms como mucho. */
static void serviceRemoteLog()
{
#if REMOTE_LOG_ENABLE
    static uint32_t lastMs = 0;
    if (!g_radio_on || millis() - lastMs < 50u) return;
    MsgSlaveLog m;
    const size_t n = remoteLogTee().take(m.text, SLAVE_LOG_MAX);
    if (n == 0) return;
    lastMs = millis();
    m.cmd = CMD_SLAVE_LOG;
    m.node_id = NODE_ID;
    m.len = (uint8_t)n;
    sendToMaster((const uint8_t *)&m, 3u + n);
#endif
}

void loop()
{
    serviceDeferredRx();
    serviceRemoteLog();
    serviceSchedSyncReport();
    serviceCoreDumpReport();
    if (g_reqBatchPendingFlag) {
        MsgReqBatch req;
        REQ_LOCK();
        memcpy(&req, (const void *)&g_reqBatchPending, sizeof(req));
        g_reqBatchPendingFlag = false;
        REQ_UNLOCK();
        handleReqBatch(&req);
    }
    serviceFirUpload();
    ota::service();   /* una descarga OTA bloquea y termina reiniciando */
    servicePsocResetRequest();
    serviceRadioSilencio(true);
    serviceFtm();
    if (g_state != SAMPLING) {
        debugEspHardwareService();
        /* Reengancharse al canal del maestro si dejo de hablar. No corre durante
         * SAMPLING/HOT_WAIT: ahi el silencio es parte del protocolo. */
        masterChannelService();
    }

    /* Drenar siempre la UART del PSoC (onBatch ignora si no está SAMPLING). */
    if (g_state == SAMPLING && g_debug_mode) {
        /* Debug ESP: un lote falso cada 30/2604 s. La expresión redondeada
         * mantiene esta ruta de laboratorio ligada a la Fs nativa canónica. */
        static const uint32_t DEBUG_BATCH_PERIOD_US =
            (SPI_BATCH_SAMPLES * 1000000UL + PSOC_NATIVE_SAMPLE_RATE_HZ / 2UL) /
            PSOC_NATIVE_SAMPLE_RATE_HZ;
        uint32_t nowUs = (uint32_t)micros();
        if ((uint32_t)(nowUs - g_debug_last_us) >= DEBUG_BATCH_PERIOD_US) {
            g_debug_last_us += DEBUG_BATCH_PERIOD_US;
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
    servicePsocSdReadTimeout();
    serviceUsbCommands();
    serviceAutoCalibration();
    serviceBootGains();
    serviceCompletionAckRetry();
    serviceLocalUi();
    serviceHotWaitWatchdog();
    live::service();
    serviceLiveEnd();
#if SLAVE_LAB_TOOLS_ENABLE
    /* rftest/sdstress/sdprobe/espsd (banco): corren EN
     * PARALELO con una captura (g_state == SAMPLING incluido: este tramo del
     * loop no se salta en SAMPLING, ver arriba). */
    serviceLabPing();
    serviceRfTest();
    serviceRfRx();
    serviceSdStress();
    serviceSdProbe();
    serviceEspSd();
#endif

    /* (El parpadeo de identificación lo hace ahora el PSoC con su Timer; el ESP
     * solo reenvía CMD_BLINK_LED por UART en onDataRecv.) */

    /* Mientras se llena el store (Ver/Test con n_batches>0), imprimir UART cada 1 s.
     * Esto separa cable/pin (uartBytes=0) de protocolo/baud (bytes sin frames)
     * de "onBatch no incrementa fill" (psoc.poll() no entrega PsocBatch). */
#if SLAVE_LOG_VIEW_UART
    static uint32_t lastViewDiagMs = 0;
    if (g_state == SAMPLING && g_rec_n_batches > 0 &&
        (millis() - lastViewDiagMs) >= 1000) {
        lastViewDiagMs = millis();
        logPsocUartDiag("VIEW_UART");
    }
#endif

    bool storeCaptureActive = g_view_store_active || g_start_store_active;
    if (g_state == SAMPLING && storeCaptureActive && g_rec_n_batches > 0 &&
        g_store_fill == 0 && !g_start_fallback_sent && !live::active()) {
        uint32_t elapsedMs = millis() - g_sampling_start_ms;
        uint32_t fallbackMs = expectedCaptureMs(g_rec_n_batches);
        uint32_t bytesNow = psoc.bytesRx();
        uint32_t batchesNow = psoc.batchesOK();
        const bool sdCapture = g_psoc_sd_store_active;
        if (sdCapture) {
            /* F1 (aceptación 10 min, 2026-07-11): en captura a SD NO llegan
             * lotes UART durante el muestreo (fill/bOK congelados por diseño)
             * y el cierre de GEOLAST.BIN crece con el tamaño (~3.3 s a 52080
             * lotes). Con el margen fijo, este watchdog declaraba stall justo
             * antes del SD_SESSION y el START_NOW fantasma re-armaba el PSoC
             * pisando la sesión recién terminada (SD_ERROR 0x08 + NACK eterno
             * al dump del maestro). Margen extra proporcional al largo. */
            fallbackMs += 15000u + (uint32_t)(g_rec_n_batches / 2u);
        }
        if (elapsedMs >= fallbackMs && batchesNow == g_sampling_start_batches_ok) {
            const char *mode = g_view_store_active ? "VIEW" : "START";
            g_start_fallback_sent = true;
            SLAVE_LOG_PRINTF("[SLAVE] %s_SYNC_STALL elapsed=%lu/%lu sync=%d bytes=%lu->%lu bOK=%lu->%lu fill=%u/%u sd=%u -> %s\n",
                             mode,
                             (unsigned long)elapsedMs, (unsigned long)fallbackMs,
                             digitalRead(SYNC_TO_PSOC_PIN),
                             (unsigned long)g_sampling_start_psoc_bytes,
                             (unsigned long)bytesNow,
                             (unsigned long)g_sampling_start_batches_ok,
                              (unsigned long)batchesNow,
                              (unsigned)g_store_fill, (unsigned)g_rec_n_batches,
                              (unsigned)sdCapture,
                              sdCapture ? "ABORT (sin START_NOW)" : "START_NOW UART");
            LOGM("CAPTURE_SYNC_STALL", "mode=%s,ms=%lu/%lu,sync=%d,bytes=%lu->%lu,bOK=%lu->%lu,fill=%u/%u,sd=%u",
                 mode,
                 (unsigned long)elapsedMs, (unsigned long)fallbackMs,
                 digitalRead(SYNC_TO_PSOC_PIN),
                 (unsigned long)g_sampling_start_psoc_bytes,
                 (unsigned long)bytesNow,
                 (unsigned long)g_sampling_start_batches_ok,
                 (unsigned long)batchesNow,
                 (unsigned)g_store_fill, (unsigned)g_rec_n_batches,
                 (unsigned)sdCapture);
            if (sdCapture) {
                /* NUNCA re-arrancar el PSoC en modo SD: si la captura estaba
                 * por cerrar, START_NOW la destruye. Abort limpio: NACK al
                 * maestro y a esperar; los datos (si los hay) siguen en la SD. */
                const bool wasView = g_view_store_active;
                digitalWrite(SYNC_TO_PSOC_PIN, LOW);
                g_view_store_active = false;
                g_start_store_active = false;
                g_state = STOPPED;
                sendCfgAck(wasView ? CMD_VIEW : CMD_START, 0);
            } else {
                psoc.startNow();
            }
        }
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
    if (!g_psocConnected && !g_psocResetActive &&
        g_state != SAMPLING && g_state != HOT_WAIT &&
        g_view_remaining == 0 && (millis() - lastProbeMs) >= 2000) {
        lastProbeMs = millis();
        g_psocConnected = psoc.probe(300);
        if (g_psocConnected) {
            g_psocEverSeen = true;
            g_psocResetFailures = 0u;
            SLAVE_LOG_PRINTF("[SLAVE %d] PSoC: DETECTADO\n", NODE_ID);
            LOGM("PSOC", "detected=1");
            scheduleAutoCalibration(PSOC_AUTO_CAL_DELAY_MS);
            scheduleBootGains();
        }
    }

    /* Sólo recuperar una conexión que alguna vez funcionó. Dos probes
     * fallidos, fuera de captura, antes de pulsar reset. Máximo 3 por arranque
     * y 60 s entre pulsos para no ocultar una falla eléctrica persistente. */
    if (g_psocConnected) {
        g_psocEverSeen = true;
        g_psocResetFailures = 0u;
    }
    if (g_psocEverSeen && !g_psocResetActive &&
        g_state == WAIT_ARM && !captureOrDumpBusy() &&
        g_view_remaining == 0 && !g_cfg_waiting &&
        psoc.hasLastByte() && psoc.lastByteAgeMs() >= PSOC_RESET_STALE_MS &&
        g_psocAutoResetCount < 3u &&
        (g_psocAutoResetCount == 0u ||
         (uint32_t)(millis() - g_psocResetLastAutoMs) >= PSOC_RESET_COOLDOWN_MS) &&
        (uint32_t)(millis() - g_psocResetLastProbeMs) >= PSOC_RESET_RETRY_MS) {
        g_psocResetLastProbeMs = millis();
        g_psocConnected = psoc.probe(300);
        if (g_psocConnected) {
            g_psocResetFailures = 0u;
        } else if (++g_psocResetFailures >= 2u) {
            g_psocResetLastAutoMs = millis();
            g_psocAutoResetCount++;
            beginPsocResetRequest("UART sin respuesta");
        }
    }

    /* Consulta de SD del PSoC mientras no se conozca (0xBC, p=0: estado
     * cacheado, no re-inicializa). Firmware PSoC viejo la ignora en silencio;
     * tras algunos intentos se asume sin SD (sd_present=0, que ya es el
     * default). */
    static uint32_t lastSdQueryMs = 0;
    if (g_psocConnected && !g_psoc_sd_known &&
        g_state != SAMPLING && g_state != HOT_WAIT && g_view_remaining == 0 &&
        (millis() - lastSdQueryMs) >= 5000) {
        lastSdQueryMs = millis();
        psoc.sdStatus(0);
    }

    /* HELLO/Fs cada 2 s mientras no haya captura ni dump pendiente.
     * Si el maestro se reinicia con los esclavos ya ARMED, necesita recuperar
     * este dato sin obligar al operador a cargar Fs manualmente en la web. */
    static uint32_t lastHelloMs = 0;
    if (g_view_remaining == 0 &&
        g_state != SAMPLING &&
        g_state != HOT_WAIT &&
        !(g_state == STOPPED && g_rec_n_batches > 0) &&
        millis() - lastHelloMs >= 2000) {
        lastHelloMs = millis();
        MsgHello h = {
            CMD_HELLO, NODE_ID, (uint8_t)g_psocConnected,
            effectivePsocSampleRateHz(), g_psoc_hw_class,
            psocSdPresent(),  /* SD del PSoC, no del ESP32 (sd_storage obsoleto) */
#if PSOC_I2C_ENABLE
            SLAVE_LINK_I2C,
#else
            SLAVE_LINK_SERIAL,
#endif
            controlStatusForHello(),
            g_pga_code,
            (g_psoc_hw_class == 0u) ? g_pgaout : (uint8_t)0xFFu,
            live::ramCapacityBatches(),
            live::sdCapacityBatches(),
            (uint8_t)esp_reset_reason(),
            (uint32_t)(millis() / 1000u)
        };
        esp_err_t err = sendToMaster((const uint8_t *)&h, sizeof(h));
#if SLAVE_LOG_HELLO_TX
        SLAVE_LOG_PRINTF("[SLAVE] HELLO tx err=%d txOK=%u txFail=%u\n",
                         (int)err, transport.sentOK(), transport.sentFail());
#else
        (void)err;
#endif
    }

    /* Informe de estado cada 10 s. En HOT_WAIT normal (dura <1 s) sigue en
     * silencio RF; pero si HOT_WAIT se estira (>2 s, algo anda mal) volvemos
     * a emitir STATUS para que el maestro no dé el nodo por muerto. */
    static uint32_t last_status = 0;
    bool hotwaitVisible = (g_state == HOT_WAIT &&
                           (millis() - g_hotwait_enter_ms) >= HOTWAIT_STATUS_AFTER_MS);
    if ((g_state != HOT_WAIT || hotwaitVisible) && g_state != SAMPLING &&
        millis() - last_status > 10000) {
        last_status = millis();
        MsgStatus st = {
            CMD_STATUS, NODE_ID,
            psoc.batchesOK(), psoc.batchesBad(),
            transport.sentOK(), transport.sentFail(),
            (uint8_t)g_psocConnected
        };
        sendToMaster((const uint8_t *)&st, sizeof(st));
#if SLAVE_LOG_STATUS_PERIODIC
        logPsocUartDiag("STATUS");
#if PSOC_RX_SCAN_ENABLE
        logPsocRxScan();
#endif
#endif
    }
}
