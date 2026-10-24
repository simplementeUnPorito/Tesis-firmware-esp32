#pragma once
/*
 * sync_protocol.h — copia del esclavo, idéntica.
 * (Mantener sincronizado manualmente si se modifica.)
 */

#include <stdint.h>

#define CMD_ARM        0x10
#define CMD_ARM_ACK    0x11
#define CMD_START      0x12
#define CMD_STOP       0x13
#define CMD_DEBUG      0x14   /* broadcast debug ramp — todos los esclavos */
#define CMD_START_ACK  0x15   /* esclavo → maestro: recibió CMD_START */
#define CMD_DATA       0x20
#define CMD_SET_CONFIG 0x21   /* maestro → esclavo unicast: configurar parámetro */
#define CMD_CFG_ACK    0x22   /* esclavo → maestro: confirmación de config */
#define CMD_DEBUG_NODE 0x23   /* maestro → esclavo unicast: debug ramp individual */
#define CMD_VIEW       0x24   /* maestro → esclavo unicast: captura única "Ver" (N lotes) */
#define CMD_DEBUG_PSOC 0x25   /* maestro → esclavo unicast: rampa de debug del PSoC */
#define CMD_STATUS     0x30
#define CMD_HELLO      0x40   /* beacon diagnóstico: esclavo → maestro */
#define CMD_SET_RECLEN 0x50   /* master → todos: cuántos batches grabar (respuesta: CMD_CFG_ACK sub=0x50) */
#define CMD_REQ_BATCH  0x51   /* master → esclavo: pedir batch específico (respuesta: 2× MsgData) */
#define CMD_SCOPE_START 0x60  /* master → esclavos: pulso START falso, sin muestreo ni ACK */
#define CMD_PRESTART    0x61  /* master → todos: entrar en HOT_WAIT con n_batches listo */
#define CMD_HOTWAIT_QUERY 0x62 /* master → esclavo: consultar si está en HOT_WAIT */
#define CMD_HOTWAIT_ACK 0x63  /* esclavo → master: confirma HOT_WAIT */
#define CMD_BLINK_LED   0x70  /* master → esclavo unicast: titilar LED para identificación */
#define CMD_SAVE_EEPROM 0x71  /* master → esclavo unicast: guardar config PSoC en EEPROM */
#define CMD_SELECT_STREAM 0x72 /* master → esclavo unicast: 0=crudo, 1=FIR hardware */
#define CMD_STREAM_ACK  0x73  /* master → esclavo: tiempo real, lotes ya entregados */
#define CMD_STREAM_END  0x74  /* esclavo → master: tiempo real, la captura cerro */
#define CMD_CAL_VDAC_STAGE_BASE       0x80 /* esclavo -> maestro ACK auxiliar: 0x80..0x83 stage VDAC code */
#define CMD_CAL_TARGET_MV_HI_BASE     0x84 /* 0x84..0x87 signed int16 target mV high byte */
#define CMD_CAL_TARGET_MV_LO_BASE     0x88 /* 0x88..0x8B signed int16 target mV low byte */
#define CMD_CAL_ERROR_MV_HI_BASE      0x8C /* 0x8C..0x8F signed int16 error mV high byte */
#define CMD_CAL_ERROR_MV_LO_BASE      0x90 /* 0x90..0x93 signed int16 error mV low byte */
#define CMD_CAL_VDAC_STAGE_COUNT      4

#define SLAVE_HW_GEO     0u
#define SLAVE_HW_HAMMER  1u
#define SLAVE_HW_UNKNOWN 0xFFu

/* Generación del enlace interno PSoC <-> ESP. Es independiente de hw_class:
 * puede haber GEO o HAMMER tanto SERIAL como I2C. En la placa I2C actual los
 * comandos bajan por UART y las respuestas/muestras suben por I2C; el nombre
 * identifica esa generación de placa, no obliga a que ambos sentidos sean I2C. */
#define SLAVE_LINK_SERIAL  0u
#define SLAVE_LINK_I2C     1u
#define SLAVE_LINK_UNKNOWN 0xFFu

