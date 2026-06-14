#pragma once
/*
 * sync_protocol.h - Mensajes ESP-NOW entre maestro y esclavos.
 *
 * Mantener sincronizado con el proyecto master.
 * Todos los structs son packed para serializacion directa.
 * Tamano maximo ESP-NOW: 250 bytes.
 */

#include <stdint.h>

#define CMD_ARM        0x10
#define CMD_ARM_ACK    0x11
#define CMD_START      0x12
#define CMD_STOP       0x13
#define CMD_DEBUG      0x14
#define CMD_START_ACK  0x15   /* esclavo → maestro: recibió CMD_START */
#define CMD_DATA       0x20
#define CMD_SET_CONFIG 0x21
#define CMD_CFG_ACK    0x22
#define CMD_DEBUG_NODE 0x23
#define CMD_VIEW       0x24   /* maestro → esclavo: captura única "Ver" (N lotes) */
#define CMD_DEBUG_PSOC 0x25   /* maestro → esclavo: rampa de debug del PSoC */
#define CMD_STATUS     0x30
#define CMD_HELLO      0x40   /* beacon diagnóstico: esclavo → maestro */
#define CMD_SET_RECLEN 0x50   /* master → todos: cuántos batches grabar (respuesta: CMD_CFG_ACK sub=0x50) */
#define CMD_REQ_BATCH  0x51   /* master → esclavo: pedir batch específico (respuesta: 2× MsgData) */
#define CMD_SCOPE_START 0x60  /* master → esclavos: pulso START falso, sin muestreo ni ACK */
#define CMD_PRESTART    0x61  /* master → todos: entrar en HOT_WAIT con n_batches listo */
#define CMD_HOTWAIT_QUERY 0x62 /* master → esclavo: consultar si está en HOT_WAIT */
#define CMD_HOTWAIT_ACK 0x63  /* esclavo → master: confirma HOT_WAIT */

#pragma pack(push, 1)

struct MsgArm {
    uint8_t cmd;
};

struct MsgArmAck {
    uint8_t cmd;
    uint8_t node_id;
    uint8_t status;
};

struct MsgStart {
    uint8_t  cmd;
    uint64_t t_start_us;
    uint8_t  target_node;
};

struct MsgStartAck {
    uint8_t  cmd;
    uint8_t  node_id;
    uint8_t  status;       /* 0=ignorado, 1=start real, 2=probe */
    uint32_t start_token;  /* low32(t_start_us), para asociar ACK con envío */
    uint32_t rx_us;
};

struct MsgStop {
    uint8_t cmd;
};

struct MsgDebug {
    uint8_t cmd;
    uint8_t enable;
};

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
    uint8_t  psoc_ok;   /* 1=PSoC detectado, 0=no */
};

struct MsgSetConfig {
    uint8_t cmd;
    uint8_t node_id;
    uint8_t sub_cmd;  /* 0xA6=PGA, 0xB5=calibrar PSoC; 0xA9/0xAA legacy */
    uint8_t param;
};

struct MsgCfgAck {
    uint8_t cmd;
    uint8_t node_id;
    uint8_t sub_cmd;
    uint8_t ok;
};

struct MsgDebugNode {
    uint8_t cmd;
    uint8_t node_id;
    uint8_t enable;
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

struct MsgPrestart {
    uint8_t  cmd;       /* CMD_PRESTART */
    uint16_t n_batches;
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

#pragma pack(pop)
