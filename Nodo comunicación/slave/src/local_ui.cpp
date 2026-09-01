#include "local_ui.h"

#ifndef LOCAL_UI_ENABLE
#define LOCAL_UI_ENABLE 0
#endif

#if LOCAL_UI_ENABLE && !defined(ESP8266)

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#ifndef LOCAL_OLED_SCK_PIN
#define LOCAL_OLED_SCK_PIN 18
#endif
#ifndef LOCAL_OLED_MOSI_PIN
#define LOCAL_OLED_MOSI_PIN 23
#endif
#ifndef LOCAL_OLED_CS_PIN
#define LOCAL_OLED_CS_PIN 33
#endif
#ifndef LOCAL_OLED_DC_PIN
#define LOCAL_OLED_DC_PIN 16
#endif
#ifndef LOCAL_OLED_RESET_PIN
#define LOCAL_OLED_RESET_PIN 17
#endif
#ifndef LOCAL_OLED_SPI_HZ
#define LOCAL_OLED_SPI_HZ 8000000UL
#endif
#ifndef LOCAL_BTN_UP_PIN
#define LOCAL_BTN_UP_PIN 34
#endif
#ifndef LOCAL_BTN_DOWN_PIN
#define LOCAL_BTN_DOWN_PIN 35
#endif
#ifndef LOCAL_BTN_OK_PIN
#define LOCAL_BTN_OK_PIN 36
#endif
#ifndef LOCAL_BTN_BACK_PIN
#define LOCAL_BTN_BACK_PIN 39
#endif
#ifndef LOCAL_BUTTON_USE_INTERNAL_PULLUPS
#define LOCAL_BUTTON_USE_INTERNAL_PULLUPS 0
#endif
#ifndef LOCAL_CAPTURE_BATCHES
#define LOCAL_CAPTURE_BATCHES 10
#endif
#ifndef NODE_ID
#define NODE_ID 1
#endif

