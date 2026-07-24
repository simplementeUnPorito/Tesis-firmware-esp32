#pragma once
/* ============================================================================
 *  link_mode.h — modo ENLACE del maestro + cola de capturas en LittleFS
 * ----------------------------------------------------------------------------
 *  Implementa la multiplexacion temporal descrita en PLAN_CONECTIVIDAD_MASTER.md:
 *  el maestro nunca esta en las dos redes a la vez.
 *
 *    CAPTURA : modo normal. AP propio en canal 1, ESP-NOW con los esclavos,
 *              SPA local. Sin cambios respecto del firmware historico.
 *    ENLACE  : el maestro baja su AP, apaga ESP-NOW y se asocia como STA al
 *              hotspot del cliente adquisidor (celular) o a un router. Ahi sigue
 *              sirviendo la SPA y ademas expone la cola para que el cliente se
 *              la lleve. Cuando el cliente confirma, vuelve a CAPTURA.
 *
 *  El maestro NO sale a internet ni sube nada: el que atraviesa el tunel hacia
 *  el server es el cliente adquisidor, que ya tiene Tailscale de verdad. Por eso
 *  aca no hay cliente HTTP, ni TLS, ni tokens — y el server nunca queda expuesto
 *  publicamente. El ESP solo tiene que ser alcanzable por IP desde el celular,
 *  que es justo lo que habilita asociarse a su hotspot: el celular conserva sus
 *  datos moviles (no los pierde como cuando se conecta al AP del ESP) y al mismo
 *  tiempo alcanza al maestro.
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
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "master_log.h"

/* ── Configuracion persistente ───────────────────────────────────────────── */
/* Archivo de texto plano clave=valor en LittleFS. Se edita por HTTP desde la
 * SPA (/enlace/config) — no se usa ArduinoJson para no sumar dependencia por
 * cinco campos, igual criterio que web_relay.h. */
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

/* Cuanto se queda sirviendo la cola sin que el cliente toque nada antes de
 * volver a CAPTURA. Estar en ENLACE es estar sordo a los esclavos, asi que no
 * se queda esperando indefinidamente a un cliente que no vino. */
#ifndef LINK_SERVE_IDLE_MS
#define LINK_SERVE_IDLE_MS  (3u * 60u * 1000u)
#endif

/* Cuanto puede durar toda la fase ENLACE antes de volver a CAPTURA igual.
 * Es un limite de seguridad: en campo, quedarse en ENLACE es quedarse sordo. */
#ifndef LINK_PHASE_MAX_MS
#define LINK_PHASE_MAX_MS  (5u * 60u * 1000u)
#endif

struct LinkConfig {
    char ssid[33]  = {0};   /* red con internet a la que asociarse (hotspot/router) */
    char pass[65]  = {0};
    /* URL del servidor de datos. El maestro NO la usa: nunca sale a internet.
     * La guarda para que cualquier cliente que abra la SPA la tenga sin volver
     * a cargarla — el que hace el POST es el navegador. */
    char server_url[129] = {0};
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
    LINK_SERVING,       /* asociado: la SPA y la cola quedan expuestas al cliente */
    LINK_RESTORING      /* volviendo a AP canal 1 + ESP-NOW */
};

static LinkPhase g_linkPhase = LINK_IDLE;
static uint32_t  g_linkPhaseStartMs = 0;
static uint32_t  g_linkConnectStartMs = 0;
static uint32_t  g_linkLastClientMs = 0;   /* ultima actividad del adquisidor */
static uint16_t  g_linkServedFiles = 0;    /* archivos que el cliente confirmo */
static uint16_t  g_linkStartFiles = 0;     /* cola al entrar en la fase */
static char      g_linkLastError[64] = {0};

/* Escritor de la cola */
static File     g_linkQueueFile;
static bool     g_linkQueueOpen = false;
static bool     g_linkQueueOverflow = false;
static uint32_t g_linkQueueBytes = 0;
static char     g_linkQueuePath[32] = {0};

inline uint8_t linkCurrentChannel();   /* definida junto a linkStaService() */

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

