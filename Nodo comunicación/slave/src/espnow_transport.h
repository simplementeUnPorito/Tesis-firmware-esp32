#pragma once
/*
 * espnow_transport.h — Envío de batches PSoC al maestro vía ESP-NOW.
 *
 * Cada batch de 30 muestras se fragmenta en 2 MsgData (15 muestras c/u)
 * para caber en el límite de 250 bytes de ESP-NOW.
 */

#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include "psoc_spi.h"
#include "sync_protocol.h"

class EspNowTransport {
public:
    /* masterMac: dirección MAC del ESP maestro (6 bytes) */
    bool begin(const uint8_t *masterMac);

    /* Envía un batch completo en 2 paquetes ESP-NOW.
     * t_start_us: timestamp del maestro recibido en CMD_START.
     * nodeId: identificador de este nodo. */
    void sendBatch(const PsocBatch &batch, uint64_t t_start_us, uint8_t nodeId);

    uint32_t sentOK()   const { return _sentOK; }
    uint32_t sentFail() const { return _sentFail; }

    /* Callback de confirmación de envío (registrar antes de begin si se desea) */
    static void onSendCb(const uint8_t *mac, esp_now_send_status_t status);

private:
    uint8_t  _masterMac[6];
    uint32_t _sentOK   = 0;
    uint32_t _sentFail = 0;

    static uint8_t _calcCrc(const uint8_t *data, size_t len);
    void _buildAndSend(const PsocBatch &batch, uint8_t part,
                       uint64_t t_start_us, uint8_t nodeId);
};

/* ── Implementación inline ─────────────────────────────────────────────── */

inline uint8_t EspNowTransport::_calcCrc(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) { crc ^= data[i]; }
    return crc;
}

inline void EspNowTransport::onSendCb(const uint8_t *, esp_now_send_status_t status)
{
    (void)status;   /* confirmación se maneja por contador en sendBatch */
}

inline bool EspNowTransport::begin(const uint8_t *masterMac)
{
    memcpy(_masterMac, masterMac, 6);

    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESPNOW] init failed");
        return false;
    }
    esp_now_register_send_cb(onSendCb);

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, masterMac, 6);
    peer.channel = 0;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK) {
        Serial.println("[ESPNOW] add_peer failed");
        return false;
    }
    Serial.println("[ESPNOW] ready");
    return true;
}

inline void EspNowTransport::_buildAndSend(const PsocBatch &batch, uint8_t part,
                                            uint64_t t_start_us, uint8_t nodeId)
{
    MsgData msg = {};
    msg.marker      = DATA_PKT_MARKER;
    msg.node_id     = nodeId;
    msg.seq         = batch.seq;
    msg.part        = part;
    msg.n_samples   = SAMPLES_PER_PART;
    msg.timestamp_us= t_start_us + batch.timestamp_us;

    int base = part * SAMPLES_PER_PART;
    for (int i = 0; i < SAMPLES_PER_PART; i++) {
        const PsocSample &s = batch.samples[base + i];
        SampleBytes &b = msg.samples[i];
        b.raw_lo = (uint8_t)( s.raw_input       & 0xFF);
        b.raw_hi = (uint8_t)((s.raw_input >> 8)  & 0xFF);
        b.alog0  = (uint8_t)( s.post_analog       & 0xFF);
        b.alog1  = (uint8_t)((s.post_analog >>  8) & 0xFF);
        b.alog2  = (uint8_t)((s.post_analog >> 16) & 0xFF);
        b.digi0  = (uint8_t)( s.post_digital       & 0xFF);
        b.digi1  = (uint8_t)((s.post_digital >>  8) & 0xFF);
        b.digi2  = (uint8_t)((s.post_digital >> 16) & 0xFF);
        b.gain   = s.gain_byte;
        b.flags  = s.sample_flags;
    }
    /* CRC sobre todos los bytes del struct excepto el último (crc mismo) */
    msg.crc = _calcCrc((const uint8_t *)&msg, sizeof(msg) - 1);

    esp_err_t err = esp_now_send(_masterMac, (const uint8_t *)&msg, sizeof(msg));
    if (err == ESP_OK) { _sentOK++; } else { _sentFail++; }
}

inline void EspNowTransport::sendBatch(const PsocBatch &batch,
                                        uint64_t t_start_us, uint8_t nodeId)
{
    _buildAndSend(batch, 0, t_start_us, nodeId);
    _buildAndSend(batch, 1, t_start_us, nodeId);
}
