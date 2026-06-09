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
#include "master_log.h"
#include "web_relay.h"

static AsyncWebServer webServer(80);

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

    webRelayBegin(webServer);

    webServer.begin();
    MASTER_LOG_PRINTLN("[WEB] HTTP server escuchando en :80");
    return true;
}
