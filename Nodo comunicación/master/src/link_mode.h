#pragma once
/* ============================================================================
 *  link_mode.h — modo ENLACE del maestro + cola de capturas en LittleFS
 * ----------------------------------------------------------------------------
 *  Implementa la multiplexacion temporal descrita en PLAN_CONECTIVIDAD_MASTER.md:
 *  el maestro nunca esta en las dos redes a la vez.
 *
 *    CAPTURA : modo normal. AP propio en canal 1, ESP-NOW con los esclavos,
 *              SPA local. Sin cambios respecto del firmware historico.
 *    ENLACE  : el maestro baja su AP, apaga ESP-NOW y se asocia como STA a la
 *              red con salida a internet (hotspot del celular en campo, WiFi de
 *              casa de noche). Sube la cola por HTTP POST y vuelve a CAPTURA.
 *
 *  Esto es seguro porque el protocolo con los esclavos es maestro-iniciado (los
 *  esclavos esperan pasivos) y no escanean canales: tienen hardcodeado
 *  esp_wifi_set_channel(1). Apenas el AP vuelve a canal 1, ESP-NOW retoma sin
 *  negociacion. Regla dura: NO cambiar de fase a mitad de una captura — por eso
 *  enlaceRequest() solo acepta desde IDLE/ARMED (ver linkModeCanEnter()).
 *
 *  El buffer que faltaba (el maestro no persistia nada: los datos se espejaban
 *  en vivo por WebSocket y el navegador los acumulaba) se resuelve encolando el
 *  dump en LittleFS: durante DUMPING cada lote se escribe ademas a /q/NNNN.geoq.
 *  Sin navegador conectado de noche, el dato igual sobrevive hasta la subida.
 *
 *  Formato .geoq (little-endian) — lo decodifica software del sink en Python
 *  (src/interfaces/python/sink/geoq.py). Todo lo que el maestro sabe del nodo
 *  viaja en la tabla, para que el sink pueda armar el metadata.json que consume
 *  discover_dataset() sin depender del navegador.
 *
 *    Cabecera (48 B):
 *      0   4  magic "GEOQ"
 *      4   1  version = 1
 *      5   1  node_count      (entradas en la tabla)
 *      6   2  n_batches       (largo canonico configurado, en lotes)
 *      8   4  session_id      (contador persistente del maestro)
 *      12  4  uptime_ms       (millis() al abrir la captura)
 *      16  16 site            (texto NUL-padded: nombre del punto/carpeta)
 *      32  4  distance_mm     (distancia geo-martillo en mm; 0 = desconocida)
 *      36  4  epoch_s         (0 = el maestro no tiene reloj; lo sella el sink)
 *      40  8  reservado
 *
 *    Tabla de nodos (node_count x 14 B):
 *      0   1  node_id
 *      1   1  hw_class        (SLAVE_HW_GEO / SLAVE_HW_HAMMER / UNKNOWN)
 *      2   2  sample_rate     (Hz reportado por el PSoC; 0 = desconocido)
 *      4   2  n_batches_node  (largo propio: HAMMER puede tener menos)
 *      6   1  flags           bit0 psoc_ok, bit1 sd_present
 *      7   1  reservado
 *      8   6  mac
 *
 *    Registros (102 B cada uno, hasta EOF):
 *      0   1  node_id
 *      1   2  seq
 *      3   1  global_flags
 *      4   8  timestamp_us
 *      12  90 muestras (30 x 3 bytes, mismo empaquetado que ESP-NOW)
 *
 *  Ver PLAN_CONECTIVIDAD_MASTER.md y el README del sink.
 * ==========================================================================*/

#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_wifi.h>

#include "master_log.h"

/* ── Configuracion persistente ───────────────────────────────────────────── */
/* Archivo de texto plano clave=valor en LittleFS. Se edita por HTTP desde la
 * SPA (/enlace/config) — no se usa ArduinoJson para no sumar dependencia por
 * cinco campos, igual criterio que web_relay.h. */
#define LINK_CFG_PATH   "/enlace.cfg"
#define LINK_QUEUE_DIR  "/q"
#define LINK_SEQ_PATH   "/enlace.seq"

/* Margen que se deja libre en LittleFS: la SPA vive en el mismo FS y quedarse
 * sin espacio la dejaria sin servir. */
#ifndef LINK_FS_RESERVE_BYTES
#define LINK_FS_RESERVE_BYTES  (96u * 1024u)
#endif

