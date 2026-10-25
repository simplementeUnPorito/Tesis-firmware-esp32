#pragma once

#if defined(SIM_SIN_HW) && SIM_SIN_HW

#include <stddef.h>
#include <stdint.h>

struct PsocBatch;
struct SampleBytes;

/* Modelo de muestras:
 *   post_analog = indice global de muestra, modulo 24 bits.
 *   post_digital = 2000*sin(12 Hz) + 1000*sin(37 Hz) + ruido LCG (+-256).
 *   raw_input = los 16 bits bajos, con signo, de post_digital.
 *
 * Los frames wire siguen llevando un unico int24, exactamente como el PSoC.
 * simPsocDecorateBatch() completa post_analog despues de que el parser real
 * haya validado y decodificado ese int24. Los artefactos conocidos se suman
 * cuando sus flags se aplican: I2C +3000 decreciente (1 ms), SD_IO +8000 en
 * rampa (5 ms), RADIO_TX +6000 (0,4 ms) y AP_BEACON +2000 (2 ms).
 */
void simPsocBegin();
void simPsocService();
void simPsocWrite(const uint8_t *data, size_t len);
void simPsocDecorateBatch(PsocBatch &batch);
void simPsocApplyNoiseArtifact(SampleBytes &sample, uint8_t flags,
                               uint32_t sampleIndex);
void simPsocSetEnabled(bool enabled);
bool simPsocEnabled();
void simPsocPrintState();

#endif
