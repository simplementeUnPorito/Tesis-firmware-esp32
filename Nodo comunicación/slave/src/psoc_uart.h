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
#define PSOC_UART_BAUD 115200UL   /* 460800/921600 no son divisibles exactos a 24 MHz */
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
#define PSOC_CTRL_ACK_BYTES 5

/* Comandos hacia el PSoC */
#define PSOC_CMD_PGA       0xA6
#define PSOC_CMD_PGAVDAC   0xA9
#define PSOC_CMD_VDAC      0xAA
#define PSOC_CMD_STATUS    0xA5
#define PSOC_CMD_SETN      0xA3
#define PSOC_CMD_PRESTART  0xB1
#define PSOC_CMD_DEBUG     0xB3
#define PSOC_CMD_START_NOW 0xB4

/* Control PSoC -> ESP */
#define PSOC_CTRL_PING     0xC0
#define PSOC_CTRL_PONG     0xC1
#define PSOC_CTRL_CFG_ACK  0xC2

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

    /* Comandos hacia el PSoC. START normal y Ver usan SYNC. */
    void setN(uint16_t n);
    void preStart();
    void startNow();
    void setVdac(uint8_t v);
    void setPga(uint8_t code);
    void setPgavdac(uint8_t code);
    void debugRamp(bool en);
    void requestStatus();
    void sendPong();
    bool takeConfigAck(uint8_t &cmd, uint8_t &val);

    /* Detecta si el PSoC responde. Primero usa el probe no invasivo 0xA5
     * y solo como fallback activa la rampa para firmware PSoC viejo. */
    bool probe(uint32_t timeoutMs = 150);

    bool     lastOK()     const { return _lastOK; }
    uint32_t batchesOK()  const { return _batchesOK; }
    uint32_t batchesBad() const { return _batchesBad; }
    uint32_t bytesRx()    const { return _bytesRx; }
    uint32_t markersRx()  const { return _markersRx; }
    uint32_t syncDrops()  const { return _syncDrops; }
    uint32_t badLen()     const { return _badLen; }
    uint32_t pingsRx()    const { return _pingsRx; }
    uint32_t configAcksRx() const { return _configAcksRx; }
    uint8_t  lastByte()   const { return _lastByte; }
    bool     hasLastByte() const { return _lastByteSeen; }
    uint32_t lastByteAgeMs() const { return _lastByteSeen ? (millis() - _lastByteMs) : 0xFFFFFFFFu; }

private:
    HardwareSerial *_ser       = nullptr;
    BatchCallback   _cb        = nullptr;
    uint8_t         _buf[PSOC_FRAME_BYTES];
    uint16_t        _idx       = 0;
    bool            _lastOK    = false;
    uint32_t        _batchesOK = 0;
    uint32_t        _batchesBad= 0;
    uint32_t        _bytesRx   = 0;
    uint32_t        _markersRx = 0;
    uint32_t        _syncDrops = 0;
    uint32_t        _badLen    = 0;
    uint32_t        _pingsRx   = 0;
    uint32_t        _configAcksRx = 0;
    bool            _cfgAckPending = false;
    uint8_t         _cfgAckCmd = 0;
    uint8_t         _cfgAckVal = 0;
    uint8_t         _confirmedPga = 0;
    uint8_t         _confirmedPgavdac = 0;
    uint8_t         _confirmedVdac = 128;
    uint8_t         _lastByte  = 0;
    uint32_t        _lastByteMs= 0;
    bool            _lastByteSeen = false;

    void _parseFrame();
    void _parseConfigAck();
    void _noteRxByte(uint8_t b);
    void _sendCmd1(uint8_t cmd, uint8_t p);
    void _sendCmd2(uint8_t cmd, uint8_t p1, uint8_t p2);
    static int32_t _sign24(uint8_t b0, uint8_t b1, uint8_t b2);
};
