#pragma once
/*
 * geo_codec.h — compresion sin perdida de un lote de 30 muestras (24 bits) y
 * sus marcas de ruido, para una sola trama ESP-NOW.
 *
 * Medido sobre 26 640 lotes reales (lab/compresion): 29,7 B por lote contra
 * 334 B de MsgData32 (11x). Cada lote se codifica solo, asi un lote perdido o
 * retransmitido se decodifica sin los demas.
 *
 * Formato (bytes):
 *   [0]    modo: bit0..1 orden de prediccion (0 o 1), bit2 escape a crudo,
 *          bit3 hay marcas
 *   crudo  (escape): 30 x 3 bytes LE con signo de 24 bits
 *   rice   [1..3] primera muestra (24 bits LE); [4] k (0..23);
 *          luego 29 residuos (diferencia con la muestra anterior) en zigzag
 *          y Rice(k): cociente en unario (1...10) y k bits del resto, MSB
 *          primero, alineado a byte al final
 *   marcas (si bit3): pares [valor][largo] que cubren las 30 muestras
 * Largo maximo: 1 + 90 + 1 + 60 = 152 bytes (siempre entra en ESP-NOW).
 */
#include <stdint.h>
#include <stddef.h>

#define GEOZ_SAMPLES   30
#define GEOZ_MAX_BYTES 160

#define GEOZ_ORDER_MASK 0x03u
#define GEOZ_ESCAPE     0x04u
#define GEOZ_HAS_FLAGS  0x08u

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   pos;    /* bytes completos */
    uint8_t  acc;
    uint8_t  nbits;  /* bits ya puestos en acc */
    int      over;
} geoz_bw_t;

static inline void geoz_putbit(geoz_bw_t *w, unsigned b)
{
    if (w->over) return;
    w->acc = (uint8_t)((w->acc << 1) | (b & 1u));
    if (++w->nbits == 8u) {
        if (w->pos >= w->cap) { w->over = 1; return; }
        w->buf[w->pos++] = w->acc;
        w->acc = 0;
        w->nbits = 0;
    }
}

static inline void geoz_flushbits(geoz_bw_t *w)
{
    if (w->nbits && !w->over) {
        if (w->pos >= w->cap) { w->over = 1; return; }
        w->buf[w->pos++] = (uint8_t)(w->acc << (8u - w->nbits));
    }
    w->acc = 0;
    w->nbits = 0;
}

static inline uint32_t geoz_zigzag(int32_t v)
{
    return v >= 0 ? (uint32_t)v << 1 : (((uint32_t)(-(v + 1))) << 1) | 1u;
}

static inline int32_t geoz_unzigzag(uint32_t u)
{
    return (u & 1u) ? -(int32_t)(u >> 1) - 1 : (int32_t)(u >> 1);
}

static inline int32_t geoz_sext24(uint32_t v)
{
    return (v & 0x800000u) ? (int32_t)(v | 0xFF000000u) : (int32_t)(v & 0xFFFFFFu);
}

static inline size_t geoz_put_flags(const uint8_t *flags, uint8_t *out, size_t pos, size_t cap)
{
    int i = 0;
    while (i < GEOZ_SAMPLES) {
        const uint8_t v = flags[i];
        int run = 1;
        while (i + run < GEOZ_SAMPLES && flags[i + run] == v) run++;
        if (pos + 2u > cap) return 0;
        out[pos++] = v;
        out[pos++] = (uint8_t)run;
        i += run;
    }
    return pos;
}

/* Codifica. x: 30 muestras (24 bits con signo), flags: 30 bytes o NULL.
 * Devuelve el largo en bytes (nunca mas de GEOZ_MAX_BYTES). */
