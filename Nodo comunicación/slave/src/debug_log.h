#pragma once
/*
 * debug_log.h — Sistema de logging unificado (humano + máquina).
 *
 * Dos flujos, ambos por el mismo stream serie (DBG_STREAM):
 *   - HUMANO  : "[<t_us>][ROLE] <texto>"          -> Log tab de MATLAB
 *   - MÁQUINA : "#M,<t_us>,<ROLE>,<evento>,<...>" -> log máquina (todos los datos)
 *
 * Gate de compilación (definir en platformio.ini):
 *   -DSLAVE_LOGS_ENABLE=1 habilita el logging del esclavo
 *   -DDBG_ENABLE=1        alias interno; 0 = TODO compila a do{}while(0), 0 CPU
 *   -DDBG_HUMAN=1    (opcional) flujo humano   (1 por defecto)
 *   -DDBG_MACHINE=1  (opcional) flujo máquina  (1 por defecto)
 *   -DDBG_STREAM=Serial1   stream destino (maestro=Serial1; esclavo=Serial)
 *
 * ROLE se deriva solo: si NODE_ID está definido -> "S<NODE_ID>", si no -> "M".
 *
 * Compatibilidad: se mantienen las macros SLAVE_LOG_* / MASTER_LOG_* usadas en
 * el código existente; ahora llevan timestamp+rol automáticamente.
 */

#include <Arduino.h>

#ifndef DBG_ENABLE
  #ifdef SLAVE_LOGS_ENABLE
    #define DBG_ENABLE SLAVE_LOGS_ENABLE
  #else
    #define DBG_ENABLE 0
  #endif
#endif
#ifndef DBG_HUMAN
#define DBG_HUMAN 1
#endif
#ifndef DBG_MACHINE
#define DBG_MACHINE 1
#endif
#ifndef DBG_STREAM
#define DBG_STREAM Serial
#endif

/* Pines/baud del stream de log secundario (usado por el maestro en Serial1). */
#ifndef DBG_LOG_BAUD
#define DBG_LOG_BAUD 115200
#endif
#ifndef DBG_LOG_RX
#define DBG_LOG_RX 16
#endif
#ifndef DBG_LOG_TX
#define DBG_LOG_TX 17
#endif

#ifndef LOG_ROLE
  #if defined(NODE_ID)
    #define DBG_STR2(x) #x
    #define DBG_STR(x)  DBG_STR2(x)
    #define LOG_ROLE "S" DBG_STR(NODE_ID)
  #else
    #define LOG_ROLE "M"
  #endif
#endif

#if DBG_ENABLE

  #define DBG_PREFIX() DBG_STREAM.printf("[%lu][" LOG_ROLE "] ", (unsigned long)micros())

  #if DBG_HUMAN
    /* Línea humana con \n automático. Usar para texto legible. */
    #define LOGH(...)   do { DBG_PREFIX(); DBG_STREAM.printf(__VA_ARGS__); DBG_STREAM.print('\n'); } while (0)
    /* Compat: el formato legado ya trae su propio \n. */
    #define MASTER_LOG_PRINTF(...)  do { DBG_PREFIX(); DBG_STREAM.printf(__VA_ARGS__); } while (0)
    #define MASTER_LOG_PRINTLN(...) do { DBG_PREFIX(); DBG_STREAM.println(__VA_ARGS__); } while (0)
    #define MASTER_LOG_PRINT(...)   DBG_STREAM.print(__VA_ARGS__)
  #else
    #define LOGH(...)               do {} while (0)
    #define MASTER_LOG_PRINTF(...)  do {} while (0)
    #define MASTER_LOG_PRINTLN(...) do {} while (0)
    #define MASTER_LOG_PRINT(...)   do {} while (0)
  #endif

  #if DBG_MACHINE
    /* Registro máquina: tag = evento corto; el resto son campos k=v. */
    #define LOGM(tag, ...) do { \
        DBG_STREAM.printf("#M,%lu," LOG_ROLE ",%s,", (unsigned long)micros(), tag); \
        DBG_STREAM.printf(__VA_ARGS__); DBG_STREAM.print('\n'); } while (0)
  #else
    #define LOGM(...) do {} while (0)
  #endif

  static inline void dbgLogBegin(unsigned long baud) { DBG_STREAM.begin(baud); }

#else  /* DBG_ENABLE == 0 : todo fuera del compilado */

  #define LOGH(...)               do {} while (0)
  #define LOGM(...)               do {} while (0)
  #define MASTER_LOG_PRINTF(...)  do {} while (0)
  #define MASTER_LOG_PRINTLN(...) do {} while (0)
  #define MASTER_LOG_PRINT(...)   do {} while (0)
  static inline void dbgLogBegin(unsigned long) {}

#endif

/* Alias esclavo (mismas semánticas) */
#define SLAVE_LOG_PRINTF(...)  MASTER_LOG_PRINTF(__VA_ARGS__)
#define SLAVE_LOG_PRINTLN(...) MASTER_LOG_PRINTLN(__VA_ARGS__)
