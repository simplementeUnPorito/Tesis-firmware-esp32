#pragma once
/*
 * web_server.h — Servidor HTTP/WS embebido del maestro (interfaz web de campo).
 *
 * Sirve la SPA desde LittleFS (partición de datos "spiffs" ya presente en la
 * tabla de particiones default.csv, ~1.4 MB — ver platformio.ini) y expone un
 * WebSocket en /ws para telemetría y comandos (ver web_relay.h, que registra
 * su propio handler sobre este mismo servidor).
 *
 * Tras llamar a webServerBegin() en setup(), conectar el espejo de paquetes
 * con: matlab.setPacketRelay(webRelayPacket);  (no se hace aquí para no
 * acoplar este header al objeto `matlab`, que es local a main.cpp).
 */

#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include "sync_protocol.h"
#include "master_log.h"
#include "web_relay.h"

static AsyncWebServer webServer(80);

#ifndef WEB_SIM_ENABLE
  #define WEB_SIM_ENABLE 1
#endif

static bool webServerArgLong(AsyncWebServerRequest *request, const char *name, long &out)
{
    if (!request->hasParam(name)) return false;
    out = request->getParam(name)->value().toInt();
    return true;
}

static uint8_t webServerSimHwClass(const String &type)
{
    if (type == "hammer" || type == "HAMMER" || type == "1") return SLAVE_HW_HAMMER;
    if (type == "geo" || type == "GEO" || type == "0") return SLAVE_HW_GEO;
    return SLAVE_HW_UNKNOWN;
}