/* Estado del lazo de control del PSoC I2C, último byte del HELLO.
 * 0xFF = sin control (placas SERIAL o sin telemetría todavía).
 * bit0 = en banda, bits1..3 = estado (0 falta aprender, 1 aprendiendo,
 * 2 corriendo, 3 pausado, 4 falló), bit4 = perfil aprendido válido. */
#define SLAVE_CTL_UNKNOWN      0xFFu

/* sub_cmd bajo CMD_SET_CONFIG que NO se reenvía al PSoC — vive 100% en el
 * ESP32 del esclavo (handleSetConfig() lo resuelve local, nunca llega a
 * psoc_uart). 0=deshabilitar SD, 1=habilitar (solo si sd_present=1, ver
 * MsgHello). NOTA: el valor numérico 0xC0 coincide con PSOC_CTRL_PING de
 * psoc_uart.h, pero son espacios de bytes distintos sin colisión real: ese
 * es un byte de protocolo UART crudo hacia el PSoC, este es un sub_cmd de
 * radio ESP-NOW que nunca se copia a la UART. */
#define SLAVE_CMD_SET_SD_ENABLE 0xC0u

#define MSG_HELLO_LEGACY_BYTES 5u
#define MSG_HELLO_HW_BYTES     6u
#define MSG_HELLO_SD_BYTES     7u

#pragma pack(push, 1)

struct MsgArm    { uint8_t cmd; };
struct MsgArmAck { uint8_t cmd; uint8_t node_id; uint8_t status; };
struct MsgStart  { uint8_t cmd; uint64_t t_start_us; uint8_t target_node; };
struct MsgStartAck {
    uint8_t  cmd;
    uint8_t  node_id;
    uint8_t  status;       /* 0=ignorado, 1=start real, 2=probe */
    uint32_t start_token;  /* low32(t_start_us), para asociar ACK con envío */
    uint32_t rx_us;
};
struct MsgStop   { uint8_t cmd; };
struct MsgDebug  { uint8_t cmd; uint8_t enable; };

#define DATA_PKT_MARKER  0xBC

/* SampleBytes.flags: interferencias conocidas durante captura en vivo. */
#define SAMPLE_FLAG_SD_IO      0x01u   /* acceso SPI a la SD del ESP esclavo */
#define SAMPLE_FLAG_SD_WRITE   SAMPLE_FLAG_SD_IO
#define SAMPLE_FLAG_SD_SYNC    0x02u   /* flush de la SD del ESP esclavo */
#define SAMPLE_FLAG_RADIO_TX   0x04u   /* transmision ESP-NOW del esclavo */
#define SAMPLE_FLAG_I2C        0x08u   /* trama I2C del PSoC al ESP */
#define SAMPLE_FLAG_AP_BEACON  0x10u   /* beacon del AP GeoNetwork */
#define SAMPLE_FLAG_MASTER_RX  0x20u   /* trama ESP-NOW recibida del maestro */
#define SAMPLES_PER_PART 15

struct SampleBytes {
    uint8_t raw_lo, raw_hi;
    uint8_t alog0, alog1, alog2;
    uint8_t digi0, digi1, digi2;
    uint8_t gain;
    uint8_t flags;
};

struct MsgData {
    uint8_t     marker;
    uint8_t     node_id;
    uint16_t    seq;
    uint8_t     part;
    uint8_t     n_samples;
    uint64_t    timestamp_us;
    SampleBytes samples[SAMPLES_PER_PART];
    uint8_t     crc;
};

/* Tiempo real: igual que MsgData pero con lote de 32 bits, porque una captura
 * sin limite pasa los 65535 lotes (12,6 min a 2604 Hz). */
#define DATA32_PKT_MARKER 0xBD
struct MsgData32 {
    uint8_t     marker;
    uint8_t     node_id;
    uint32_t    seq;
    uint8_t     part;
    uint8_t     n_samples;
    uint64_t    timestamp_us;
    SampleBytes samples[SAMPLES_PER_PART];
    uint8_t     crc;
};