/* La config vive en NVS, no en LittleFS, por una razon operativa concreta:
 * `pio run -t uploadfs` reescribe la particion entera de LittleFS, asi que cada
 * vez que se sube la SPA se borraria la red guardada. NVS es otra particion y
 * `uploadfs` no la toca: la red se carga una vez y sobrevive a las
 * actualizaciones de la interfaz. La cola de capturas sigue en LittleFS porque
 * es dato a granel, no configuracion. */
static Preferences g_linkPrefs;
#define LINK_NVS_NS  "enlace"

inline void linkConfigLoad()
{
    if (!g_linkPrefs.begin(LINK_NVS_NS, true)) return;   /* solo lectura */
    linkCfgCopy(g_linkCfg.ssid, sizeof(g_linkCfg.ssid), g_linkPrefs.getString("ssid", ""));
    linkCfgCopy(g_linkCfg.pass, sizeof(g_linkCfg.pass), g_linkPrefs.getString("pass", ""));
    linkCfgCopy(g_linkCfg.site, sizeof(g_linkCfg.site), g_linkPrefs.getString("site", ""));
    linkCfgCopy(g_linkCfg.server_url, sizeof(g_linkCfg.server_url),
                g_linkPrefs.getString("srv_url", ""));
    g_linkCfg.distance_mm = g_linkPrefs.getUInt("dist_mm", 0);
    g_linkCfg.auto_upload = g_linkPrefs.getBool("auto", false);
    g_linkPrefs.end();
}

inline bool linkConfigSave()
{
    if (!g_linkPrefs.begin(LINK_NVS_NS, false)) return false;
    g_linkPrefs.putString("ssid", g_linkCfg.ssid);
    g_linkPrefs.putString("pass", g_linkCfg.pass);
    g_linkPrefs.putString("site", g_linkCfg.site);
    g_linkPrefs.putString("srv_url", g_linkCfg.server_url);
    g_linkPrefs.putUInt("dist_mm", g_linkCfg.distance_mm);
    g_linkPrefs.putBool("auto", g_linkCfg.auto_upload);
    g_linkPrefs.end();
    return true;
}

