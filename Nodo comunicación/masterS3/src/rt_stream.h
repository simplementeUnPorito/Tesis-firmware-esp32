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
#include "geo_codec.h"

#define RT_WINDOW     64u
#define RT_MAX_NODES  9u
#define RT_QUEUE      96u
/* Cada STREAM_ACK es una trama ESP-NOW y cada una puede trabar el TX del
 * maestro en IDF 5.5 (esp-idf #19269): la mitad de ACK que antes. El RTO del
 * esclavo (live_capture.cpp) tiene que quedar por encima de RT_ACK_MS. */
#define RT_ACK_EVERY  16u
#define RT_ACK_MS     120u

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
        if (!_q) _q = xQueueCreate(RT_QUEUE, sizeof(Frame));
    }

    /* nodeMask: bit n = nodo n esta en la captura. */
    bool start(uint32_t nodeMask)
    {
        stop();
        if (!_q) return false;
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
        xQueueReset(_q);
        _qDrop = 0;
        _lastProgressMs = millis();
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
    /* ms desde el ultimo lote nuevo o STREAM_END: el vigia del maestro cierra
     * si un esclavo desaparece (reset, sin enlace) y nunca manda su fin. */
    uint32_t msSinceProgress() const { return millis() - _lastProgressMs; }

    /* Tarea WiFi: solo encolar. */
    void onFrame(const uint8_t *data, int len)
    {
        if (!_active || !_q || len < MSG_DATAZ_HEADER || len > (int)sizeof(Frame::d)) return;
        Frame f;
        f.len = (uint16_t)len;
        memcpy(f.d, data, (size_t)len);
        if (xQueueSend(_q, &f, 0) != pdTRUE) _qDrop++;
    }

    void onEnd(const MsgStreamEnd &e)
    {
        if (!_active || e.node_id == 0 || e.node_id >= RT_MAX_NODES) return;
        Node &d = _nodes[e.node_id];
        if (!d.on) return;
        d.total = e.total;
        d.reason = e.reason;
        d.ended = true;
        _lastProgressMs = millis();
    }

    void service()
    {
        if (!_active) return;
        Frame f;
        for (int k = 0; k < 32 && xQueueReceive(_q, &f, 0) == pdTRUE; k++) {
            if (f.d[0] == DATAZ_PKT_MARKER) _takeZ(f);
            else if (f.len >= sizeof(MsgData32)) _take(*(const MsgData32 *)f.d);
        }
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

    bool nodeOn(uint8_t node) const { return node < RT_MAX_NODES && _nodes[node].on; }
    bool nodeEnded(uint8_t node) const { return node < RT_MAX_NODES && _nodes[node].ended; }

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

    /* Trama cruda en la cola: MsgData32 (167 B) o MsgDataZ (8 + len). */
    struct Frame { uint16_t len; uint8_t d[sizeof(MsgData32) > MSG_DATAZ_HEADER + MSG_DATAZ_MAX ?
                                            sizeof(MsgData32) : MSG_DATAZ_HEADER + MSG_DATAZ_MAX]; };

    /* Lote comprimido: se decodifica entero y ocupa las dos mitades. */
    void _takeZ(const Frame &f)
    {
        const MsgDataZ *z = (const MsgDataZ *)f.d;
        if (z->node_id == 0 || z->node_id >= RT_MAX_NODES) return;
        Node &d = _nodes[z->node_id];
        if (!d.on) return;
        int32_t x[GEOZ_SAMPLES];
        uint8_t fl[GEOZ_SAMPLES];
        if (f.len != MSG_DATAZ_HEADER + z->len || dataZCrc(z) != z->crc ||
            !geoz_decode(z->data, z->len, x, fl)) { d.crc++; return; }
        if (z->seq < d.next) { d.dup++; return; }
        if (z->seq >= d.next + RT_WINDOW) { d.win++; return; }
        const uint32_t slot = z->seq % RT_WINDOW;
        if (d.tag[slot] == z->seq && d.parts[slot] == 3u) { d.dup++; return; }
        SampleBytes *s = &d.s[(size_t)slot * 2u * SAMPLES_PER_PART];
        for (int i = 0; i < GEOZ_SAMPLES; i++) {
            const uint32_t v = (uint32_t)x[i] & 0xFFFFFFu;
            memset(&s[i], 0, sizeof(SampleBytes));
            s[i].digi0 = (uint8_t)(v & 0xFFu);
            s[i].digi1 = (uint8_t)((v >> 8) & 0xFFu);
            s[i].digi2 = (uint8_t)((v >> 16) & 0xFFu);
            s[i].raw_lo = s[i].digi0;
            s[i].raw_hi = s[i].digi1;
            s[i].flags = fl[i];
        }
        d.tag[slot] = z->seq;
        d.parts[slot] = 3u;
        d.rx++;
        _lastProgressMs = millis();
    }

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
        if (d.parts[slot] == 3u) { d.rx++; _lastProgressMs = millis(); }
    }

    EmitFn _emit = nullptr;
    ReadyFn _ready = nullptr;
    AckFn _ack = nullptr;
    QueueHandle_t _q = nullptr;
    volatile uint32_t _qDrop = 0;
    bool _active = false;
    volatile uint32_t _lastProgressMs = 0;
    Node _nodes[RT_MAX_NODES];
};
