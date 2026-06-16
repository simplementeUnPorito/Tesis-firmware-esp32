#pragma once
/*
 * web_relay.h — Puente WebSocket <-> protocolo binario existente.
 *
 * Telemetría (maestro -> navegador): cada paquete de 6 bytes que
 * MatlabTransport ya emite hacia MATLAB (vía _emit, ver matlab_transport.h)
 * se espeja por WS como mensaje binario, con el mismo framing
 * [0x56][node][type][b2][b1][b0] — protocol.js lo decodifica igual que
 * protocol.py, así que no hace falta un protocolo nuevo.
 *
 * Comandos (navegador -> maestro): mensajes de texto JSON planos
 *   {"cmd":"A1","param":1}                      comando estándar   (4 bytes)
 *   {"cmd":"A3","value":100}                    set-N de 16 bits   (5 bytes)
 *   {"cmd":"BD","node":1,"sub":"A6","param":3}  dirigido a esclavo (6 bytes)
 * ("cmd"/"sub" llevan el byte en hex, p.ej. "A3"/"BD"/"A6" — igual que en
 * config.py). Se decodifican a MatlabTransport::RxCmd y se encolan para que
 * loop() los drene exactamente como matlab.hasCmd()/handleMatlabCmd(): un solo
 * despachador, sin duplicar lógica de ARM/START/STOP/PGA/VDAC/etc.
 */

#include <ESPAsyncWebServer.h>
#include "matlab_transport.h"
#include "master_log.h"

static AsyncWebSocket ws("/ws");

typedef void (*WebRelayClientHook)();
static WebRelayClientHook g_webRelayClientHook = nullptr;

/* ── Autenticación de aplicación (WS_AUTH_TOKEN) ───────────────────────────
 * Definir en platformio.ini: -DWS_AUTH_TOKEN='"tu_password"'
 * Sin la macro (o vacío) → sin autenticación (modo legacy).               */
#ifndef WS_AUTH_TOKEN
  #define WS_AUTH_TOKEN ""
#endif
static const bool WS_AUTH_ENABLED = (WS_AUTH_TOKEN[0] != '\0');
static bool     g_wsAuthOk     = false;   /* true: cliente autenticado */
static uint32_t g_wsAuthDeadMs = 0;       /* deadline de auth (0 = inactivo) */

static constexpr size_t WEB_RELAY_PKT_LEN = 6;
static constexpr size_t WEB_RELAY_TX_PKTS = 300;
static constexpr size_t WEB_RELAY_TX_BYTES = WEB_RELAY_TX_PKTS * WEB_RELAY_PKT_LEN;
static constexpr size_t WEB_RELAY_DUMP_BATCH_BYTES = 30 * WEB_RELAY_PKT_LEN;
static constexpr size_t WEB_RELAY_QUEUE_SOFT_MAX = 12;
static constexpr uint32_t WEB_RELAY_FLUSH_MS = 8u;
static uint8_t  g_webRelayTxBuf[WEB_RELAY_TX_BYTES];
static size_t   g_webRelayTxLen = 0;
static uint32_t g_webRelayTxFirstMs = 0;
static uint32_t g_webRelayDropCount = 0;
#if defined(ESP32)
static portMUX_TYPE g_webRelayTxMux = portMUX_INITIALIZER_UNLOCKED;
#endif

static inline void webRelayTxLock()
{
#if defined(ESP32)
    portENTER_CRITICAL(&g_webRelayTxMux);
#endif
}

static inline void webRelayTxUnlock()
{
#if defined(ESP32)
    portEXIT_CRITICAL(&g_webRelayTxMux);
#endif
}

static void webRelayFlush();

inline void webRelaySetClientHook(WebRelayClientHook hook)
{
    g_webRelayClientHook = hook;
}

/* Cerrar el cliente WS activo a propósito (p. ej. justo antes de
 * beacon_pause(): el STA va a quedar sin beacons ~60 s y la conexión
 * quedaría "zombie" — viva para el servidor, muerta para el cliente — y
 * cualquier paquete emitido en ese hueco (incluido el volcado completo de
 * una captura corta) se perdería en silencio. Cerrar de entrada hace que
 * el navegador dispare su reconexión (1 s de backoff) de inmediato en vez
 * de esperar el timeout TCP pasivo (~9-10 s observados), dándole una
 * chance real de estar de vuelta cuando beacon_resume()/el dump arranquen. */
