#include "psoc_uart.h"

#if !defined(ESP8266)
#include <driver/gpio.h>
#endif

int32_t PsocUART::_sign24(uint8_t b0, uint8_t b1, uint8_t b2)
{
    uint32_t val = (uint32_t)b0 | ((uint32_t)b1 << 8) | ((uint32_t)b2 << 16);
    return (val & 0x800000u) ? (int32_t)(val | 0xFF000000u) : (int32_t)val;
}

void PsocUART::begin(BatchCallback cb, HardwareSerial *serial)
{
    _cb = cb;
    if (serial != nullptr) {
        _ser = serial;
    } else {
#if defined(ESP8266)
        _ser = &Serial1;   /* ESP8266: Serial1 (TX GPIO2). RX limitado. */
#else
        _ser = &Serial2;   /* ESP32: Serial2 */
#endif
    }
#if defined(ESP8266)
    _ser->begin(PSOC_UART_BAUD);
#else
    _ser->setRxBufferSize(PSOC_UART_RX_BUFFER_SIZE);
    _ser->begin(PSOC_UART_BAUD, SERIAL_8N1, PSOC_UART_RX, PSOC_UART_TX);
    /* El TX actual del PSoC esta como open-drain-low. Esto no reemplaza al
     * pull-up externo a 3V3, pero ayuda a que la linea no quede flotando. */
    gpio_pulldown_dis((gpio_num_t)PSOC_UART_RX);
    gpio_pullup_en((gpio_num_t)PSOC_UART_RX);
#endif
    _idx = 0;
}

void PsocUART::onDiag(DiagCallback cb)
{
    _diagCb = cb;
}

void PsocUART::_noteRxByte(uint8_t b)
{
    _bytesRx++;
    _lastByte = b;
    _lastByteMs = millis();
    _lastByteSeen = true;
}

void PsocUART::poll()
{
    if (_ser == nullptr) { return; }

    while (_ser->available() > 0) {
        uint8_t b = (uint8_t)_ser->read();
        _noteRxByte(b);

        if (_idx == 0) {
            if (b != PSOC_FRAME_MARKER) {
                _syncDrops++;
                continue;
            }
            _markersRx++;
        }
        _buf[_idx++] = b;

        /* Validar n=30 temprano para resincronizar rápido si hay basura. */
        if (_idx == 2 && _buf[1] != SPI_BATCH_SAMPLES) {
            /* Ping del PSoC (0xAB 0xC0): responder con pong (0xAB 0xC1 0x00 0xC1) */
            if (_buf[1] == PSOC_CTRL_PING) {
                _pingsRx++;
                _lastOK = true;
                const uint8_t pong[4] = {
                    PSOC_FRAME_MARKER, PSOC_CTRL_PONG, 0x00u,
                    (uint8_t)(PSOC_CTRL_PONG ^ 0x00u)
                };
                _ser->write(pong, sizeof(pong));
            } else if (_buf[1] == PSOC_CTRL_CFG_ACK) {
                continue;   /* frame corto: [AB][C2][cmd][val][crc] */
            } else if (_buf[1] == PSOC_CTRL_FS_REPORT) {
                continue;   /* frame corto: [AB][C3][fs_lo][fs_hi][crc] */
            } else if (_buf[1] == PSOC_CTRL_DIAG_EVT) {
                continue;   /* frame corto: [AB][C4][event][val][state][crc] */
            } else {
                _badLen++;
            }
            _idx = 0;
            continue;
        }
        if (_idx >= PSOC_CTRL_ACK_BYTES && _buf[1] == PSOC_CTRL_CFG_ACK) {
            _idx = 0;
            _parseConfigAck();
            continue;
        }
        if (_idx >= PSOC_CTRL_ACK_BYTES && _buf[1] == PSOC_CTRL_FS_REPORT) {
            _idx = 0;
            _parseFsReport();
            continue;
        }
        if (_idx >= PSOC_CTRL_DIAG_BYTES && _buf[1] == PSOC_CTRL_DIAG_EVT) {
            _idx = 0;
            _parseDiagEvent();
            continue;
        }
        if (_idx >= PSOC_FRAME_BYTES) {
            _idx = 0;
            _parseFrame();
        }
    }
}