namespace {

constexpr uint8_t OLED_WIDTH = 128;
constexpr uint8_t OLED_HEIGHT = 64;
constexpr uint32_t DEBOUNCE_MS = 30u;
constexpr uint32_t BACK_STOP_HOLD_MS = 900u;
constexpr uint32_t SCREEN_REFRESH_MS = 250u;
constexpr uint32_t NOTICE_MS = 1800u;

Adafruit_SSD1306 g_display(
    OLED_WIDTH, OLED_HEIGHT, &SPI,
    LOCAL_OLED_DC_PIN, LOCAL_OLED_RESET_PIN, LOCAL_OLED_CS_PIN,
    LOCAL_OLED_SPI_HZ);

struct Button {
    uint8_t pin;
    bool raw_pressed;
    bool stable_pressed;
    bool press_event;
    bool long_event;
    bool long_reported;
    uint32_t changed_ms;
    uint32_t pressed_ms;
};

Button g_buttons[] = {
    {LOCAL_BTN_UP_PIN, false, false, false, false, false, 0u, 0u},
    {LOCAL_BTN_DOWN_PIN, false, false, false, false, false, 0u, 0u},
    {LOCAL_BTN_OK_PIN, false, false, false, false, false, 0u, 0u},
    {LOCAL_BTN_BACK_PIN, false, false, false, false, false, 0u, 0u}
};

enum Screen : uint8_t { SCREEN_STATUS, SCREEN_MENU };
Screen g_screen = SCREEN_STATUS;
uint8_t g_menu_index = 0u;
bool g_ready = false;
bool g_dirty = true;
bool g_was_critical = false;
uint32_t g_last_draw_ms = 0u;
uint32_t g_notice_until_ms = 0u;
bool g_notice_ok = false;
char g_notice[22] = {0};

/* Las entradas 0 (captura), 5 (PGA) y 6 (PGAout) se dibujan a mano porque
 * llevan el valor actual al lado; el resto usa esta tabla. */
const char *const MENU_LABELS[] = {
    nullptr,
    "Calibrar PSoC",
    "Snapshot ADC",
    "Identificar nodo",
    "Limpiar captura",
    nullptr,
    nullptr
};

const LocalUiAction MENU_ACTIONS[] = {
    LOCAL_UI_ACTION_CAPTURE,
    LOCAL_UI_ACTION_CALIBRATE,
    LOCAL_UI_ACTION_ADC_SNAPSHOT,
    LOCAL_UI_ACTION_IDENTIFY,
    LOCAL_UI_ACTION_CLEAR,
    LOCAL_UI_ACTION_PGA_NEXT,
    LOCAL_UI_ACTION_PGAOUT_NEXT
};

constexpr uint8_t MENU_COUNT = sizeof(MENU_ACTIONS) / sizeof(MENU_ACTIONS[0]);
constexpr uint8_t MENU_ROW_PGA = 5u;
constexpr uint8_t MENU_ROW_PGAOUT = 6u;

/* Mismos codigos que PGAgain/PGAout del PSoC (PGAgain_GAIN_01..GAIN_50). */
const uint8_t GAIN_X[] = {1u, 2u, 4u, 8u, 16u, 24u, 32u, 48u, 50u};

/* Copia del ultimo estado dibujado: el menu necesita mostrar la ganancia
 * vigente y localUiService() ya la recibe en cada vuelta. */
uint8_t g_pga_code = 0u;
uint8_t g_pgaout_code = 0u;
bool g_has_pgaout = false;

const char *stateName(uint8_t state)
{
    switch (state) {
        case 0: return "ESPERA";
        case 1: return "ARMADO";
        case 2: return "HOT WAIT";
        case 3: return "CAPTURA";
        case 4: return "DETENIDO";
        default: return "?";
    }
}

void pollButton(Button &button, uint32_t now)
{
    button.press_event = false;
    button.long_event = false;
    const bool pressed = (digitalRead(button.pin) == LOW);
    if (pressed != button.raw_pressed) {
        button.raw_pressed = pressed;
        button.changed_ms = now;
    }
    if (button.stable_pressed != button.raw_pressed &&
        (uint32_t)(now - button.changed_ms) >= DEBOUNCE_MS) {
        button.stable_pressed = button.raw_pressed;
        if (button.stable_pressed) {
            button.press_event = true;
            button.pressed_ms = now;
            button.long_reported = false;
        }
    }
    if (button.stable_pressed && !button.long_reported &&
        (uint32_t)(now - button.pressed_ms) >= BACK_STOP_HOLD_MS) {
        button.long_reported = true;
        button.long_event = true;
    }
}

/* Avanza el cursor saltando la fila de PGAout cuando el nodo no la tiene. */
uint8_t menuStep(uint8_t index, int8_t delta)
{
    for (uint8_t guard = 0u; guard < MENU_COUNT; guard++) {
        index = (uint8_t)((index + MENU_COUNT + delta) % MENU_COUNT);
        if (index != MENU_ROW_PGAOUT || g_has_pgaout) {
            return index;
        }
    }
    return 0u;
}

void drawNotice()
{
    g_display.clearDisplay();
    g_display.setTextColor(SSD1306_WHITE);
    g_display.setTextSize(2);
    g_display.setCursor(0, 4);
    g_display.println(g_notice_ok ? "OK" : "ERROR");
    g_display.setTextSize(1);
    g_display.setCursor(0, 34);
    g_display.println(g_notice);
    g_display.display();
}

void drawStatus(const LocalUiStatus &status)
{
    g_display.clearDisplay();
    g_display.setTextColor(SSD1306_WHITE);
    g_display.setTextSize(1);
    g_display.setCursor(0, 0);
    g_display.print("NODO ");
    g_display.print(status.node_id);
    g_display.print("  PSoC:");
    g_display.println(status.psoc_connected ? "OK" : "NO");
    g_display.print("Estado: ");
    g_display.println(stateName(status.slave_state));
    g_display.print("Fs:");
    g_display.print(status.sample_rate_hz);
    g_display.print("Hz  WiFi:");
    g_display.println(status.wifi_channel);
    g_display.print("Lotes: ");
    g_display.print(status.batches_stored);
    g_display.print('/');
    g_display.println(status.batches_target);
    g_display.print("SD:");
    g_display.print(status.sd_present ? "OK" : "NO");
    g_display.print("  CFG:");
    g_display.println(status.config_busy ? "BUSY" : "OK");
    g_display.setCursor(0, 56);
    g_display.print("OK: menu");
    g_display.display();
}

/* 7 filas no entran en 64 px con el alto de linea viejo: se pasa a 8 px por
 * fila y se arranca justo debajo del titulo. */
void drawMenu()
{
    g_display.clearDisplay();
    g_display.setTextColor(SSD1306_WHITE);
    g_display.setTextSize(1);
    g_display.setCursor(0, 0);
    g_display.println("MENU LOCAL");
    for (uint8_t row = 0u; row < MENU_COUNT; row++) {
        if (row == MENU_ROW_PGAOUT && !g_has_pgaout) {
            continue;   /* HAMMER / placa vieja: PGAout no existe */
        }
        g_display.setCursor(0, (int16_t)(10 + row * 8));
        g_display.print(row == g_menu_index ? '>' : ' ');
        if (row == 0u) {
            g_display.print("Capturar ");
            g_display.print(LOCAL_CAPTURE_BATCHES);
            g_display.println(" lotes");
        } else if (row == MENU_ROW_PGA) {
            g_display.print("PGA ");
            g_display.print(GAIN_X[g_pga_code % (sizeof(GAIN_X) / sizeof(GAIN_X[0]))]);
            g_display.println("x");
        } else if (row == MENU_ROW_PGAOUT) {
            g_display.print("PGAout ");
            g_display.print(GAIN_X[g_pgaout_code % (sizeof(GAIN_X) / sizeof(GAIN_X[0]))]);
            g_display.println("x");
        } else {
            g_display.println(MENU_LABELS[row]);
        }
    }
    g_display.display();
}

} // namespace