inline void webRelayCloseAll()
{
    webRelayFlush();
    ws.closeAll();
}

/* ── Cola de comandos decodificados desde el navegador (drenada en loop()) ── */
static constexpr uint8_t WEB_CMD_Q_SIZE = 8;
static MatlabTransport::RxCmd g_webCmdQueue[WEB_CMD_Q_SIZE];
static uint8_t g_webCmdHead = 0;
static uint8_t g_webCmdTail = 0;

inline bool webRelayHasCmd() { return g_webCmdHead != g_webCmdTail; }

inline MatlabTransport::RxCmd webRelayPopCmd()
{
    MatlabTransport::RxCmd r = g_webCmdQueue[g_webCmdTail];
    g_webCmdTail = (uint8_t)((g_webCmdTail + 1) % WEB_CMD_Q_SIZE);
    return r;
}

static void webRelayPushCmd(const MatlabTransport::RxCmd &c)
{
    uint8_t next = (uint8_t)((g_webCmdHead + 1) % WEB_CMD_Q_SIZE);
    if (next != g_webCmdTail) { g_webCmdQueue[g_webCmdHead] = c; g_webCmdHead = next; }
}

/* ── Parser JSON minimal — objetos planos de un nivel, sin arrays/anidado ──
 * Suficiente para el esquema fijo de comandos de arriba; evita sumar
 * ArduinoJson como dependencia para mensajes tan simples. */
static bool webRelayJsonStr(const String &s, const char *key, String &out)
{
    String pat = String("\"") + key + "\"";
    int k = s.indexOf(pat);
    if (k < 0) return false;
    int colon = s.indexOf(':', k + pat.length());
    if (colon < 0) return false;
    int q1 = s.indexOf('"', colon + 1);
    if (q1 < 0) return false;
    int q2 = s.indexOf('"', q1 + 1);
    if (q2 < 0) return false;
    out = s.substring(q1 + 1, q2);
    return true;
}

static bool webRelayJsonInt(const String &s, const char *key, long &out)
{
    String pat = String("\"") + key + "\"";
    int k = s.indexOf(pat);
    if (k < 0) return false;
    int colon = s.indexOf(':', k + pat.length());
    if (colon < 0) return false;
    int i = colon + 1;
    while (i < (int)s.length() && (s[i] == ' ' || s[i] == '\t')) i++;
    int j = i;
    if (j < (int)s.length() && s[j] == '-') j++;
    int numStart = j;
    while (j < (int)s.length() && isDigit(s[j])) j++;
    if (j == numStart) return false;
    out = s.substring(i, j).toInt();
    return true;
}

static inline uint8_t webRelayHexByte(const String &hex)
{
    return (uint8_t)strtoul(hex.c_str(), nullptr, 16);
}

/* Decodifica un mensaje JSON plano del navegador a RxCmd y lo encola */
static void webRelayDecodeCmd(const String &json)
{
    String cmdStr;
    if (!webRelayJsonStr(json, "cmd", cmdStr)) return;
    uint8_t cmd = webRelayHexByte(cmdStr);

    MatlabTransport::RxCmd r = {0, 0, 0, 0, 0, true};
    r.cmd = cmd;

    if (cmd == MATLAB_CMD_DIRECTED) {            /* 0xBD — dirigido a esclavo */
        long node = 0, param = 0;
        String subStr;
        webRelayJsonInt(json, "node", node);
        webRelayJsonStr(json, "sub", subStr);
        webRelayJsonInt(json, "param", param);
        r.node_id = (uint8_t)node;
        r.sub_cmd = webRelayHexByte(subStr);
        r.param   = (uint8_t)param;
    } else if (cmd == 0xA3 || cmd == 0xAE) {     /* set-N de 16 bits */
        long value = 0;
        webRelayJsonInt(json, "value", value);
        r.value = (uint16_t)value;
        r.param = (uint8_t)(value & 0xFF);
    } else {                                     /* comando estándar 0xA1/0xA2/... */
        long param = 0;
        webRelayJsonInt(json, "param", param);
        r.param = (uint8_t)param;
    }

    webRelayPushCmd(r);
    LOGM("WEBCMD", "cmd=0x%02X,param=%u,value=%u,node=%u,sub=0x%02X",
         r.cmd, r.param, r.value, r.node_id, r.sub_cmd);
}

