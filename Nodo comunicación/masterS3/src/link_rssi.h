#pragma once
/*
 * link_rssi.h — Intensidad de señal: esclavos<->maestro (ESP-NOW) e
 * interfaz (celular/PC)<->maestro (WiFi AP).
 *
 * El ESP-NOW de este core (esp_now_recv_cb_t clásico) no expone RSSI en el
 * callback normal, así que se sniffea en modo promiscuo (solo frames de
 * management) filtrando "Action" frames cuyo MAC origen coincide con un
 * esclavo conocido (aprendido vía HELLO, ver g_cachedHello en main.cpp).
 * El RSSI hacia la interfaz web sale directo de esp_wifi_ap_get_sta_list().
 *
 * Requiere que main.cpp lo incluya DESPUÉS de declarar NUM_SLAVES, g_state
 * y g_cachedHello[], y después de que web_server.h (que trae `ws`) ya esté
 * incluido.
 *
 * Política de acquisition: linkRssiLoop() sniffea/reporta en IDLE/ARMING/
 * ARMED (estados de espera, sin RF sample-critica); se apaga del todo (no
 * solo se silencia) en PRESTART/RUNNING/STOPPING/DUMPING/SCOPE_MULTI para
 * no competir por radio/CPU con la ventana real de captura — la UI se
 * queda con el último valor mostrado.
 */

#include <esp_wifi.h>

#ifndef LINK_RSSI_REPORT_MS
  #define LINK_RSSI_REPORT_MS 3000u
#endif
#ifndef LINK_RSSI_STALE_MS
  #define LINK_RSSI_STALE_MS 15000u
#endif

struct SlaveLinkRssi {
    int8_t   rssi = 0;
    uint32_t lastSeenMs = 0;
    bool     valid = false;
};
static SlaveLinkRssi g_slaveLinkRssi[NUM_SLAVES + 1];

static void linkRssiPromiscuousCb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    if (pkt->rx_ctrl.sig_len < 24) return;

    const uint8_t *p = pkt->payload;
    const uint8_t frameType    = (p[0] >> 2) & 0x3;
    const uint8_t frameSubtype = (p[0] >> 4) & 0xF;
    if (frameType != 0 || frameSubtype != 0xD) return;   /* solo Action frames (ESP-NOW) */

    const uint8_t *src = p + 10;   /* addr2 = 2(fc)+2(dur)+6(addr1) */
    for (uint8_t n = 1; n <= NUM_SLAVES; n++) {
        if (g_cachedHello[n].valid && memcmp(src, g_cachedHello[n].mac, 6) == 0) {
            g_slaveLinkRssi[n].rssi       = pkt->rx_ctrl.rssi;
            g_slaveLinkRssi[n].lastSeenMs = millis();
            g_slaveLinkRssi[n].valid      = true;
            break;
        }
    }
}

static bool g_linkRssiPromiscuousOn = false;

static void linkRssiSetPromiscuous(bool on)
{
    if (on == g_linkRssiPromiscuousOn) return;
    if (on) {
        wifi_promiscuous_filter_t filter = { WIFI_PROMIS_FILTER_MASK_MGMT };
        esp_wifi_set_promiscuous_filter(&filter);
        esp_wifi_set_promiscuous_rx_cb(&linkRssiPromiscuousCb);
        esp_wifi_set_promiscuous(true);
    } else {
        esp_wifi_set_promiscuous(false);
    }
    g_linkRssiPromiscuousOn = on;
}

inline void linkRssiBegin()
{
    /* Arranca apagado: linkRssiLoop() lo enciende recién en IDLE. */
    g_linkRssiPromiscuousOn = false;
}

static uint32_t g_linkRssiLastReportMs = 0;

static void linkRssiSendReport()
{
    if (!webRelayHasActiveClient()) return;

    wifi_sta_list_t staList = {};
    esp_wifi_ap_get_sta_list(&staList);

    String json;
    json.reserve(192);
    json += "{\"type\":\"link\",\"wifi_rssi\":";
    if (staList.num > 0) {
        int8_t best = staList.sta[0].rssi;
        for (int i = 1; i < staList.num; i++) {
            if (staList.sta[i].rssi > best) best = staList.sta[i].rssi;
        }
        json += (int)best;
    } else {
        json += "null";
    }
    json += ",\"wifi_clients\":";
    json += (int)staList.num;
    json += ",\"slaves\":[";
    for (uint8_t n = 1; n <= NUM_SLAVES; n++) {
        if (n > 1) json += ",";
        SlaveLinkRssi &r = g_slaveLinkRssi[n];
        uint32_t age = r.valid ? (uint32_t)(millis() - r.lastSeenMs) : 0xFFFFFFFFu;
        bool fresh = r.valid && age <= LINK_RSSI_STALE_MS;
        json += "{\"node\":";
        json += (int)n;
        json += ",\"rssi\":";
        json += fresh ? String((int)r.rssi) : String("null");
        json += ",\"age_ms\":";
        json += fresh ? String((unsigned)age) : String("null");
        json += "}";
    }
    json += "]}";

    ws.textAll(json);
}

inline void linkRssiLoop()
{
    /* IDLE/ARMING/ARMED son estados de espera (sin RF sample-critica) donde
     * el sistema puede quedarse minutos entre disparos — ahi el RSSI tiene
     * que verse. Solo se pausa en la ventana real de captura/dump:
     * PRESTART (arranca el handshake de START), RUNNING/STOPPING/DUMPING
     * (store-and-forward por ESP-NOW) y SCOPE_MULTI (ráfaga de starts). */
    bool acquisitionActive = (g_state == PRESTART) || (g_state == RUNNING) ||
                              (g_state == STOPPING) || (g_state == DUMPING) ||
                              (g_state == SCOPE_MULTI);
    bool wantPromiscuous = !acquisitionActive;
    linkRssiSetPromiscuous(wantPromiscuous);
    if (!wantPromiscuous) return;

    uint32_t now = millis();
    if (g_linkRssiLastReportMs != 0 &&
        (uint32_t)(now - g_linkRssiLastReportMs) < LINK_RSSI_REPORT_MS) {
        return;
    }
    g_linkRssiLastReportMs = now;
    linkRssiSendReport();
}