/* El maestro confirma estilo TCP: todo lo anterior a next_expected llego y se
 * entrego a la web; el bit i de sack dice que llego next_expected + 1 + i. El
 * esclavo libera lo confirmado y reenvia los huecos. */
struct MsgStreamAck {
    uint8_t  cmd;            /* CMD_STREAM_ACK */
    uint8_t  node_id;
    uint32_t next_expected;
    uint32_t sack;
};

#define STREAM_END_COMPLETE  0u  /* llego al N pedido */
#define STREAM_END_STOP      1u  /* STOP del usuario */
#define STREAM_END_NO_MEMORY 2u  /* el esclavo se quedo sin donde guardar */
#define STREAM_END_SD_FULL   3u
#define STREAM_END_ERROR     4u
struct MsgStreamEnd {
    uint8_t  cmd;            /* CMD_STREAM_END */
    uint8_t  node_id;
    uint32_t total;          /* lotes de la captura; el ultimo es total-1 */
    uint8_t  reason;         /* STREAM_END_* */
};

struct MsgStatus {
    uint8_t  cmd;
    uint8_t  node_id;
    uint32_t batches_ok;
    uint32_t batches_bad;
    uint32_t espnow_sent;
    uint32_t espnow_fail;
    uint8_t  psoc_ok;   /* 1=PSoC detectado, 0=no */
};

/* Fase 2: configuración individual por esclavo */
struct MsgSetConfig {
    uint8_t cmd;      /* CMD_SET_CONFIG */
    uint8_t node_id;
    uint8_t sub_cmd;  /* 0xA6=PGA, 0xA8=PGAout, 0xAA=IDAC manual, 0xB5=calibrar */
    uint8_t param;    /* normal; para 0xAA: bit7=signo, bits0..3=etapa */
    uint8_t param2;   /* 0 normal; para 0xAA: magnitud 0..255 */
};

struct MsgCfgAck {
    uint8_t cmd;      /* CMD_CFG_ACK */
    uint8_t node_id;
    uint8_t sub_cmd;
    uint8_t ok;       /* 1=PSoC confirmó, 0=sin respuesta */
};

struct MsgDebugNode {
    uint8_t cmd;      /* CMD_DEBUG_NODE */
    uint8_t node_id;
    uint8_t enable;   /* 1=activar ramp, 0=desactivar */
};

struct MsgView {
    uint8_t  cmd;     /* CMD_VIEW */
    uint8_t  node_id;
    uint16_t n;       /* lotes a capturar en disparo único */
};

struct MsgDebugPsoc {
    uint8_t cmd;      /* CMD_DEBUG_PSOC */
    uint8_t node_id;
    uint8_t enable;   /* 1=rampa PSoC ON, 0=OFF */
};

struct MsgHello {
    uint8_t  cmd;         /* CMD_HELLO */
    uint8_t  node_id;
    uint8_t  psoc_ok;     /* 1=PSoC detectado en boot, 0=no detectado */
    uint16_t sample_rate; /* ADC sample rate en Hz reportado por PSoC; 0=desconocido */
    uint8_t  hw_class;    /* SLAVE_HW_* reportado por el PSoC (GEO/HAMMER/UNKNOWN) */
    uint8_t  sd_present;  /* 1=módulo SD detectado en ese ESP32 (solo firmwares GEO
                            * con pines SD compilados), 0=sin SD/sin detectar. Campo
                            * agregado al final: HELLO viejos (sin este byte) llegan
                            * con menos bytes y espnow_rx.h ya tolera eso (memcpy
                            * parcial sobre un struct zero-init → sd_present=0). */
    uint8_t  link_model;  /* SLAVE_LINK_*; agregado al final para que maestros y
                            * esclavos anteriores sigan aceptando el prefijo. */
    uint8_t  ctl_status;  /* SLAVE_CTL_*; aditivo, solo lo llena la placa I2C. */
    uint8_t  pga_code;    /* código de PGA confirmado por el PSoC (0xFF = sin dato) */
    uint8_t  pgaout_code; /* código de PGAout confirmado (0xFF = no existe / sin dato) */
    uint32_t ram_batches; /* lotes que entran en RAM (PSoC + ESP) en una captura; aditivo */
    uint32_t sd_batches;  /* lotes que entran en la SD del ESP; 0 = sin SD; aditivo */
    uint8_t  reset_reason; /* esp_reset_reason() del ultimo arranque (9 = caida de
                            * tension); aditivo: detecta esclavos que se reinician */
    uint32_t uptime_s;     /* segundos desde el arranque; aditivo */
};

