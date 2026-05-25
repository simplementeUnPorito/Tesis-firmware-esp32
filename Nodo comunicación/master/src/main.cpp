/*
 * ESP Maestro — Gateway geófono WiFi
 *
 * Roles:
 *   1. WiFi AP ("GeoNetwork") para que MATLAB y esclavos se conecten.
 *   2. ESP-NOW: recibe batches de los esclavos, les envía ARM/START/STOP.
 *   3. TCP server en puerto 5005: stream de datos hacia MATLAB.
 *   4. Muestreo de aceleración del martillo via IMU (~1020 Hz).
 *
 * Flujo de sincronización:
 *   MATLAB → cmd 0xA2 (ARM)   → maestro broadcast CMD_ARM a esclavos
 *   Esclavos responden CMD_ARM_ACK
 *   MATLAB → cmd 0xA3 (START) → maestro graba t_start_us, broadcast CMD_START
 *   Todos los nodos inician muestreo simultáneamente
 *   MATLAB → cmd 0xA4 (STOP)  → maestro broadcast CMD_STOP
 *
 * Flags en platformio.ini:
 *   -DHAMMER_IMU_ENABLED=0    0=stub (sin hardware), 1=real (ver hammer_imu.h)
 *   -DNUM_SLAVES=3            cuántos esclavos soportar como máximo
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include "sync_protocol.h"
#include "hammer_imu.h"
#include "espnow_rx.h"
#include "matlab_transport.h"
#include "master_log.h"

/* ── Configuración ────────────────────────────────────────────────────────── */
static const char *AP_SSID = "GeoNetwork";
static const char *AP_PASS = "geophone2026";
static const uint8_t ESPNOW_BROADCAST[6] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

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

/* MACs de los esclavos — actualizar con las MACs reales */
static const uint8_t SLAVE_MACS[NUM_SLAVES][6] = {
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01},   /* Esclavo 1 — reemplazar */
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02},   /* Esclavo 2 — reemplazar */
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x03},   /* Esclavo 3 — reemplazar */
};

/* ── Pin GPIO hardware sync ───────────────────────────────────────────────── */
#define SYNC_OUT_PIN 25   /* Salida: HIGH = muestrear, LOW = parar */
#define LED_PIN      2    /* GPIO2 = LED azul integrado ESP32-DevKitC V4 */

/* ── Estado ──────────────────────────────────────────────────────────────── */
enum MasterState { IDLE, ARMING, ARMED, RUNNING, STOPPING };
static MasterState g_state       = IDLE;
static uint8_t     g_armedCount  = 0;
static uint8_t     g_expectedSlaves = NUM_SLAVES;
static bool        g_streaming   = false;
static bool        g_espnowReady = false;
static bool        g_testMode    = false;   /* rampa en stub martillo durante 0xA7 */
static uint64_t    g_t_start_us  = 0;
static volatile uint32_t g_armAckMask = 0;

/* ── Objetos ─────────────────────────────────────────────────────────────── */
static HammerIMU      hammer;
static EspNowRx       espnowRx;
static MatlabTransport matlab;

/* ── Hammer timing ───────────────────────────────────────────────────────── */
static uint32_t g_lastHammerUs = 0;

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

static bool sameMac(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, 6) == 0;
}