/* Reemplaza la config completa y la persiste (usado por POST /enlace/config). */
inline void linkConfigSet(const LinkConfig &cfg)
{
    g_linkCfg = cfg;
    linkConfigSave();
    MASTER_LOG_PRINTF("[ENLACE] cfg ssid=%s site=%s dist=%u mm auto=%d\n",
                      g_linkCfg.ssid, g_linkCfg.site,
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
                           uint32_t expectedBytes, bool isView)
{
    if (g_linkQueueOpen) return false;
    if (nodeCount == 0)  return false;

    /* Se encola siempre que haya lugar: el dato es lo caro. Si no hay sitio
     * configurado igual se guarda y el server le pide el nombre al usuario. */

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
    hdr[40] = isView ? 0x01 : 0x00;   /* flags: bit0 = dump de VER (un nodo) */
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
    return g_linkCfg.ssid[0] != '\0';
}

inline bool linkRequestUpload(const char **why)
{
    if (g_linkPhase != LINK_IDLE) { if (why) *why = "ya en ENLACE"; return false; }
    if (!linkConfigUsable())      { if (why) *why = "falta ssid";   return false; }
    /* Con la cola vacia se entra igual: sirve para probar la conexion sin tener
     * que capturar antes. El idle de la fase lo devuelve a CAPTURA solo. */
    g_linkPhase = LINK_REQUESTED;
    g_linkPhaseStartMs = millis();
    g_linkServedFiles = 0;
    g_linkStartFiles = linkQueueCount();
    g_linkLastError[0] = '\0';
    MASTER_LOG_PRINTF("[ENLACE] pedido: %u archivos, %u B\n",
                      (unsigned)linkQueueCount(), (unsigned)linkQueueBytesPending());
    LOGM("ENLACE_REQ", "files=%u,bytes=%u",
         (unsigned)linkQueueCount(), (unsigned)linkQueueBytesPending());
    return true;
}

/* ── API para los endpoints que consume el cliente adquisidor ────────────── */

/* Cada request del cliente corre el reloj de inactividad: mientras se este
 * llevando la cola, el maestro no se vuelve a canal 1 abajo de sus pies. */
inline void linkNoteClientActivity() { g_linkLastClientMs = millis(); }

/* Listado de la cola en texto plano: "<nombre> <bytes>" por linea. */
inline String linkQueueListText()
{
    String s;
    File dir = LittleFS.open(LINK_QUEUE_DIR);
    if (!dir || !dir.isDirectory()) return s;
    File e = dir.openNextFile();
    while (e) {
        if (!e.isDirectory()) {
            s += e.name();
            s += ' ';
            s += String((unsigned)e.size());
            s += '\n';
        }
        e = dir.openNextFile();
    }
    dir.close();
    return s;
}

/* Ruta absoluta del archivo pedido, rechazando cualquier cosa con separadores:
 * el nombre viene de un query param, no se confia en el. */
inline bool linkQueueResolve(const String &name, char *out, size_t cap)
{
    if (name.length() == 0 || name.length() > 24) return false;
    if (name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 || name.indexOf("..") >= 0) {
        return false;
    }
    snprintf(out, cap, LINK_QUEUE_DIR "/%s", name.c_str());
    return LittleFS.exists(out);
}

/* El cliente confirma que ya subio el archivo al server: recien ahi se borra.
 * Si el cliente nunca confirma, el archivo sobrevive al proximo ENLACE — la
 * cola es la copia buena hasta que alguien diga lo contrario. */
inline bool linkQueueAck(const String &name)
{
    char path[40];
    if (!linkQueueResolve(name, path, sizeof(path))) return false;
    bool ok = LittleFS.remove(path);
    if (ok) {
        g_linkServedFiles++;
        MASTER_LOG_PRINTF("[ENLACE] cliente confirmo %s (quedan %u)\n",
                          path, (unsigned)linkQueueCount());
        LOGM("ENLACE_ACK", "file=%s,left=%u", path, (unsigned)linkQueueCount());
    }
    linkNoteClientActivity();
    return ok;
}

/* Hook para disparar ENLACE desde un endpoint HTTP. La decision de si el estado
 * lo permite (solo IDLE/ARMED) vive en main.cpp, que es quien conoce la maquina
 * de estados del maestro; aca solo se guarda el puntero. */
typedef bool (*LinkEnterFn)(const char **why);
static LinkEnterFn g_linkEnterHook = nullptr;
inline void linkSetEnterHook(LinkEnterFn fn) { g_linkEnterHook = fn; }

inline bool linkEnterViaHook(const char **why)
{
    if (!g_linkEnterHook) { if (why) *why = "sin hook"; return false; }
    return g_linkEnterHook(why);
}

/* El cliente avisa que termino: volver a CAPTURA sin esperar el idle. */
inline void linkClientDone()
{
    if (g_linkPhase == LINK_SERVING || g_linkPhase == LINK_CONNECTING) {
        MASTER_LOG_PRINTLN("[ENLACE] cliente termino, volviendo a CAPTURA");
        g_linkPhase = LINK_RESTORING;
    }
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
    MASTER_LOG_PRINTF("[ENLACE] vuelta a CAPTURA (entregados=%u, quedan %u)\n",
                      g_linkServedFiles, (unsigned)linkQueueCount());
    LOGM("ENLACE_DONE", "served=%u,left=%u",
         g_linkServedFiles, (unsigned)linkQueueCount());
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
                MASTER_LOG_PRINTF("[ENLACE] asociado a %s, IP %s — cola expuesta\n",
                                  g_linkCfg.ssid, WiFi.localIP().toString().c_str());
                LOGM("ENLACE_UP", "ip=%s,files=%u",
                     WiFi.localIP().toString().c_str(), (unsigned)linkQueueCount());
                linkNoteClientActivity();
                g_linkPhase = LINK_SERVING;
            } else if (millis() - g_linkConnectStartMs > LINK_STA_CONNECT_TIMEOUT_MS) {
                snprintf(g_linkLastError, sizeof(g_linkLastError),
                         "sin asociar a %s", g_linkCfg.ssid);
                MASTER_LOG_PRINTLN("[ENLACE] timeout de asociacion");
                LOGM("ENLACE_TIMEOUT", "ssid=%s", g_linkCfg.ssid);
                g_linkPhase = LINK_RESTORING;
            }
            return true;

        case LINK_SERVING:
            /* No hay nada que bombear: la SPA y los endpoints /enlace/* los
             * atiende AsyncWebServer en su propia task. Aca solo se vigila que
             * la fase no se eternice — estar en ENLACE es estar sordo. */
            /* Solo se corta por cola vacia si habia algo para entregar: si se
             * entro con la cola vacia (prueba de conexion) hay que quedarse
             * hasta el idle, o no se llega ni a ver la IP. */
            if (g_linkStartFiles > 0 && linkQueueCount() == 0) {
                MASTER_LOG_PRINTLN("[ENLACE] cola vacia, cliente se llevo todo");
                g_linkPhase = LINK_RESTORING;
            } else if (millis() - g_linkLastClientMs > LINK_SERVE_IDLE_MS) {
                snprintf(g_linkLastError, sizeof(g_linkLastError), "sin cliente");
                MASTER_LOG_PRINTF("[ENLACE] nadie vino en %u s, volviendo\n",
                                  (unsigned)(LINK_SERVE_IDLE_MS / 1000u));
                g_linkPhase = LINK_RESTORING;
            } else if (millis() - g_linkPhaseStartMs > LINK_PHASE_MAX_MS) {
                snprintf(g_linkLastError, sizeof(g_linkLastError), "limite de fase");
                MASTER_LOG_PRINTLN("[ENLACE] limite de fase, volviendo a CAPTURA");
                g_linkPhase = LINK_RESTORING;
            }
            return true;

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
        case LINK_SERVING:    s += "sirviendo";  break;
        case LINK_RESTORING:  s += "volviendo";  break;
    }
    s += "\nssid=";        s += g_linkCfg.ssid;
    s += "\nserver_url=";  s += g_linkCfg.server_url;
    s += "\nsite=";        s += g_linkCfg.site;
    s += "\ndistance_mm="; s += String(g_linkCfg.distance_mm);
    s += "\nauto=";        s += g_linkCfg.auto_upload ? "1" : "0";
    s += "\nsta=";         s += (WiFi.status() == WL_CONNECTED) ? "up" : "down";
    s += "\nip=";          s += (WiFi.status() == WL_CONNECTED)
                                  ? WiFi.localIP().toString() : String("");
    s += "\nchannel=";     s += String(linkCurrentChannel());
    s += "\nqueue_files="; s += String(linkQueueCount());
    s += "\nqueue_bytes="; s += String(linkQueueBytesPending());
    s += "\nfs_free=";     s += String(linkQueueFreeBytes());
    s += "\nserved=";      s += String(g_linkServedFiles);
    s += "\nlast_error=";  s += g_linkLastError;
    s += "\n";
    return s;
}

