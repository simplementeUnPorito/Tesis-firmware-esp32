/*
 * ota_slave.cpp — ver ota_slave.h.
 */
#include "ota_slave.h"

#if OTA_ENABLE
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include <esp_arduino_version.h>
/* mbedtls 3 (IDF 5) quito el sufijo _ret. */
#if ESP_ARDUINO_VERSION_MAJOR >= 3
#define mbedtls_sha256_starts_ret mbedtls_sha256_starts
#define mbedtls_sha256_update_ret mbedtls_sha256_update
#define mbedtls_sha256_finish_ret mbedtls_sha256_finish
#endif

/* Marca que el maestro busca en el .bin antes de ofrecerlo: evita mandarle a
 * un esclavo la imagen del maestro u otra cosa. */
extern "C" const char GEO_FW_ROLE_MARK[] = "GEO_FW_ROLE:slave:v1";

/* El core de Arduino confirma la imagen nueva apenas arranca salvo que esta
 * funcion devuelva true: la confirmacion la hace onMasterAlive(). */
extern "C" bool verifyRollbackLater() { return true; }

#ifndef OTA_CONFIRM_TIMEOUT_MS
#define OTA_CONFIRM_TIMEOUT_MS 180000u
#endif
#ifndef OTA_JOIN_TIMEOUT_MS
#define OTA_JOIN_TIMEOUT_MS 15000u
#endif
#ifndef OTA_HTTP_TIMEOUT_MS
#define OTA_HTTP_TIMEOUT_MS 10000u
#endif

