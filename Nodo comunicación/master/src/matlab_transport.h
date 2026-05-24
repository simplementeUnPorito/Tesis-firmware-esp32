#pragma once
/*
 * matlab_transport.h — Transporte Serial USB para comunicación con MATLAB.
 *
 * El ESP maestro se conecta al PC de MATLAB por USB (COM port).
 * El WiFi del maestro queda libre para WiFi AP + ESP-NOW con los esclavos.
 *
 * Protocolo (6 bytes por muestra) — idéntico al anterior:
 *   [0x56][node_id][type][b2][b1][b0]
 *   node_id : 0x00 = maestro (martillo), 0x01-0x0N = esclavos (geófonos)
 *   type    : 0x00 dato, 0x01 heartbeat, 0x07 ACK, 0xFE ready (n_nodes)
 *
 * Comandos MATLAB → maestro (4 bytes + checksum XOR):
 *   [0xAB][cmd][param][cmd^param]
 *   cmd 0xA1: stream on/off   param: 0/1
 *   cmd 0xA2: arm nodes       param: n_nodes esperados
 *   cmd 0xA3: start sampling  param: ignorado
 *   cmd 0xA4: stop sampling   param: ignorado
 *   cmd 0xA5: request status  param: ignorado
 */

#include <Arduino.h>
#include "sync_protocol.h"

#define MATLAB_SERIAL_BAUD 115200
#define MATLAB_PKT_HEADER  0x56u

class MatlabTransport {
public:
    /* Inicializar Serial USB */
    void begin(uint32_t baud = MATLAB_SERIAL_BAUD);
    /* Llamar desde loop() para leer comandos entrantes */
    void loop();

    void sendSample(uint8_t nodeId, int32_t value24);
    void sendHammer(int32_t value24);
    void sendHeartbeat(uint8_t nodeId, uint8_t pga, uint8_t vdac, uint8_t mode);
    void sendAck(uint8_t nodeId, uint8_t cmd, uint8_t val);
    void sendReady(uint8_t nNodes);

    /* Serial USB siempre disponible */
    bool connected() const { return true; }

    struct RxCmd { uint8_t cmd; uint8_t param; bool valid; };
    RxCmd lastCmd() { RxCmd r = _lastCmd; _lastCmd.valid = false; return r; }

private:
    RxCmd   _lastCmd = {0, 0, false};
    uint8_t _rxBuf[4];
    uint8_t _rxIdx = 0;

    void _write6(uint8_t nodeId, uint8_t type, int32_t val24);
    void _parseRx();
};

/* ── Implementación inline ─────────────────────────────────────────────── */

inline void MatlabTransport::begin(uint32_t baud)
{
    Serial.begin(baud);
    /* Esperar a que el host enumere el USB (hasta 2 s) */
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 2000) {}
    Serial.println("[SERIAL] MATLAB transport listo");
}

inline void MatlabTransport::loop()
{
    while (Serial.available()) {
        _rxBuf[_rxIdx++] = (uint8_t)Serial.read();
        if (_rxIdx >= 4) { _parseRx(); _rxIdx = 0; }
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

inline void MatlabTransport::sendHammer(int32_t value24)
{
    _write6(0x00, 0x00, value24);
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

inline void MatlabTransport::_parseRx()
{
    if (_rxBuf[0] != 0xAB) return;
    uint8_t cmd   = _rxBuf[1];
    uint8_t param = _rxBuf[2];
    uint8_t cs    = _rxBuf[3];
    if (cs != (uint8_t)(cmd ^ param)) { return; }
    _lastCmd = { cmd, param, true };
}