/* ── Telemetría: espejar paquetes de 6 bytes hacia los clientes WS ─────────
 *
 * No mandamos una trama WebSocket por muestra: durante un dump eso crea cientos
 * de frames chicos y el ESP32/cliente puede perder parte del burst. El cliente
 * web ya soporta frames binarios con varios paquetes [0x56 ...] concatenados.
 */
static void webRelayFlush()
{
    uint8_t out[WEB_RELAY_TX_BYTES];
    size_t len = 0;

    webRelayTxLock();
    if (g_webRelayTxLen > 0) {
        len = g_webRelayTxLen;
        memcpy(out, g_webRelayTxBuf, len);
        g_webRelayTxLen = 0;
        g_webRelayTxFirstMs = 0;
    }
    webRelayTxUnlock();

    if (len == 0) return;
    if (ws.count() == 0) return;

    if (!ws.availableForWriteAll()) {
        webRelayTxLock();
        if (g_webRelayTxLen == 0 && len <= WEB_RELAY_TX_BYTES) {
            memcpy(g_webRelayTxBuf, out, len);
            g_webRelayTxLen = len;
            if (g_webRelayTxFirstMs == 0) g_webRelayTxFirstMs = millis();
        } else {
            g_webRelayDropCount++;
            MASTER_LOG_PRINTF("[WEB] WS cola llena: drop %u (%u bytes)\n",
                              (unsigned)g_webRelayDropCount, (unsigned)len);
        }
        webRelayTxUnlock();
        return;
    }

    AsyncWebSocket::SendStatus st = ws.binaryAll(out, len);
    if (st != AsyncWebSocket::ENQUEUED) {
        g_webRelayDropCount++;
        MASTER_LOG_PRINTF("[WEB] WS envio parcial/drop %u status=%d bytes=%u\n",
                          (unsigned)g_webRelayDropCount, (int)st, (unsigned)len);
    }
}

static void webRelayService()
{
    bool due = false;
    webRelayTxLock();
    due = (g_webRelayTxLen > 0 &&
           g_webRelayTxFirstMs != 0 &&
           (uint32_t)(millis() - g_webRelayTxFirstMs) >= WEB_RELAY_FLUSH_MS);
    webRelayTxUnlock();
    if (due) webRelayFlush();

    /* Timeout de auth: si el cliente no autenticó en 5 s → expulsar */
    if (WS_AUTH_ENABLED && !g_wsAuthOk && g_wsAuthDeadMs != 0 &&
        ws.count() > 0 &&
        (int32_t)(millis() - g_wsAuthDeadMs) >= 0) {
        MASTER_LOG_PRINTLN("[WEB] auth timeout — cerrando cliente");
        ws.closeAll(4401, "auth timeout");
        g_wsAuthDeadMs = 0;
    }

    /* Ping periódico para detectar conexiones muertas y limpiarlas.
     * Sin ping, un cliente con TCP zombi sigue en WS_CONNECTED para siempre
     * y bloquea reconexiones del mismo dispositivo. */
    static uint32_t g_wsPingMs = 0;
    if (ws.count() > 0 && (uint32_t)(millis() - g_wsPingMs) > 10000u) {
        ws.pingAll();
        g_wsPingMs = millis();
    }
}

inline bool webRelayReadyForDumpRequest()
{
    webRelayService();
    if (ws.count() == 0) return true;

    size_t pending = 0;
    webRelayTxLock();
    pending = g_webRelayTxLen;
    webRelayTxUnlock();

    if (pending + WEB_RELAY_DUMP_BATCH_BYTES > WEB_RELAY_TX_BYTES) {
        webRelayFlush();
        return false;
    }
    if (!ws.availableForWriteAll()) return false;

    for (auto &c : ws.getClients()) {
        if (c.status() == WS_CONNECTED && c.queueLen() >= WEB_RELAY_QUEUE_SOFT_MAX) {
            return false;
        }
    }
    return true;
}

