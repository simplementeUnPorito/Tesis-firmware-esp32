#pragma once
/*
 * ota_master.h — el maestro reparte firmware nuevo a los esclavos (OTA).
 *
 *   POST /ota/upload      (multipart, campo "file") guarda /ota/slave.bin en
 *                         LittleFS, calcula SHA-256 y exige la marca de rol
 *                         del esclavo dentro del binario.
 *   GET  /ota/slave.bin   lo baja el esclavo, asociado como STA al AP.
 *   POST /ota/start?node=N   ofrece la imagen al nodo N (0 = todos los vistos).
 *   POST /ota/abort?node=N
 *   GET  /ota/status      JSON con la imagen y el estado de cada nodo.
 *
 * Nunca durante una captura: el esclavo la rechaza igual (OTA_ERR_BUSY).
 */
#include <Arduino.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <mbedtls/sha256.h>
#include <esp_arduino_version.h>
/* mbedtls 3 (IDF 5) quito el sufijo _ret. */
#if ESP_ARDUINO_VERSION_MAJOR >= 3
#define mbedtls_sha256_starts_ret mbedtls_sha256_starts
#define mbedtls_sha256_update_ret mbedtls_sha256_update
#define mbedtls_sha256_finish_ret mbedtls_sha256_finish
#endif
#include "sync_protocol.h"

#ifndef OTA_MAX_NODES
#define OTA_MAX_NODES 9
#endif

