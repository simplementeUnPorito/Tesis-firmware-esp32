#pragma once
/*
 * espnow_rx.h — Recepción ESP-NOW de batches de los esclavos.
 *
 * Los paquetes MsgData llegan en fragmentos (part 0/1).
 * Se reensamblan aquí y se notifican como batch completo.
 */

#include <Arduino.h>
#include <esp_now.h>
#include <esp_arduino_version.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "sync_protocol.h"
#include "master_log.h"

/* Unica salida ESP-NOW del maestro: agrega la marca de red (sync_protocol.h). */
static inline esp_err_t geonetSend(const uint8_t *dst, const uint8_t *data, size_t len)
{
    uint8_t buf[ESPNOW_MAX_PAYLOAD];
    const int n = geonetWrap(buf, data, (int)len);
    if (n == 0) return ESP_ERR_INVALID_ARG;
    return esp_now_send(dst, buf, (size_t)n);
}

#define MAX_NODES 9

/* Un batch reensamblado */
struct ReassembledBatch {
    uint8_t     node_id;
    uint16_t    seq;
    uint8_t     global_flags;
    uint64_t    timestamp_us;
    SampleBytes samples[SAMPLES_PER_PART * 2];  /* 30 muestras */
    bool        active;
    bool        part_rx[2];
    bool        complete() const { return part_rx[0] && part_rx[1]; }
};

typedef void (*BatchReadyCallback)(const ReassembledBatch &batch);
typedef void (*ArmAckCallback)(const MsgArmAck &msg);
typedef void (*StatusCallback)(const MsgStatus &msg);
typedef void (*CfgAckCallback)(const MsgCfgAck &msg);
typedef void (*HelloCallback)(const MsgHello &msg, const uint8_t senderMac[6]);
typedef void (*StartAckCallback)(const MsgStartAck &msg);
typedef void (*HotWaitAckCallback)(const MsgHotWaitAck &msg);
typedef void (*RtDataCallback)(const uint8_t *data, int len);
typedef void (*RtEndCallback)(const MsgStreamEnd &msg);
typedef void (*OtaStatusCallback)(const MsgOtaStatus &msg);
typedef void (*FtmReportCallback)(const MsgFtmReport &msg);

class EspNowRx {
public:
    static EspNowRx *instance;

    bool begin(BatchReadyCallback cb,
               ArmAckCallback armAckCb = nullptr,
               StatusCallback statusCb = nullptr,
               CfgAckCallback cfgAckCb = nullptr,
               HelloCallback helloCb = nullptr,
               StartAckCallback startAckCb = nullptr,
               HotWaitAckCallback hotWaitAckCb = nullptr);

    /* Llamar desde el loop() o directamente del callback ESP-NOW */
    static void onRecv(const uint8_t *mac, const uint8_t *data, int len);
    static void onSent(const uint8_t *mac, esp_now_send_status_t status);

    /* Tiempo real (rt_stream.h): tramas MsgData32 y STREAM_END. */
    void setOtaHandler(OtaStatusCallback cb) { _otaCb = cb; }
    void setFtmHandler(FtmReportCallback cb) { _ftmCb = cb; }
    bool enqueueBatch(const ReassembledBatch &batch);
    void service();
    void setRtHandlers(RtDataCallback data, RtEndCallback end)
    {
        _rtDataCb = data;
        _rtEndCb = end;
    }

    uint32_t pktsRx()   const { return _pktsRx; }
    uint32_t crcFail()  const { return _crcFail; }
    uint32_t foreignRx() const { return _foreignRx; }  /* tramas sin marca de red */

private:
    BatchReadyCallback _cb = nullptr;
    ArmAckCallback     _armAckCb  = nullptr;
    StatusCallback     _statusCb  = nullptr;
    CfgAckCallback     _cfgAckCb  = nullptr;
    HelloCallback      _helloCb   = nullptr;
    StartAckCallback   _startAckCb = nullptr;
    HotWaitAckCallback _hotWaitAckCb = nullptr;
    RtDataCallback     _rtDataCb = nullptr;
    RtEndCallback      _rtEndCb = nullptr;
    OtaStatusCallback  _otaCb = nullptr;
    FtmReportCallback  _ftmCb = nullptr;
    ReassembledBatch   _bufs[MAX_NODES] = {};
    QueueHandle_t       _batchQueue = nullptr;
    uint32_t           _pktsRx  = 0;
    uint32_t           _crcFail = 0;
    volatile uint32_t  _foreignRx = 0;

