/*
 * ESP Maestro — Gateway geófono WiFi
 *
 * Roles:
 *   1. WiFi AP ("GeoNetwork") para que MATLAB y esclavos se conecten.
 *   2. ESP-NOW: recibe batches de los esclavos, les envía ARM/START/STOP.
 *   3. TCP server en puerto 5005: stream de datos hacia MATLAB.
 *   4. Muestreo del martillo (STUB cuando HAMMER_BTN_ENABLED=0).
 *
 * Flujo de sincronización:
 *   MATLAB → cmd 0xA2 (ARM)   → maestro broadcast CMD_ARM a esclavos
 *   Esclavos responden CMD_ARM_ACK
 *   MATLAB → cmd 0xA3 (START) → maestro graba t_start_us, broadcast CMD_START
 *   Todos los nodos inician muestreo simultáneamente
 *   MATLAB → cmd 0xA4 (STOP)  → maestro broadcast CMD_STOP
 *
 * Flags en platformio.ini:
 *   -DHAMMER_BTN_ENABLED=0    disable hammer hardware (stub sinusoidal)
 *   -DHAMMER_ACCEL_ENABLED=0  disable accel hardware  (stub plano)
 *   -DNUM_SLAVES=2            cuántos ARM_ACK esperar antes de reportar listo
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include "sync_protocol.h"
#include "hammer_adc.h"
#include "espnow_rx.h"
#include "matlab_transport.h"

/* ── Configuración ────────────────────────────────────────────────────────── */
static const char *AP_SSID = "GeoNetwork";
static const char *AP_PASS = "geophone2026";

#ifndef NUM_SLAVES
  #define NUM_SLAVES 2
#endif

/* MACs de los esclavos — actualizar con las MACs reales */
static const uint8_t SLAVE_MACS[NUM_SLAVES][6] = {
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01},   /* Esclavo 1 — reemplazar */
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02},   /* Esclavo 2 — reemplazar */
};

/* ── Pin GPIO hardware sync ───────────────────────────────────────────────── */
#define SYNC_OUT_PIN 25   /* Salida: HIGH = muestrear, LOW = parar */
#define LED_PIN      2    /* GPIO2 = LED azul integrado ESP32-DevKitC V4 */

/* ── Estado ──────────────────────────────────────────────────────────────── */
enum MasterState { IDLE, ARMING, ARMED, RUNNING, STOPPING };
static MasterState g_state       = IDLE;
static uint8_t     g_armedCount  = 0;
static bool        g_streaming   = false;
static uint64_t    g_t_start_us  = 0;

/* ── Objetos ─────────────────────────────────────────────────────────────── */
static HammerADC      hammer;
static EspNowRx       espnowRx;
static MatlabTransport matlab;

/* ── Hammer timing ───────────────────────────────────────────────────────── */
static uint32_t g_lastHammerUs = 0;

/* ── Comando dirigido a un esclavo específico ────────────────────────────── */

static void handleDirectedCmd(uint8_t node_id, uint8_t sub_cmd, uint8_t param)
{
    /* Fase 1: ACK inmediato siempre.
     * Fase 2: reemplazar con esp_now_send unicast (MsgSetConfig) al esclavo. */
    matlab.sendAck(node_id, sub_cmd, param);
    Serial.printf("[MASTER] directed node=%d sub=0x%02X param=%d\n",
                  node_id, sub_cmd, param);
}

/* ── Broadcast ESP-NOW a todos los esclavos ──────────────────────────────── */

static void broadcastArm()
{
    MsgArm msg = { CMD_ARM };
    for (int i = 0; i < NUM_SLAVES; i++) {
        esp_now_send(SLAVE_MACS[i], (uint8_t *)&msg, sizeof(msg));
    }
    Serial.println("[MASTER] ARM enviado");
}

static void broadcastStart()
{
    g_t_start_us = (uint64_t)micros();
    /* GPIO primero — flanco hardware llega a los PSoC antes que el ESP-NOW */
    digitalWrite(SYNC_OUT_PIN, HIGH);
    MsgStart msg = { CMD_START, g_t_start_us };
    for (int i = 0; i < NUM_SLAVES; i++) {
        esp_now_send(SLAVE_MACS[i], (uint8_t *)&msg, sizeof(msg));
    }
    Serial.printf("[MASTER] START t0=%llu\n", g_t_start_us);
}

