#pragma once
/*
 * rt_stream.h — captura en tiempo real: los esclavos mandan cada lote
 * mientras capturan (MsgData32, lote de 32 bits).
 *
 * El callback de ESP-NOW corre en la tarea WiFi y solo encola la trama. El
 * loop() la arma por (nodo, lote) en una ventana de RT_WINDOW lotes y la
 * entrega EN ORDEN con el mismo formato de siempre: la web no se entera de
 * que hubo huecos. Lo entregado se confirma con MsgStreamAck (estilo TCP:
 * proximo esperado + bitmap de lo que llego despues) y el esclavo libera su
 * respaldo y reenvia lo que falta.
 *
 * Contrapresion: si la web no da abasto, no se entrega, no se confirma y el
 * esclavo retiene. Un nodo termina cuando llego su STREAM_END y se entrego
 * hasta su total.
 */
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "sync_protocol.h"

#define RT_WINDOW     64u
#define RT_MAX_NODES  9u
#define RT_QUEUE      96u
#define RT_ACK_EVERY  8u
#define RT_ACK_MS     50u

class RtStream {
public:
    typedef void (*EmitFn)(uint8_t node, const SampleBytes *s30, uint32_t seq);
    typedef bool (*ReadyFn)();
    typedef void (*AckFn)(uint8_t node, uint32_t next, uint32_t sack);

    void begin(EmitFn emit, ReadyFn ready, AckFn ack)
    {
        _emit = emit;
        _ready = ready;
        _ack = ack;
        if (!_q) _q = xQueueCreate(RT_QUEUE, sizeof(MsgData32));
    }

    /* nodeMask: bit n = nodo n esta en la captura. */
    bool start(uint32_t nodeMask)
    {
        stop();
        for (uint8_t n = 1; n < RT_MAX_NODES; n++) {
            if ((nodeMask & (1u << n)) == 0u) continue;
            Node &d = _nodes[n];
            d.s = (SampleBytes *)malloc((size_t)RT_WINDOW * 2u * SAMPLES_PER_PART * sizeof(SampleBytes));
            if (!d.s) { stop(); return false; }
            for (uint32_t i = 0; i < RT_WINDOW; i++) { d.tag[i] = 0xFFFFFFFFu; d.parts[i] = 0; }
            d.next = 0;
            d.total = 0;
            d.ended = false;
            d.reason = 0;
            d.sinceAck = 0;
            d.lastAckMs = millis();
            d.rx = d.dup = d.win = d.crc = 0;
            d.on = true;
        }
        if (_q) xQueueReset(_q);
        _qDrop = 0;
        _active = true;
        return true;
    }

    void stop()
    {
        _active = false;
        for (uint8_t n = 0; n < RT_MAX_NODES; n++) {
            free(_nodes[n].s);
            _nodes[n].s = nullptr;
            _nodes[n].on = false;
        }
    }

    bool active() const { return _active; }

    /* Tarea WiFi: solo encolar. */
    void onFrame(const uint8_t *data, int len)
    {
        if (!_active || !_q || len < (int)sizeof(MsgData32)) return;
        if (xQueueSend(_q, data, 0) != pdTRUE) _qDrop++;
    }

    void onEnd(const MsgStreamEnd &e)
    {
        if (!_active || e.node_id == 0 || e.node_id >= RT_MAX_NODES) return;
        Node &d = _nodes[e.node_id];
        if (!d.on) return;
        d.total = e.total;
        d.reason = e.reason;
        d.ended = true;
    }