struct MsgSetRecLen {
    uint8_t  cmd;        /* CMD_SET_RECLEN */
    uint16_t n_batches;
};

struct MsgReqBatch {
    uint8_t  cmd;       /* CMD_REQ_BATCH */
    uint8_t  node_id;
    uint16_t batch_seq;
};

struct MsgScopeStart {
    uint8_t cmd;        /* CMD_SCOPE_START */
};

/* Modo de captura: bits 0-1 almacenamiento, bit 2 tiempo real. Viaja al final
 * de PRESTART; un PRESTART viejo de 3 bytes llega con modo 0 (el clasico). */
#define CAPMODE_STORAGE_MASK   0x03u
#define CAPMODE_STORAGE_PSOC   0u   /* solo RAM del PSoC */
#define CAPMODE_STORAGE_SHARED 1u   /* RAM del PSoC + RAM del ESP */
#define CAPMODE_STORAGE_SD     2u   /* RAM del PSoC + RAM del ESP + SD del ESP */
#define CAPMODE_REALTIME       0x04u
#define CAPMODE_STREAM_DUMP    0x08u /* asincronico: descarga por tiempo real al terminar */

struct MsgPrestart {
    uint8_t  cmd;       /* CMD_PRESTART */
    uint16_t n_batches; /* 0 en tiempo real = sin limite */
    uint8_t  mode;      /* CAPMODE_*; aditivo */
};

struct MsgHotWaitQuery {
    uint8_t cmd;        /* CMD_HOTWAIT_QUERY */
    uint8_t node_id;
};

struct MsgHotWaitAck {
    uint8_t  cmd;       /* CMD_HOTWAIT_ACK */
    uint8_t  node_id;
    uint8_t  ok;        /* 1=HOT_WAIT listo, 0=no listo */
    uint8_t  state;
    uint16_t n_batches;
};

struct MsgBlinkLed {
    uint8_t cmd;      /* CMD_BLINK_LED */
    uint8_t node_id;
};

struct MsgSaveEeprom {
    uint8_t cmd;      /* CMD_SAVE_EEPROM */
    uint8_t node_id;
};

struct MsgSelectStream {
    uint8_t cmd;      /* CMD_SELECT_STREAM */
    uint8_t node_id;
    uint8_t mode;     /* 0=crudo ADC, 1=filtrado FIR hardware */
};

#pragma pack(pop)

/* Marca de red: toda trama ESP-NOW del sistema lleva al final estos 2 bytes
 * (FNV-1a de "GeoNetwork", 0x1E5746E0). El que recibe descarta lo que no la
 * tiene y la saca antes de procesar, asi los structs no cambian. Sin esto, el
 * ESP-NOW de otro equipo al alcance se tomaba como maestro o como esclavo
 * (2026-10-23: un "esclavo 4" fantasma y el ARM_ACK mandado a una MAC ajena). */
#define GEONET_TAG0    0xE0u
#define GEONET_TAG1    0x46u
#define GEONET_TAG_LEN 2
#define ESPNOW_MAX_PAYLOAD 250

static inline bool geonetTagged(const uint8_t *d, int len)
{
    return len > GEONET_TAG_LEN && d[len - 2] == GEONET_TAG0 && d[len - 1] == GEONET_TAG1;
}

/* Copia la trama a buf con la marca; devuelve el largo o 0 si no entra. */
static inline int geonetWrap(uint8_t *buf, const uint8_t *data, int len)
{
    if (len < 0 || len + GEONET_TAG_LEN > ESPNOW_MAX_PAYLOAD) return 0;
    for (int i = 0; i < len; i++) buf[i] = data[i];
    buf[len] = GEONET_TAG0;
    buf[len + 1] = GEONET_TAG1;
    return len + GEONET_TAG_LEN;
}

