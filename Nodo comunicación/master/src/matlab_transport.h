#pragma once
/*
 * matlab_transport.h — Transporte Serial USB para comunicación con MATLAB.
 *
 * El ESP maestro se conecta al PC de MATLAB por USB (COM port).
 * El WiFi del maestro queda libre para WiFi AP + ESP-NOW con los esclavos.
 *
 * Protocolo RX — dos formatos:
 *
 *   Comando estándar (4 bytes):
 *     [0xAB][cmd][param][cmd^param]
 *     cmd 0xA1: stream on/off   param: 0/1
 *     cmd 0xA2: arm nodes       param: n_nodes esperados
 *     cmd 0xA4: stop sampling   param: ignorado
 *     cmd 0xA5: request status  param: ignorado
 *     cmd 0xA7: debug broadcast param: 0/1
 *     cmd 0xB0: scope multi-start param: n_batches, 0=cancelar
 *
 *   Comando "set N" 16 bits (5 bytes):
 *     [0xAB][cmd][n_lo][n_hi][cmd^n_lo^n_hi]
 *     cmd 0xA3: start sampling  N = n_lo|n_hi<<8 (lotes, 0=usar actual)
 *     cmd 0xAE: set record len  N = n_lo|n_hi<<8
 *
 *   Comando dirigido a esclavo (6 bytes):
 *     [0xAB][0xBD][node_id][sub_cmd][param][node_id^sub_cmd^param]
 *     sub_cmd 0xA6: set PGA gain    param: código 0-8
 *     sub_cmd 0xA9: set PGAvdac     param: código 0-8
 *     sub_cmd 0xAA: set VDAC byte   param: 0-255
 *     sub_cmd 0xA7: debug node ESP  param: 0/1
 *     sub_cmd 0xB2: ver (captura única) param: ignorado
 *     sub_cmd 0xB3: debug PSoC rampa param: 0/1
 *
 * Protocolo TX (6 bytes por paquete):
 *   [0x56][node_id][type][b2][b1][b0]
 *   node_id : 0x00 = maestro (martillo), 0x01-0x0N = esclavos
 *   type    : 0x00 dato, 0x01 heartbeat, 0x07 ACK, 0xFC START RTT/2 us,
 *             0xFE ready (n_nodes)
 */

#include <Arduino.h>
#include "sync_protocol.h"
#include "master_log.h"

#define MATLAB_SERIAL_BAUD 921600
#define MATLAB_PKT_HEADER  0x56u
#define MATLAB_CMD_DIRECTED 0xBDu

class MatlabTransport {
public:
    /* Inicializar Serial USB */
    void begin(uint32_t baud = MATLAB_SERIAL_BAUD);
    /* Llamar desde loop() para leer comandos entrantes */
    void loop();

    void sendSample(uint8_t nodeId, int32_t value24);
    void sendHeartbeat(uint8_t nodeId, uint8_t pga, uint8_t vdac, uint8_t mode);
    void sendAck(uint8_t nodeId, uint8_t cmd, uint8_t val);
    void sendReady(uint8_t nNodes);
    void sendStartLatency(uint8_t nodeId, uint32_t tofUs);
    /* Diagnóstico: estado ESP-NOW del maestro al arrancar */
    void sendStatus(uint8_t espnow_ok, uint8_t ap_ch);
    /* Diagnóstico: maestro recibió HELLO de un esclavo (incluye estado PSoC + MAC) */
    void sendHelloNotif(uint8_t nodeId, uint8_t psoc_ok = 0, const uint8_t mac[6] = nullptr);

    /* Serial USB siempre disponible */
    bool connected() const { return true; }

    struct RxCmd {
        uint8_t  cmd;      /* 0xA1-0xA7: estándar | 0xBD: dirigido */
        uint8_t  param;    /* parámetro del comando estándar (= value & 0xFF) */
        uint8_t  node_id;  /* válido solo cuando cmd == 0xBD */
        uint8_t  sub_cmd;  /* válido solo cuando cmd == 0xBD */
        uint16_t value;    /* valor 16 bits para 0xA3/0xAE (set N) */
        bool     valid;
    };
    /* Drain one command from the queue (returns {valid=false} when empty) */
    RxCmd lastCmd() {
        if (_cmdHead == _cmdTail) return {0,0,0,0,0,false};
        RxCmd r = _cmdQueue[_cmdTail];
        _cmdTail = (uint8_t)((_cmdTail + 1) % CMD_Q_SIZE);
        return r;
    }
    bool hasCmd() const { return _cmdHead != _cmdTail; }

private:
    static constexpr uint8_t CMD_Q_SIZE = 8;
    RxCmd   _cmdQueue[CMD_Q_SIZE];
    uint8_t _cmdHead    = 0;
    uint8_t _cmdTail    = 0;
    uint8_t _rxBuf[6];
    uint8_t _rxIdx      = 0;
    uint8_t _rxExpected = 4;

    void _write6(uint8_t nodeId, uint8_t type, int32_t val24);
    void _parseRx();
    void _pushCmd(const RxCmd &c) {
        uint8_t next = (uint8_t)((_cmdHead + 1) % CMD_Q_SIZE);
        if (next != _cmdTail) { _cmdQueue[_cmdHead] = c; _cmdHead = next; }
    }
};

/* ── Implementación inline ─────────────────────────────────────────────── */

inline void MatlabTransport::begin(uint32_t baud)
{
    Serial.begin(baud);
    /* Esperar a que el host enumere el USB (hasta 2 s) */
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 2000) {}
    MASTER_LOG_PRINTLN("[SERIAL] MATLAB transport listo");
}