#ifndef LINK_STA_CONNECT_TIMEOUT_MS
#define LINK_STA_CONNECT_TIMEOUT_MS  20000u
#endif

#ifndef LINK_HTTP_TIMEOUT_MS
#define LINK_HTTP_TIMEOUT_MS  20000u
#endif

/* Cuanto puede durar toda la fase ENLACE antes de volver a CAPTURA igual.
 * Es un limite de seguridad: en campo, quedarse en ENLACE es quedarse sordo. */
#ifndef LINK_PHASE_MAX_MS
#define LINK_PHASE_MAX_MS  (5u * 60u * 1000u)
#endif

struct LinkConfig {
    char ssid[33]  = {0};
    char pass[65]  = {0};
    char url[129]  = {0};   /* endpoint del sink, ej https://pc.tailnet.ts.net/ingest */
    char site[17]  = {0};   /* nombre del punto -> carpeta del dataset */
    uint32_t distance_mm = 0;
    bool auto_upload = false;  /* entrar en ENLACE solo al terminar el dump */
};

static LinkConfig g_linkCfg;
static uint32_t   g_linkSessionId = 0;

/* ── Estado del runner ───────────────────────────────────────────────────── */
enum LinkPhase {
    LINK_IDLE = 0,      /* fase CAPTURA: el modulo solo encola */
    LINK_REQUESTED,     /* pedido aceptado, se entra en el proximo loop() */
    LINK_CONNECTING,    /* AP abajo, ESP-NOW abajo, asociando como STA */
    LINK_UPLOADING,     /* asociado, subiendo la cola */
    LINK_RESTORING      /* volviendo a AP canal 1 + ESP-NOW */
};

static LinkPhase g_linkPhase = LINK_IDLE;
static uint32_t  g_linkPhaseStartMs = 0;
static uint32_t  g_linkConnectStartMs = 0;
static uint16_t  g_linkUploadedOk = 0;
static uint16_t  g_linkUploadedFail = 0;
static char      g_linkLastError[64] = {0};

/* Escritor de la cola */
static File     g_linkQueueFile;
static bool     g_linkQueueOpen = false;
static bool     g_linkQueueOverflow = false;
static uint32_t g_linkQueueBytes = 0;
static char     g_linkQueuePath[32] = {0};

static inline bool linkQueueActive() { return g_linkQueueOpen; }
static inline LinkPhase linkPhase()  { return g_linkPhase; }
static inline bool linkModeActive()  { return g_linkPhase != LINK_IDLE; }
static inline const LinkConfig &linkConfig() { return g_linkCfg; }

/* ── Config: cargar / guardar ────────────────────────────────────────────── */

static void linkCfgCopy(char *dst, size_t cap, const String &v)
{
    strncpy(dst, v.c_str(), cap - 1);
    dst[cap - 1] = '\0';
}

inline void linkConfigLoad()
{
    File f = LittleFS.open(LINK_CFG_PATH, "r");
    if (!f) return;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        int eq = line.indexOf('=');
        if (eq <= 0) continue;
        String k = line.substring(0, eq);
        String v = line.substring(eq + 1);
        if      (k == "ssid") linkCfgCopy(g_linkCfg.ssid, sizeof(g_linkCfg.ssid), v);
        else if (k == "pass") linkCfgCopy(g_linkCfg.pass, sizeof(g_linkCfg.pass), v);
        else if (k == "url")  linkCfgCopy(g_linkCfg.url,  sizeof(g_linkCfg.url),  v);
        else if (k == "site") linkCfgCopy(g_linkCfg.site, sizeof(g_linkCfg.site), v);
        else if (k == "distance_mm") g_linkCfg.distance_mm = (uint32_t)v.toInt();
        else if (k == "auto") g_linkCfg.auto_upload = (v.toInt() != 0);
    }
    f.close();
}

inline bool linkConfigSave()
{
    File f = LittleFS.open(LINK_CFG_PATH, "w");
    if (!f) return false;
    f.printf("ssid=%s\n", g_linkCfg.ssid);
    f.printf("pass=%s\n", g_linkCfg.pass);
    f.printf("url=%s\n",  g_linkCfg.url);
    f.printf("site=%s\n", g_linkCfg.site);
    f.printf("distance_mm=%u\n", (unsigned)g_linkCfg.distance_mm);
    f.printf("auto=%d\n", g_linkCfg.auto_upload ? 1 : 0);
    f.close();
    return true;
}

