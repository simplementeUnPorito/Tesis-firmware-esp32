/*
 * ESP Esclavo — Geofono node
 *
 * Lee muestras del PSoC via SPI y las envía al maestro via ESP-NOW.
 *
 * Configurar en platformio.ini:
 *   build_flags = -DNODE_ID=1        (1 o 2 según el nodo físico)
 *
 * Configurar MAC del maestro en MASTER_MAC[].
 *
 * Estados:
 *   WAIT_ARM  → espera CMD_ARM del maestro
 *   ARMED     → envía CMD_ARM_ACK, espera CMD_START
 *   SAMPLING  → lee PSoC y envía batches
 *   STOPPED   → vuelve a WAIT_ARM al recibir CMD_STOP
 */

#include <Arduino.h>
#include "psoc_spi.h"
#include "espnow_transport.h"
#include "sync_protocol.h"

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
enum SlaveState { WAIT_ARM, ARMED, SAMPLING, STOPPED };
static volatile SlaveState g_state       = WAIT_ARM;
static volatile uint64_t   g_t_start_us  = 0;
static volatile bool       g_debug_mode  = false;
static          uint32_t   g_debug_count = 0;
static          uint8_t    g_pga_code    = 0;
static          uint8_t    g_pgavdac     = 0;
static          uint8_t    g_vdac_byte   = 128;
static          uint8_t    g_tx_filtered = 0;

/* ── Objetos ─────────────────────────────────────────────────────────────── */
static PsocSPI        psoc;
static EspNowTransport transport;

static inline uint8_t samplePulseActiveLevel()
{
    return (SAMPLE_PULSE_IDLE == LOW) ? HIGH : LOW;
}

static void samplePulseBegin()
{
#if SAMPLE_PULSE_PIN >= 0
    pinMode(SAMPLE_PULSE_PIN, OUTPUT);
    digitalWrite(SAMPLE_PULSE_PIN, SAMPLE_PULSE_IDLE);
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
    digitalWrite(SYNC_TO_PSOC_PIN, level);
    if (level == HIGH && (g_state == ARMED || g_state == WAIT_ARM)) {
        g_t_start_us = (uint64_t)micros();
        g_state = SAMPLING;
    } else if (level == LOW && g_state == SAMPLING) {
        g_state = STOPPED;
    }
}

/* ── Callbacks ESP-NOW ───────────────────────────────────────────────────── */

static void sendCfgAck(uint8_t sub_cmd, uint8_t ok)
{
    MsgCfgAck ack = { CMD_CFG_ACK, NODE_ID, sub_cmd, ok };
    espnowSend(MASTER_MAC, (const uint8_t *)&ack, sizeof(ack));
}

static void setDebugMode(bool enable)
{
    g_debug_mode = enable;
    g_debug_count = 0;
    if (enable) {
        g_t_start_us = (uint64_t)micros();
        g_state = SAMPLING;
    } else if (g_state == SAMPLING) {
        g_state = STOPPED;
    } else {
        g_state = WAIT_ARM;
    }
}

static void handleSetConfig(const MsgSetConfig *cfg)
{
    if (cfg->node_id != NODE_ID) return;

    uint8_t ok = 1;
    switch (cfg->sub_cmd) {
        case 0xA6:
            g_pga_code = cfg->param;
            break;
        case 0xA8:
            g_tx_filtered = cfg->param ? 1 : 0;
            break;
        case 0xA9:
            g_pgavdac = cfg->param;
            break;
        case 0xAA:
            g_vdac_byte = cfg->param;
            break;
        default:
            ok = 0;
            break;
    }
    sendCfgAck(cfg->sub_cmd, ok);
    Serial.printf("[SLAVE] cfg sub=0x%02X p=%u ok=%u\n",
                  cfg->sub_cmd, cfg->param, ok);
}

static void handleDebugNode(const MsgDebugNode *dbg)
{
    if (dbg->node_id != NODE_ID) return;
    setDebugMode(dbg->enable != 0);
    sendCfgAck(0xA7, 1);
    Serial.printf("[SLAVE] debug_node=%d\n", (int)g_debug_mode);
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

    if (cmd == CMD_ARM && (g_state == WAIT_ARM || g_state == ARMED)) {
        g_state = ARMED;
        MsgArmAck ack = { CMD_ARM_ACK, NODE_ID, 0 };
        espnowSend(MASTER_MAC, (const uint8_t *)&ack, sizeof(ack));
        Serial.println("[SLAVE] ARMED");
    }
    else if (cmd == CMD_START && g_state == ARMED) {
        /* START por software: usar como fallback si no hay cable GPIO */
        const MsgStart *msg = (const MsgStart *)data;
        g_t_start_us = msg->t_start_us;
        g_state = SAMPLING;
        Serial.printf("[SLAVE] START(SW) t0=%llu\n", (unsigned long long)g_t_start_us);
    }
    else if (cmd == CMD_STOP) {
        g_state = STOPPED;
        Serial.println("[SLAVE] STOP");
    }
    else if (cmd == CMD_DEBUG) {
        const MsgDebug *d = (const MsgDebug *)data;
        setDebugMode(d->enable != 0);
        MsgArmAck ack = { CMD_ARM_ACK, NODE_ID, (uint8_t)(g_debug_mode ? 0xDD : 0x00) };
        espnowSend(MASTER_MAC, (const uint8_t *)&ack, sizeof(ack));
        Serial.printf("[SLAVE] debug=%d\n", (int)g_debug_mode);
    }
    else if (cmd == CMD_SET_CONFIG && len >= (int)sizeof(MsgSetConfig)) {
        handleSetConfig((const MsgSetConfig *)data);
    }
    else if (cmd == CMD_DEBUG_NODE && len >= (int)sizeof(MsgDebugNode)) {
        handleDebugNode((const MsgDebugNode *)data);
    }
}