/* ── Coexistencia permanente (AP + STA a la vez) ─────────────────────────── */
/* El maestro no "cambia de fase": vive en las dos redes. Se queda en
 * WIFI_AP_STA con su AP arriba (192.168.4.1 nunca muere, no hace falta redirect
 * ni buscar la IP nueva) y ademas asociado a la red del cliente.
 *
 * La letra chica es la radio: el ESP32 tiene UNA sola. Al asociarse la STA, el
 * AP es arrastrado al canal del hotspot. ESP-NOW transmite en el canal vigente,
 * asi que los esclavos tienen que estar en ESE canal, no en uno fijo. Por eso el
 * esclavo escanea el SSID del maestro y adopta su canal (ver slave). El maestro
 * publica el canal en /enlace/status para poder diagnosticarlo. */

#ifndef LINK_STA_RETRY_MS
#define LINK_STA_RETRY_MS  20000u
#endif

/* Una sola direccion para todo, en las dos redes: http://geo.local
 * Del lado del AP propio tambien resuelve sin mDNS, porque el portal cautivo
 * contesta cualquier dominio con 192.168.4.1 (ver dnsServer en main.cpp). */
#ifndef LINK_MDNS_NAME
#define LINK_MDNS_NAME  "geo"
#endif

static uint32_t g_linkStaTryMs = 0;
static bool     g_linkStaWasUp = false;