    void service()
    {
        if (!_active) return;
        MsgData32 m;
        for (int k = 0; k < 32 && xQueueReceive(_q, &m, 0) == pdTRUE; k++) _take(m);
        const uint32_t now = millis();
        for (uint8_t n = 1; n < RT_MAX_NODES; n++) {
            Node &d = _nodes[n];
            if (!d.on) continue;
            for (;;) {
                const uint32_t slot = d.next % RT_WINDOW;
                if (d.tag[slot] != d.next || d.parts[slot] != 3u) break;
                if (_ready && !_ready()) break;
                if (_emit) _emit(n, &d.s[(size_t)slot * 2u * SAMPLES_PER_PART], d.next);
                d.parts[slot] = 0;
                d.tag[slot] = 0xFFFFFFFFu;
                d.next++;
                d.sinceAck++;
            }
            const bool done = d.ended && d.next >= d.total;
            if (!done && (d.sinceAck >= RT_ACK_EVERY || (now - d.lastAckMs) >= RT_ACK_MS)) {
                uint32_t sack = 0;
                for (uint32_t i = 0; i < 32u; i++) {
                    const uint32_t s = d.next + 1u + i;
                    const uint32_t slot = s % RT_WINDOW;
                    if (d.tag[slot] == s && d.parts[slot] == 3u) sack |= (1u << i);
                }
                if (_ack) _ack(n, d.next, sack);
                d.sinceAck = 0;
                d.lastAckMs = now;
            }
        }
    }

    bool allDone() const
    {
        if (!_active) return false;
        for (uint8_t n = 1; n < RT_MAX_NODES; n++) {
            const Node &d = _nodes[n];
            if (d.on && !(d.ended && d.next >= d.total)) return false;
        }
        return true;
    }

    void printStats(Print &out) const
    {
        for (uint8_t n = 1; n < RT_MAX_NODES; n++) {
            const Node &d = _nodes[n];
            if (!d.on) continue;
            out.printf("#RT node=%u next=%lu total=%ld ended=%u reason=%u rx=%lu dup=%lu win=%lu crc=%lu qdrop=%lu\n",
                       (unsigned)n, (unsigned long)d.next,
                       d.ended ? (long)d.total : -1L, (unsigned)d.ended,
                       (unsigned)d.reason, (unsigned long)d.rx, (unsigned long)d.dup,
                       (unsigned long)d.win, (unsigned long)d.crc, (unsigned long)_qDrop);
        }
    }

    uint32_t delivered(uint8_t node) const
    {
        return (node < RT_MAX_NODES && _nodes[node].on) ? _nodes[node].next : 0u;
    }

private:
    struct Node {
        bool on = false;
        uint32_t next = 0;
        uint32_t tag[RT_WINDOW];
        uint8_t parts[RT_WINDOW];
        SampleBytes *s = nullptr;
        uint32_t sinceAck = 0;
        uint32_t lastAckMs = 0;
        volatile bool ended = false;
        volatile uint32_t total = 0;
        volatile uint8_t reason = 0;
        uint32_t rx = 0, dup = 0, win = 0, crc = 0;
    };

    void _take(const MsgData32 &m)
    {
        if (m.node_id == 0 || m.node_id >= RT_MAX_NODES || m.part > 1u) return;
        Node &d = _nodes[m.node_id];
        if (!d.on) return;
        uint8_t c = 0;
        const uint8_t *b = (const uint8_t *)&m;
        for (size_t i = 0; i < sizeof(m) - 1u; i++) c ^= b[i];
        if (c != m.crc) { d.crc++; return; }
        if (m.seq < d.next) { d.dup++; return; }
        if (m.seq >= d.next + RT_WINDOW) { d.win++; return; }
        const uint32_t slot = m.seq % RT_WINDOW;
        if (d.tag[slot] != m.seq) { d.tag[slot] = m.seq; d.parts[slot] = 0; }
        if (d.parts[slot] & (1u << m.part)) { d.dup++; return; }
        memcpy(&d.s[((size_t)slot * 2u + m.part) * SAMPLES_PER_PART], m.samples, sizeof(m.samples));
        d.parts[slot] |= (uint8_t)(1u << m.part);
        if (d.parts[slot] == 3u) d.rx++;
    }

    EmitFn _emit = nullptr;
    ReadyFn _ready = nullptr;
    AckFn _ack = nullptr;
    QueueHandle_t _q = nullptr;
    volatile uint32_t _qDrop = 0;
    bool _active = false;
    Node _nodes[RT_MAX_NODES];
};
