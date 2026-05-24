#pragma once
/*
 * sync_protocol.h — Mensajes ESP-NOW entre maestro y esclavos.
 *
 * Todos los structs son packed para serialización directa.
 * Tamaño máximo ESP-NOW: 250 bytes.
 */

#include <stdint.h>

/* ── IDs de comando ──────────────────────────────────────────────────────── */
#define CMD_ARM       0x10   /* Maestro → Esclavo: armarse */
#define CMD_ARM_ACK   0x11   /* Esclavo → Maestro: listo */
#define CMD_START     0x12   /* Maestro → Esclavo: iniciar muestreo */
#define CMD_STOP      0x13   /* Maestro → Esclavo: detener */
#define CMD_DEBUG     0x14   /* Maestro → Esclavo: activar/desactivar rampa debug */
#define CMD_DATA      0x20   /* Esclavo → Maestro: paquete de datos */
#define CMD_STATUS    0x30   /* Esclavo → Maestro: informe de estado */

/* ── Mensajes de control ─────────────────────────────────────────────────── */

#pragma pack(push, 1)

struct MsgArm {
    uint8_t  cmd;          /* CMD_ARM */
};

struct MsgArmAck {
    uint8_t  cmd;          /* CMD_ARM_ACK */
    uint8_t  node_id;
    uint8_t  status;       /* 0 = OK, 1 = no PSoC, 2 = error */
};

struct MsgStart {
    uint8_t  cmd;          /* CMD_START */
    uint64_t t_start_us;   /* micros() del maestro al disparar */
};

struct MsgStop {
    uint8_t  cmd;          /* CMD_STOP */
};

struct MsgDebug {
    uint8_t  cmd;    /* CMD_DEBUG */
    uint8_t  enable; /* 1 = debug on (rampa), 0 = debug off (PSoC real) */
};

/* ── Paquete de datos (esclavo → maestro) ────────────────────────────────── */
/*
 * Un batch de 30 muestras = 306 bytes > 250 bytes límite ESP-NOW.
 * Se fragmenta en 2 partes de 15 muestras cada una.
 * part=0: muestras 0-14,  part=1: muestras 15-29.
 *
 * Tamaño: 1+1+2+1+1+8+15*10+1 = 165 bytes  (<250 ✓)
 */
#define DATA_PKT_MARKER  0xBC
#define SAMPLES_PER_PART 15

struct SampleBytes {           /* 10 bytes (mismo layout que SPI frame) */
    uint8_t raw_lo, raw_hi;
    uint8_t alog0, alog1, alog2;
    uint8_t digi0, digi1, digi2;
    uint8_t gain;
    uint8_t flags;
};

struct MsgData {
    uint8_t     marker;                      /* DATA_PKT_MARKER = 0xBC */
    uint8_t     node_id;
    uint16_t    seq;                         /* número de batch */
    uint8_t     part;                        /* 0 o 1 */
    uint8_t     n_samples;                   /* siempre SAMPLES_PER_PART = 15 */
    uint64_t    timestamp_us;               /* micros desde T_start del maestro */
    SampleBytes samples[SAMPLES_PER_PART];  /* 150 bytes */
    uint8_t     crc;                         /* XOR de todos los bytes anteriores */
};  /* total: 165 bytes */

struct MsgStatus {
    uint8_t  cmd;            /* CMD_STATUS */
    uint8_t  node_id;
    uint32_t batches_ok;
    uint32_t batches_bad;
    uint32_t espnow_sent;
    uint32_t espnow_fail;
};

#pragma pack(pop)