/* ── Callback PSoC — batch recibido ─────────────────────────────────────── */

static void onBatch(const PsocBatch &batch)
{
    if (g_state != SAMPLING) return;
    pulseSampleCount(batch.n_samples);
    transport.sendBatch(batch, g_t_start_us, NODE_ID);
}

/* ── Setup ───────────────────────────────────────────────────────────────── */

void setup()
{
    Serial.begin(115200);
    Serial.printf("[SLAVE %d] boot\n", NODE_ID);
    samplePulseBegin();
    Serial.printf("[SCOPE] sample pulse pin=%d idle=%d us=%d\n",
                  SAMPLE_PULSE_PIN, SAMPLE_PULSE_IDLE, SAMPLE_PULSE_US);

#if defined(ESP8266)
    /* Modo AP oculto en canal 1: evita el background scanning que hace el
     * ESP8266 en modo STA desconectado (recorre ch1-13 buscando APs y pierde
     * paquetes ESP-NOW del canal 1 a distancia real). */
    WiFi.mode(WIFI_AP);
    WiFi.softAP("GeoSlave", "", 1, 1);   /* ssid, pass vacío, canal 1, oculto */
    delay(100);
    Serial.printf("[SLAVE] MAC: %s  ch=%d (AP-fixed)\n",
                  WiFi.macAddress().c_str(), wifi_get_channel());
#else
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    Serial.printf("[SLAVE] MAC: %s  ch=%d\n",
                  WiFi.macAddress().c_str(), WiFi.channel());
#endif

    /* ESP-NOW */
    if (!transport.begin(MASTER_MAC)) {
        Serial.println("[SLAVE] ESP-NOW FAIL — halt");
        while (true) { delay(1000); }
    }
    esp_now_register_recv_cb(onDataRecv);

    /* GPIO hardware sync */
    pinMode(SYNC_IN_PIN,      INPUT);
    pinMode(SYNC_TO_PSOC_PIN, OUTPUT);
    digitalWrite(SYNC_TO_PSOC_PIN, LOW);
    attachInterrupt(digitalPinToInterrupt(SYNC_IN_PIN), onSyncEdge, CHANGE);

    /* SPI → PSoC */
    psoc.begin(onBatch);

    Serial.printf("[SLAVE %d] listo, esperando ARM\n", NODE_ID);
}

/* ── Loop ────────────────────────────────────────────────────────────────── */

void loop()
{
    /* Poll PSoC o generar debug ramp */
    if (g_state == SAMPLING) {
        if (g_debug_mode) {
            /* Generar batch falso a 29412 µs (30 muestras @ 1020 Hz) */
            static uint32_t lastDebugUs = 0;
            if ((uint32_t)micros() - lastDebugUs >= 29412) {
                lastDebugUs += 29412;
                PsocBatch fake = {};
                fake.seq         = (uint16_t)(g_debug_count / SPI_BATCH_SAMPLES);
                fake.n_samples   = SPI_BATCH_SAMPLES;
                fake.timestamp_us= (uint64_t)micros() - g_t_start_us;
                for (int i = 0; i < SPI_BATCH_SAMPLES; i++) {
                    fake.samples[i].post_digital = (int32_t)(g_debug_count & 0xFFFFFF);
                    fake.samples[i].gain_byte = g_pga_code;
                    fake.samples[i].sample_flags = g_tx_filtered;
                    g_debug_count++;
                }
                onBatch(fake);
            }
        } else {
            psoc.poll();
        }
    }

    /* Volver a esperar ARM después de STOP */
    if (g_state == STOPPED) {
        g_state = WAIT_ARM;
        Serial.printf("[SLAVE %d] volviendo a WAIT_ARM\n", NODE_ID);
    }

    /* HELLO beacon cada 2 s en WAIT_ARM — diagnóstico de ESP-NOW bidireccional */
    static uint32_t lastHelloMs = 0;
    if (g_state == WAIT_ARM && millis() - lastHelloMs >= 2000) {
        lastHelloMs = millis();
        MsgHello h = { CMD_HELLO, NODE_ID };
        esp_err_t err = espnowSend(MASTER_MAC, (const uint8_t *)&h, sizeof(h));
        Serial.printf("[SLAVE] HELLO tx err=%d txOK=%u txFail=%u\n",
                      (int)err, transport.sentOK(), transport.sentFail());
    }

    /* Informe de estado cada 10 s */
    static uint32_t last_status = 0;
    if (millis() - last_status > 10000) {
        last_status = millis();
        MsgStatus st = {
            CMD_STATUS, NODE_ID,
            psoc.batchesOK(), psoc.batchesBad(),
            transport.sentOK(), transport.sentFail()
        };
        espnowSend(MASTER_MAC, (const uint8_t *)&st, sizeof(st));
        Serial.printf("[SLAVE %d] bOK=%u bBad=%u txOK=%u txFail=%u state=%d\n",
                      NODE_ID,
                      psoc.batchesOK(), psoc.batchesBad(),
                      transport.sentOK(), transport.sentFail(),
                      (int)g_state);
    }
}
