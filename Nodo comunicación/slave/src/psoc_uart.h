#pragma once
/*
 * psoc_uart.h — Enlace ESP esclavo ↔ PSoC 5LP.
 *
 * El PSoC envía lotes RAW (sin Filter) y el esclavo le manda comandos
 * (set N, PGA, calibracion, pre-start, ver, debug).
 *
 * Cableado vigente en la placa nueva (2026-08-03) — el enlace es ASIMÉTRICO:
 *
 *   ESP → PSoC : UART (Serial2 TX). El PSoC tiene la UART configurada como
 *                SOLO RX, así que este sentido no cambió.
 *                PSOC_UART_TX (GPIO26) → RX del PSoC (P2[0])
 *
 *   PSoC → ESP : I2C, con el PSoC de MAESTRO y el ESP de ESCLAVO en
 *                PSOC_I2C_ADDR. Reemplaza al TX de la UART, que a 115200
 *                era el cuello de botella del stream de muestras.
 *                SDA=PSOC_I2C_SDA, SCL=PSOC_I2C_SCL.
 *
 * Los bytes que llegan por I2C entran a un ring y poll() los consume con el
 * mismo parser de frames de siempre: el protocolo no cambió, solo el medio.
 * PSOC_UART_RX queda definido para los diagnósticos de pin, pero ya no llega
 * ningún dato por ahí.
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
#define PSOC_UART_RX 25
#endif
#ifndef PSOC_UART_TX
#define PSOC_UART_TX 26
#endif
#ifndef PSOC_UART_RX_BUFFER_SIZE
#define PSOC_UART_RX_BUFFER_SIZE 32768
#endif
#ifndef PSOC_NATIVE_SAMPLE_RATE_HZ
#define PSOC_NATIVE_SAMPLE_RATE_HZ 2604UL
#endif

/* ── Enlace de subida PSoC → ESP por I2C (ESP = esclavo) ─────────────────── */
#ifndef PSOC_I2C_ENABLE
#define PSOC_I2C_ENABLE 1
#endif
/* Debe coincidir con PSOC_LINK_I2C_ADDR del firmware del PSoC. */
#ifndef PSOC_I2C_ADDR
#define PSOC_I2C_ADDR 0x42
#endif
#ifndef PSOC_I2C_SDA
#define PSOC_I2C_SDA 21
#endif
#ifndef PSOC_I2C_SCL
#define PSOC_I2C_SCL 22
#endif
/* Ring de recepción. Un frame son 95 bytes; con 4 KiB entran ~43 frames sin
 * que poll() tenga que correr a una cadencia particular. */
#ifndef PSOC_I2C_RX_RING
#define PSOC_I2C_RX_RING 4096
#endif

/* Velocidad del bus. TIENE que coincidir con la del maestro, que es el PSoC:
 * I2C_DATA_RATE = 1000 kbps en el I2C.h de su TopDesign. El esclavo del ESP32
 * ajusta su filtro de glitch y sus umbrales de FIFO con este valor; si queda
 * configurado mas lento que el maestro, descarta los flancos y no dispara
 * onReceive nunca, con el bus electricamente perfecto. */
#ifndef PSOC_I2C_FREQ_HZ
#define PSOC_I2C_FREQ_HZ 1000000UL
#endif

/* Buffer del esclavo. Un frame de datos son 95 bytes; el default de 32 los
 * partiria en varias llamadas a onReceive (no es fatal, el ring reensambla,
 * pero se pierden bytes en rafaga). Se aplica ANTES de Wire.begin(). */
#ifndef PSOC_I2C_RX_BUF
#define PSOC_I2C_RX_BUF 256
#endif

#define PSOC_FRAME_MARKER 0xAB
#define SPI_BATCH_SAMPLES 30                              /* muestras por lote */
#define PSOC_FRAME_BYTES  (4 + SPI_BATCH_SAMPLES * 3 + 1) /* 95 */
#define PSOC_CTRL_ACK_BYTES 5
#define PSOC_CTRL_DIAG_BYTES 6
#define PSOC_CTRL_ST_RESULT  0xC5
#define PSOC_ST_RESULT_BYTES 13

/* Comandos del autotest (ESP -> PSoC). Codigos libres en la tabla del PSoC. */
#define PSOC_CMD_ST_REPORT   0xA0   /* 1 param: que reporte emitir */
#define PSOC_CMD_ST_SYNC     0xA1   /* 1 param: 1=armar contador, 0=leer */
#define PSOC_CMD_ST_SET_IDAC 0xA2   /* 2 params: [etapa][codigo] */
#define PSOC_CMD_ST_MEAS_DC  0xA4   /* 1 param: [settle_sel<<4 | canal] */
#define PSOC_CMD_ST_MEAS_AC  0xA7   /* 1 param: [n_sel<<4 | canal] */

