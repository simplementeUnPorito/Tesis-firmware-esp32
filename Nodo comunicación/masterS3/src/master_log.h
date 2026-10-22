#pragma once
/*
 * master_log.h — shim de compatibilidad.
 *
 * El logging real vive en debug_log.h (sistema unificado humano+máquina,
 * gateado por DBG_ENABLE). Las macros MASTER_LOG_* se definen allí.
 * El stream del maestro es Serial1 (definir -DDBG_STREAM=Serial1 en
 * platformio.ini) para no corromper el binario 0x56 hacia MATLAB por Serial.
 */

#include "debug_log.h"
