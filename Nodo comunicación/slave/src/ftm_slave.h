#pragma once
/*
 * ftm_slave.h — sesiones FTM (802.11mc) del esclavo contra el AP del maestro.
 *
 * El esclavo es el iniciador (STA) y el softAP del maestro el responder. Cada
 * sesion entrega hasta 16 tramas con T1..T4; de ahi salen:
 *   - RTT = (T4-T1) - (T3-T2), que no depende del origen de los relojes y da
 *     la estimacion de distancia (sesgada: ver docs/diseno_s3/OTA_FTM_DISENO.md §3);
 *   - theta = ((T2-T1) + (T3-T4)) / 2, el desfasaje entre los relojes MAC. Su
 *     pendiente contra el reloj local es la deriva (ppb) y su dispersion la
 *     calidad de sincronia, ambas independientes del origen de cada reloj.
 * El origen de T1/T4 respecto del esp_timer del maestro todavia no esta medido
 * en IDF 4.4, asi que el START no se programa con este modelo: solo se informa.
 *
 * Solo los S3 traen FTM; en el ESP32 clasico todo compila y status() da
 * FTM_ST_UNSUPPORTED.
 */
#include <Arduino.h>
#include "sync_protocol.h"

namespace ftm {

void begin();
/* Llamar en cada loop(). allowed=false suspende las sesiones nuevas (captura en
 * modo pausado o radio apagada, OTA, radio apagada). */
void service(bool allowed, const uint8_t responder[6], uint8_t channel);
/* Llena el resumen para mandarlo al maestro. false si no hay nada nuevo que
 * informar desde la ultima llamada. */
bool takeReport(MsgFtmReport &out, uint8_t nodeId, uint8_t syncMode);
/* Imprime por USB el estado y, si raw, la ultima sesion trama por trama. */
void printStatus(bool raw);
/* Periodo entre sesiones (ms, 0 = apagado). */
void setPeriodMs(uint32_t ms);

}  // namespace ftm