/* Reportes de PSOC_CMD_ST_REPORT */
#define ST_REP_IDENTITY 0
#define ST_REP_EEPROM   1
#define ST_REP_CAL      2
#define ST_REP_ADCCFG   3
#define ST_REP_IRQ      4
#define ST_REP_BUTTON   6
#define ST_REP_SD       5
#define ST_REP_RESTORE  7

/* status dentro de la trama 0xC5 */
#define ST_OK       0
#define ST_ERR      1
#define ST_REJECTED 2

/* test_id de la trama 0xC5 (ver psoc_selftest.h del proyecto PSoC de test) */
#define ST_ID_IDENTITY  0x01
#define ST_ID_EEPROM    0x02
#define ST_ID_SYNC      0x03
#define ST_ID_SD        0x04   /* v0=estado; v1=[stage][R1][pads][errores] */
#define ST_ID_ADCCFG    0x05
#define ST_ID_STAGES    0x06
#define ST_ID_IRQTRAP   0x07
#define ST_ID_BUTTON    0x08
#define ST_ID_CAL_BASE  0x10   /* |etapa */
#define ST_ID_IDAC_BASE 0x20   /* |etapa */
#define ST_ID_DC_BASE   0x50   /* |canal */
#define ST_ID_ACA_BASE  0x60   /* |canal */
#define ST_ID_ACB_BASE  0x70   /* |canal */

/* Comandos hacia el PSoC */
#define PSOC_CMD_PGA       0xA6
#define PSOC_CMD_PGAOUT    0xA8   /* GEO: ganancia de la etapa PGAout (código 0-8) */
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
#define PSOC_CMD_BLINK_LED    0xB9   /* Titilar el LED del PSoC para identificar el nodo */
#define PSOC_CMD_ADC_CONFIG   0xBA   /* 1=±2.5V, 2=±0.512V, 3=±1.024V, 4=±0.625V; las 4 a 2604 Hz nativos */
#define PSOC_CMD_SET_DECIMATION 0xBB /* factor 1..100 (1=sin decimar). Fs efectiva = 2604/factor */
#define PSOC_CMD_SD_STATUS   0xBC    /* p=0 estado cacheado, p=1 re-init. Ack: byte de estado SD */
#define PSOC_CMD_SD_TEST     0xBD    /* self-test FatFs GEOTEST.BIN. Ack: 1 OK / 0 fallo */
#define PSOC_CMD_SD_CAPTURE  0xBE    /* p=1 captura a SD (permite N>512), p=0 RAM-only. Ack 0/1, 0xEE=rechazo */
#define PSOC_CMD_SD_READ_BATCH 0xBF  /* uint16 LE; éxito: frame normal seq=índice */

/* Control PSoC -> ESP */
#define PSOC_CTRL_PING      0xC0
#define PSOC_CTRL_PONG      0xC1
#define PSOC_CTRL_CFG_ACK   0xC2
#define PSOC_CTRL_FS_REPORT 0xC3  /* PSoC → ESP: frecuencia de muestreo ADC */
#define PSOC_CTRL_DIAG_EVT  0xC4  /* PSoC → ESP: evento diagnostico */

/* ── Autotest de placa ────────────────────────────────────────────────────
 * Trama de resultado que emite el firmware de autotest del PSoC
 * (proyecto AcondicionamientoAnalogicoTest). 13 bytes:
 *   [0xAB][0xC5][test_id][status][v0 int32 LE][v1 int32 LE][XOR de 2..11]
 *
 * El firmware de campo del PSoC NUNCA la manda, asi que reconocerla aca es
 * inocuo para el build de campo: lo unico que cambia es que un 0xC5 deja de
 * contar como _badLen. Se deja sin guardar por #if a proposito, para que el
 * parser sea uno solo y no diverja entre variantes. */  /* PSoC → ESP: evento diagnostico */

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
#define PSOC_EVT_CAL_PI_GAIN32    0x3F
#define PSOC_EVT_CAL_PI_DEADBAND  0x40
#define PSOC_EVT_CAL_PI_ERROR32   0x41
#define PSOC_EVT_CAL_PI_BUCKET32  0x42
#define PSOC_EVT_CAL_PI_STABLE    0x43
#define PSOC_EVT_CAL_AMUX_CAP     0x44
#define PSOC_EVT_CAPTURE_WATCHDOG 0x45
#define PSOC_EVT_TIMER_STORM      0x46
#define PSOC_EVT_CHAIN_NEXT       0x47  /* trozo encadenado listo; el PSoC re-arma la próxima corrida. El ESP lo ignora durante SAMPLING (solo DUMP_DONE cierra la captura). */
#define PSOC_EVT_SD_STATUS        0x48  /* bit0 presente, bits1-2 tipo, bit3 test, bit4 FAT, bit5 sesión, bit6 captura */
#define PSOC_EVT_SD_SESSION       0x49  /* value=1: GEOLAST.BIN COMPLETE/listo; 0: inválido */
#define PSOC_EVT_SD_ERROR         0x4A  /* value: bit0 write fail, bit1 overrun, bit2 dir/header, bit3 read fail dump */
#define PSOC_EVT_ARMED_TIMEOUT    0x4B  /* PSoC aborto ARMED porque no llego SYNC */

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

