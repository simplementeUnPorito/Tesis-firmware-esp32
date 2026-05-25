#pragma once
/*
 * hammer_adc.h — Muestreo del martillo y acelerómetro en el ESP maestro.
 *
 * HAMMER_BTN_ENABLED  (0/1)  — Martillo trigger por botón / salida PCB
 * HAMMER_ACCEL_ENABLED (0/1) — Canal acelerómetro analógico
 *
 * Cuando ambos flags son 0 (PoC sin hardware), las funciones retornan
 * constantes fijas para que el protocolo y MATLAB puedan desarrollarse
 * sin hardware conectado.
 *
 * Cuando se active el hardware real:
 *   - La salida del martillo PCB (~±5V) requiere un divisor de tensión
 *     y un offset de +1.65 V para llevarla al rango 0–3.3 V del ADC.
 *   - El acelerómetro analógico ídem.
 *   - Reemplazar los #if 0 por la lógica de ESP32 ADC correspondiente.
 *
 * Protocolo hacia MATLAB: cada muestra del maestro se envía como
 *   [0x56][0x00][type_hammer][b2][b1][b0]  con node_id=0x00.
 */

#include <Arduino.h>
#include <stdint.h>
#include "master_log.h"

#ifndef HAMMER_BTN_ENABLED
  #define HAMMER_BTN_ENABLED   0
#endif
#ifndef HAMMER_ACCEL_ENABLED
  #define HAMMER_ACCEL_ENABLED 0
#endif

/* Pines (solo usados cuando el flag correspondiente es 1) */
#define HAMMER_BTN_PIN   34   /* ADC1_CH6 — divisor de tensión de la salida PCB */
#define HAMMER_ACCEL_PIN 35   /* ADC1_CH7 — señal acelerómetro */
#define HAMMER_SAMPLE_US 980  /* ~1020 Hz de muestreo */

struct HammerSample {
    int32_t  btn_raw;    /* fuerza del impacto (stub: cte) */
    int32_t  accel_raw;  /* aceleración (stub: cte) */
    uint64_t timestamp_us;
};

class HammerADC {
public:
    void begin()
    {
#if HAMMER_BTN_ENABLED
        analogReadResolution(12);
        pinMode(HAMMER_BTN_PIN, INPUT);
#endif
#if HAMMER_ACCEL_ENABLED
        pinMode(HAMMER_ACCEL_PIN, INPUT);
#endif
        MASTER_LOG_PRINTF("[HAMMER] BTN=%d ACCEL=%d\n",
                          HAMMER_BTN_ENABLED, HAMMER_ACCEL_ENABLED);
    }

    /* Leer una muestra; llamar desde el loop con cadencia HAMMER_SAMPLE_US */
    HammerSample read()
    {
        HammerSample s;
        s.timestamp_us = (uint64_t)micros();

#if HAMMER_BTN_ENABLED
        int raw = analogRead(HAMMER_BTN_PIN);
        /* Convertir 0–4095 a valor centrado en 0 (referencia = mitad escala) */
        s.btn_raw = (int32_t)raw - 2048;
#else
        s.btn_raw = 0;   /* stub: 0 en reposo; el maestro inyecta rampa en modo test */
#endif

#if HAMMER_ACCEL_ENABLED
        int rawA = analogRead(HAMMER_ACCEL_PIN);
        s.accel_raw = (int32_t)rawA - 2048;
#else
        s.accel_raw = 0;    /* stub plano */
#endif
        return s;
    }

    /* Detección de impacto: retorna true si la muestra supera el umbral.
     * threshold_counts: amplitud mínima para considerar un golpe. */
    bool isImpact(const HammerSample &s, int32_t threshold_counts = 512) const
    {
#if HAMMER_BTN_ENABLED || HAMMER_ACCEL_ENABLED
        return (s.btn_raw > threshold_counts || s.accel_raw > threshold_counts);
#else
        (void)s; (void)threshold_counts;
        return false;   /* stub: nunca detecta impacto */
#endif
    }
};
