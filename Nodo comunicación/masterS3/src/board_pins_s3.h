#pragma once
/* Delta de placa ESP32-S3 N16R8. Este archivo se fuerza con platformio.ini. */
#if !defined(ARDUINO_ESP32S3_DEV)
#error "masterS3 requiere ESP32-S3; usar master para ESP32 clasico"
#endif

#define SYNC_OUT_PIN 7
