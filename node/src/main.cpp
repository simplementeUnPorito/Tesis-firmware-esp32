/*
 * main.cpp — ESP32 geophone node firmware.
 *
 * Boot sequence:
 *   1. Connect to WiFi
 *   2. Sync NTP time
 *   3. Init SPI interface to PSoC
 *   4. Main loop: poll SPI → timestamp batch → send via UDP
 *
 * Configuration — edit the section below before flashing.
 */

#include <Arduino.h>
#include <WiFi.h>
#include "psoc_spi.h"
#include "time_sync.h"
#include "wifi_transport.h"

// ─── USER CONFIGURATION ───────────────────────────────────────────────────────
static const char *WIFI_SSID     = "GeoNetwork";        // AP or router SSID
static const char *WIFI_PASS     = "geophone2026";       // WiFi password
static const char *BASE_STATION  = "192.168.4.1";        // base station IP
static const char *NTP_SERVER    = "192.168.4.1";        // NTP server (base station)
static const uint8_t NODE_ID     = 1;                    // unique per node (1, 2, 3...)
// ─────────────────────────────────────────────────────────────────────────────

PsocSPI       psocSpi;
TimeSync      timeSync;
WiFiTransport udpTx;

static uint32_t s_batchCount    = 0;
static uint32_t s_dropCount     = 0;
static uint32_t s_lastReportMs  = 0;

void onBatch(const PsocBatch &batch)
{
    udpTx.sendBatch(batch, &timeSync, NODE_ID);
    s_batchCount++;
}

void setup()
{
    Serial.begin(115200);
    Serial.printf("\n[NODE %u] Geophone node — booting\n", NODE_ID);

    // ── WiFi ──────────────────────────────────────────────────────────────────
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("[NODE] WiFi connecting");
    uint8_t retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries++ < 30)
    {
        delay(500);
        Serial.print('.');
    }
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("\n[NODE] WiFi FAIL — rebooting in 5s");
        delay(5000);
        ESP.restart();
    }
    Serial.printf("\n[NODE] WiFi OK  IP=%s\n", WiFi.localIP().toString().c_str());

    // ── NTP ───────────────────────────────────────────────────────────────────
    timeSync.begin(NTP_SERVER);
    timeSync.setNodeId(NODE_ID);
    Serial.print("[NODE] NTP syncing");
    for (int i = 0; i < 20 && !timeSync.synced(); i++)
    {
        delay(500);
        Serial.print('.');
    }
    if (timeSync.synced())
    {
        Serial.printf("\n[NODE] NTP OK  epoch=%llu\n", timeSync.nowUs() / 1000000ULL);
    }
    else
    {
        Serial.println("\n[NODE] NTP timeout — continuing with local micros()");
    }

    // ── UDP transport ─────────────────────────────────────────────────────────
    udpTx.begin(BASE_STATION);
    Serial.printf("[NODE] UDP target: %s:%u\n", BASE_STATION, WIFI_UDP_PORT);

    // ── PSoC SPI ──────────────────────────────────────────────────────────────
    psocSpi.begin(onBatch);
    Serial.println("[NODE] SPI ready — waiting for PSoC data");
}

void loop()
{
    psocSpi.poll();
    timeSync.update();

    s_dropCount = psocSpi.batchesBad();

    // Periodic status print every 10 s
    unsigned long now = millis();
    if (now - s_lastReportMs >= 10000UL)
    {
        s_lastReportMs = now;
        Serial.printf("[NODE %u] batches_ok=%lu  bad=%lu  udp_sent=%lu  ntp=%s\n",
                      NODE_ID, psocSpi.batchesOK(), psocSpi.batchesBad(),
                      udpTx.pktsSent(),
                      timeSync.synced() ? "OK" : "NO");
    }
}
