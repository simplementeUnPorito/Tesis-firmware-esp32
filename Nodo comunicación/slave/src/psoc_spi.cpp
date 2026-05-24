#include "psoc_spi.h"

int32_t PsocSPI::_sign24(uint8_t b0, uint8_t b1, uint8_t b2)
{
    uint32_t val = (uint32_t)b0 | ((uint32_t)b1 << 8) | ((uint32_t)b2 << 16);
    return (val & 0x800000u) ? (int32_t)(val | 0xFF000000u) : (int32_t)val;
}

void PsocSPI::begin(BatchCallback cb)
{
    _cb = cb;
    pinMode(PSOC_PIN_SS,   OUTPUT);
    pinMode(PSOC_PIN_DRDY, INPUT);
    digitalWrite(PSOC_PIN_SS, HIGH);
    SPI.begin();
}

void PsocSPI::poll()
{
    if (digitalRead(PSOC_PIN_DRDY) == LOW) { return; }

    PsocBatch batch;
    batch.timestamp_us = (uint64_t)micros();

    if (_readFrame() && _parseFrame(batch))
    {
        _lastOK = true;
        _batchesOK++;
        if (_cb) { _cb(batch); }
    }
    else
    {
        _lastOK = false;
        _batchesBad++;
    }
}

bool PsocSPI::_readFrame()
{
    SPI.beginTransaction(SPISettings(PSOC_SPI_CLK_HZ, MSBFIRST, PSOC_SPI_MODE));
    digitalWrite(PSOC_PIN_SS, LOW);
    for (int i = 0; i < SPI_FRAME_BYTES; i++) {
        _rxBuf[i] = SPI.transfer(0x00);
    }
    digitalWrite(PSOC_PIN_SS, HIGH);
    SPI.endTransaction();
    return (_rxBuf[0] == PSOC_FRAME_MARKER);
}

bool PsocSPI::_parseFrame(PsocBatch &out)
{
    if (_rxBuf[0] != PSOC_FRAME_MARKER) { return false; }

    out.n_samples    = _rxBuf[1] | (_rxBuf[2] << 8);
    out.seq          = _rxBuf[3] | (_rxBuf[4] << 8);
    out.global_flags = _rxBuf[5];

    if (out.n_samples != SPI_BATCH_SAMPLES) { return false; }

    const uint8_t *p = &_rxBuf[6];
    for (int i = 0; i < SPI_BATCH_SAMPLES; i++, p += 10) {
        PsocSample &s  = out.samples[i];
        s.raw_input    = (int16_t)(p[0] | (p[1] << 8));
        s.post_analog  = _sign24(p[2], p[3], p[4]);
        s.post_digital = _sign24(p[5], p[6], p[7]);
        s.gain_byte    = p[8];
        s.sample_flags = p[9];
    }
    return true;
}
