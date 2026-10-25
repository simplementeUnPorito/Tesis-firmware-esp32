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
 * En S3 / IDF 5 el modelo permite programar START contra el reloj del maestro.
 * En IDF 4.4 no hay FTM y se usa el respaldo de recepcion inmediata.
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
/* Turno fijo por nodo: con la baliza del maestro, una sesion por ventana de
 * 1 s cada 8 ventanas, para no superponer sesiones entre esclavos. */
void setTurn(uint8_t nodeId);

/* ── Modelo de reloj (solo S3 / IDF 5) ─────────────────────────────────────
 * theta = reloj MAC local - reloj MAC del maestro (us, modulo 2^32), ajustado
 * por recta sobre las ultimas sesiones FTM. La baliza del maestro solo resuelve
 * la epoca de T1/T4 (48 bits de ps). */
void noteMasterBeacon(uint32_t masterMacUs, uint32_t localRxMacUs);
bool modelOk();
uint32_t modelAgeMs();
/* Convierte un instante del reloj MAC del maestro en un plazo del esp_timer
 * local. false si el modelo no es valido o el instante no esta entre 50 ms y
 * 5 s en el futuro. */
bool masterMacToLocalEsp(uint32_t masterMacUs, int64_t &deadlineEsp, uint32_t &uncertNs);
/* Instante local (esp_timer) expresado en el reloj MAC del maestro. */
bool localEspToMasterMac(int64_t esp, uint32_t &masterMacUs);
/* Ultima deriva y residuo aceptados. Siguen disponibles si el modelo envejece,
 * para que el reloj de muestreo mantenga la ultima disciplina durante captura. */
int32_t modelSkewPpb();
uint32_t modelResidualNs();

}  // namespace ftm