static void webRelayPacket(const uint8_t pkt[6])
{
    if (ws.count() == 0) {
        webRelayTxLock();
        g_webRelayTxLen = 0;
        g_webRelayTxFirstMs = 0;
        webRelayTxUnlock();
        return;
    }

    bool flushFirst = false;
    webRelayTxLock();
    flushFirst = (g_webRelayTxLen + WEB_RELAY_PKT_LEN > WEB_RELAY_TX_BYTES);
    webRelayTxUnlock();
    if (flushFirst) webRelayFlush();

    bool queued = false;
    webRelayTxLock();
    if (g_webRelayTxLen + WEB_RELAY_PKT_LEN <= WEB_RELAY_TX_BYTES) {
        if (g_webRelayTxLen == 0) g_webRelayTxFirstMs = millis();
        memcpy(&g_webRelayTxBuf[g_webRelayTxLen], pkt, WEB_RELAY_PKT_LEN);
        g_webRelayTxLen += WEB_RELAY_PKT_LEN;
        queued = true;
    }
    webRelayTxUnlock();

    if (!queued) {
        ws.binaryAll(const_cast<uint8_t *>(pkt), WEB_RELAY_PKT_LEN);
        return;
    }

    if (pkt[2] != 0x00) webRelayFlush();
}

/* ── Eventos WebSocket ─────────────────────────────────────────────────────  */
static void webRelayOnEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                            AwsEventType type, void *arg, uint8_t *data, size_t len)
{
    switch (type) {
        case WS_EVT_CONNECT: {
            /* Un cliente a la vez: el primero que entra queda como dueño. */
            bool rejected = false;
            for (auto &c : server->getClients()) {
                if (c.id() == client->id() || c.status() != WS_CONNECTED) continue;
                MASTER_LOG_PRINTF("[WEB] WS rechazado #%u (lock: dueño #%u %s)\n",
                                  client->id(), c.id(),
                                  c.remoteIP().toString().c_str());
                client->text("{\"type\":\"busy\",\"reason\":\"ws_lock\"}");
                client->close(1008, "busy");
                rejected = true;
                break;
            }
            if (!rejected) {
                MASTER_LOG_PRINTF("[WEB] WS #%u conectado (%s)\n",
                                  client->id(), client->remoteIP().toString().c_str());
                if (WS_AUTH_ENABLED) {
                    g_wsAuthOk     = false;
                    g_wsAuthDeadMs = millis() + 5000u;
                    client->text("{\"type\":\"auth_required\"}");
                    MASTER_LOG_PRINTLN("[WEB] auth requerida — 5 s para autenticar");
                } else {
                    g_wsAuthOk = true;
                    if (g_webRelayClientHook) g_webRelayClientHook();
                }
            }
            break;
        }
        case WS_EVT_DISCONNECT:
            MASTER_LOG_PRINTF("[WEB] WS cliente #%u desconectado\n", client->id());
            g_wsAuthOk     = !WS_AUTH_ENABLED;   /* reset para próximo cliente */
            g_wsAuthDeadMs = 0;
            break;
        case WS_EVT_DATA: {
            AwsFrameInfo *info = (AwsFrameInfo *)arg;
            if (!(info->final && info->index == 0 && info->len == len &&
                  info->opcode == WS_TEXT)) break;

            String msg;
            msg.reserve(len + 1);
            for (size_t i = 0; i < len; i++) msg += (char)data[i];

            /* Validar auth antes de aceptar cualquier comando */
            if (WS_AUTH_ENABLED && !g_wsAuthOk) {
                String cmdStr;
                if (webRelayJsonStr(msg, "cmd", cmdStr) && cmdStr == "AUTH") {
                    String tok;
                    webRelayJsonStr(msg, "token", tok);
                    if (tok == WS_AUTH_TOKEN) {
                        g_wsAuthOk     = true;
                        g_wsAuthDeadMs = 0;
                        client->text("{\"type\":\"auth\",\"ok\":true}");
                        MASTER_LOG_PRINTF("[WEB] auth ok #%u\n", client->id());
                        if (g_webRelayClientHook) g_webRelayClientHook();
                    } else {
                        client->text("{\"type\":\"auth\",\"ok\":false}");
                        client->close(4401, "unauthorized");
                        MASTER_LOG_PRINTF("[WEB] auth fail #%u\n", client->id());
                    }
                }
                break;   /* descartar cualquier otro comando hasta auth */
            }

            webRelayDecodeCmd(msg);
            break;
        }
        default:
            break;
    }
}

inline void webRelayBegin(AsyncWebServer &server)
{
    ws.onEvent(webRelayOnEvent);
    server.addHandler(&ws);
    MASTER_LOG_PRINTLN("[WEB] WebSocket /ws listo");
}