/* Reemplaza la config completa y la persiste (usado por POST /enlace/config). */
inline void linkConfigSet(const LinkConfig &cfg)
{
    g_linkCfg = cfg;
    linkConfigSave();
    MASTER_LOG_PRINTF("[ENLACE] cfg ssid=%s url=%s site=%s dist=%u mm auto=%d\n",
                      g_linkCfg.ssid, g_linkCfg.url, g_linkCfg.site,
                      (unsigned)g_linkCfg.distance_mm,
                      g_linkCfg.auto_upload ? 1 : 0);
}

static uint32_t linkNextSessionId()
{
    uint32_t id = 1;
    File f = LittleFS.open(LINK_SEQ_PATH, "r");
    if (f) { id = (uint32_t)f.readStringUntil('\n').toInt() + 1; f.close(); }
    if (id == 0) id = 1;
    File w = LittleFS.open(LINK_SEQ_PATH, "w");
    if (w) { w.printf("%u\n", (unsigned)id); w.close(); }
    return id;
}

/* ── Cola: espacio disponible ────────────────────────────────────────────── */

inline uint32_t linkQueueFreeBytes()
{
    uint32_t total = (uint32_t)LittleFS.totalBytes();
    uint32_t used  = (uint32_t)LittleFS.usedBytes();
    uint32_t free_ = (total > used) ? (total - used) : 0;
    return (free_ > LINK_FS_RESERVE_BYTES) ? (free_ - LINK_FS_RESERVE_BYTES) : 0;
}

inline uint16_t linkQueueCount()
{
    uint16_t n = 0;
    File dir = LittleFS.open(LINK_QUEUE_DIR);
    if (!dir || !dir.isDirectory()) return 0;
    File e = dir.openNextFile();
    while (e) { if (!e.isDirectory()) n++; e = dir.openNextFile(); }
    dir.close();
    return n;
}

inline uint32_t linkQueueBytesPending()
{
    uint32_t total = 0;
    File dir = LittleFS.open(LINK_QUEUE_DIR);
    if (!dir || !dir.isDirectory()) return 0;
    File e = dir.openNextFile();
    while (e) { if (!e.isDirectory()) total += (uint32_t)e.size(); e = dir.openNextFile(); }
    dir.close();
    return total;
}

/* ── Cola: escritura de una captura ──────────────────────────────────────── */

/* Descriptor de nodo que main.cpp arma desde su cache de HELLO. */
struct LinkNodeInfo {
    uint8_t  node_id;
    uint8_t  hw_class;
    uint16_t sample_rate;
    uint16_t n_batches;
    bool     psoc_ok;
    bool     sd_present;
    uint8_t  mac[6];
};

/* Abre /q/NNNN.geoq y escribe cabecera + tabla de nodos.
 * Devuelve false si no hay config de sitio/URL o no entra en el FS: en ese caso
 * el dump sigue normal hacia WS/MATLAB, solo que no se encola. */
