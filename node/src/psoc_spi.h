#pragma once
/*
 * psoc_spi.h — ESP32 SPI master interface to PSoC 5LP slave.
 *
 * Wiring (ESP32 VSPI):
 *   GPIO 18 → SCLK
 *   GPIO 19 → MISO  (PSoC drives)
 *   GPIO 23 → MOSI  (PSoC reads — future use: send coefficients)
 *   GPIO  5 → SS    (active low)
 *   GPIO  4 ← DATA_READY from PSoC (active high, interrupt-capable)
 *
 * Frame received per transaction:
 *   Bytes 0-5  : header [0xAB][n_lo][n_hi][seq_lo][seq_hi][flags]
 *   Bytes 6..  : n_samples × 10 bytes each
 *                [raw_lo][raw_hi][alog×3][digi×3][gain][flags]
 *
 * SPI settings: Mode 0, 4 MHz, MSB first.
 */

#include <Arduino.h>
#include <SPI.h>
#include <stdint.h>

#define PSOC_SPI_CLK_HZ   4000000UL   /* 4 MHz — safe for PSoC 5LP slave */
#define PSOC_SPI_MODE     SPI_MODE0
#define PSOC_PIN_SS       5
#define PSOC_PIN_DRDY     4
#define PSOC_FRAME_MARKER 0xAB

#define SPI_BATCH_SAMPLES 30
#define SPI_FRAME_BYTES   (6 + SPI_BATCH_SAMPLES * 10)   /* 306 */

/* One decoded sample from PSoC */
struct PsocSample {
    int16_t  raw_input;      /* SAR ADC (12-bit) or VDAC deviation */
    int32_t  post_analog;    /* ADC_DelSig before DFB (24-bit signed) */
    int32_t  post_digital;   /* After DFB + optional FIR (24-bit signed) */
    uint8_t  gain_byte;      /* low nibble=cfg, high nibble=log2(buf_gain) */
    uint8_t  sample_flags;   /* bit0=mux_in, bit1=mux_out */
};

/* One SPI batch (30 samples + metadata) */
struct PsocBatch {
    uint16_t     seq;          /* sequence counter from PSoC */
    uint8_t      global_flags; /* bit0=vdac_mode, bit1=filter_sel, bit2=fir_loaded */
    uint8_t      n_samples;
    PsocSample   samples[SPI_BATCH_SAMPLES];
    uint64_t     timestamp_us; /* local microseconds at reception (from NTP epoch) */
};

/* Callback type invoked after each successful batch receive */
typedef void (*BatchCallback)(const PsocBatch &batch);

class PsocSPI {
public:
    /* Call once from setup() */
    void begin(BatchCallback cb = nullptr);

    /* Call from loop() — polls DRDY and triggers SPI read if asserted */
    void poll();

    /* True if last transaction was error-free */
    bool lastOK() const { return _lastOK; }

    /* Total batches received / dropped */
    uint32_t batchesOK()   const { return _batchesOK; }
    uint32_t batchesBad()  const { return _batchesBad; }

private:
    bool          _lastOK     = false;
    uint32_t      _batchesOK  = 0;
    uint32_t      _batchesBad = 0;
    BatchCallback _cb         = nullptr;

    uint8_t  _rxBuf[SPI_FRAME_BYTES];

    bool _readFrame();
    bool _parseFrame(PsocBatch &out);

    static int32_t _sign24(uint8_t b0, uint8_t b1, uint8_t b2);
};