inline bool webServerBegin()
{
    bool fsOk = LittleFS.begin(true);   /* true = formatear si el mount falla */
    if (!fsOk) {
        MASTER_LOG_PRINTLN("[WEB] LittleFS mount FAILED");
    } else {
        MASTER_LOG_PRINTF("[WEB] LittleFS OK — %u/%u bytes usados\n",
                          (unsigned)LittleFS.usedBytes(),
                          (unsigned)LittleFS.totalBytes());
    }

    DefaultHeaders::Instance().addHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    DefaultHeaders::Instance().addHeader("Pragma", "no-cache");
    DefaultHeaders::Instance().addHeader("Expires", "0");

    webServer.on("/health", HTTP_GET, [fsOk](AsyncWebServerRequest *request) {
        String body;
        body.reserve(160);
        body += "ok\n";
        body += "ap_ip=192.168.4.1\n";
        body += "littlefs=";
        body += fsOk ? "ok" : "fail";
        body += "\nused=";
        body += fsOk ? String((unsigned)LittleFS.usedBytes()) : "0";
        body += "\ntotal=";
        body += fsOk ? String((unsigned)LittleFS.totalBytes()) : "0";
        body += "\n";
        request->send(200, "text/plain", body);
    });

    webServer.on("/ws-reset", HTTP_GET, [](AsyncWebServerRequest *request) {
        webRelayCloseAll();
        request->send(200, "text/plain", "ok\nws=reset\n");
    });

#if WEB_SIM_ENABLE
    webServer.on("/sim/hello", HTTP_GET, [](AsyncWebServerRequest *request) {
        long nodeArg = 1;
        long fsArg = 2929;
        long psocArg = 1;
        webServerArgLong(request, "node", nodeArg);
        webServerArgLong(request, "fs", fsArg);
        webServerArgLong(request, "psoc", psocArg);
        if (nodeArg < 1) nodeArg = 1;
        if (nodeArg > 254) nodeArg = 254;
        if (fsArg < 0) fsArg = 0;
        if (fsArg > 65535) fsArg = 65535;

        String type = request->hasParam("type") ? request->getParam("type")->value() : "hammer";
        const uint8_t node = (uint8_t)nodeArg;
        const uint16_t fs = (uint16_t)fsArg;
        const uint8_t hwClass = webServerSimHwClass(type);
        const uint8_t psocOk = psocArg ? 1u : 0u;
        const uint8_t fsCode = (fs >= 100u) ? (uint8_t)(fs / 100u) : 0u;

        uint8_t hello[6] = {
            MATLAB_PKT_HEADER, node, 0xFD, 0x01, psocOk, fsCode
        };
        webRelayPacket(hello);

        if (fs > 0u) {
            uint8_t fsExact[6] = {
                MATLAB_PKT_HEADER, node, 0xFD, 0x05,
                (uint8_t)((fs >> 8) & 0xFFu), (uint8_t)(fs & 0xFFu)
            };
            webRelayPacket(fsExact);
        }

        if (hwClass != SLAVE_HW_UNKNOWN) {
            uint8_t hwPkt[6] = { MATLAB_PKT_HEADER, node, 0xFD, 0x06, hwClass, 0x00 };
            webRelayPacket(hwPkt);
        }

        uint8_t mac[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, node };
        uint8_t m0[6] = { MATLAB_PKT_HEADER, node, 0xFD, 0x02, mac[0], mac[1] };
        uint8_t m1[6] = { MATLAB_PKT_HEADER, node, 0xFD, 0x03, mac[2], mac[3] };
        uint8_t m2[6] = { MATLAB_PKT_HEADER, node, 0xFD, 0x04, mac[4], mac[5] };
        webRelayPacket(m0);
        webRelayPacket(m1);
        webRelayPacket(m2);

        String body;
        body.reserve(160);
        body += "ok\nnode="; body += node;
        body += "\ntype="; body += (hwClass == SLAVE_HW_HAMMER ? "HAMMER" : (hwClass == SLAVE_HW_GEO ? "GEO" : "UNKNOWN"));
        body += "\nfs="; body += fs;
        body += "\nmac=DE:AD:BE:EF:00:";
        if (node < 16) body += "0";
        body += String(node, HEX);
        body += "\n";
        request->send(200, "text/plain", body);
    });
#endif

    webServer.on("/", HTTP_GET, [fsOk](AsyncWebServerRequest *request) {
        if (fsOk && LittleFS.exists("/index.html")) {
            request->send(LittleFS, "/index.html", "text/html");
            return;
        }
        request->send(
            200,
            "text/html",
            "<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>Geophone Scope</title>"
            "<body style='font-family:sans-serif;background:#111;color:#eee;padding:16px'>"
            "<h1>Geophone Scope</h1>"
            "<p>El servidor HTTP del maestro esta funcionando en 192.168.4.1.</p>"
            "<p>Pero no se encontro <code>/index.html</code> en LittleFS.</p>"
            "<p>Desde la PC ejecuta:</p>"
            "<pre>pio run -t uploadfs</pre>"
            "<p>Luego reinicia el ESP32 y recarga esta pagina.</p>"
            "<p><a style='color:#9cf' href='/health'>/health</a></p>"
            "</body>"
        );
    });

    if (fsOk) {
        webServer.serveStatic("/", LittleFS, "/");
    }
    webServer.onNotFound([](AsyncWebServerRequest *request) {
        request->send(404, "text/plain", "Not found. Try /health");
    });

    /* Respuestas a probes de conectividad — evita que Windows/Android/iOS
     * marquen la red como "sin internet" e intenten hacer auto-switch. */
    webServer.on("/connecttest.txt", HTTP_GET, [](AsyncWebServerRequest *r) {
        r->send(200, "text/plain", "Microsoft Connect Test");
    });
    webServer.on("/ncsi.txt", HTTP_GET, [](AsyncWebServerRequest *r) {
        r->send(200, "text/plain", "Microsoft NCSI");
    });
    webServer.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest *r) {
        r->send(204, "text/plain", "");
    });
    webServer.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest *r) {
        r->send(200, "text/html",
            "<!DOCTYPE html><html><head><title>Success</title></head>"
            "<body>Success</body></html>");
    });
    webServer.on("/redirect", HTTP_GET, [](AsyncWebServerRequest *r) {
        r->redirect("/");
    });

    webRelayBegin(webServer);

    webServer.begin();
    MASTER_LOG_PRINTLN("[WEB] HTTP server escuchando en :80");
    return true;
}