static bool isConfiguredSlaveMac(const uint8_t mac[6])
{
    static const uint8_t ZERO_MAC[6] = {0, 0, 0, 0, 0, 0};
    if (sameMac(mac, ZERO_MAC) || sameMac(mac, ESPNOW_BROADCAST)) return false;
    return !(mac[0] == 0xFF && mac[1] == 0xFF && mac[2] == 0xFF &&
             mac[3] == 0xFF && mac[4] == 0xFF);
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

static void onArmAck(const MsgArmAck &msg)
{
    if (msg.node_id == 0 || msg.node_id > NUM_SLAVES) return;
    if (msg.status == 0) {
        g_armAckMask |= (1UL << msg.node_id);
    }
}

static void onSlaveStatus(const MsgStatus &msg)
{
    (void)msg;
}

static void onCfgAck(const MsgCfgAck &msg)
{
    matlab.sendAck(msg.node_id, msg.sub_cmd, msg.ok);
}

static void onHello(const MsgHello &msg)
{
    matlab.sendHelloNotif(msg.node_id);
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

    const uint8_t *dst = isConfiguredSlaveMac(SLAVE_MACS[node_id - 1])
                       ? SLAVE_MACS[node_id - 1]
                       : ESPNOW_BROADCAST;
    esp_err_t err;
    if (sub_cmd == 0xA7) {
        MsgDebugNode msg = { CMD_DEBUG_NODE, node_id, param };
        err = esp_now_send(dst, (uint8_t *)&msg, sizeof(msg));
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
    /* GPIO primero — flanco hardware llega a los PSoC antes que el ESP-NOW */
    digitalWrite(SYNC_OUT_PIN, HIGH);
    MsgStart msg = { CMD_START, g_t_start_us };
    if (g_espnowReady) {
        esp_now_send(ESPNOW_BROADCAST, (uint8_t *)&msg, sizeof(msg));
    }
    MASTER_LOG_PRINTF("[MASTER] START t0=%llu\n", g_t_start_us);
}

static void broadcastStop()
{
    digitalWrite(SYNC_OUT_PIN, LOW);
    MsgStop msg = { CMD_STOP };
    if (g_espnowReady) {
        esp_now_send(ESPNOW_BROADCAST, (uint8_t *)&msg, sizeof(msg));
    }
    MASTER_LOG_PRINTLN("[MASTER] STOP enviado");
}

/* ── Callback: batch reensamblado de un esclavo ─────────────────────────── */

static void onBatchReady(const ReassembledBatch &batch)
{
    if (!g_streaming) return;

    /* Reenviar las 30 muestras (post_digital) al MATLAB */
    for (int i = 0; i < SAMPLES_PER_PART * 2; i++) {
        const SampleBytes &s = batch.samples[i];
        int32_t val = (int32_t)s.digi0
                    | ((int32_t)s.digi1 << 8)
                    | ((int32_t)s.digi2 << 16);
        /* Extensión de signo 24→32 */
        if (val & 0x800000) val |= (int32_t)0xFF000000;
        matlab.sendSample(batch.node_id, val);
        samplePulse();
    }
}

/* ── Procesar comandos de MATLAB ─────────────────────────────────────────── */

static void handleMatlabCmd(const MatlabTransport::RxCmd &rxCmd)
{
    uint8_t cmd   = rxCmd.cmd;
    uint8_t param = rxCmd.param;
    switch (cmd) {
        case 0xA1:   /* stream on/off */
            g_streaming = (param != 0);
            matlab.sendAck(0xFF, cmd, g_streaming ? 1 : 0);
            MASTER_LOG_PRINTF("[MASTER] stream %s\n", g_streaming ? "ON" : "OFF");
            break;

        case 0xA2:   /* ARM esclavos */
            if (g_state == IDLE || g_state == ARMED || g_state == ARMING) {
                g_expectedSlaves = param;
                if (g_expectedSlaves > NUM_SLAVES) g_expectedSlaves = NUM_SLAVES;
                /* Preservar ACKs existentes en re-ARM — no perder esclavos ya conectados */
                if (g_state == IDLE) {
                    g_armAckMask = 0;
                    g_armedCount = 0;
                } else {
                    g_armedCount = countArmAcks(g_armAckMask);
                }
                g_state = ARMING;
                broadcastArm();
                matlab.sendAck(0xFF, cmd, 0);
            }
            break;

        case 0xA3:   /* START */
            if (g_state == ARMED || g_state == ARMING) {
                broadcastStart();
                g_state = RUNNING;
                matlab.sendAck(0xFF, cmd, 1);
            }
            break;

        case 0xA4:   /* STOP */
            broadcastStop();
            g_state = STOPPING;
            matlab.sendAck(0xFF, cmd, 0);
            break;

        case 0xA5:   /* STATUS */
            matlab.sendReady(g_armedCount);
            break;

        case 0xBD:   /* comando dirigido a esclavo específico */
            handleDirectedCmd(rxCmd.node_id, rxCmd.sub_cmd, rxCmd.param);
            break;

        case 0xA7: {  /* debug mode on/off — solo maestro, esclavos tienen su propio cmd */
            if (param) {
                g_testMode   = true;
                g_streaming  = true;
                g_state      = RUNNING;
                g_t_start_us = (uint64_t)micros();
                digitalWrite(SYNC_OUT_PIN, HIGH);
            } else {
                g_testMode  = false;
                g_streaming = false;
                g_state     = IDLE;
                digitalWrite(SYNC_OUT_PIN, LOW);
            }
            matlab.sendAck(0x00, cmd, param);
            MASTER_LOG_PRINTF("[MASTER] debug=%d (streaming=%d state=%d)\n",
                              param, g_streaming, g_state);
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
    MASTER_LOG_PRINTLN("[MASTER] boot");
    samplePulseBegin();
    MASTER_LOG_PRINTF("[SCOPE] sample pulse pin=%d idle=%d us=%d\n",
                      SAMPLE_PULSE_PIN, SAMPLE_PULSE_IDLE, SAMPLE_PULSE_US);

    /* WiFi AP_STA — AP_STA es necesario para que los action frames de ESP-NOW
     * sean compatibles con esclavos ESP8266 en modo STA no conectado.
     * El modo puro WIFI_AP lleva el AP MAC como BSSID en el action frame,
     * que el ESP8266 puede descartar si no reconoce ese BSSID. */
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASS, 1);   /* canal 1 fijo */
    MASTER_LOG_PRINTF("[MASTER] AP IP:     %s\n", WiFi.softAPIP().toString().c_str());
    MASTER_LOG_PRINTF("[MASTER] STA MAC:   %s\n", WiFi.macAddress().c_str());
    MASTER_LOG_PRINTF("[MASTER] AP MAC:    %s  ch=%d\n",
                      WiFi.softAPmacAddress().c_str(), WiFi.softAPChannel());

    /* ESP-NOW (convive con AP mode) */
    if (!espnowRx.begin(onBatchReady, onArmAck, onSlaveStatus, onCfgAck, onHello)) {
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

    /* GPIO hardware sync — salida hacia esclavos y PSoC */
    pinMode(SYNC_OUT_PIN, OUTPUT);
    digitalWrite(SYNC_OUT_PIN, LOW);

    /* Martillo (stub o real según flags) */
    hammer.begin();

    MASTER_LOG_PRINTLN("[MASTER] listo");
}

/* ── Loop ────────────────────────────────────────────────────────────────── */

void loop()
{
    /* Gestionar conexión TCP de MATLAB */
    matlab.loop();

    /* Procesar TODOS los comandos encolados (evita pérdida de A1+A3 back-to-back) */
    while (matlab.hasCmd()) {
        handleMatlabCmd(matlab.lastCmd());
    }

    /* Muestrar martillo a ~1020 Hz */
    uint32_t nowUs = (uint32_t)micros();
    if ((uint32_t)(nowUs - g_lastHammerUs) >= HAMMER_SAMPLE_US) {
        g_lastHammerUs = nowUs;
        if (g_streaming && g_state == RUNNING) {
            int32_t val;
            if (g_testMode) {
                /* Rampa ascendente durante test — igual que el stub de esclavos */
                static uint32_t ramp = 0;
                val = (int32_t)(ramp++ & 0xFFFFFF);
            } else {
                val = hammer.readAccel();
            }
            matlab.sendHammer(val);
            samplePulse();
        }
    }

    /* Detectar transición ARMING → ARMED cuando llegan suficientes ACK
     * (los ACK se imprimieron en espnow_rx.h; acá solo chequeamos con timeout) */
    static uint32_t armStartMs = 0;
    if (g_state == ARMING) {
        if (armStartMs == 0) armStartMs = millis();
        uint8_t ackCount = countArmAcks(g_armAckMask);
        if (ackCount != g_armedCount) {
            g_armedCount = ackCount;
            matlab.sendReady(g_armedCount);
        }
        if (g_armedCount >= g_expectedSlaves) {
            g_state = ARMED;
            matlab.sendReady(g_armedCount);
            armStartMs = 0;
        }
        else if (millis() - armStartMs > 3000) {
            /* Timeout: reportar cuántos respondieron igual */
            MASTER_LOG_PRINTF("[MASTER] ARM timeout - %d esclavos listos\n", g_armedCount);
            g_state = ARMED;
            matlab.sendReady(g_armedCount);
            armStartMs = 0;
        }
    } else {
        armStartMs = 0;
    }

    /* Transición STOPPING → IDLE */
    static uint32_t stopStartMs = 0;
    if (g_state == STOPPING) {
        if (stopStartMs == 0) stopStartMs = millis();
        if (millis() - stopStartMs > 500) {
            g_state = IDLE;
            stopStartMs = 0;
            MASTER_LOG_PRINTLN("[MASTER] IDLE");
        }
    } else {
        stopStartMs = 0;
    }

    /* Heartbeat al MATLAB cada ~1 s cuando no hay stream activo */
    static uint32_t lastHbMs = 0;
    if (!g_streaming && millis() - lastHbMs > 1000) {
        lastHbMs = millis();
        if (matlab.connected()) {
            matlab.sendHeartbeat(0x00, 0, 0, (uint8_t)g_state);
        }
    }
}
