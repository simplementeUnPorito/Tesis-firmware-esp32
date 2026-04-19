#pragma once
/*
 * wifi_transport.h — UDP transmission of timestamped batches to base station.
 *
 * Packet format (UDP payload, binary, little-endian):
 *   [0]     0xBC   (node packet marker)
 *   [1]     node_id
 *   [2..3]  batch_seq (uint16 LE)
 *   [4]     global_flags
 *   [5]     n_samples
 *   [6..13] timestamp_us (uint64 LE — microseconds since Unix epoch)
 *   [14..]  n_samples × 10 bytes (same layout as SPI frame samples)
 *   [-1]    xor_crc (XOR of all preceding bytes)
 *
 * Base station listens on UDP_PORT (default 5005).
 * Each node sends to the base station IP configured at setup().
 */

#include <Arduino.h>
#include <WiFiUdp.h>
#include "psoc_spi.h"
#include "time_sync.h"

#define WIFI_UDP_PORT    5005
#define NODE_PKT_MARKER  0xBC

class WiFiTransport {
public:
    void begin(const char *baseIP, uint16_t port = WIFI_UDP_PORT)
    {
        _port = port;
        _udp.begin(port);
        strncpy(_baseIP, baseIP, sizeof(_baseIP));
    }

    /* Send one batch to base station.
     * Injects NTP timestamp from ts.nowUs() if ts is not null. */
    void sendBatch(const PsocBatch &batch, const TimeSync *ts = nullptr,
                   uint8_t nodeId = 0)
    {
        uint8_t  buf[6 + 8 + SPI_BATCH_SAMPLES * 10 + 1];  /* 321 bytes max */
        uint16_t pos = 0;
        uint8_t  crc = 0;

        uint64_t ts_us = ts ? ts->nowUs() : batch.timestamp_us;

        auto wr = [&](uint8_t b) { buf[pos++] = b; crc ^= b; };

        wr(NODE_PKT_MARKER);
        wr(nodeId);
        wr((uint8_t)(batch.seq & 0xFF));
        wr((uint8_t)(batch.seq >> 8));
        wr(batch.global_flags);
        wr(batch.n_samples);
        /* timestamp: 8 bytes LE */
        for (int b = 0; b < 8; b++) { wr((uint8_t)(ts_us >> (b * 8))); }

        for (int i = 0; i < batch.n_samples; i++)
        {
            const PsocSample &s = batch.samples[i];
            wr((uint8_t)(s.raw_input & 0xFF));
            wr((uint8_t)((s.raw_input >> 8) & 0xFF));
            wr((uint8_t)(s.post_analog & 0xFF));
            wr((uint8_t)((s.post_analog >> 8)  & 0xFF));
            wr((uint8_t)((s.post_analog >> 16) & 0xFF));
            wr((uint8_t)(s.post_digital & 0xFF));
            wr((uint8_t)((s.post_digital >> 8)  & 0xFF));
            wr((uint8_t)((s.post_digital >> 16) & 0xFF));
            wr(s.gain_byte);
            wr(s.sample_flags);
        }
        buf[pos++] = crc;   /* CRC not included in its own XOR */

        _udp.beginPacket(_baseIP, _port);
        _udp.write(buf, pos);
        _udp.endPacket();
        _pktsSent++;
    }

    uint32_t pktsSent() const { return _pktsSent; }

private:
    WiFiUDP  _udp;
    uint16_t _port     = WIFI_UDP_PORT;
    char     _baseIP[20] = "192.168.1.1";
    uint32_t _pktsSent = 0;
};
