#pragma once
/*
 * scope_pulse.h — Helpers de pulsos GPIO para osciloscopio (maestro).
 *
 * Incluir DESPUÉS de definir los macros SAMPLE_PULSE_* y DEBUG_HW_* (vienen de
 * platformio.ini con defaults en main.cpp). Todo se compila a nada si los pines
 * están deshabilitados (SAMPLE_PULSE_PIN<0 / DEBUG_HARDWARE=0).
 *
 * Nota: el pulso de START (`debugEspHardwareStartPulse`) es SOLO marcador de
 * osciloscopio; no participa en la sincronización real (esa va por ESP-NOW).
 */

#include <Arduino.h>

static volatile bool     g_debugHwActive = false;
static volatile uint32_t g_debugHwFallUs = 0;

static inline uint8_t samplePulseActiveLevel()
{
    return (SAMPLE_PULSE_IDLE == LOW) ? HIGH : LOW;
}

static inline uint8_t debugHwActiveLevel()
{
    return (DEBUG_HW_START_IDLE == LOW) ? HIGH : LOW;
}

static inline void samplePulseBegin()
{
#if SAMPLE_PULSE_PIN >= 0
    pinMode(SAMPLE_PULSE_PIN, OUTPUT);
    digitalWrite(SAMPLE_PULSE_PIN, SAMPLE_PULSE_IDLE);
#endif
}

static inline void debugEspHardwareBegin()
{
#if DEBUG_HARDWARE && DEBUG_HW_START_PIN >= 0
    pinMode(DEBUG_HW_START_PIN, OUTPUT);
    digitalWrite(DEBUG_HW_START_PIN, DEBUG_HW_START_IDLE);
#endif
}

static inline void samplePulse()
{
#if SAMPLE_PULSE_PIN >= 0
    digitalWrite(SAMPLE_PULSE_PIN, samplePulseActiveLevel());
#if SAMPLE_PULSE_US > 0
    delayMicroseconds(SAMPLE_PULSE_US);
#endif
    digitalWrite(SAMPLE_PULSE_PIN, SAMPLE_PULSE_IDLE);
#endif
}

static inline void debugEspHardwareStartPulse()
{
#if DEBUG_HARDWARE && DEBUG_HW_START_PIN >= 0
    digitalWrite(DEBUG_HW_START_PIN, debugHwActiveLevel());
#if DEBUG_HW_START_US > 0
    g_debugHwFallUs = (uint32_t)micros() + (uint32_t)DEBUG_HW_START_US;
    g_debugHwActive = true;
#else
    digitalWrite(DEBUG_HW_START_PIN, DEBUG_HW_START_IDLE);
    g_debugHwActive = false;
#endif
#endif
}

static inline void debugEspHardwareService()
{
#if DEBUG_HARDWARE && DEBUG_HW_START_PIN >= 0
    if (g_debugHwActive &&
        (int32_t)((uint32_t)micros() - g_debugHwFallUs) >= 0) {
        digitalWrite(DEBUG_HW_START_PIN, DEBUG_HW_START_IDLE);
        g_debugHwActive = false;
    }
#endif
}