static inline size_t geoz_encode(const int32_t *x, const uint8_t *flags, uint8_t *out)
{
    uint8_t hasFlags = 0;
    int i;
    if (flags) {
        for (i = 0; i < GEOZ_SAMPLES; i++) if (flags[i]) { hasFlags = 1; break; }
    }
    /* Elegir k: minimiza la suma de largos Rice de los residuos. */
    uint32_t u[GEOZ_SAMPLES - 1];
    for (i = 1; i < GEOZ_SAMPLES; i++) {
        const int32_t a = geoz_sext24((uint32_t)x[i] & 0xFFFFFFu);
        const int32_t b = geoz_sext24((uint32_t)x[i - 1] & 0xFFFFFFu);
        u[i - 1] = geoz_zigzag(a - b);
    }
    unsigned bestK = 0;
    uint32_t bestBits = 0xFFFFFFFFu;
    unsigned k;
    for (k = 0; k < 24u; k++) {
        uint32_t bits = 0;
        for (i = 0; i < GEOZ_SAMPLES - 1; i++) bits += (u[i] >> k) + 1u + k;
        if (bits < bestBits) { bestBits = bits; bestK = k; }
    }
    /* Rice si gana al crudo; si no, escape (transitorios enormes). */
    const size_t riceBytes = 5u + (bestBits + 7u) / 8u;
    size_t pos;
    if (riceBytes < 1u + 90u) {
        out[0] = (uint8_t)(1u | (hasFlags ? GEOZ_HAS_FLAGS : 0u));
        const uint32_t x0 = (uint32_t)x[0] & 0xFFFFFFu;
        out[1] = (uint8_t)(x0 & 0xFFu);
        out[2] = (uint8_t)((x0 >> 8) & 0xFFu);
        out[3] = (uint8_t)((x0 >> 16) & 0xFFu);
        out[4] = (uint8_t)bestK;
        geoz_bw_t w = { out, GEOZ_MAX_BYTES, 5u, 0, 0, 0 };
        for (i = 0; i < GEOZ_SAMPLES - 1; i++) {
            uint32_t q = u[i] >> bestK;
            while (q--) geoz_putbit(&w, 1u);
            geoz_putbit(&w, 0u);
            for (k = bestK; k-- > 0u;) geoz_putbit(&w, (u[i] >> k) & 1u);
        }
        geoz_flushbits(&w);
        pos = w.pos;
    } else {
        out[0] = (uint8_t)(GEOZ_ESCAPE | (hasFlags ? GEOZ_HAS_FLAGS : 0u));
        pos = 1u;
        for (i = 0; i < GEOZ_SAMPLES; i++) {
            const uint32_t v = (uint32_t)x[i] & 0xFFFFFFu;
            out[pos++] = (uint8_t)(v & 0xFFu);
            out[pos++] = (uint8_t)((v >> 8) & 0xFFu);
            out[pos++] = (uint8_t)((v >> 16) & 0xFFu);
        }
    }
    if (hasFlags) pos = geoz_put_flags(flags, out, pos, GEOZ_MAX_BYTES);
    return pos;
}

/* Decodifica. Devuelve 1 si el lote es valido. flags puede ser NULL. */
static inline int geoz_decode(const uint8_t *in, size_t len, int32_t *x, uint8_t *flags)
{
    int i;
    if (len < 1u) return 0;
    const uint8_t mode = in[0];
    size_t pos;
    if (mode & GEOZ_ESCAPE) {
        if (len < 1u + 90u) return 0;
        pos = 1u;
        for (i = 0; i < GEOZ_SAMPLES; i++) {
            x[i] = geoz_sext24((uint32_t)in[pos] | ((uint32_t)in[pos + 1] << 8) |
                               ((uint32_t)in[pos + 2] << 16));
            pos += 3u;
        }
    } else {
        if ((mode & GEOZ_ORDER_MASK) != 1u || len < 5u) return 0;
        const unsigned k = in[4];
        if (k > 23u) return 0;
        x[0] = geoz_sext24((uint32_t)in[1] | ((uint32_t)in[2] << 8) | ((uint32_t)in[3] << 16));
        size_t byte = 5u;
        unsigned bit = 0;
        for (i = 1; i < GEOZ_SAMPLES; i++) {
            uint32_t q = 0;
            for (;;) {
                if (byte >= len) return 0;
                const unsigned b = (in[byte] >> (7u - bit)) & 1u;
                if (++bit == 8u) { bit = 0; byte++; }
                if (!b) break;
                if (++q > 0x2000000u) return 0;
            }
            uint32_t r = 0;
            unsigned j;
            for (j = 0; j < k; j++) {
                if (byte >= len) return 0;
                r = (r << 1) | ((in[byte] >> (7u - bit)) & 1u);
                if (++bit == 8u) { bit = 0; byte++; }
            }
            const int32_t d = geoz_unzigzag((q << k) | r);
            x[i] = geoz_sext24(((uint32_t)(x[i - 1] + d)) & 0xFFFFFFu);
        }
        pos = byte + (bit ? 1u : 0u);
    }
    if (flags) {
        for (i = 0; i < GEOZ_SAMPLES; i++) flags[i] = 0;
        if (mode & GEOZ_HAS_FLAGS) {
            int at = 0;
            while (at < GEOZ_SAMPLES) {
                if (pos + 2u > len) return 0;
                const uint8_t v = in[pos++];
                const int run = in[pos++];
                if (run <= 0 || at + run > GEOZ_SAMPLES) return 0;
                for (i = 0; i < run; i++) flags[at + i] = v;
                at += run;
            }
        }
    }
    return 1;
}