inline void MatlabTransport::loop()
{
    while (Serial.available()) {
        uint8_t b = (uint8_t)Serial.read();
        if (_rxIdx == 0 && b != 0xAB) continue;  /* esperar sync */
        _rxBuf[_rxIdx++] = b;
        if (_rxIdx == 2) {
            uint8_t c = _rxBuf[1];
            if (c == MATLAB_CMD_DIRECTED)            _rxExpected = 6;
            else if (c == 0xA3 || c == 0xAE)         _rxExpected = 5; /* set N 16 bits */
            else                                     _rxExpected = 4;
        }
        if (_rxIdx >= _rxExpected) {
            _parseRx();
            _rxIdx      = 0;
            _rxExpected = 4;
        }
    }
}

inline void MatlabTransport::_write6(uint8_t nodeId, uint8_t type, int32_t val24)
{
    uint8_t pkt[6] = {
        MATLAB_PKT_HEADER,
        nodeId,
        type,
        (uint8_t)((val24 >> 16) & 0xFF),
        (uint8_t)((val24 >>  8) & 0xFF),
        (uint8_t)( val24        & 0xFF)
    };
    Serial.write(pkt, 6);
}

inline void MatlabTransport::sendSample(uint8_t nodeId, int32_t value24)
{
    _write6(nodeId, 0x00, value24);
}

inline void MatlabTransport::sendHeartbeat(uint8_t nodeId, uint8_t pga,
                                            uint8_t vdac, uint8_t mode)
{
    uint8_t pkt[6] = { MATLAB_PKT_HEADER, nodeId, 0x01, pga, vdac, mode };
    Serial.write(pkt, 6);
}

inline void MatlabTransport::sendAck(uint8_t nodeId, uint8_t cmd, uint8_t val)
{
    uint8_t pkt[6] = { MATLAB_PKT_HEADER, nodeId, 0x07, cmd, val, 0x00 };
    Serial.write(pkt, 6);
}

inline void MatlabTransport::sendReady(uint8_t nNodes)
{
    uint8_t pkt[6] = { MATLAB_PKT_HEADER, 0xFF, 0xFE, nNodes, 0x00, 0x00 };
    Serial.write(pkt, 6);
}

inline void MatlabTransport::sendStartLatency(uint8_t nodeId, uint32_t tofUs)
{
    if (tofUs > 0xFFFFFFu) tofUs = 0xFFFFFFu;
    _write6(nodeId, 0xFC, (int32_t)tofUs);
}

inline void MatlabTransport::sendStatus(uint8_t espnow_ok, uint8_t ap_ch)
{
    /* node_id=0xFF, type=0xFD, b2=espnow_ok, b1=ap_ch, b0=0 */
    uint8_t pkt[6] = { MATLAB_PKT_HEADER, 0xFF, 0xFD, espnow_ok, ap_ch, 0x00 };
    Serial.write(pkt, 6);
}

inline void MatlabTransport::sendHelloNotif(uint8_t nodeId, uint8_t psoc_ok, const uint8_t mac[6])
{
    /* node_id=slave, type=0xFD, b2=0x01 (hello), b1=psoc_ok, b0=0 */
    uint8_t pkt[6] = { MATLAB_PKT_HEADER, nodeId, 0xFD, 0x01, psoc_ok, 0x00 };
    Serial.write(pkt, 6);
    if (mac != nullptr) {
        /* Enviar MAC en 3 paquetes: sub-tipo 0x02/0x03/0x04, 2 bytes por paquete */
        uint8_t m0[6] = { MATLAB_PKT_HEADER, nodeId, 0xFD, 0x02, mac[0], mac[1] };
        uint8_t m1[6] = { MATLAB_PKT_HEADER, nodeId, 0xFD, 0x03, mac[2], mac[3] };
        uint8_t m2[6] = { MATLAB_PKT_HEADER, nodeId, 0xFD, 0x04, mac[4], mac[5] };
        Serial.write(m0, 6);
        Serial.write(m1, 6);
        Serial.write(m2, 6);
    }
}

inline void MatlabTransport::_parseRx()
{
    if (_rxBuf[0] != 0xAB) return;

    if (_rxExpected == 6) {
        /* Comando dirigido: [0xAB][0xBD][node_id][sub_cmd][param][cs] */
        uint8_t node_id = _rxBuf[2];
        uint8_t sub_cmd = _rxBuf[3];
        uint8_t param   = _rxBuf[4];
        uint8_t cs      = _rxBuf[5];
        if (cs != (uint8_t)(node_id ^ sub_cmd ^ param)) return;
        _pushCmd({ MATLAB_CMD_DIRECTED, param, node_id, sub_cmd, param, true });
    } else if (_rxExpected == 5) {
        /* set N (16 bits): [0xAB][cmd][n_lo][n_hi][cmd^n_lo^n_hi] */
        uint8_t cmd  = _rxBuf[1];
        uint8_t nlo  = _rxBuf[2];
        uint8_t nhi  = _rxBuf[3];
        uint8_t cs   = _rxBuf[4];
        if (cs != (uint8_t)(cmd ^ nlo ^ nhi)) return;
        uint16_t val = (uint16_t)nlo | ((uint16_t)nhi << 8);
        _pushCmd({ cmd, nlo, 0, 0, val, true });
    } else {
        /* Comando estándar: [0xAB][cmd][param][cs] */
        uint8_t cmd   = _rxBuf[1];
        uint8_t param = _rxBuf[2];
        uint8_t cs    = _rxBuf[3];
        if (cs != (uint8_t)(cmd ^ param)) return;
        _pushCmd({ cmd, param, 0, 0, param, true });
    }
}