namespace ota {
namespace {

SendFn      g_send = nullptr;
uint8_t     g_node = 0;
const char *g_ssid = nullptr;
const char *g_pass = nullptr;

volatile bool g_offerPending = false;
volatile bool g_abort = false;
MsgOtaOffer   g_offer = {};

uint8_t  g_state = OTA_ST_IDLE;
uint8_t  g_error = OTA_ERR_NONE;
uint32_t g_session = 0;
uint32_t g_received = 0;
uint32_t g_size = 0;
bool     g_inProgress = false;

/* Imagen nueva sin confirmar (arranque tras una OTA). */
bool     g_pending = false;
uint32_t g_pendingSinceMs = 0;
uint8_t  g_confirmRepeats = 0;
uint32_t g_lastStatusMs = 0;

void sendStatus()
{
    if (!g_send) return;
    MsgOtaStatus st = { CMD_OTA_STATUS, g_node, g_session, g_state, g_error,
                        g_received, g_size };
    g_send((const uint8_t *)&st, sizeof(st));
    g_lastStatusMs = millis();
}

void setState(uint8_t state, uint8_t error = OTA_ERR_NONE)
{
    g_state = state;
    g_error = error;
    Serial.printf("[OTA] estado=%u error=%u recibido=%lu/%lu\n", (unsigned)state,
                  (unsigned)error, (unsigned long)g_received, (unsigned long)g_size);
    sendStatus();
}

void saveSession(uint32_t session)
{
    Preferences p;
    if (p.begin("ota", false)) {
        p.putUInt("session", session);
        p.end();
    }
}

uint32_t loadSession()
{
    Preferences p;
    uint32_t s = 0;
    if (p.begin("ota", true)) {
        s = p.getUInt("session", 0);
        p.end();
    }
    return s;
}

void leaveAp()
{
    /* Sin apagar el WiFi: ESP-NOW sigue en el mismo canal. */
    WiFi.disconnect(false, false);
}

bool joinAp()
{
    const uint8_t ch = WiFi.channel();
    WiFi.begin(g_ssid, g_pass, ch);
    const uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if ((millis() - t0) > OTA_JOIN_TIMEOUT_MS || g_abort) return false;
        delay(100);
    }
    Serial.printf("[OTA] asociado a %s ip=%s canal=%u\n", g_ssid,
                  WiFi.localIP().toString().c_str(), (unsigned)WiFi.channel());
    return true;
}

/* Baja y escribe la imagen. Devuelve OTA_ERR_*. */
uint8_t download()
{
    char url[64];
    snprintf(url, sizeof(url), "http://%s%s", WiFi.gatewayIP().toString().c_str(),
             g_offer.path);
    HTTPClient http;
    http.setTimeout(OTA_HTTP_TIMEOUT_MS);
    if (!http.begin(url)) return OTA_ERR_HTTP;
    const int code = http.GET();
    if (code != 200) {
        Serial.printf("[OTA] HTTP %d en %s\n", code, url);
        http.end();
        return OTA_ERR_HTTP;
    }
    const int len = http.getSize();
    if (len <= 0 || (uint32_t)len != g_size) {
        Serial.printf("[OTA] tamano %d, esperado %lu\n", len, (unsigned long)g_size);
        http.end();
        return OTA_ERR_SIZE;
    }
    if (!Update.begin(g_size, U_FLASH)) {
        http.end();
        return OTA_ERR_FLASH;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts_ret(&sha, 0);

    WiFiClient *stream = http.getStreamPtr();
    static uint8_t buf[4096];
    uint32_t lastReport = 0;
    uint32_t lastDataMs = millis();
    uint8_t err = OTA_ERR_NONE;
    while (g_received < g_size) {
        if (g_abort) { err = OTA_ERR_ABORTED; break; }
        const size_t avail = stream->available();
        if (avail == 0) {
            if (!http.connected() || (millis() - lastDataMs) > OTA_HTTP_TIMEOUT_MS) {
                err = OTA_ERR_HTTP;
                break;
            }
            delay(2);
            continue;
        }
        size_t want = g_size - g_received;
        if (want > sizeof(buf)) want = sizeof(buf);
        if (want > avail) want = avail;
        const int n = stream->readBytes(buf, want);
        if (n <= 0) continue;
        lastDataMs = millis();
        mbedtls_sha256_update_ret(&sha, buf, (size_t)n);
        if (Update.write(buf, (size_t)n) != (size_t)n) { err = OTA_ERR_FLASH; break; }
        g_received += (uint32_t)n;
        if (g_received - lastReport >= 65536u) {
            lastReport = g_received;
            sendStatus();
        }
    }
    http.end();

    uint8_t digest[32];
    mbedtls_sha256_finish_ret(&sha, digest);
    mbedtls_sha256_free(&sha);

    if (err != OTA_ERR_NONE) {
        Update.abort();
        return err;
    }
    setState(OTA_ST_VERIFYING);
    if (memcmp(digest, g_offer.sha256, sizeof(digest)) != 0) {
        Update.abort();
        return OTA_ERR_SHA;
    }
    if (!Update.end(true)) return OTA_ERR_FLASH;
    return OTA_ERR_NONE;
}

void runOffer()
{
    g_inProgress = true;
    g_abort = false;
    g_session = g_offer.session;
    g_size = g_offer.size;
    g_received = 0;
    setState(OTA_ST_JOINING);
    if (!joinAp()) {
        leaveAp();
        setState(OTA_ST_FAILED, g_abort ? OTA_ERR_ABORTED : OTA_ERR_WIFI);
        g_inProgress = false;
        return;
    }
    setState(OTA_ST_DOWNLOADING);
    const uint8_t err = download();
    leaveAp();
    if (err != OTA_ERR_NONE) {
        setState(OTA_ST_FAILED, err);
        g_inProgress = false;
        return;
    }
    saveSession(g_session);
    setState(OTA_ST_REBOOTING);
    delay(300);
    ESP.restart();
}

}  // namespace

void begin(SendFn send, uint8_t nodeId, const char *apSsid, const char *apPass)
{
    g_send = send;
    g_node = nodeId;
    g_ssid = apSsid;
    g_pass = apPass;

    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    const bool rollback = (run != nullptr) &&
                          esp_ota_get_state_partition(run, &st) == ESP_OK;
    const uint32_t session = loadSession();
    if (rollback && st == ESP_OTA_IMG_PENDING_VERIFY) {
        g_pending = true;
        g_pendingSinceMs = millis();
        g_session = session;
        g_state = OTA_ST_PENDING;
        Serial.printf("[OTA] imagen nueva en %s sin confirmar (sesion %lu)\n",
                      run->label, (unsigned long)session);
    } else if (session != 0) {
        /* Sin rollback en el bootloader la imagen ya corre confirmada: avisar
         * una vez y olvidar la sesion. */
        g_session = session;
        g_state = OTA_ST_CONFIRMED;
        g_confirmRepeats = 5;
        saveSession(0);
    }
    Serial.printf("[OTA] particion activa %s\n", run ? run->label : "?");
}

void onOffer(const MsgOtaOffer &offer, bool busyCapture)
{
    if (offer.node_id != g_node) return;
    if (g_inProgress) return;
    if (busyCapture) {
        g_session = offer.session;
        g_size = offer.size;
        setState(OTA_ST_FAILED, OTA_ERR_BUSY);
        return;
    }
    memcpy(&g_offer, &offer, sizeof(g_offer));
    g_offer.path[sizeof(g_offer.path) - 1] = '\0';
    g_offerPending = true;
}

void onControl(const MsgOtaControl &ctl)
{
    if (ctl.node_id != g_node) return;
    if (ctl.action == 1u) g_abort = true;
}

volatile bool g_masterAlive = false;

/* Corre en la tarea WiFi: solo marca; la escritura en flash va en service(). */
void onMasterAlive()
{
    if (g_pending) g_masterAlive = true;
}

static void confirmPending()
{
    g_pending = false;
    g_masterAlive = false;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        Serial.println("[OTA] imagen nueva confirmada");
    }
    saveSession(0);
    g_state = OTA_ST_CONFIRMED;
    g_error = OTA_ERR_NONE;
    g_confirmRepeats = 5;
}

