#pragma once
/*
 * psoc_uart.h — Enlace UART entre el ESP esclavo y el PSoC 5LP.
 *
 * Reemplaza al antiguo psoc_spi. El PSoC envía lotes RAW (sin Filter) y el
 * esclavo le manda comandos (set N, VDAC, pre-start, ver, debug).
 *
 * Cableado (ESP32, Serial2 por defecto):
 *   PSOC_UART_RX (GPIO16) ← TX del PSoC (UART.TX)
 *   PSOC_UART_TX (GPIO17) → RX del PSoC (UART.RX)
 *   Baud: PSOC_UART_BAUD (coincidir con el PSoC).
 *
 * Frame de datos PSoC→ESP (95 bytes):
 *   [0xAB][n=30][seq_lo][seq_hi] + 30×3 bytes (raw LE) + [crc XOR]
 *
 * Comandos ESP→PSoC (checksum XOR):
 *   1 param : [0xAB][cmd][param][cmd^param]
 *   2 param : [0xAB][0xA3][n_lo][n_hi][0xA3^n_lo^n_hi]   (set N 16 bits)
 */

#include <Arduino.h>
#include <stdint.h>

#ifndef PSOC_UART_BAUD
#define PSOC_UART_BAUD 460800UL
#endif
#ifndef PSOC_UART_RX
#define PSOC_UART_RX 16
#endif
#ifndef PSOC_UART_TX
#define PSOC_UART_TX 17
#endif

#define PSOC_FRAME_MARKER 0xAB
#define SPI_BATCH_SAMPLES 30                              /* muestras por lote */
#define PSOC_FRAME_BYTES  (4 + SPI_BATCH_SAMPLES * 3 + 1) /* 95 */

/* Comandos hacia el PSoC */
#define PSOC_CMD_PGA       0xA6
#define PSOC_CMD_PGAVDAC   0xA9
#define PSOC_CMD_VDAC      0xAA
#define PSOC_CMD_SETN      0xA3
#define PSOC_CMD_PRESTART  0xB1
#define PSOC_CMD_VIEW      0xB2
#define PSOC_CMD_DEBUG     0xB3

struct PsocSample {
    int16_t  raw_input;
    int32_t  post_analog;
    int32_t  post_digital;   /* aquí va el RAW 24-bit del PSoC */
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

class PsocUART {
public:
    void begin(BatchCallback cb = nullptr, HardwareSerial *serial = nullptr);
    void poll();                 /* drena UART y ensambla frames */

    /* Comandos hacia el PSoC */
    void setN(uint16_t n);
    void preStart();
    void captureNow();
    void setVdac(uint8_t v);
    void setPga(uint8_t code);
    void setPgavdac(uint8_t code);
    void debugRamp(bool en);

    bool     lastOK()     const { return _lastOK; }
    uint32_t batchesOK()  const { return _batchesOK; }
    uint32_t batchesBad() const { return _batchesBad; }

private:
    HardwareSerial *_ser       = nullptr;
    BatchCallback   _cb        = nullptr;
    uint8_t         _buf[PSOC_FRAME_BYTES];
    uint16_t        _idx       = 0;
    bool            _lastOK    = false;
    uint32_t        _batchesOK = 0;
    uint32_t        _batchesBad= 0;

    void _parseFrame();
    void _sendCmd1(uint8_t cmd, uint8_t p);
    void _sendCmd2(uint8_t cmd, uint8_t p1, uint8_t p2);
    static int32_t _sign24(uint8_t b0, uint8_t b1, uint8_t b2);
};