/* Una medicion o reporte del autotest del PSoC. Los dos int32 significan
 * cosas distintas segun test_id; la tabla esta en selftest_report.cpp. */
struct PsocSelfTestResult {
    uint8_t  test_id;
    uint8_t  status;     /* ST_OK / ST_ERR / ST_REJECTED */
    int32_t  v0;
    int32_t  v1;
    uint32_t timestamp_ms;
};

struct PsocDiagEvent {
    uint8_t  event;
    uint8_t  value;
    uint8_t  psoc_state;
    uint32_t timestamp_ms;
};

typedef void (*BatchCallback)(const PsocBatch &batch);
typedef void (*DiagCallback)(const PsocDiagEvent &event);
typedef void (*SelfTestCallback)(const PsocSelfTestResult &result);

class PsocUART {
public:
    void begin(BatchCallback cb = nullptr, HardwareSerial *serial = nullptr);
    void onDiag(DiagCallback cb);
    void onSelfTest(SelfTestCallback cb);

    /* Comandos del autotest (ver psoc_selftest.h del proyecto PSoC de test). */
    void stReport(uint8_t what)              { _sendCmd1(PSOC_CMD_ST_REPORT, what); }
    void stSync(uint8_t arm)                 { _sendCmd1(PSOC_CMD_ST_SYNC, arm); }
    void stSetIdac(uint8_t stage, uint8_t c) { _sendCmd2(PSOC_CMD_ST_SET_IDAC, stage, c); }
    void stMeasDc(uint8_t settleSel, uint8_t ch)
        { _sendCmd1(PSOC_CMD_ST_MEAS_DC, (uint8_t)(((settleSel & 0x07u) << 4) | (ch & 0x0Fu))); }
    void stMeasAc(uint8_t nSel, uint8_t ch)
        { _sendCmd1(PSOC_CMD_ST_MEAS_AC, (uint8_t)(((nSel & 0x07u) << 4) | (ch & 0x0Fu))); }
    void poll();                 /* drena UART y ensambla frames */

    /* Comandos hacia el PSoC. START normal y Ver usan SYNC. */
    void setN(uint16_t n);
    void preStart();
    void startNow();
    void setVdac(uint8_t v);
    void setPga(uint8_t code);
    void setPgaout(uint8_t code);
    void setPgavdac(uint8_t code);
    void calibrate();
    void saveEeprom();
    void selectStream(uint8_t mode);
    void setAdcConfig(uint8_t cfg);
    void setDecimation(uint8_t factor);
    void sdStatus(uint8_t reinit);   /* consulta/re-init de la SD del PSoC (0xBC) */
    void sdTest();                   /* self-test de bloque en la SD del PSoC (0xBD) */
    void sdCapture(uint8_t enable);  /* modo captura a SD del PSoC (0xBE) */
    void sdReadBatch(uint16_t index);/* lectura bajo demanda desde GEOLAST.BIN (0xBF) */
    void adcSnapshot();
    void blinkLed();             /* pide al PSoC titilar su LED (identificación) */
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
    uint32_t i2cOverruns() const { return _i2cOverruns; }

private:
    HardwareSerial *_ser       = nullptr;
    BatchCallback   _cb        = nullptr;
    DiagCallback    _diagCb    = nullptr;
    SelfTestCallback _stCb     = nullptr;
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
    uint8_t         _confirmedPgaout = 0;
    uint8_t         _confirmedPgavdac = 0;
    uint8_t         _confirmedVdac = 128;
    uint8_t         _lastByte  = 0;
    uint32_t        _lastByteMs= 0;
    bool            _lastByteSeen = false;

    /* ── Recepción por I2C (ESP esclavo) ──────────────────────────────────
     * onReceive() de Wire toma un puntero a función suelto, así que el ring
     * es estático. _rxAvailable()/_rxRead() son la única puerta de entrada de
     * bytes a poll(); si PSOC_I2C_ENABLE=0 vuelven a leer de la UART. */
    static volatile uint8_t  _i2cRing[PSOC_I2C_RX_RING];
    static volatile uint16_t _i2cHead;
    static volatile uint16_t _i2cTail;
    static volatile uint32_t _i2cOverruns;
    static void _onI2cReceive(int count);

    bool _rxAvailable();
    bool _rxRead(uint8_t &b);

    void _parseFrame();
    void _parseConfigAck();
    void _parseFsReport();
    void _parseDiagEvent();
    void _parseSelfTestResult();
    void _noteRxByte(uint8_t b);
    void _sendCmd1(uint8_t cmd, uint8_t p);
    void _sendCmd2(uint8_t cmd, uint8_t p1, uint8_t p2);
    static int32_t _sign24(uint8_t b0, uint8_t b1, uint8_t b2);
};
