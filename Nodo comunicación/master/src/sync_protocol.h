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
#define CMD_DATA       0x20
#define CMD_SET_CONFIG 0x21   /* maestro → esclavo unicast: configurar parámetro */
#define CMD_CFG_ACK    0x22   /* esclavo → maestro: confirmación de config */
#define CMD_DEBUG_NODE 0x23   /* maestro → esclavo unicast: debug ramp individual */
#define CMD_STATUS     0x30

#pragma pack(push, 1)

struct MsgArm    { uint8_t cmd; };
struct MsgArmAck { uint8_t cmd; uint8_t node_id; uint8_t status; };
struct MsgStart  { uint8_t cmd; uint64_t t_start_us; };
struct MsgStop   { uint8_t cmd; };
struct MsgDebug  { uint8_t cmd; uint8_t enable; };

#define DATA_PKT_MARKER  0xBC
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

struct MsgStatus {
    uint8_t  cmd;
    uint8_t  node_id;
    uint32_t batches_ok;
    uint32_t batches_bad;
    uint32_t espnow_sent;
    uint32_t espnow_fail;
};

/* Fase 2: configuración individual por esclavo */
struct MsgSetConfig {
    uint8_t cmd;      /* CMD_SET_CONFIG */
    uint8_t node_id;
    uint8_t sub_cmd;  /* 0xA6=PGA, 0xA8=TXmode, 0xA9=PGAvdac, 0xAA=VDAC */
    uint8_t param;
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

#pragma pack(pop)