/* ── OTA de esclavos ────────────────────────────────────────────────────────
 * El maestro guarda el .bin en su LittleFS y ofrece la actualizacion por
 * ESP-NOW; el esclavo se asocia como STA al AP del maestro, baja la imagen por
 * HTTP, verifica tamano y SHA-256 y reinicia en el slot nuevo. La imagen nueva
 * se confirma recien cuando vuelve a hablar con el maestro (si no, rollback).
 * Nunca durante una captura. */
#define CMD_OTA_OFFER   0x7A   /* maestro -> esclavo unicast */
#define CMD_OTA_STATUS  0x7B   /* esclavo -> maestro */
#define CMD_OTA_CONTROL 0x7C   /* maestro -> esclavo: abortar */

#define OTA_ST_IDLE        0u
#define OTA_ST_JOINING     1u   /* asociandose al AP del maestro */
#define OTA_ST_DOWNLOADING 2u
#define OTA_ST_VERIFYING   3u
#define OTA_ST_REBOOTING   4u
#define OTA_ST_PENDING     5u   /* imagen nueva corriendo, sin confirmar */
#define OTA_ST_CONFIRMED   6u
#define OTA_ST_FAILED      7u

#define OTA_ERR_NONE      0u
#define OTA_ERR_BUSY      1u   /* captura en curso */
#define OTA_ERR_WIFI      2u   /* no se pudo asociar al AP */
#define OTA_ERR_HTTP      3u
#define OTA_ERR_SIZE      4u
#define OTA_ERR_SHA       5u
#define OTA_ERR_FLASH     6u   /* Update.begin/write/end */
#define OTA_ERR_ABORTED   7u

#pragma pack(push, 1)
struct MsgOtaOffer {
    uint8_t  cmd;          /* CMD_OTA_OFFER */
    uint8_t  node_id;
    uint32_t session;      /* identifica la actualizacion en los estados */
    uint32_t size;         /* bytes del .bin */
    uint8_t  sha256[32];
    char     path[32];     /* ruta HTTP en el maestro, terminada en 0 */
};

struct MsgOtaStatus {
    uint8_t  cmd;          /* CMD_OTA_STATUS */
    uint8_t  node_id;
    uint32_t session;
    uint8_t  state;        /* OTA_ST_* */
    uint8_t  error;        /* OTA_ERR_* */
    uint32_t received;     /* bytes escritos */
    uint32_t size;
};

struct MsgOtaControl {
    uint8_t  cmd;          /* CMD_OTA_CONTROL */
    uint8_t  node_id;
    uint32_t session;
    uint8_t  action;       /* 1 = abortar */
};
#pragma pack(pop)

/* ── Lote comprimido sin perdida (geo_codec.h) ───────────────────────────────
 * Una sola trama por lote en tiempo real y descarga rapida: 30 muestras de 24
 * bits + marcas, ~24 B de datos contra 334 B de MsgData32. Se manda con largo
 * variable: MSG_DATAZ_HEADER + len bytes. crc = xor de cabecera (sin el crc)
 * y datos. */
#define DATAZ_PKT_MARKER 0xBE
#define MSG_DATAZ_HEADER 8
#define MSG_DATAZ_MAX    160
#pragma pack(push, 1)
struct MsgDataZ {
    uint8_t  marker;      /* DATAZ_PKT_MARKER */
    uint8_t  node_id;
    uint32_t seq;
    uint8_t  crc;
    uint8_t  len;         /* bytes de data */
    uint8_t  data[MSG_DATAZ_MAX];
};
#pragma pack(pop)

static inline uint8_t dataZCrc(const struct MsgDataZ *m)
{
    const uint8_t *p = (const uint8_t *)m;
    uint8_t c = 0;
    for (int i = 0; i < MSG_DATAZ_HEADER; i++) if (i != 6) c ^= p[i];
    for (int i = 0; i < m->len; i++) c ^= m->data[i];
    return c;
}