void PsocUART::_parseFrame()
{
    uint8_t crc = 0;
    for (int i = 0; i < PSOC_FRAME_BYTES - 1; i++) { crc ^= _buf[i]; }
    if (crc != _buf[PSOC_FRAME_BYTES - 1]) {
        _lastOK = false;
        _batchesBad++;
        return;
    }

    PsocBatch batch;
    batch.seq          = (uint16_t)(_buf[2] | (_buf[3] << 8));
    batch.global_flags = 0;
    batch.n_samples    = SPI_BATCH_SAMPLES;
    batch.timestamp_us = (uint64_t)micros();

    const uint8_t *p = &_buf[4];
    for (int i = 0; i < SPI_BATCH_SAMPLES; i++, p += 3) {
        PsocSample &s  = batch.samples[i];
        int32_t raw    = _sign24(p[0], p[1], p[2]);
        s.post_digital = raw;                         /* RAW 24-bit */
        s.raw_input    = (int16_t)(raw & 0xFFFF);
        s.post_analog  = 0;
        s.gain_byte    = _confirmedPga;
        s.sample_flags = 0;
    }

    _lastOK = true;
    _batchesOK++;
    if (_cb) { _cb(batch); }
}

void PsocUART::_parseConfigAck()
{
    const uint8_t cmd = _buf[2];
    const uint8_t val = _buf[3];
    const uint8_t crc = _buf[4];
    if (crc != (uint8_t)(PSOC_CTRL_CFG_ACK ^ cmd ^ val)) {
        _lastOK = false;
        _badLen++;
        return;
    }

    _lastOK = true;
    _configAcksRx++;
    _cfgAckCmd = cmd;
    _cfgAckVal = val;
    _cfgAckPending = true;

    if (cmd == PSOC_CMD_PGA) {
        _confirmedPga = val;
    } else if (cmd == PSOC_CMD_PGAVDAC) {
        _confirmedPgavdac = val;
    } else if (cmd == PSOC_CMD_VDAC) {
        _confirmedVdac = val;
    }
}

bool PsocUART::takeConfigAck(uint8_t &cmd, uint8_t &val)
{
    if (!_cfgAckPending) { return false; }
    cmd = _cfgAckCmd;
    val = _cfgAckVal;
    _cfgAckPending = false;
    return true;
}

void PsocUART::_sendCmd1(uint8_t cmd, uint8_t p)
{
    if (_ser == nullptr) { return; }
    uint8_t f[4] = { PSOC_FRAME_MARKER, cmd, p, (uint8_t)(cmd ^ p) };
    _ser->write(f, 4);
}

void PsocUART::_sendCmd2(uint8_t cmd, uint8_t p1, uint8_t p2)
{
    if (_ser == nullptr) { return; }
    uint8_t f[5] = { PSOC_FRAME_MARKER, cmd, p1, p2, (uint8_t)(cmd ^ p1 ^ p2) };
    _ser->write(f, 5);
}

void PsocUART::setN(uint16_t n)        { _sendCmd2(PSOC_CMD_SETN, (uint8_t)(n & 0xFF), (uint8_t)(n >> 8)); }
void PsocUART::preStart()              { _sendCmd1(PSOC_CMD_PRESTART, 0); }
void PsocUART::startNow()              { _sendCmd1(PSOC_CMD_START_NOW, 0); }
void PsocUART::setVdac(uint8_t v)      { _sendCmd1(PSOC_CMD_VDAC, v); }
void PsocUART::setPga(uint8_t code)    { _sendCmd1(PSOC_CMD_PGA, code); }
void PsocUART::setPgavdac(uint8_t code){ _sendCmd1(PSOC_CMD_PGAVDAC, code); }
void PsocUART::calibrate()             { _sendCmd1(PSOC_CMD_CALIBRATE, 1); }
void PsocUART::saveEeprom()            { _sendCmd1(PSOC_CMD_SAVE_EEPROM, 0); }
void PsocUART::selectStream(uint8_t m) { _sendCmd1(PSOC_CMD_SELECT_STREAM, m); }
void PsocUART::adcSnapshot()           { _sendCmd1(PSOC_CMD_ADC_SNAPSHOT, 1); }
void PsocUART::debugRamp(bool en)      { _sendCmd1(PSOC_CMD_DEBUG, en ? 1 : 0); }
void PsocUART::requestStatus()         { _sendCmd1(PSOC_CMD_STATUS, 0); }
void PsocUART::sendPong()              { _sendCmd1(PSOC_CTRL_PONG, 0); }

