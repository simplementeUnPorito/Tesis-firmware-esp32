#pragma once
/*
 * ota_slave.h — actualizacion de firmware del esclavo por OTA.
 *
 * El maestro ofrece la imagen (CMD_OTA_OFFER) y el esclavo, fuera de captura:
 *   1. se asocia como STA al AP del maestro (mismo canal que ESP-NOW),
 *   2. baja el .bin por HTTP y lo escribe con Update mientras calcula SHA-256,
 *   3. verifica tamano y hash, marca el slot nuevo y reinicia.
 * Al arrancar la imagen nueva queda pendiente: se confirma cuando vuelve a
 * recibir algo del maestro; si no lo logra en OTA_CONFIRM_TIMEOUT_MS vuelve a
 * la imagen anterior (si el bootloader tiene rollback habilitado).
 * Cada paso se informa con CMD_OTA_STATUS.
 */
#include <Arduino.h>
#include "sync_protocol.h"

#ifndef OTA_ENABLE
#define OTA_ENABLE 1
#endif

namespace ota {

typedef esp_err_t (*SendFn)(const uint8_t *data, size_t len);

/* Llamar en setup(), despues de tener ESP-NOW. */
void begin(SendFn send, uint8_t nodeId, const char *apSsid, const char *apPass);
/* Oferta del maestro; busy = hay una captura en curso. */
void onOffer(const MsgOtaOffer &offer, bool busy);
void onControl(const MsgOtaControl &ctl);
/* Hubo trafico valido del maestro: confirma una imagen pendiente. */
void onMasterAlive();
/* Llamar en cada vuelta de loop(); la descarga bloquea mientras dura. */
void service();
/* true mientras hay una actualizacion en curso: no aceptar capturas. */
bool busy();
void printStatus();

}  // namespace ota