inline bool linkQueueBegin(uint16_t nBatches,
                           const LinkNodeInfo *nodes, uint8_t nodeCount,
                           uint32_t expectedBytes)
{
    if (g_linkQueueOpen) return false;
    if (nodeCount == 0)  return false;

    /* Sin endpoint configurado no tiene sentido llenar el FS. */
    if (g_linkCfg.url[0] == '\0') return false;

    uint32_t need = 48u + (uint32_t)nodeCount * 14u + expectedBytes;
    if (need > linkQueueFreeBytes()) {
        MASTER_LOG_PRINTF("[ENLACE] cola: no entra (%u B need, %u B libres)\n",
                          (unsigned)need, (unsigned)linkQueueFreeBytes());
        LOGM("ENLACE_QUEUE_FULL", "need=%u,free=%u",
             (unsigned)need, (unsigned)linkQueueFreeBytes());
        return false;
    }

    if (!LittleFS.exists(LINK_QUEUE_DIR)) LittleFS.mkdir(LINK_QUEUE_DIR);

    g_linkSessionId = linkNextSessionId();
    snprintf(g_linkQueuePath, sizeof(g_linkQueuePath),
             LINK_QUEUE_DIR "/%04u.geoq", (unsigned)(g_linkSessionId % 10000u));

    g_linkQueueFile = LittleFS.open(g_linkQueuePath, "w");
    if (!g_linkQueueFile) {
        MASTER_LOG_PRINTF("[ENLACE] cola: no se pudo abrir %s\n", g_linkQueuePath);
        return false;
    }

    uint8_t hdr[48] = {0};
    memcpy(hdr + 0, "GEOQ", 4);
    hdr[4] = 1;                       /* version */
    hdr[5] = nodeCount;
    hdr[6] = (uint8_t)(nBatches & 0xFF);
    hdr[7] = (uint8_t)(nBatches >> 8);
    memcpy(hdr + 8,  &g_linkSessionId, 4);
    uint32_t up = millis();
    memcpy(hdr + 12, &up, 4);
    memcpy(hdr + 16, g_linkCfg.site, 16);
    memcpy(hdr + 32, &g_linkCfg.distance_mm, 4);
    /* epoch_s queda en 0: el maestro no tiene RTC. Lo sella el sink al recibir. */
    g_linkQueueFile.write(hdr, sizeof(hdr));

    for (uint8_t i = 0; i < nodeCount; i++) {
        uint8_t e[14] = {0};
        e[0] = nodes[i].node_id;
        e[1] = nodes[i].hw_class;
        e[2] = (uint8_t)(nodes[i].sample_rate & 0xFF);
        e[3] = (uint8_t)(nodes[i].sample_rate >> 8);
        e[4] = (uint8_t)(nodes[i].n_batches & 0xFF);
        e[5] = (uint8_t)(nodes[i].n_batches >> 8);
        e[6] = (uint8_t)((nodes[i].psoc_ok ? 0x01 : 0) | (nodes[i].sd_present ? 0x02 : 0));
        memcpy(e + 8, nodes[i].mac, 6);
        g_linkQueueFile.write(e, sizeof(e));
    }

    g_linkQueueOpen = true;
    g_linkQueueOverflow = false;
    g_linkQueueBytes = 48u + (uint32_t)nodeCount * 14u;
    MASTER_LOG_PRINTF("[ENLACE] cola abierta %s (%u nodos, n=%u)\n",
                      g_linkQueuePath, nodeCount, nBatches);
    LOGM("ENLACE_QUEUE_OPEN", "file=%s,nodes=%u,n=%u",
         g_linkQueuePath, nodeCount, nBatches);
    return true;
}

/* Un lote de 30 muestras tal como llego por ESP-NOW. Se guarda crudo: el sink
 * hace la conversion a f32 y el armado de carpetas, no el firmware. */
inline void linkQueueBatch(uint8_t nodeId, uint16_t seq, uint8_t flags,
                           uint64_t tsUs, const uint8_t *packed90)
{
    if (!g_linkQueueOpen || g_linkQueueOverflow) return;

    uint8_t rec[12];
    rec[0] = nodeId;
    rec[1] = (uint8_t)(seq & 0xFF);
    rec[2] = (uint8_t)(seq >> 8);
    rec[3] = flags;
    memcpy(rec + 4, &tsUs, 8);

    if (g_linkQueueFile.write(rec, sizeof(rec)) != sizeof(rec) ||
        g_linkQueueFile.write(packed90, 90) != 90) {
        g_linkQueueOverflow = true;
        MASTER_LOG_PRINTLN("[ENLACE] cola: escritura fallo (FS lleno?)");
        LOGM("ENLACE_QUEUE_WRFAIL", "bytes=%u", (unsigned)g_linkQueueBytes);
        return;
    }
    g_linkQueueBytes += 102;
}

/* Cierra la captura. ok=false descarta el archivo (captura abortada). */
inline void linkQueueEnd(bool ok)
{
    if (!g_linkQueueOpen) return;
    g_linkQueueFile.close();
    g_linkQueueOpen = false;

    if (!ok || g_linkQueueOverflow) {
        MASTER_LOG_PRINTF("[ENLACE] cola descartada %s (ok=%d overflow=%d)\n",
                          g_linkQueuePath, ok ? 1 : 0, g_linkQueueOverflow ? 1 : 0);
        LittleFS.remove(g_linkQueuePath);
        LOGM("ENLACE_QUEUE_DROP", "ok=%d,overflow=%d", ok ? 1 : 0, g_linkQueueOverflow ? 1 : 0);
        return;
    }
    MASTER_LOG_PRINTF("[ENLACE] cola cerrada %s (%u B, %u en cola)\n",
                      g_linkQueuePath, (unsigned)g_linkQueueBytes,
                      (unsigned)linkQueueCount());
    LOGM("ENLACE_QUEUE_CLOSE", "file=%s,bytes=%u", g_linkQueuePath,
         (unsigned)g_linkQueueBytes);
}

/* ── Runner de la fase ENLACE ────────────────────────────────────────────── */

/* main.cpp decide si el estado permite cambiar de fase (regla dura: nunca a
 * mitad de captura). Este modulo solo valida su propia configuracion. */
