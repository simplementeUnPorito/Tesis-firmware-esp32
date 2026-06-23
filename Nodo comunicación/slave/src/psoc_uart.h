#pragma once
/*
 * psoc_uart.h — Enlace UART entre el ESP esclavo y el PSoC 5LP.
 *
 * Reemplaza al antiguo psoc_spi. El PSoC envía lotes RAW (sin Filter) y el
 * esclavo le manda comandos (set N, PGA, calibracion, pre-start, ver, debug).
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
#ifndef PSOC_UART_RX_BUFFER_SIZE
#define PSOC_UART_RX_BUFFER_SIZE 32768
#endif

#define PSOC_FRAME_MARKER 0xAB
#define SPI_BATCH_SAMPLES 30                              /* muestras por lote */
#define PSOC_FRAME_BYTES  (4 + SPI_BATCH_SAMPLES * 3 + 1) /* 95 */
#define PSOC_CTRL_ACK_BYTES 5
#define PSOC_CTRL_DIAG_BYTES 6

/* Comandos hacia el PSoC */
#define PSOC_CMD_PGA       0xA6
#define PSOC_CMD_PGAVDAC   0xA9
#define PSOC_CMD_VDAC      0xAA
#define PSOC_CMD_STATUS    0xA5
#define PSOC_CMD_SETN      0xA3
#define PSOC_CMD_PRESTART  0xB1
#define PSOC_CMD_DEBUG     0xB3
#define PSOC_CMD_START_NOW 0xB4
#define PSOC_CMD_CALIBRATE    0xB5
#define PSOC_CMD_SAVE_EEPROM  0xB6
#define PSOC_CMD_SELECT_STREAM 0xB7
#define PSOC_CMD_ADC_SNAPSHOT 0xB8

/* Control PSoC -> ESP */
#define PSOC_CTRL_PING      0xC0
#define PSOC_CTRL_PONG      0xC1
#define PSOC_CTRL_CFG_ACK   0xC2
#define PSOC_CTRL_FS_REPORT 0xC3  /* PSoC → ESP: frecuencia de muestreo ADC */
#define PSOC_CTRL_DIAG_EVT  0xC4  /* PSoC → ESP: evento diagnostico */

#define PSOC_EVT_BOOT             0x01
#define PSOC_EVT_ANALOG_READY     0x02
#define PSOC_EVT_CAL_START        0x10
#define PSOC_EVT_CAL_DONE         0x11
#define PSOC_EVT_CAL_BUSY         0x12
#define PSOC_EVT_CAL_STAGE_DAC    0x13
#define PSOC_EVT_CAL_STAGE_MEAS   0x14
#define PSOC_EVT_CAL_STAGE_BEGIN  0x15
#define PSOC_EVT_CAL_STAGE_OK     0x16
#define PSOC_EVT_CAL_VERIFY_BEGIN 0x17
#define PSOC_EVT_CAL_VERIFY_OK    0x18
#define PSOC_EVT_CAL_AMUX_IN      0x19
#define PSOC_EVT_CAL_PROGRESS     0x1A
#define PSOC_EVT_CAL_WATCHDOG     0x1B
#define PSOC_EVT_CAL_LP_BAD       0x1C
#define PSOC_EVT_CAL_STAGE_MEAS32 0x1D
#define PSOC_EVT_SERVO_STAGE      0x1E
#define PSOC_EVT_SERVO_STEP       0x1F
#define PSOC_EVT_WAIT_ESP         0x20
#define PSOC_EVT_ESP_SEEN         0x21
#define PSOC_EVT_CAL_LOOP         0x22
#define PSOC_EVT_CAL_STAGE_SAT    0x23
#define PSOC_EVT_CAL_STAGE_SAT_ALL 0x24
#define PSOC_EVT_CAL_REALCHECK_BEGIN 0x25
#define PSOC_EVT_CAL_REALCHECK_DAC   0x26
#define PSOC_EVT_CAL_REALCHECK_MEAS32 0x27
#define PSOC_EVT_CAL_REALCHECK_NUDGE 0x28
#define PSOC_EVT_CAL_REALCHECK_OK    0x29
#define PSOC_EVT_CAL_STAGE_TARGET32 0x2A
#define PSOC_EVT_CAL_SWEEP_DAC      0x2B
#define PSOC_EVT_CAL_SWEEP_MEAS32   0x2C
#define PSOC_EVT_ADC_SNAPSHOT_BEGIN 0x2D
#define PSOC_EVT_ADC_RAW32          0x2E
#define PSOC_EVT_ADC_FILT32         0x2F
#define PSOC_EVT_RX_CMD           0x30
#define PSOC_EVT_SETN             0x31
#define PSOC_EVT_ARMED            0x32
#define PSOC_EVT_SYNC_RISE        0x33
#define PSOC_EVT_SYNC_FALL        0x34
#define PSOC_EVT_SAMPLING_START   0x35
#define PSOC_EVT_CAPTURE_DONE     0x36
#define PSOC_EVT_DUMP_START       0x37
#define PSOC_EVT_DUMP_DONE        0x38
#define PSOC_EVT_START_NOW        0x39
#define PSOC_EVT_DEBUG_MODE       0x3A
#define PSOC_EVT_STATUS_REQ       0x3B
#define PSOC_EVT_BUTTON           0x3C
#define PSOC_EVT_BUTTON_IGNORED   0x3D
#define PSOC_EVT_CAPTURE_CLAMPED  0x3E

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

