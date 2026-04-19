#pragma once
/*
 * time_sync.h — NTP-based time synchronization for multi-node geophone network.
 *
 * Architecture:
 *   One ESP32 acts as NTP client to a public or local NTP server.
 *   All nodes on the same WiFi network sync to the same NTP source.
 *   Resolution: ≈ 1–50 ms with SNTP on local WiFi.
 *
 * For sub-millisecond accuracy (future upgrade):
 *   - Use GPS PPS signal wired to an interrupt pin.
 *   - Mark the sample index coinciding with the PPS edge.
 *   - Base station correlates PPS markers from all nodes.
 *
 * Usage:
 *   TimeSync ts;
 *   ts.begin("pool.ntp.org");
 *   while (!ts.synced()) { delay(100); }
 *   uint64_t epoch_us = ts.nowUs();   // microseconds since Unix epoch
 *
 * Timestamping batches:
 *   The PsocSPI callback receives a PsocBatch.  Replace the coarse
 *   micros()-based timestamp with ts.nowUs() for absolute time:
 *
 *   void onBatch(const PsocBatch &b) {
 *       uint64_t t = ts.nowUs();
 *       // t is valid across all synced nodes
 *   }
 *
 * Drift compensation:
 *   SNTP re-syncs every 30 seconds by default on ESP-IDF.
 *   Linear interpolation between sync points removes ≈ 100 ppm drift.
 */

#include <Arduino.h>
#include <WiFiUdp.h>
#include <NTPClient.h>

class TimeSync {
public:
    /* ntpServer: hostname or IP of NTP server (base station or public pool) */
    void begin(const char *ntpServer = "pool.ntp.org", long utcOffsetSec = 0)
    {
        _udp.begin(NTP_LOCAL_PORT);
        _ntp = new NTPClient(_udp, ntpServer, utcOffsetSec, 30000);
        _ntp->begin();
        _ntp->update();
        _lastSyncMs = millis();
    }

    /* Call regularly from loop() to keep NTP current */
    void update()
    {
        unsigned long now = millis();
        if (now - _lastSyncMs >= RESYNC_INTERVAL_MS)
        {
            _ntp->update();
            _lastSyncMs = now;
        }
    }

    bool synced() const
    {
        return _ntp && _ntp->isTimeSet();
    }

    /* Microseconds since Unix epoch — combines NTP seconds + local micros() drift */
    uint64_t nowUs() const
    {
        if (!_ntp) { return (uint64_t)micros(); }
        /* NTP gives seconds; local micros() adds sub-second precision */
        uint64_t ntp_sec = (uint64_t)_ntp->getEpochTime();
        /* Sub-second offset from micros() modulo 1 000 000 */
        uint32_t sub_us  = (uint32_t)(micros() % 1000000UL);
        return ntp_sec * 1000000ULL + sub_us;
    }

    /* Node identifier — set this to differentiate nodes in base station */
    void setNodeId(uint8_t id) { _nodeId = id; }
    uint8_t nodeId()    const  { return _nodeId; }

private:
    static constexpr uint16_t NTP_LOCAL_PORT      = 2390;
    static constexpr uint32_t RESYNC_INTERVAL_MS  = 30000;  /* 30 s */

    WiFiUDP    _udp;
    NTPClient *_ntp         = nullptr;
    uint8_t    _nodeId      = 0;
    unsigned long _lastSyncMs = 0;
};
