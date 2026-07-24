// Demo minima: el ESP32 se conecta como cliente (STA) al hotspot del celular
// y sirve una pagina que responde "OK". Objetivo: validar que el celular
// mantiene internet (datos moviles) mientras el ESP esta conectado a su
// hotspot, y que se puede navegar a la vez a la pagina del ESP.
//
// No toca nada del firmware del maestro real: es un sketch descartable.

#include <WiFi.h>
#include <WebServer.h>

// --- Editar con los datos del hotspot del celular ---
static const char *HOTSPOT_SSID = "S21 Ultra de Elías David";
static const char *HOTSPOT_PASS = "hwjs2708";

WebServer server(80);

void handleRoot() {
  server.send(200, "text/plain", "OK");
}

void setup() {
  Serial.begin(115200);
  delay(300);

  WiFi.mode(WIFI_STA);
  WiFi.begin(HOTSPOT_SSID, HOTSPOT_PASS);

  Serial.printf("Conectando a %s", HOTSPOT_SSID);
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Conectado. IP del ESP32 en la red del celular: ");
  Serial.println(WiFi.localIP());
  Serial.println("Desde el celular, abrir esa IP en el navegador.");

  server.on("/", handleRoot);
  server.begin();
}

void loop() {
  server.handleClient();
}