void localUiBegin()
{
    SPI.begin(LOCAL_OLED_SCK_PIN, -1, LOCAL_OLED_MOSI_PIN, LOCAL_OLED_CS_PIN);
    const uint8_t inputMode = LOCAL_BUTTON_USE_INTERNAL_PULLUPS ? INPUT_PULLUP : INPUT;
    for (Button &button : g_buttons) {
        pinMode(button.pin, inputMode);
        button.raw_pressed = (digitalRead(button.pin) == LOW);
        button.stable_pressed = button.raw_pressed;
        button.changed_ms = millis();
    }

    g_ready = g_display.begin(SSD1306_SWITCHCAPVCC, 0, true, false);
    if (!g_ready) {
        return;
    }
    g_display.clearDisplay();
    g_display.setTextColor(SSD1306_WHITE);
    g_display.setTextSize(1);
    g_display.setCursor(0, 18);
    g_display.println("Nodo geofono");
    g_display.print("Iniciando nodo ");
    g_display.println(NODE_ID);
    g_display.display();
    g_dirty = true;
}

LocalUiAction localUiService(const LocalUiStatus &status, bool criticalWindow)
{
    if (!g_ready) {
        return LOCAL_UI_ACTION_NONE;
    }

    const uint32_t now = millis();
    for (Button &button : g_buttons) {
        pollButton(button, now);
    }

    if (status.pga_code != g_pga_code || status.pgaout_code != g_pgaout_code ||
        status.has_pgaout != g_has_pgaout) {
        g_pga_code = status.pga_code;
        g_pgaout_code = status.pgaout_code;
        g_has_pgaout = status.has_pgaout;
        g_dirty = true;
    }

    if (criticalWindow) {
        g_was_critical = true;
        if (g_buttons[3].long_event) {
            return LOCAL_UI_ACTION_STOP;
        }
        return LOCAL_UI_ACTION_NONE;
    }

    if (g_was_critical) {
        g_was_critical = false;
        g_dirty = true;
    }

    if (g_notice_until_ms != 0u) {
        if ((int32_t)(now - g_notice_until_ms) < 0) {
            return LOCAL_UI_ACTION_NONE;
        }
        g_notice_until_ms = 0u;
        g_dirty = true;
    }

    if (g_screen == SCREEN_STATUS) {
        if (g_buttons[2].press_event) {
            g_screen = SCREEN_MENU;
            g_dirty = true;
        }
    } else {
        if (g_buttons[0].press_event) {
            g_menu_index = menuStep(g_menu_index, -1);
            g_dirty = true;
        }
        if (g_buttons[1].press_event) {
            g_menu_index = menuStep(g_menu_index, +1);
            g_dirty = true;
        }
        if (g_buttons[3].press_event) {
            g_screen = SCREEN_STATUS;
            g_dirty = true;
        }
        if (g_buttons[2].press_event) {
            return MENU_ACTIONS[g_menu_index];
        }
    }

    if (g_dirty || (uint32_t)(now - g_last_draw_ms) >= SCREEN_REFRESH_MS) {
        g_last_draw_ms = now;
        g_dirty = false;
        if (g_screen == SCREEN_MENU) {
            drawMenu();
        } else {
            drawStatus(status);
        }
    }
    return LOCAL_UI_ACTION_NONE;
}

bool localUiReady()
{
    return g_ready;
}

void localUiShowSelfTest(uint16_t nPass, uint16_t nFail, uint16_t nWarn,
                         uint16_t nSkip, const char *firstFail)
{
    if (!g_ready) {
        return;
    }
    g_display.clearDisplay();
    g_display.setTextColor(SSD1306_WHITE);
    g_display.setTextSize(2);
    g_display.setCursor(0, 0);
    g_display.println(nFail == 0u ? "APTO" : "NO APTO");
    g_display.setTextSize(1);
    g_display.setCursor(0, 22);
    g_display.print("PASS "); g_display.println(nPass);
    g_display.print("FAIL "); g_display.println(nFail);
    g_display.print("WARN "); g_display.print(nWarn);
    g_display.print("  SKIP "); g_display.println(nSkip);
    if (nFail != 0u && firstFail != nullptr) {
        g_display.print("1er fallo: ");
        g_display.println(firstFail);
    }
    g_display.display();
    /* Se deja fija: el autotest termina y la pantalla queda con el veredicto,
     * que es justamente lo que se quiere mirar en campo. */
    g_notice_until_ms = 0u;
    g_dirty = false;
}

void localUiNotify(const char *message, bool ok)
{
    if (!g_ready) {
        return;
    }
    snprintf(g_notice, sizeof(g_notice), "%s", message ? message : "");
    g_notice_ok = ok;
    g_notice_until_ms = millis() + NOTICE_MS;
    drawNotice();
}

#else

void localUiBegin() {}
LocalUiAction localUiService(const LocalUiStatus &, bool)
{
    return LOCAL_UI_ACTION_NONE;
}
void localUiNotify(const char *, bool) {}
bool localUiReady() { return false; }
void localUiShowSelfTest(uint16_t, uint16_t, uint16_t, uint16_t, const char *) {}

#endif
