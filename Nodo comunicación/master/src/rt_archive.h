#pragma once
/*
 * rt_archive.h — copia de los lotes que el maestro entrego en tiempo real.
 *
 * Por radio no se pierde nada (STREAM_ACK y retransmision), pero entre el
 * maestro y el navegador si: la cola del WebSocket descarta mensajes cuando la
 * pestana no consume, y una reconexion pierde lo que paso mientras tanto. Cada
 * lote sale con su numero (cabecera 0xFA) y queda guardado aca; la web detecta
 * los huecos y los pide con FILL, y el maestro los reenvia desde esta copia.
 *
 * En el S3 va en PSRAM (~20 000 lotes, unos 4 min de 8 nodos o 30 min de uno);
 * en el WROOM, lo que entre en el heap. Lo que ya no esta se informa con
 * fill_miss y queda como hueco declarado en los metadatos.
 */
#include <Arduino.h>
#include <esp_heap_caps.h>
#include "sync_protocol.h"

#ifndef RT_ARCHIVE_BYTES_PSRAM
#define RT_ARCHIVE_BYTES_PSRAM (6u * 1024u * 1024u)
#endif
#ifndef RT_ARCHIVE_BYTES_HEAP
#define RT_ARCHIVE_BYTES_HEAP (64u * 1024u)
#endif
#ifndef RT_ARCHIVE_MAX_NODES
#define RT_ARCHIVE_MAX_NODES 9
#endif

namespace rtarc {

static constexpr size_t BATCH_SAMPLES = (size_t)SAMPLES_PER_PART * 2u;
static constexpr size_t SLOT_BYTES = BATCH_SAMPLES * sizeof(SampleBytes);

struct NodeRing {
    SampleBytes *slots = nullptr;
    uint32_t    *tags = nullptr;
    uint32_t     cap = 0;
};

static NodeRing g_rings[RT_ARCHIVE_MAX_NODES];
static uint8_t *g_pool = nullptr;
static size_t   g_poolBytes = 0;

static void release()
{
    for (auto &r : g_rings) r = NodeRing();
    if (g_pool) heap_caps_free(g_pool);
    g_pool = nullptr;
    g_poolBytes = 0;
}

/* Reparte la memoria entre los nodos de la captura. Devuelve lotes por nodo. */
static uint32_t start(uint32_t nodeMask)
{
    release();
    uint32_t nodes = 0;
    for (uint8_t n = 1; n < RT_ARCHIVE_MAX_NODES; n++) if (nodeMask & (1u << n)) nodes++;
    if (nodes == 0) return 0;
    size_t want = RT_ARCHIVE_BYTES_PSRAM;
    g_pool = (uint8_t *)heap_caps_malloc(want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_pool) {
        want = RT_ARCHIVE_BYTES_HEAP;
        while (!g_pool && want >= 8u * 1024u) {
            g_pool = (uint8_t *)heap_caps_malloc(want, MALLOC_CAP_8BIT);
            if (!g_pool) want /= 2u;
        }
    }
    if (!g_pool) return 0;
    g_poolBytes = want;
    const uint32_t perNode = (uint32_t)(want / nodes / (SLOT_BYTES + sizeof(uint32_t)));
    uint8_t *p = g_pool;
    for (uint8_t n = 1; n < RT_ARCHIVE_MAX_NODES; n++) {
        if (!(nodeMask & (1u << n))) continue;
        NodeRing &r = g_rings[n];
        r.cap = perNode;
        r.slots = (SampleBytes *)p;
        p += (size_t)perNode * SLOT_BYTES;
        r.tags = (uint32_t *)p;
        p += (size_t)perNode * sizeof(uint32_t);
        for (uint32_t i = 0; i < perNode; i++) r.tags[i] = 0xFFFFFFFFu;
    }
    return perNode;
}

static void put(uint8_t node, uint32_t seq, const SampleBytes *s)
{
    if (node >= RT_ARCHIVE_MAX_NODES) return;
    NodeRing &r = g_rings[node];
    if (!r.cap) return;
    const uint32_t slot = seq % r.cap;
    memcpy(&r.slots[(size_t)slot * BATCH_SAMPLES], s, SLOT_BYTES);
    r.tags[slot] = seq;
}

static bool get(uint8_t node, uint32_t seq, SampleBytes *out)
{
    if (node >= RT_ARCHIVE_MAX_NODES) return false;
    const NodeRing &r = g_rings[node];
    if (!r.cap) return false;
    const uint32_t slot = seq % r.cap;
    if (r.tags[slot] != seq) return false;
    memcpy(out, &r.slots[(size_t)slot * BATCH_SAMPLES], SLOT_BYTES);
    return true;
}

static uint32_t capacity(uint8_t node)
{
    return node < RT_ARCHIVE_MAX_NODES ? g_rings[node].cap : 0u;
}

/* ── Pedidos de relleno de la web (comando FILL) ─────────────────────────── */
struct Fill { uint8_t node; uint32_t seq; uint16_t count; };
static constexpr uint8_t FILL_Q = 32;
static Fill    g_fillQ[FILL_Q];
static volatile uint8_t g_fillHead = 0, g_fillTail = 0;

static void pushFill(uint8_t node, uint32_t seq, uint16_t count)
{
    const uint8_t next = (uint8_t)((g_fillHead + 1u) % FILL_Q);
    if (next == g_fillTail) return;   /* llena: la web vuelve a pedir */
    g_fillQ[g_fillHead] = { node, seq, count };
    g_fillHead = next;
}

static bool peekFill(Fill &f)
{
    if (g_fillHead == g_fillTail) return false;
    f = g_fillQ[g_fillTail];
    return true;
}

static void popFill() { g_fillTail = (uint8_t)((g_fillTail + 1u) % FILL_Q); }

static void advanceFill(uint16_t done)
{
    Fill &f = g_fillQ[g_fillTail];
    if (done >= f.count) { popFill(); return; }
    f.seq += done;
    f.count = (uint16_t)(f.count - done);
}

}  // namespace rtarc