struct PsocDiagEvent {
    uint8_t  event;
    uint8_t  value;
    uint8_t  psoc_state;
    uint32_t timestamp_ms;
};

typedef void (*BatchCallback)(const PsocBatch &batch);
typedef void (*DiagCallback)(const PsocDiagEvent &event);

class PsocUART {
public:
    void begin(BatchCallback cb = nullptr, HardwareSerial *serial = nullptr);
    void onDiag(DiagCallback cb);
    void poll();                 /* drena UART y ensambla frames */

    /* Comandos hacia el PSoC. START normal y Ver usan SYNC. */
    void setN(uint16_t n);
    void preStart();
    void startNow();
    void setVdac(uint8_t v);
    void setPga(uint8_t code);
    void setPgavdac(uint8_t code);
    void calibrate();
    void saveEeprom();
    void selectStream(uint8_t mode);
    void adcSnapshot();
    void debugRamp(bool en);
    void requestStatus();
    void sendPong();
    bool takeConfigAck(uint8_t &cmd, uint8_t &val);

    /* Detecta si el PSoC responde. Primero usa el probe no invasivo 0xA5
     * y solo como fallback activa la rampa para firmware PSoC viejo. */
    bool probe(uint32_t timeoutMs = 150);

    uint16_t sampleRate() const { return _sampleRate; }
    bool     lastOK()     const { return _lastOK; }
    uint32_t batchesOK()  const { return _batchesOK; }
    uint32_t batchesBad() const { return _batchesBad; }
    uint32_t bytesRx()    const { return _bytesRx; }
    uint32_t markersRx()  const { return _markersRx; }
    uint32_t syncDrops()  const { return _syncDrops; }
    uint32_t badLen()     const { return _badLen; }
    uint32_t pingsRx()    const { return _pingsRx; }
    uint32_t configAcksRx() const { return _configAcksRx; }
    uint32_t diagEventsRx() const { return _diagEventsRx; }
    uint8_t  lastByte()   const { return _lastByte; }
    bool     hasLastByte() const { return _lastByteSeen; }
    uint32_t lastByteAgeMs() const { return _lastByteSeen ? (millis() - _lastByteMs) : 0xFFFFFFFFu; }

private:
    HardwareSerial *_ser       = nullptr;
    BatchCallback   _cb        = nullptr;
    DiagCallback    _diagCb    = nullptr;
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
    uint32_t        _diagEventsRx = 0;
    uint16_t        _sampleRate = 0;
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
    void _parseFsReport();
    void _parseDiagEvent();
    void _noteRxByte(uint8_t b);
    void _sendCmd1(uint8_t cmd, uint8_t p);
    void _sendCmd2(uint8_t cmd, uint8_t p1, uint8_t p2);
    static int32_t _sign24(uint8_t b0, uint8_t b1, uint8_t b2);
};
