#include "psoc_uart.h"

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
    _ser->begin(PSOC_UART_BAUD, SERIAL_8N1, PSOC_UART_RX, PSOC_UART_TX);
#endif
    _idx = 0;
}

void PsocUART::poll()
{
    if (_ser == nullptr) { return; }

    while (_ser->available() > 0) {
        uint8_t b = (uint8_t)_ser->read();

        if (_idx == 0) {
            if (b != PSOC_FRAME_MARKER) { continue; }   /* sync al marcador */
        }
        _buf[_idx++] = b;

        /* Validar n=30 temprano para resincronizar rápido si hay basura */
        if (_idx == 2 && _buf[1] != SPI_BATCH_SAMPLES) {
            _idx = 0;
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
        s.gain_byte    = 0;
        s.sample_flags = 0;
    }

    _lastOK = true;
    _batchesOK++;
    if (_cb) { _cb(batch); }
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
void PsocUART::captureNow()            { _sendCmd1(PSOC_CMD_VIEW, 0); }
void PsocUART::setVdac(uint8_t v)      { _sendCmd1(PSOC_CMD_VDAC, v); }
void PsocUART::setPga(uint8_t code)    { _sendCmd1(PSOC_CMD_PGA, code); }
void PsocUART::setPgavdac(uint8_t code){ _sendCmd1(PSOC_CMD_PGAVDAC, code); }
void PsocUART::debugRamp(bool en)      { _sendCmd1(PSOC_CMD_DEBUG, en ? 1 : 0); }