    void _handleData(const MsgData *msg);
    void _handleStatus(const MsgStatus *msg);
    void _handleArmAck(const MsgArmAck *msg);
    void _handleCfgAck(const MsgCfgAck *msg);
    void _handleHello(const MsgHello *msg, const uint8_t senderMac[6]);
    void _handleStartAck(const MsgStartAck *msg);
    void _handleHotWaitAck(const MsgHotWaitAck *msg);

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
                            HelloCallback helloCb,
                            StartAckCallback startAckCb,
                            HotWaitAckCallback hotWaitAckCb)
{
    _cb = cb;
    _armAckCb = armAckCb;
    _statusCb = statusCb;
    _cfgAckCb = cfgAckCb;
    _helloCb  = helloCb;
    _startAckCb = startAckCb;
    _hotWaitAckCb = hotWaitAckCb;
    instance = this;
    if (!_batchQueue) _batchQueue = xQueueCreate(4, sizeof(ReassembledBatch));
    if (!_batchQueue) {
        MASTER_LOG_PRINTLN("[ESPNOW-RX] cola de lotes sin memoria");
        return false;
    }
    if (esp_now_init() != ESP_OK) {
        MASTER_LOG_PRINTLN("[ESPNOW-RX] init failed");
        return false;
    }
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    /* IDF 5: el callback recibe esp_now_recv_info_t. */
    esp_now_register_recv_cb([](const esp_now_recv_info_t *info, const uint8_t *data, int len) {
        onRecv(info->src_addr, data, len);
    });
#else
    esp_now_register_recv_cb(onRecv);
#endif
#if defined(ESP_IDF_VERSION) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
    /* IDF 5.5: el aviso de envio recibe wifi_tx_info_t. */
    esp_now_register_send_cb([](const wifi_tx_info_t *info, esp_now_send_status_t st) {
        onSent(info ? info->des_addr : nullptr, st);
    });
#else
    esp_now_register_send_cb(onSent);
#endif
    MASTER_LOG_PRINTLN("[ESPNOW-RX] ready");
    return true;
}

inline bool EspNowRx::enqueueBatch(const ReassembledBatch &batch)
{
    return _batchQueue && xQueueSend(_batchQueue, &batch, 0) == pdTRUE;
}

inline void EspNowRx::service()
{
    if (!_batchQueue || !_cb) return;
    ReassembledBatch batch;
    for (uint8_t i = 0; i < 4 && xQueueReceive(_batchQueue, &batch, 0) == pdTRUE; i++) {
        _cb(batch);
    }
}

/* Unicast confirmados (ACK de MAC del destino) y fallidos: diagnostico. */
inline volatile uint32_t &espnowTxAckOk()   { static volatile uint32_t n = 0; return n; }
inline volatile uint32_t &espnowTxAckFail() { static volatile uint32_t n = 0; return n; }
inline void EspNowRx::onSent(const uint8_t *, esp_now_send_status_t status)
{
    if (status == ESP_NOW_SEND_SUCCESS) espnowTxAckOk()++;
    else espnowTxAckFail()++;
}

