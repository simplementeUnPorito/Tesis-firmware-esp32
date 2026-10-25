#pragma once
/*
 * live_capture.h — captura en vivo: el PSoC pasa cada lote mientras muestrea
 * y el ESP lo guarda (RAM y, si se pide, SD) y/o lo transmite.
 *
 * Almacenamiento (CAPMODE_STORAGE_*):
 *   PSOC    solo la RAM del PSoC (512 lotes). El asincronico clasico no pasa
 *           por aca; en los modos stream el PSoC guarda todo y el ESP le pide
 *           de nuevo los lotes que necesita.
 *   SHARED  RAM del PSoC + RAM del ESP. El ESP retiene hasta que el maestro
 *           confirma; sin lugar, el PSoC retiene en su anillo.
 *   SD      ademas, todo queda en /live.bin de la SD del ESP.
 *
 * Contadores (lotes, 32 bits, crecen siempre):
 *   recv    proximo lote que se espera del PSoC; todo lo anterior llego.
 *   tail    lote mas viejo que sigue en la RAM del ESP.
 *   sdw     lotes ya escritos en la SD.
 *   txNext  proximo lote nuevo a transmitir por el camino stream.
 *   mack    proximo lote que el maestro espera; todo lo anterior le llego.
 */
#include <Arduino.h>
#include "sync_protocol.h"
#include "psoc_uart.h"

namespace live {

typedef esp_err_t (*RawSendFn)(const uint8_t *data, size_t len);

struct Config {
    uint8_t  storage;   /* CAPMODE_STORAGE_* */
    bool     realtime;
    bool     streamDump;
    uint32_t nBatches;  /* 0 = sin limite (solo tiempo real) */
    bool     ramp;      /* banco: el PSoC manda el indice de muestra */
};

void begin(PsocUART *psoc, RawSendFn send, uint8_t nodeId);
void setSdHz(uint32_t hz);
uint32_t sdHz();
void setRadioReady(bool ready);

/* Reserva memoria y abre la SD. Devuelve el N efectivo (puede recortar al
 * almacenamiento disponible) o -1 si no hay donde guardar. */
int32_t prepare(const Config &c);
void close();
bool active();
const Config &config();

void start(uint64_t tStartUs, uint32_t fsHz);
/* Convierte el intervalo de reloj ESP a muestras. Seguro desde callbacks y tareas. */
void noteEvent(uint8_t bits, uint64_t t0Us, uint64_t t1Us);
void noteRadioTxStart(size_t len);
void noteRadioTxDone(uint64_t t1Us);
bool latestAnchor(uint32_t &sample, uint64_t &espUs, uint32_t &generation);
void onPsocBatch(const PsocBatch &b);
void onPsocTotalByte(uint8_t index, uint8_t value);  /* 1 y 2: TOTAL1/TOTAL2 */
void onPsocDone(uint8_t lowByte);
void onPsocOverrun();
void onMasterAck(uint32_t nextExpected, uint32_t sack);
void requestStop();
/* Asincronico: el maestro pide el lote seq. false = todavia no esta o no
 * existe (el que llama manda NACK). */
bool serveReq(uint32_t seq, bool compressed);
void service();

bool psocDone();
/* ms sin ningun STREAM_ACK del maestro mientras se transmite (0 si no aplica). */
uint32_t msSinceMasterAck();
uint32_t total();
bool finished();       /* listo para avisar al maestro */
uint8_t endReason();   /* STREAM_END_* */
bool wantsStop();      /* sin memoria o SD llena: hay que bajar SYNC */

uint32_t ramCapacityBatches();  /* PSoC + ESP, para el HELLO */
uint32_t sdCapacityBatches();   /* 0 sin SD */
void printStatus();

/* Banco */
void simMaster(bool on, uint8_t lossPct);
bool simEnabled();
void dumpUsb(bool hex);
}