inline uint8_t linkCurrentChannel()
{
    uint8_t ch = 0;
    wifi_second_chan_t sec;
    if (esp_wifi_get_channel(&ch, &sec) != ESP_OK) return 0;
    return ch;
}

/* Se llama desde loop() en fase CAPTURA. No toca el AP ni ESP-NOW: solo empuja
 * la STA hacia la red guardada y reporta cuando el estado cambia. */
inline void linkStaService()
{
    if (!linkConfigUsable()) return;
    if (g_linkPhase != LINK_IDLE) return;   /* la fase ENLACE maneja su propia STA */

    bool up = (WiFi.status() == WL_CONNECTED);
    if (up != g_linkStaWasUp) {
        g_linkStaWasUp = up;
        if (up) {
            MASTER_LOG_PRINTF("[ENLACE] STA arriba: %s ip=%s canal=%u\n",
                              g_linkCfg.ssid, WiFi.localIP().toString().c_str(),
                              linkCurrentChannel());
            LOGM("STA_UP", "ssid=%s,ip=%s,ch=%u", g_linkCfg.ssid,
                 WiFi.localIP().toString().c_str(), linkCurrentChannel());
            /* Re-anunciar mDNS con la interfaz nueva: si no, geo.local sigue
             * resolviendo solo del lado del AP y en la red del cliente no
             * aparece. La idea es que geo.local sea la unica direccion que haya
             * que recordar, en cualquiera de las dos redes. */
            MDNS.end();
            if (MDNS.begin(LINK_MDNS_NAME)) {
                MDNS.addService("http", "tcp", 80);
                MASTER_LOG_PRINTF("[ENLACE] mDNS re-anunciado: http://%s.local\n",
                                  LINK_MDNS_NAME);
            }
        } else {
            MASTER_LOG_PRINTF("[ENLACE] STA caida; AP sigue en canal %u\n",
                              linkCurrentChannel());
            LOGM("STA_DOWN", "ch=%u", linkCurrentChannel());
        }
    }
    if (up) return;

    if (g_linkStaTryMs != 0 && (millis() - g_linkStaTryMs) < LINK_STA_RETRY_MS) return;
    g_linkStaTryMs = millis();
    /* WiFi.begin sobre WIFI_AP_STA no baja el AP: la SPA sigue servida en
     * 192.168.4.1 durante todo el intento. */
    WiFi.begin(g_linkCfg.ssid, g_linkCfg.pass);
    MASTER_LOG_PRINTF("[ENLACE] STA intentando %s (canal actual %u)\n",
                      g_linkCfg.ssid, linkCurrentChannel());
}

/* Fuerza un intento inmediato: lo llama el POST /enlace/config, para que apretar
 * Guardar se sienta como "conectate ahora" sin que haya un boton aparte. */
inline void linkStaRetryNow()
{
    g_linkStaTryMs = 0;
    if (linkConfigUsable() && WiFi.status() == WL_CONNECTED) {
        WiFi.disconnect(false, false);   /* reasociar con la config nueva */
        g_linkStaWasUp = false;
    }
}

inline void linkModeBegin()
{
    linkConfigLoad();
    if (!LittleFS.exists(LINK_QUEUE_DIR)) LittleFS.mkdir(LINK_QUEUE_DIR);
    MASTER_LOG_PRINTF("[ENLACE] cfg ssid=%s site=%s auto=%d cola=%u (%u B)\n",
                      g_linkCfg.ssid, g_linkCfg.site,
                      g_linkCfg.auto_upload ? 1 : 0,
                      (unsigned)linkQueueCount(), (unsigned)linkQueueBytesPending());
}
