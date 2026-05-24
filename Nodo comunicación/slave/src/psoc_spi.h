#pragma once
/*
 * psoc_spi.h — ESP32 SPI master / PSoC 5LP slave interface.
 *
 * Wiring (ESP32 VSPI):
 *   GPIO 18 → SCLK
 *   GPIO 19 → MISO  (PSoC drives)
 *   GPIO 23 → MOSI  (PSoC ignores — future: recibir coeficientes)
 *   GPIO  5 → SS    (activo bajo)
 *   GPIO  4 ← DATA_READY del PSoC (activo alto)
 *
 * Frame por transacción (306 bytes):
 *   Bytes 0-5  : header [0xAB][n_lo][n_hi][seq_lo][seq_hi][flags]
 *   Bytes 6..  : n_samples × 10 bytes
 *                [raw_lo][raw_hi][alog0][alog1][alog2][digi0][digi1][digi2][gain][flags]
 *
 * SPI: Mode 0, 4 MHz, MSB first.
 */

#include <Arduino.h>
#include <SPI.h>
#include <stdint.h>

#define PSOC_SPI_CLK_HZ   4000000UL
#define PSOC_SPI_MODE     SPI_MODE0
#define PSOC_PIN_SS       5
#define PSOC_PIN_DRDY     4
#define PSOC_FRAME_MARKER 0xAB

#define SPI_BATCH_SAMPLES 30
#define SPI_FRAME_BYTES   (6 + SPI_BATCH_SAMPLES * 10)   /* 306 */

struct PsocSample {
    int16_t  raw_input;
    int32_t  post_analog;
    int32_t  post_digital;
    uint8_t  gain_byte;
    uint8_t  sample_flags;
};

struct PsocBatch {
    uint16_t   seq;
    uint8_t    global_flags;
    uint8_t    n_samples;
    PsocSample samples[SPI_BATCH_SAMPLES];
    uint64_t   timestamp_us;
};

typedef void (*BatchCallback)(const PsocBatch &batch);

class PsocSPI {
public:
    void begin(BatchCallback cb = nullptr);
    void poll();

    bool     lastOK()      const { return _lastOK; }
    uint32_t batchesOK()   const { return _batchesOK; }
    uint32_t batchesBad()  const { return _batchesBad; }

private:
    bool          _lastOK    = false;
    uint32_t      _batchesOK = 0;
    uint32_t      _batchesBad= 0;
    BatchCallback _cb        = nullptr;
    uint8_t       _rxBuf[SPI_FRAME_BYTES];

    bool _readFrame();
    bool _parseFrame(PsocBatch &out);
    static int32_t _sign24(uint8_t b0, uint8_t b1, uint8_t b2);
};