static void broadcastStop()
{
    digitalWrite(SYNC_OUT_PIN, LOW);
    MsgStop msg = { CMD_STOP };
    for (int i = 0; i < NUM_SLAVES; i++) {
        esp_now_send(SLAVE_MACS[i], (uint8_t *)&msg, sizeof(msg));
    }
    Serial.println("[MASTER] STOP enviado");
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
            Serial.printf("[MASTER] stream %s\n", g_streaming ? "ON" : "OFF");
            break;

        case 0xA2:   /* ARM esclavos */
            if (g_state == IDLE || g_state == ARMED) {
                g_armedCount = 0;
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

        case 0xA7: {  /* debug mode on/off — maestro + esclavos */
            if (param) {
                /* Activar: maestro entra en RUNNING+streaming para enviar stub */
                g_streaming  = true;
                g_state      = RUNNING;
                g_t_start_us = (uint64_t)micros();
                digitalWrite(SYNC_OUT_PIN, HIGH);
            } else {
                /* Desactivar: volver a IDLE */
                g_streaming = false;
                g_state     = IDLE;
                digitalWrite(SYNC_OUT_PIN, LOW);
            }
            /* Propagar a esclavos para su propio debug */
            MsgDebug dbg = { CMD_DEBUG, param };
            for (int i = 0; i < NUM_SLAVES; i++) {
                esp_now_send(SLAVE_MACS[i], (uint8_t *)&dbg, sizeof(dbg));
            }
            matlab.sendAck(0x00, cmd, param);
            Serial.printf("[MASTER] debug=%d (streaming=%d state=%d)\n",
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
    Serial.begin(115200);
    Serial.println("[MASTER] boot");

    /* WiFi AP */
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.printf("[MASTER] AP IP: %s\n", WiFi.softAPIP().toString().c_str());
    Serial.printf("[MASTER] MAC:   %s\n", WiFi.macAddress().c_str());

    /* ESP-NOW (convive con AP mode) */
    if (!espnowRx.begin(onBatchReady)) {
        Serial.println("[MASTER] ESP-NOW FAIL — halt");
        while (true) { delay(1000); }
    }

    /* Registrar esclavos como peers para poder enviarles */
    for (int i = 0; i < NUM_SLAVES; i++) {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, SLAVE_MACS[i], 6);
        peer.channel = 0;
        peer.encrypt = false;
        esp_now_add_peer(&peer);
    }

    /* GPIO hardware sync — salida hacia esclavos y PSoC */
    pinMode(SYNC_OUT_PIN, OUTPUT);
    digitalWrite(SYNC_OUT_PIN, LOW);

    /* Serial USB para MATLAB */
    matlab.begin();

    /* Martillo (stub o real según flags) */
    hammer.begin();

    Serial.println("[MASTER] listo");
}

/* ── Loop ────────────────────────────────────────────────────────────────── */

void loop()
{
    /* Gestionar conexión TCP de MATLAB */
    matlab.loop();

    /* Procesar comandos de MATLAB */
    auto rxCmd = matlab.lastCmd();
    if (rxCmd.valid) {
        handleMatlabCmd(rxCmd);
    }

    /* Muestrar martillo a ~4 kHz */
    uint32_t nowUs = (uint32_t)micros();
    if ((uint32_t)(nowUs - g_lastHammerUs) >= HAMMER_SAMPLE_US) {
        g_lastHammerUs = nowUs;
        if (g_streaming && g_state == RUNNING) {
            HammerSample hs = hammer.read();
            matlab.sendHammer(hs.btn_raw);
        }
    }

    /* Detectar transición ARMING → ARMED cuando llegan suficientes ACK
     * (los ACK se imprimieron en espnow_rx.h; acá solo chequeamos con timeout) */
    static uint32_t armStartMs = 0;
    if (g_state == ARMING) {
        if (armStartMs == 0) armStartMs = millis();
        if (millis() - armStartMs > 3000) {
            /* Timeout: reportar cuántos respondieron igual */
            Serial.printf("[MASTER] ARM timeout — %d esclavos listos\n", g_armedCount);
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
            Serial.println("[MASTER] IDLE");
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