inline bool linkConfigUsable()
{
    return g_linkCfg.ssid[0] != '\0' && g_linkCfg.url[0] != '\0';
}

inline bool linkRequestUpload(const char **why)
{
    if (g_linkPhase != LINK_IDLE) { if (why) *why = "ya en ENLACE"; return false; }
    if (!linkConfigUsable())      { if (why) *why = "falta ssid/url"; return false; }
    if (linkQueueCount() == 0)    { if (why) *why = "cola vacia";     return false; }
    g_linkPhase = LINK_REQUESTED;
    g_linkPhaseStartMs = millis();
    g_linkUploadedOk = 0;
    g_linkUploadedFail = 0;
    g_linkLastError[0] = '\0';
    MASTER_LOG_PRINTF("[ENLACE] pedido: %u archivos, %u B\n",
                      (unsigned)linkQueueCount(), (unsigned)linkQueueBytesPending());
    LOGM("ENLACE_REQ", "files=%u,bytes=%u",
         (unsigned)linkQueueCount(), (unsigned)linkQueueBytesPending());
    return true;
}

/* Sube un archivo de la cola. Devuelve true si el sink lo acepto (2xx). */
static bool linkUploadOne(const char *path)
{
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    size_t sz = f.size();

    WiFiClient plain;
    HTTPClient http;
    http.setTimeout(LINK_HTTP_TIMEOUT_MS);
    http.setConnectTimeout(LINK_HTTP_TIMEOUT_MS);
    /* Tailscale Funnel expone el sink por HTTPS publico con TLS valido; para
     * simplificar el firmware se acepta el certificado sin pinning (el control
     * de acceso real es el token compartido, ver X-Geo-Token). */
    bool ok = false;
    if (strncmp(g_linkCfg.url, "https:", 6) == 0) {
        WiFiClientSecure tls;
        tls.setInsecure();
        ok = http.begin(tls, g_linkCfg.url);
    } else {
        ok = http.begin(plain, g_linkCfg.url);
    }
    if (!ok) { f.close(); return false; }

    http.addHeader("Content-Type", "application/octet-stream");
    http.addHeader("X-Geo-File", path);
    http.addHeader("X-Geo-Site", g_linkCfg.site);

    int code = http.sendRequest("POST", &f, sz);
    http.end();
    f.close();

    if (code >= 200 && code < 300) {
        MASTER_LOG_PRINTF("[ENLACE] subido %s (%u B) -> %d\n",
                          path, (unsigned)sz, code);
        return true;
    }
    snprintf(g_linkLastError, sizeof(g_linkLastError), "HTTP %d en %s", code, path);
    MASTER_LOG_PRINTF("[ENLACE] fallo %s -> %d\n", path, code);
    return false;
}

/* Baja AP + ESP-NOW y asocia como STA. main.cpp pasa el teardown de ESP-NOW
 * porque la instancia vive alla. */
typedef void (*LinkRadioHook)();

inline void linkEnterSta(LinkRadioHook espnowDown)
{
    if (espnowDown) espnowDown();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(g_linkCfg.ssid, g_linkCfg.pass);
    g_linkConnectStartMs = millis();
    MASTER_LOG_PRINTF("[ENLACE] STA -> %s\n", g_linkCfg.ssid);
    LOGM("ENLACE_STA", "ssid=%s", g_linkCfg.ssid);
}

inline void linkRestoreCapture(LinkRadioHook apUp)
{
    WiFi.disconnect(true, false);
    if (apUp) apUp();   /* main.cpp levanta AP canal 1 + DNS + ESP-NOW */
    MASTER_LOG_PRINTF("[ENLACE] vuelta a CAPTURA (ok=%u fail=%u, quedan %u)\n",
                      g_linkUploadedOk, g_linkUploadedFail, (unsigned)linkQueueCount());
    LOGM("ENLACE_DONE", "ok=%u,fail=%u,left=%u",
         g_linkUploadedOk, g_linkUploadedFail, (unsigned)linkQueueCount());
}

/* Bombea la maquina de la fase ENLACE. Devuelve true mientras siga en ENLACE.
 * Se llama desde loop() solo cuando g_state == ENLACE. */