inline void EspNowRx::onRecv(const uint8_t *mac, const uint8_t *data, int len)
{
    if (!instance || len < 1) return;
    /* Solo tramas de esta red (marca al final, sync_protocol.h). */
    if (!geonetTagged(data, len)) { instance->_foreignRx++; return; }
    len -= GEONET_TAG_LEN;
    uint8_t first = data[0];

    if (first == DATA_PKT_MARKER && (size_t)len >= sizeof(MsgData)) {
        instance->_handleData((const MsgData *)data);
    } else if (first == DATAZ_PKT_MARKER && len >= MSG_DATAZ_HEADER) {
        if (instance->_rtDataCb) instance->_rtDataCb(data, len);
    } else if (first == DATA32_PKT_MARKER && (size_t)len >= sizeof(MsgData32)) {
        if (instance->_rtDataCb) instance->_rtDataCb(data, len);
    } else if (first == CMD_STREAM_END && (size_t)len >= sizeof(MsgStreamEnd)) {
        if (instance->_rtEndCb) instance->_rtEndCb(*(const MsgStreamEnd *)data);
    } else if (first == CMD_ARM_ACK && (size_t)len >= sizeof(MsgArmAck)) {
        instance->_handleArmAck((const MsgArmAck *)data);
    } else if (first == CMD_STATUS && (size_t)len >= sizeof(MsgStatus)) {
        instance->_handleStatus((const MsgStatus *)data);
    } else if (first == CMD_CFG_ACK && (size_t)len >= sizeof(MsgCfgAck)) {
        instance->_handleCfgAck((const MsgCfgAck *)data);
    } else if (first == CMD_HELLO && (size_t)len >= MSG_HELLO_LEGACY_BYTES) {
        MsgHello hello = {};
        hello.hw_class = SLAVE_HW_UNKNOWN;
        hello.link_model = SLAVE_LINK_UNKNOWN;
        hello.ctl_status = SLAVE_CTL_UNKNOWN;
        hello.pga_code = 0xFFu;
        hello.pgaout_code = 0xFFu;
        hello.reset_reason = 0xFFu;   /* esclavos viejos no lo mandan */
        const size_t copyLen = ((size_t)len < sizeof(MsgHello)) ? (size_t)len : sizeof(MsgHello);
        memcpy(&hello, data, copyLen);
        instance->_handleHello(&hello, mac);
    } else if (first == CMD_START_ACK && (size_t)len >= sizeof(MsgStartAck)) {
        instance->_handleStartAck((const MsgStartAck *)data);
    } else if (first == CMD_OTA_STATUS && (size_t)len >= sizeof(MsgOtaStatus)) {
        if (instance->_otaCb) instance->_otaCb(*(const MsgOtaStatus *)data);
    } else if (first == CMD_FTM_REPORT && (size_t)len >= offsetof(MsgFtmReport, init_err)) {
        /* Esclavos anteriores no mandan los campos de error: quedan en cero. */
        MsgFtmReport r = {};
        r.ev_status = 0xFFu;
        memcpy(&r, data, ((size_t)len < sizeof(r)) ? (size_t)len : sizeof(r));
        if (instance->_ftmCb) instance->_ftmCb(r);
    } else if (first == CMD_SLAVE_LOG && len >= 3) {
        const MsgSlaveLog *l = (const MsgSlaveLog *)data;
        int n = l->len;
        if (n > len - 3) n = len - 3;
        /* Partido en lineas: cada una con el nodo adelante. */
        int start = 0;
        for (int i = 0; i <= n; i++) {
            if (i == n || l->text[i] == '\n') {
                if (i > start) MASTER_LOG_PRINTF("[S%u>] %.*s\n", (unsigned)l->node_id, i - start, &l->text[start]);
                start = i + 1;
            }
        }
    } else if (first == CMD_HOTWAIT_ACK && (size_t)len >= offsetof(MsgHotWaitAck, reason)) {
        MsgHotWaitAck a = {};   /* esclavos viejos no mandan el motivo */
        memcpy(&a, data, ((size_t)len < sizeof(a)) ? (size_t)len : sizeof(a));
        instance->_handleHotWaitAck(&a);
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

    /* Reset si es un batch nuevo. Importante: seq puede ser 0, igual que
       el valor por defecto del buffer limpio, asi que no alcanza con seq. */
    if (!buf.active || buf.node_id != nid || buf.seq != msg->seq) {
        buf = {};
        buf.active       = true;
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
        /* El callback WiFi solo copia. LittleFS y WebSocket corren en loop(). */
        enqueueBatch(buf);
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

inline void EspNowRx::_handleHello(const MsgHello *msg, const uint8_t senderMac[6])
{
    MASTER_LOG_PRINTF("[ESPNOW-RX] HELLO node=%d mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
                      msg->node_id,
                      senderMac[0], senderMac[1], senderMac[2],
                      senderMac[3], senderMac[4], senderMac[5]);
    if (_helloCb) { _helloCb(*msg, senderMac); }
}

inline void EspNowRx::_handleStartAck(const MsgStartAck *msg)
{
    MASTER_LOG_PRINTF("[ESPNOW-RX] START_ACK node=%d status=%d token=%u rx_us=%u\n",
                      msg->node_id, msg->status,
                      (unsigned)msg->start_token, (unsigned)msg->rx_us);
    if (_startAckCb) { _startAckCb(*msg); }
}

inline void EspNowRx::_handleHotWaitAck(const MsgHotWaitAck *msg)
{
    MASTER_LOG_PRINTF("[ESPNOW-RX] HOTWAIT_ACK node=%d ok=%d state=%d n=%u motivo=%u\n",
                      msg->node_id, msg->ok, msg->state, msg->n_batches, msg->reason);
    if (_hotWaitAckCb) { _hotWaitAckCb(*msg); }
}
