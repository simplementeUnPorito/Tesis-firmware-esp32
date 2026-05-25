#pragma once

#include <Arduino.h>

#ifndef MASTER_SERIAL_LOG
#define MASTER_SERIAL_LOG 0
#endif

#if MASTER_SERIAL_LOG
#define MASTER_LOG_PRINT(...)   Serial.print(__VA_ARGS__)
#define MASTER_LOG_PRINTLN(...) Serial.println(__VA_ARGS__)
#define MASTER_LOG_PRINTF(...)  Serial.printf(__VA_ARGS__)
#else
#define MASTER_LOG_PRINT(...)   do {} while (0)
#define MASTER_LOG_PRINTLN(...) do {} while (0)
#define MASTER_LOG_PRINTF(...)  do {} while (0)
#endif
