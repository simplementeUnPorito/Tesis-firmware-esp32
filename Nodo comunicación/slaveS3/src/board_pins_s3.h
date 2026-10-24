#pragma once
/* Delta de placa ESP32-S3 N16R8. Este archivo se fuerza con platformio.ini. */
#if !defined(ARDUINO_ESP32S3_DEV)
#error "slaveS3 requiere ESP32-S3; usar slave para ESP32 clasico"
#endif

/* Enlace PSoC y sincronismo. */
#define PSOC_I2C_ENABLE 1
#define PSOC_I2C_ADDR 0x42
#define PSOC_I2C_SDA 8
#define PSOC_I2C_SCL 9
#define PSOC_UART_BAUD 115200
#define PSOC_UART_RX 18
#define PSOC_UART_TX 17
#define PSOC_RESET_REQUEST_PIN 16
#define SYNC_TO_PSOC_PIN 7

/* OLED y botones. */
#define LOCAL_UI_ENABLE 1
#define LOCAL_OLED_SCK_PIN 12
#define LOCAL_OLED_MOSI_PIN 11
#define LOCAL_OLED_CS_PIN 10
#define LOCAL_OLED_DC_PIN 5
#define LOCAL_OLED_RESET_PIN 6
#define LOCAL_OLED_SPI_HZ 8000000UL
#define LOCAL_BTN_UP_PIN 38
#define LOCAL_BTN_DOWN_PIN 39
#define LOCAL_BTN_OK_PIN 40
#define LOCAL_BTN_BACK_PIN 41
#define LOCAL_BUTTON_USE_INTERNAL_PULLUPS 1
#define LOCAL_CAPTURE_BATCHES 10

/* SD local opcional del modo live: bus separado del OLED. */
#define LIVE_SD_SPI_HOST FSPI
#define LIVE_SD_SCK 14
#define LIVE_SD_MISO 21
#define LIVE_SD_MOSI 13
#define LIVE_SD_CS 42
#define LIVE_SD_HZ 1000000
#define SD_SPI_HOST LIVE_SD_SPI_HOST
#define SDDIAG_SCK LIVE_SD_SCK
#define SDDIAG_MISO LIVE_SD_MISO
#define SDDIAG_MOSI LIVE_SD_MOSI
#define SDDIAG_CS LIVE_SD_CS

/* 2048 x 32 x 90 B = 5,90 MiB; deja margen en los 8 MiB de PSRAM. */
#define LIVE_MAX_PAGES 2048u
#define LIVE_SD_RT_RING 512u
