#pragma once
/*
 * Compatibilidad ESP-NOW para ESP32 y ESP8266.
 *
 * El ESP8266 usa <espnow.h> y una API distinta a la del ESP32. Estos helpers
 * mantienen el resto del esclavo con una sola llamada para enviar/agregar peers.
 */

#include <Arduino.h>
#include "sync_protocol.h"

namespace live {
void noteRadioTxStart(size_t len);
void noteRadioTxDone(uint64_t t1Us);
}

#if defined(ESP8266)
  #include <ESP8266WiFi.h>
  extern "C" {
    #include <espnow.h>
    #include <user_interface.h>
  }
  #ifndef ESP_OK
    #define ESP_OK 0
  #endif
  typedef uint8_t esp_err_t;

  static inline uint8_t espnowCurrentChannel()
  {
      uint8_t ch = wifi_get_channel();
      return ch ? ch : 1;
  }

  static inline bool espnowInitOk()
  {
      return esp_now_init() == ESP_OK;
  }

  static inline void espnowSetRole()
  {
      esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  }

  static inline bool espnowAddPeer(const uint8_t *mac)
  {
      if (esp_now_is_peer_exist((uint8_t *)mac)) return true;
      return esp_now_add_peer((uint8_t *)mac, ESP_NOW_ROLE_COMBO,
                              espnowCurrentChannel(), nullptr, 0) == ESP_OK;
  }

  static inline esp_err_t espnowSend(const uint8_t *mac, const uint8_t *data, size_t len)
  {
      uint8_t buf[ESPNOW_MAX_PAYLOAD];
      const int n = geonetWrap(buf, data, (int)len);
      if (n == 0) return 1;
      const esp_err_t err = esp_now_send((uint8_t *)mac, buf, (uint8_t)n);
      if (err == ESP_OK) live::noteRadioTxStart(len);
      return err;
  }
#else
  #include <WiFi.h>
  #include <esp_now.h>
  #include <esp_timer.h>

  static inline uint8_t espnowCurrentChannel()
  {
      return WiFi.channel();
  }

  static inline bool espnowInitOk()
  {
      return esp_now_init() == ESP_OK;
  }

  static inline void espnowSetRole()
  {
  }

  static inline bool espnowAddPeer(const uint8_t *mac)
  {
      if (esp_now_is_peer_exist(mac)) return true;
      esp_now_peer_info_t peer = {};
      memcpy(peer.peer_addr, mac, 6);
      peer.channel = 0;
      peer.encrypt = false;
      return esp_now_add_peer(&peer) == ESP_OK;
  }

  /* Toda trama sale con la marca de red (sync_protocol.h). */
  static inline esp_err_t espnowSend(const uint8_t *mac, const uint8_t *data, size_t len)
  {
      uint8_t buf[ESPNOW_MAX_PAYLOAD];
      const int n = geonetWrap(buf, data, (int)len);
      if (n == 0) return ESP_ERR_INVALID_ARG;
      const esp_err_t err = esp_now_send(mac, buf, (size_t)n);
      if (err == ESP_OK) live::noteRadioTxStart(len);
      return err;
  }
#endif