void PsocUART::_parseFsReport()
{
    const uint8_t fs_lo = _buf[2];
    const uint8_t fs_hi = _buf[3];
    const uint8_t crc   = _buf[4];
    if (crc != (uint8_t)(PSOC_CTRL_FS_REPORT ^ fs_lo ^ fs_hi)) {
        _badLen++;
        return;
    }
    const uint16_t fs = (uint16_t)fs_lo | ((uint16_t)fs_hi << 8);
    if (fs > 0) { _sampleRate = fs; }
}

void PsocUART::_parseDiagEvent()
{
    const uint8_t event = _buf[2];
    const uint8_t value = _buf[3];
    const uint8_t psocState = _buf[4];
    const uint8_t crc = _buf[5];
    if (crc != (uint8_t)(PSOC_CTRL_DIAG_EVT ^ event ^ value ^ psocState)) {
        _lastOK = false;
        _badLen++;
        return;
    }

    _lastOK = true;
    _diagEventsRx++;
    if (_diagCb) {
        PsocDiagEvent ev = { event, value, psocState, millis() };
        _diagCb(ev);
    }
}

bool PsocUART::probe(uint32_t timeoutMs)
{
    if (_ser == nullptr) return false;

    const uint32_t startPings = _pingsRx;
    const uint32_t startBatches = _batchesOK;
    const uint32_t startDiag = _diagEventsRx;

    while (_ser->available() > 0) {
        poll();   /* no descartar pings pendientes: poll() responde el pong */
    }
    if (_pingsRx != startPings || _batchesOK != startBatches || _diagEventsRx != startDiag) {
        return true;
    }

    /* Fase 1: probe activo no invasivo. En firmware PSoC nuevo, 0xA5 responde
     * con un ping 0xC0 y poll() contesta el pong 0xC1. Esto cubre el caso en que
     * el ESP reinicia mientras el PSoC ya estaba conectado e idle. */
    uint32_t t0 = millis();
    uint32_t lastTxMs = 0;
    const uint32_t activeWindowMs = (timeoutMs > 90u) ? ((timeoutMs * 2u) / 3u) : timeoutMs;
    while ((millis() - t0) < activeWindowMs) {
        const uint32_t now = millis();
        if (lastTxMs == 0u || (now - lastTxMs) >= 25u) {
            sendPong();       /* Destraba PSoC viejo que sigue esperando PONG. */
            requestStatus();
            lastTxMs = now;
        }
        poll();
        if (_pingsRx != startPings || _batchesOK != startBatches || _diagEventsRx != startDiag) {
            return true;
        }
        delay(1);
    }

    /* Fase 2: fallback legacy — activar rampa de debug para provocar respuesta
     * en PSoC viejos que aún no respondan al 0xA5. */
    debugRamp(true);
    t0 = millis();
    bool got = false;
    const uint32_t fallbackMs = (timeoutMs > activeWindowMs)
                              ? (timeoutMs - activeWindowMs)
                              : timeoutMs;
    while ((millis() - t0) < fallbackMs) {
        poll();
        if (_pingsRx != startPings || _batchesOK != startBatches || _diagEventsRx != startDiag) {
            got = true;
            break;
        }
        delay(1);
    }
    debugRamp(false);
    delay(20);
    t0 = millis();
    while ((millis() - t0) < 20u) {
        poll();
        if (_pingsRx != startPings || _batchesOK != startBatches || _diagEventsRx != startDiag) {
            got = true;
        }
        delay(1);
    }
    _idx = 0;
    return got;
}
