#pragma once
/*
 * sd_storage.h - SD opcional para esclavos GEO (docs/plan_2929_decimation_sd.md H5).
 *
 * API siempre presente y siempre segura de llamar, en CUALQUIER firmware de
 * esclavo (HAMMER incluido): si SD_SPI_CS_PIN no está definido en el build
 * (ver slave/platformio.ini — solo lo definen los envs slave2/slave3, los
 * GEO), sd_storage.cpp compila la variante stub de abajo y todas estas
 * funciones son no-op/false. Esto evita llenar main.cpp de #ifdef: el mismo
 * código fuente sirve para HAMMER (sin SD, comportamiento 100% inalterado)
 * y GEO (con SD, si el módulo está soldado y detectado).
 *
 * Diseño de la sesión (derrame TOTAL, no ring RAM+SD): una captura entra
 * ENTERA en RAM (como hoy, hasta PSOC_CAPTURE_MAX_BATCHES) o, si se pide más
 * de eso y la SD está habilitada+presente, se graba ENTERA en un único
 * archivo de la SD (sdStorageBeginSession/WriteBatch/ReadBatch/EndSession).
 * No hay un tercer modo mixto: mantiene el modelo mental simple y evita
 * lógica de derrame incremental sin poder probarla en banco esta sesión.
 */

#include <stdint.h>
#include "sync_protocol.h"   /* SampleBytes */

void     sdStorageBegin();                 /* llamar una vez en setup() */
bool     sdStoragePresent();                /* true si SD.begin() detectó tarjeta */
bool     sdStorageEnabled();                /* estado actual del toggle (default: false) */
bool     sdStorageSetEnabled(bool enable);  /* false si enable=true y !present */
uint32_t sdStorageMaxBatches();             /* 0 si no present/enabled; approx por tamaño de tarjeta */

bool     sdStorageBeginSession(uint16_t total_batches);
bool     sdStorageWriteBatch(uint16_t seq, const SampleBytes *samples, uint32_t ts_us);
bool     sdStorageReadBatch(uint16_t seq, SampleBytes *outSamples, uint32_t *outTsUs);
void     sdStorageEndSession();
