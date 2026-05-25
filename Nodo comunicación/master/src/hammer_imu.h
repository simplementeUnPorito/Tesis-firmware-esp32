#pragma once
/*
 * hammer_imu.h — Muestreo de aceleración del martillo via IMU.
 *
 * El IMU (acelerómetro + giroscopio) se conecta directamente al ESP maestro
 * y mide la aceleración del impacto del martillo.
 *
 * HAMMER_IMU_ENABLED=0  — stub: retorna 0 siempre (sin hardware)
 * HAMMER_IMU_ENABLED=1  — real: leer IMU via I2C/SPI (TODO: implementar driver)
 *
 * Cadencia de muestreo: HAMMER_SAMPLE_US → ~1020 Hz
 */

#include <Arduino.h>
#include <stdint.h>
#include "master_log.h"

#ifndef HAMMER_IMU_ENABLED
  #define HAMMER_IMU_ENABLED 0
#endif

#define HAMMER_SAMPLE_US 980   /* ~1020 Hz */

class HammerIMU {
public:
    void begin()
    {
#if HAMMER_IMU_ENABLED
        /* TODO: inicializar bus I2C o SPI y configurar el IMU.
         * Ejemplo MPU-6050 (I2C, dirección 0x68):
         *   Wire.begin(SDA_PIN, SCL_PIN);
         *   Wire.beginTransmission(0x68);
         *   Wire.write(0x6B); Wire.write(0x00);   // salir de sleep
         *   Wire.endTransmission();
         */
#endif
        MASTER_LOG_PRINTF("[IMU] HAMMER_IMU_ENABLED=%d\n", HAMMER_IMU_ENABLED);
    }

    /* Retorna la aceleración del eje de impacto como valor 24-bit signed.
     * Llamar con cadencia HAMMER_SAMPLE_US desde loop(). */
    int32_t readAccel()
    {
#if HAMMER_IMU_ENABLED
        /* TODO: leer eje Z (o el eje de impacto según montaje) del IMU.
         * Ejemplo MPU-6050:
         *   Wire.beginTransmission(0x68);
         *   Wire.write(0x3F);   // ACCEL_ZOUT_H
         *   Wire.endTransmission(false);
         *   Wire.requestFrom(0x68, 2);
         *   int16_t raw = (Wire.read() << 8) | Wire.read();
         *   return (int32_t)raw;
         */
        return 0;
#else
        return 0;   /* stub */
#endif
    }
};
