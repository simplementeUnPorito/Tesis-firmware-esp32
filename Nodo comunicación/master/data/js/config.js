// config.js — constants mirrored from python/geophone_scope/config.py.
// Keep in sync with that file; it is the canonical source.

export const PKT_HEADER = 0x56;
export const CMD_HEADER = 0xAB;
export const CMD_DIRECTED = 0xBD;
export const PKT_LEN = 6;

// Packet types (byte 3 of every PKT_HEADER packet)
export const PTYPE_DATA = 0x00;
export const PTYPE_HEARTBEAT = 0x01;
export const PTYPE_ACK = 0x07;
export const PTYPE_LATENCY = 0xFC;
export const PTYPE_STATUS = 0xFD;
export const PTYPE_READY = 0xFE;

// Commands (browser -> master)
export const CMD_STREAM = 0xA1;
export const CMD_ARM = 0xA2;
export const CMD_START = 0xA3;
export const CMD_STOP = 0xA4;
export const CMD_STATUS = 0xA5;
export const CMD_DEBUG = 0xA7;
export const CMD_SET_RECLEN = 0xAE;
export const CMD_SET_RECLEN_HAMMER = 0xAD;  /* largo propio de nodos HAMMER; 0 = mismo que GEO */

// Sub-commands (directed to slave)
export const SUBCMD_PGA = 0xA6;
export const SUBCMD_PGAOUT = 0xA8;   /* GEO: etapa PGAout del pipeline nuevo (mismo codigo que PSOC_CMD_PGAOUT) */
export const SUBCMD_VDAC = 0xAA;
export const SUBCMD_DEBUG = 0xA7;
export const SUBCMD_SYNC_START = 0x12;
export const SUBCMD_VER = 0xB2;
export const SUBCMD_CALIBRATE     = 0xB5;
export const SUBCMD_SAVE_EEPROM   = 0xB6;   /* mismo código que PSOC_CMD_SAVE_EEPROM */
export const SUBCMD_SELECT_STREAM = 0xB7;   /* mismo código que PSOC_CMD_SELECT_STREAM */
export const SUBCMD_ADC_CONFIG    = 0xBA;   /* 1=±2.5V, 2=±0.512V, 3=±1.024V, 4=±0.625V; las 4 a 2604 Hz nativos */
export const SUBCMD_DECIMATION    = 0xBB;   /* factor 1..100 (1=sin decimar); Fs efectiva = 2604/factor */
export const SUBCMD_SD_CAPTURE    = 0xBE;   /* 0/1: captura a SD del PSoC (solo nodos con sd_present=1). Viaja web→maestro→esclavo→PSoC como cualquier sub_cmd */
export const SUBCMD_SD_ENABLE     = 0xC0;   /* OBSOLETO (era SD-en-ESP32, purgada 2026-07-11): el esclavo lo rechaza siempre; usar SUBCMD_SD_CAPTURE */
export const SUBCMD_BLINK_LED     = 0x70;
export const SUBCMD_LATENCY       = 0xAF;
export const SUBCMD_CAL_VDAC_BASE = 0x80;
export const SUBCMD_CAL_TARGET_MV_HI_BASE = 0x84;
export const SUBCMD_CAL_TARGET_MV_LO_BASE = 0x88;
export const SUBCMD_CAL_ERROR_MV_HI_BASE = 0x8C;
export const SUBCMD_CAL_ERROR_MV_LO_BASE = 0x90;
export const CAL_VDAC_STAGE_COUNT = 4;

// Acquisition parameters
// Hardware timing/export uses the slave HELLO (see effectiveFs() in
// data_store.js). DEFAULT_SAMPLE_RATE_HZ below is the canonical N=1 startup
// fallback used for placeholders before that exact report arrives.
export const SAMPLES_PER_BATCH = 30;
// GEO: archivo FatFs del PSoC. HAMMER/RAM conserva el tope de 512.
export const PSOC_CAPTURE_MAX_BATCHES = 60000;
export const PSOC_RAM_CAPTURE_MAX_BATCHES = 512;
export const MAX_CAPTURE_SECONDS = 3600;
export const TEST_DEFAULT_SECONDS = 0.2;
// Placeholder sample counts for buffer/display sizing before any slave's
// HELLO reports the real Fs — syncDataBufferForFs()/applyDisplayWindow()
// (app.js) resize these for real the moment Fs becomes known, so the exact
// values here only seed startup sizing before HELLO. Use the measured real
// rate so the initial 3 s / 10 s windows are close even before the first HELLO.
export const DEFAULT_SAMPLE_RATE_HZ = 2604;
export const DISP_SAMP = DEFAULT_SAMPLE_RATE_HZ * 3;
export const MAX_BUF_S = 10;
export const MAX_BUF = DEFAULT_SAMPLE_RATE_HZ * MAX_BUF_S;

