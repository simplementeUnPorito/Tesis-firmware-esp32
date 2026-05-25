#pragma once
/*
 * espnow_rx.h — Recepción ESP-NOW de batches de los esclavos.
 *
 * Los paquetes MsgData llegan en fragmentos (part 0/1).
 * Se reensamblan aquí y se notifican como batch completo.
 */

#include <Arduino.h>
#include <esp_now.h>
#include "sync_protocol.h"
#include "master_log.h"

#define MAX_NODES 8

/* Un batch reensamblado */
struct ReassembledBatch {
    uint8_t     node_id;
    uint16_t    seq;
    uint8_t     global_flags;
    uint64_t    timestamp_us;
    SampleBytes samples[SAMPLES_PER_PART * 2];  /* 30 muestras */
    bool        part_rx[2];
    bool        complete() const { return part_rx[0] && part_rx[1]; }
};

typedef void (*BatchReadyCallback)(const ReassembledBatch &batch);
typedef void (*ArmAckCallback)(const MsgArmAck &msg);
typedef void (*StatusCallback)(const MsgStatus &msg);
typedef void (*CfgAckCallback)(const MsgCfgAck &msg);
typedef void (*HelloCallback)(const MsgHello &msg);

class EspNowRx {
public:
    static EspNowRx *instance;

    bool begin(BatchReadyCallback cb,
               ArmAckCallback armAckCb = nullptr,
               StatusCallback statusCb = nullptr,
               CfgAckCallback cfgAckCb = nullptr,
               HelloCallback helloCb = nullptr);

    /* Llamar desde el loop() o directamente del callback ESP-NOW */
    static void onRecv(const uint8_t *mac, const uint8_t *data, int len);

    uint32_t pktsRx()   const { return _pktsRx; }
    uint32_t crcFail()  const { return _crcFail; }

private:
    BatchReadyCallback _cb = nullptr;
    ArmAckCallback     _armAckCb = nullptr;
    StatusCallback     _statusCb = nullptr;
    CfgAckCallback     _cfgAckCb = nullptr;
    HelloCallback      _helloCb  = nullptr;
    ReassembledBatch   _bufs[MAX_NODES] = {};
    uint32_t           _pktsRx  = 0;
    uint32_t           _crcFail = 0;

    void _handleData(const MsgData *msg);
    void _handleStatus(const MsgStatus *msg);
    void _handleArmAck(const MsgArmAck *msg);
    void _handleCfgAck(const MsgCfgAck *msg);
    void _handleHello(const MsgHello *msg);

    static uint8_t _calcCrc(const uint8_t *data, size_t len)
    {
        uint8_t crc = 0;
        for (size_t i = 0; i < len; i++) { crc ^= data[i]; }
        return crc;
    }
};

EspNowRx *EspNowRx::instance = nullptr;

inline bool EspNowRx::begin(BatchReadyCallback cb,
                            ArmAckCallback armAckCb,
                            StatusCallback statusCb,
                            CfgAckCallback cfgAckCb,
                            HelloCallback helloCb)
{
    _cb = cb;
    _armAckCb = armAckCb;
    _statusCb = statusCb;
    _cfgAckCb = cfgAckCb;
    _helloCb  = helloCb;
    instance = this;
    if (esp_now_init() != ESP_OK) {
        MASTER_LOG_PRINTLN("[ESPNOW-RX] init failed");
        return false;
    }
    esp_now_register_recv_cb(onRecv);
    MASTER_LOG_PRINTLN("[ESPNOW-RX] ready");
    return true;
}

inline void EspNowRx::onRecv(const uint8_t *mac, const uint8_t *data, int len)
{
    if (!instance || len < 1) return;
    uint8_t first = data[0];

    if (first == DATA_PKT_MARKER && (size_t)len >= sizeof(MsgData)) {
        instance->_handleData((const MsgData *)data);
    } else if (first == CMD_ARM_ACK && (size_t)len >= sizeof(MsgArmAck)) {
        instance->_handleArmAck((const MsgArmAck *)data);
    } else if (first == CMD_STATUS && (size_t)len >= sizeof(MsgStatus)) {
        instance->_handleStatus((const MsgStatus *)data);
    } else if (first == CMD_CFG_ACK && (size_t)len >= sizeof(MsgCfgAck)) {
        instance->_handleCfgAck((const MsgCfgAck *)data);
    } else if (first == CMD_HELLO && (size_t)len >= sizeof(MsgHello)) {
        instance->_handleHello((const MsgHello *)data);
    }
}

inline void EspNowRx::_handleData(const MsgData *msg)
{
    _pktsRx++;
    /* Verificar CRC */
    uint8_t expected = _calcCrc((const uint8_t *)msg, sizeof(MsgData) - 1);
    if (msg->crc != expected) { _crcFail++; return; }

    uint8_t nid = msg->node_id;
    if (nid == 0 || nid >= MAX_NODES) return;

    ReassembledBatch &buf = _bufs[nid];

    /* Reset si es nuevo seq */
    if (buf.seq != msg->seq) {
        buf = {};
        buf.node_id      = nid;
        buf.seq          = msg->seq;
        buf.timestamp_us = msg->timestamp_us;
    }

    uint8_t part = msg->part;
    if (part > 1) return;
    if (!buf.part_rx[part]) {
        int base = part * SAMPLES_PER_PART;
        memcpy(&buf.samples[base], msg->samples, sizeof(msg->samples));
        buf.part_rx[part] = true;
    }

    if (buf.complete() && _cb) {
        _cb(buf);
        buf = {};   /* limpiar para siguiente batch */
    }
}

inline void EspNowRx::_handleArmAck(const MsgArmAck *msg)
{
    MASTER_LOG_PRINTF("[ESPNOW-RX] ARM_ACK node=%d status=%d\n",
                      msg->node_id, msg->status);
    if (_armAckCb) { _armAckCb(*msg); }
}

inline void EspNowRx::_handleStatus(const MsgStatus *msg)
{
    MASTER_LOG_PRINTF("[ESPNOW-RX] STATUS node=%d bOK=%u bBad=%u txOK=%u txFail=%u\n",
                      msg->node_id, msg->batches_ok, msg->batches_bad,
                      msg->espnow_sent, msg->espnow_fail);
    if (_statusCb) { _statusCb(*msg); }
}

inline void EspNowRx::_handleCfgAck(const MsgCfgAck *msg)
{
    MASTER_LOG_PRINTF("[ESPNOW-RX] CFG_ACK node=%d sub=0x%02X ok=%d\n",
                      msg->node_id, msg->sub_cmd, msg->ok);
    if (_cfgAckCb) { _cfgAckCb(*msg); }
}

inline void EspNowRx::_handleHello(const MsgHello *msg)
{
    MASTER_LOG_PRINTF("[ESPNOW-RX] HELLO node=%d\n", msg->node_id);
    if (_helloCb) { _helloCb(*msg); }
}