namespace otam {

typedef bool (*SendToNodeFn)(uint8_t node, const uint8_t *data, size_t len);
typedef bool (*BusyFn)();
typedef bool (*NodeSeenFn)(uint8_t node);

static const char *const IMG_PATH = "/ota/slave.bin";
static const char *const TMP_PATH = "/ota/slave.tmp";
/* La marca del esclavo se guarda corrida en +1 y se decodifica al arrancar:
 * si estuviera literal, el binario del maestro tambien "la tendria". */
static const char ROLE_MARK_ENC[] = "HFP`GX`SPMF;tmbwf;w2";
static char ROLE_MARK[sizeof(ROLE_MARK_ENC)] = {0};

struct NodeState {
    uint32_t session = 0;
    uint8_t  state = OTA_ST_IDLE;
    uint8_t  error = OTA_ERR_NONE;
    uint32_t received = 0;
    uint32_t size = 0;
    uint32_t lastMs = 0;
    bool     offered = false;
    bool     ackPending = false;   /* imagen nueva sin confirmar: contestarle */
};

static SendToNodeFn g_sendTo = nullptr;
static BusyFn       g_busy = nullptr;
static NodeSeenFn   g_seen = nullptr;
static NodeState    g_nodes[OTA_MAX_NODES];

static bool     g_imgOk = false;
static uint32_t g_imgSize = 0;
static uint8_t  g_imgSha[32] = {0};

/* Estado de la subida en curso (una a la vez). */
static File g_up;
static mbedtls_sha256_context g_upSha;
static uint32_t g_upBytes = 0;
static bool     g_upMark = false;
static String   g_upError;

static String shaHex(const uint8_t *d)
{
    static const char *hx = "0123456789abcdef";
    String s;
    s.reserve(64);
    for (int i = 0; i < 32; i++) { s += hx[d[i] >> 4]; s += hx[d[i] & 15]; }
    return s;
}

/* Busca la marca de rol byte a byte, con estado entre bloques. Alcanza un
 * reinicio simple porque la primera letra de la marca no se repite en ella. */
static size_t g_upMatch = 0;
static void scanMark(const uint8_t *data, size_t len)
{
    const size_t m = sizeof(ROLE_MARK) - 1;
    for (size_t i = 0; i < len && !g_upMark; i++) {
        const char c = (char)data[i];
        if (c == ROLE_MARK[g_upMatch]) {
            if (++g_upMatch == m) g_upMark = true;
        } else {
            g_upMatch = (c == ROLE_MARK[0]) ? 1u : 0u;
        }
    }
}

static void loadImageMeta()
{
    g_imgOk = false;
    if (!LittleFS.exists(IMG_PATH)) return;
    File f = LittleFS.open(IMG_PATH, "r");
    if (!f) return;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts_ret(&sha, 0);
    static uint8_t buf[2048];
    uint32_t n = 0;
    int r;
    while ((r = f.read(buf, sizeof(buf))) > 0) {
        mbedtls_sha256_update_ret(&sha, buf, (size_t)r);
        n += (uint32_t)r;
    }
    f.close();
    mbedtls_sha256_finish_ret(&sha, g_imgSha);
    mbedtls_sha256_free(&sha);
    g_imgSize = n;
    g_imgOk = n > 0;
}

static void onUpload(AsyncWebServerRequest *req, const String &filename, size_t index,
                     uint8_t *data, size_t len, bool final)
{
    (void)req; (void)filename;
    if (index == 0) {
        g_upError = "";
        g_upBytes = 0;
        g_upMark = false;
        g_upMatch = 0;
        if (g_busy && g_busy()) { g_upError = "captura en curso"; return; }
        if (!LittleFS.exists("/ota")) LittleFS.mkdir("/ota");
        g_up = LittleFS.open(TMP_PATH, "w");
        if (!g_up) { g_upError = "no se pudo abrir LittleFS"; return; }
        mbedtls_sha256_init(&g_upSha);
        mbedtls_sha256_starts_ret(&g_upSha, 0);
    }
    if (g_upError.length()) return;
    if (len) {
        if (g_up.write(data, len) != len) { g_upError = "LittleFS lleno"; g_up.close(); return; }
        mbedtls_sha256_update_ret(&g_upSha, data, len);
        scanMark(data, len);
        g_upBytes += (uint32_t)len;
    }
    if (final) {
        g_up.close();
        uint8_t d[32];
        mbedtls_sha256_finish_ret(&g_upSha, d);
        mbedtls_sha256_free(&g_upSha);
        if (!g_upMark) {
            g_upError = "el archivo no es firmware de esclavo (falta la marca de rol)";
            LittleFS.remove(TMP_PATH);
            return;
        }
        LittleFS.remove(IMG_PATH);
        LittleFS.rename(TMP_PATH, IMG_PATH);
        memcpy(g_imgSha, d, sizeof(d));
        g_imgSize = g_upBytes;
        g_imgOk = true;
    }
}

static String statusJson()
{
    String s = "{\"image\":{\"ok\":";
    s += g_imgOk ? "true" : "false";
    s += ",\"size\":"; s += g_imgSize;
    s += ",\"sha256\":\""; s += g_imgOk ? shaHex(g_imgSha) : String(""); s += "\"}";
    s += ",\"nodes\":[";
    bool first = true;
    const uint32_t now = millis();
    for (uint8_t n = 1; n < OTA_MAX_NODES; n++) {
        const NodeState &st = g_nodes[n];
        if (!st.offered && st.session == 0) continue;
        if (!first) s += ",";
        first = false;
        s += "{\"node\":"; s += n;
        s += ",\"session\":"; s += st.session;
        s += ",\"state\":"; s += st.state;
        s += ",\"error\":"; s += st.error;
        s += ",\"received\":"; s += st.received;
        s += ",\"size\":"; s += st.size;
        s += ",\"age_ms\":"; s += st.lastMs ? (now - st.lastMs) : 0;
        s += "}";
    }
    s += "]}";
    return s;
}

static bool offerTo(uint8_t node, uint32_t session)
{
    if (!g_sendTo) return false;
    MsgOtaOffer o = {};
    o.cmd = CMD_OTA_OFFER;
    o.node_id = node;
    o.session = session;
    o.size = g_imgSize;
    memcpy(o.sha256, g_imgSha, sizeof(o.sha256));
    strncpy(o.path, IMG_PATH, sizeof(o.path) - 1);
    NodeState &st = g_nodes[node];
    st = NodeState();
    st.session = session;
    st.size = g_imgSize;
    st.offered = true;
    st.lastMs = millis();
    /* Unicast con reintentos cortos: si se pierde, el usuario reintenta. */
    bool ok = false;
    for (int i = 0; i < 3; i++) ok |= g_sendTo(node, (const uint8_t *)&o, sizeof(o));
    return ok;
}

inline void begin(AsyncWebServer &server, SendToNodeFn sendTo, BusyFn busy, NodeSeenFn seen)
{
    g_sendTo = sendTo;
    g_busy = busy;
    g_seen = seen;
    for (size_t i = 0; i + 1 < sizeof(ROLE_MARK_ENC); i++) ROLE_MARK[i] = (char)(ROLE_MARK_ENC[i] - 1);
    loadImageMeta();

    server.on("/ota/upload", HTTP_POST,
        [](AsyncWebServerRequest *req) {
            if (g_upError.length()) {
                req->send(400, "application/json",
                          String("{\"ok\":false,\"error\":\"") + g_upError + "\"}");
                return;
            }
            req->send(200, "application/json",
                      String("{\"ok\":true,\"size\":") + g_imgSize +
                      ",\"sha256\":\"" + shaHex(g_imgSha) + "\"}");
        },
        onUpload);

    server.on("/ota/slave.bin", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!g_imgOk) { req->send(404, "text/plain", "sin imagen"); return; }
        req->send(LittleFS, IMG_PATH, "application/octet-stream");
    });

    server.on("/ota/status", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "application/json", statusJson());
    });

    server.on("/ota/start", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!g_imgOk) { req->send(409, "application/json", "{\"ok\":false,\"error\":\"no hay imagen\"}"); return; }
        if (g_busy && g_busy()) { req->send(409, "application/json", "{\"ok\":false,\"error\":\"captura en curso\"}"); return; }
        long node = req->hasParam("node") ? req->getParam("node")->value().toInt() : 0;
        const uint32_t session = (uint32_t)esp_random();
        int sent = 0;
        for (uint8_t n = 1; n < OTA_MAX_NODES; n++) {
            if (node != 0 && n != node) continue;
            if (node == 0 && g_seen && !g_seen(n)) continue;
            if (offerTo(n, session)) sent++;
        }
        req->send(200, "application/json",
                  String("{\"ok\":") + (sent ? "true" : "false") + ",\"offered\":" + sent +
                  ",\"session\":" + session + "}");
    });

    server.on("/ota/abort", HTTP_POST, [](AsyncWebServerRequest *req) {
        long node = req->hasParam("node") ? req->getParam("node")->value().toInt() : 0;
        for (uint8_t n = 1; n < OTA_MAX_NODES; n++) {
            if (node != 0 && n != node) continue;
            if (!g_nodes[n].offered || !g_sendTo) continue;
            MsgOtaControl c = { CMD_OTA_CONTROL, n, g_nodes[n].session, 1u };
            g_sendTo(n, (const uint8_t *)&c, sizeof(c));
        }
        req->send(200, "application/json", "{\"ok\":true}");
    });
}

/* Estado que manda un esclavo (tarea WiFi: solo copia). */
inline void onStatus(const MsgOtaStatus &m)
{
    if (m.node_id == 0 || m.node_id >= OTA_MAX_NODES) return;
    NodeState &st = g_nodes[m.node_id];
    st.session = m.session;
    st.state = m.state;
    st.error = m.error;
    st.received = m.received;
    if (m.size) st.size = m.size;
    st.lastMs = millis();
    st.offered = true;
    if (m.state == OTA_ST_PENDING) st.ackPending = true;
}

/* Desde loop(): el esclavo confirma la imagen nueva cuando oye al maestro, y en
 * reposo el maestro no le manda nada; sin esto volvia a la anterior a los 180 s. */
inline void service()
{
    if (!g_sendTo) return;
    for (uint8_t n = 1; n < OTA_MAX_NODES; n++) {
        NodeState &st = g_nodes[n];
        if (!st.ackPending) continue;
        st.ackPending = false;
        MsgOtaControl c = { CMD_OTA_CONTROL, n, st.session, 2u };
        g_sendTo(n, (const uint8_t *)&c, sizeof(c));
    }
}

}  // namespace otam