// Nodes
export const MAX_NODES = 9; // node 0 = master, nodes 1..8 = slave slots
export const NODE_NAMES = ['Maestro', ...Array.from({ length: MAX_NODES - 1 }, (_, i) => `Esclavo ${i + 1}`)];
// Generate N geo options matching the number of slave slots so the UI scales with hardware
export const SLAVE_TYPE_ORDER = ['Hammer', ...Array.from({ length: MAX_NODES - 1 }, (_, i) => `Geo${i + 1}`)];
export const MASTER_NODE_ID = 0xFF;

// PGA
export const GAIN_CODES = [1, 2, 4, 8, 16, 24, 32, 48, 50];
export const GAIN_NAMES = ['1x', '2x', '4x', '8x', '16x', '24x', '32x', '48x', '50x'];
export const CAL_VDAC_LABELS_GEO = ['PGA', 'BP', 'ADDER', 'LP'];
export const CAL_VDAC_LABELS_HAMMER = ['PGA', 'LP'];

// VDAC
export const VDAC_STEP = 0.016;
export const VDAC_MIN = 0;
export const VDAC_MAX = 255;
export const VDAC_FULL_SCALE_V = VDAC_MAX * VDAC_STEP;
export const VDAC_MV_PER_CODE = VDAC_STEP * 1000;

// ADC scaling — configs expuestas del PSoC. Las 4 usan la misma Fs base nativa
// (2604 Hz); la Fs efectiva real depende además del factor de decimación de
// cada nodo y SIEMPRE se toma de su HELLO (fsHz acá es solo informativo).
export const ADC_CONFIGS = [
  { code: 1, label: '±2.5 V', rangeV: 2.5, fsHz: 2604, countsPerVolt: 131072 / 2.5 },
  { code: 2, label: '±0.512 V', rangeV: 0.512, fsHz: 2604, countsPerVolt: 131072 / 0.512 },
  { code: 3, label: '±1.024 V', rangeV: 1.024, fsHz: 2604, countsPerVolt: 131072 / 1.024 },
  { code: 4, label: '±0.625 V', rangeV: 0.625, fsHz: 2604, countsPerVolt: 131072 / 0.625 },
];
export const ADC_COUNTS_PER_VOLT = ADC_CONFIGS[0].countsPerVolt;

// Master states (b0 of heartbeat)
export const MASTER_STATE_NAMES = {
  0: 'IDLE', 1: 'ARMING', 2: 'ARMED', 3: 'RUNNING',
  4: 'STOPPING', 5: 'DUMPING', 6: 'PRESTART', 7: 'SCOPE_MULTI',
};
export const MASTER_STATE_IDLE = 0;
export const MASTER_STATE_ARMING = 1;
export const MASTER_STATE_ARMED = 2;
export const MASTER_STATE_RUNNING = 3;
export const MASTER_STATE_STOPPING = 4;
export const MASTER_STATE_DUMPING = 5;
export const MASTER_STATE_PRESTART = 6;
export const MASTER_STATE_SCOPE_MULTI = 7;

// TX modes
export const TX_RAW = 0;
export const TX_FILTERED = 1;
export const TX_DEBUG = 2;
export const TX_MODE_NAMES = ['Raw ADC', 'Filtered ADC', 'Debug'];

// GUI / rendering
export const RENDER_PERIOD_MS = 150;
export const RAW_COLOR = 'rgb(80, 140, 255)';
export const FILT_COLOR = 'rgb(255, 80, 80)';
export const ENV_RAW_COLOR = 'rgba(242, 130, 13, 0.86)';
export const ENV_FILT_COLOR = 'rgba(255, 190, 80, 0.86)';

// Cancelación de ruido de línea (mínimos cuadrados sobre la captura completa).
// Espejo de python/geophone_scope/config.py: NOTCH_F0 / NOTCH_DEFAULT_HARM.
export const LINE_NOTCH_F0 = 50;          // Hz — red eléctrica (Argentina)
export const LINE_NOTCH_DEFAULT_HARM = 3; // armónicos por defecto
export const LINE_NOTCH_MAX_HARM = 12;    // tope del input N
export const LINE_NOTCH_SEARCH_HZ = 2;    // buscar pico real en 50±2 Hz

// Retry / latency probing
export const RETRY_SEC = 1.5;
export const START_LATENCY_PROBES = 12;
export const START_LATENCY_PROBE_GAP_S = 0.04;

export const DEFAULT_SAVE_NAME = 'muestra';