void service()
{
    if (g_offerPending) {
        g_offerPending = false;
        runOffer();   /* bloquea mientras baja; reinicia si sale bien */
        return;
    }
    const uint32_t now = millis();
    if (g_pending && g_masterAlive) confirmPending();
    if (g_pending) {
        if ((now - g_pendingSinceMs) > OTA_CONFIRM_TIMEOUT_MS) {
            Serial.println("[OTA] sin maestro: vuelvo a la imagen anterior");
            g_state = OTA_ST_FAILED;
            sendStatus();
            delay(100);
            esp_ota_mark_app_invalid_rollback_and_reboot();
        } else if ((now - g_lastStatusMs) > 2000u) {
            sendStatus();
        }
    } else if (g_confirmRepeats > 0 && (now - g_lastStatusMs) > 1000u) {
        g_confirmRepeats--;
        sendStatus();
    }
}

bool busy() { return g_inProgress || g_offerPending; }

void printStatus()
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    /* Imprimir la marca la mantiene dentro del binario: el maestro la busca. */
    Serial.printf("#OTA %s estado=%u error=%u sesion=%lu recibido=%lu/%lu particion=%s pendiente=%u\n",
                  GEO_FW_ROLE_MARK, (unsigned)g_state, (unsigned)g_error, (unsigned long)g_session,
                  (unsigned long)g_received, (unsigned long)g_size,
                  run ? run->label : "?", (unsigned)g_pending);
}

}  // namespace ota

#else  /* !OTA_ENABLE */

namespace ota {
void begin(SendFn, uint8_t, const char *, const char *) {}
void onOffer(const MsgOtaOffer &, bool) {}
void onControl(const MsgOtaControl &) {}
void onMasterAlive() {}
void service() {}
bool busy() { return false; }
void printStatus() {}
}  // namespace ota

#endif