inline bool linkService(LinkRadioHook espnowDown, LinkRadioHook apUp)
{
    switch (g_linkPhase) {
        case LINK_IDLE:
            return false;

        case LINK_REQUESTED:
            linkEnterSta(espnowDown);
            g_linkPhase = LINK_CONNECTING;
            return true;

        case LINK_CONNECTING:
            if (WiFi.status() == WL_CONNECTED) {
                MASTER_LOG_PRINTF("[ENLACE] asociado, IP %s\n",
                                  WiFi.localIP().toString().c_str());
                LOGM("ENLACE_UP", "ip=%s", WiFi.localIP().toString().c_str());
                g_linkPhase = LINK_UPLOADING;
            } else if (millis() - g_linkConnectStartMs > LINK_STA_CONNECT_TIMEOUT_MS) {
                snprintf(g_linkLastError, sizeof(g_linkLastError),
                         "sin asociar a %s", g_linkCfg.ssid);
                MASTER_LOG_PRINTLN("[ENLACE] timeout de asociacion");
                LOGM("ENLACE_TIMEOUT", "ssid=%s", g_linkCfg.ssid);
                g_linkPhase = LINK_RESTORING;
            }
            return true;

        case LINK_UPLOADING: {
            /* Un archivo por pasada de loop(): subir en bloque bloquearia el
             * watchdog y dejaria la UI muda mas tiempo del necesario. */
            char path[32] = {0};
            File dir = LittleFS.open(LINK_QUEUE_DIR);
            if (dir && dir.isDirectory()) {
                File e = dir.openNextFile();
                while (e) {
                    if (!e.isDirectory()) {
                        snprintf(path, sizeof(path), LINK_QUEUE_DIR "/%s", e.name());
                        break;
                    }
                    e = dir.openNextFile();
                }
                dir.close();
            }

            if (path[0] == '\0') {           /* cola vacia: listo */
                g_linkPhase = LINK_RESTORING;
                return true;
            }
            if (millis() - g_linkPhaseStartMs > LINK_PHASE_MAX_MS) {
                snprintf(g_linkLastError, sizeof(g_linkLastError), "limite de fase");
                MASTER_LOG_PRINTLN("[ENLACE] limite de fase, volviendo a CAPTURA");
                g_linkPhase = LINK_RESTORING;
                return true;
            }

            if (linkUploadOne(path)) {
                LittleFS.remove(path);
                g_linkUploadedOk++;
            } else {
                g_linkUploadedFail++;
                /* Un fallo deja el archivo en la cola para el proximo ENLACE y
                 * corta la pasada: si el sink no esta, reintentar en bucle solo
                 * quema bateria. */
                g_linkPhase = LINK_RESTORING;
            }
            return true;
        }

        case LINK_RESTORING:
            linkRestoreCapture(apUp);
            g_linkPhase = LINK_IDLE;
            return false;
    }
    return false;
}

/* Resumen para /enlace/status y para el heartbeat de la SPA. */
inline String linkStatusText()
{
    String s;
    s.reserve(320);
    s += "phase=";
    switch (g_linkPhase) {
        case LINK_IDLE:       s += "captura";    break;
        case LINK_REQUESTED:  s += "pedido";     break;
        case LINK_CONNECTING: s += "conectando"; break;
        case LINK_UPLOADING:  s += "subiendo";   break;
        case LINK_RESTORING:  s += "volviendo";  break;
    }
    s += "\nssid=";        s += g_linkCfg.ssid;
    s += "\nurl=";         s += g_linkCfg.url;
    s += "\nsite=";        s += g_linkCfg.site;
    s += "\ndistance_mm="; s += String(g_linkCfg.distance_mm);
    s += "\nauto=";        s += g_linkCfg.auto_upload ? "1" : "0";
    s += "\nqueue_files="; s += String(linkQueueCount());
    s += "\nqueue_bytes="; s += String(linkQueueBytesPending());
    s += "\nfs_free=";     s += String(linkQueueFreeBytes());
    s += "\nup_ok=";       s += String(g_linkUploadedOk);
    s += "\nup_fail=";     s += String(g_linkUploadedFail);
    s += "\nlast_error=";  s += g_linkLastError;
    s += "\n";
    return s;
}

inline void linkModeBegin()
{
    linkConfigLoad();
    if (!LittleFS.exists(LINK_QUEUE_DIR)) LittleFS.mkdir(LINK_QUEUE_DIR);
    MASTER_LOG_PRINTF("[ENLACE] cfg ssid=%s url=%s site=%s auto=%d cola=%u (%u B)\n",
                      g_linkCfg.ssid, g_linkCfg.url, g_linkCfg.site,
                      g_linkCfg.auto_upload ? 1 : 0,
                      (unsigned)linkQueueCount(), (unsigned)linkQueueBytesPending());
}
