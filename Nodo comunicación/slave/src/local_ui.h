#pragma once

#include <Arduino.h>

enum LocalUiAction : uint8_t {
    LOCAL_UI_ACTION_NONE = 0,
    LOCAL_UI_ACTION_CAPTURE,
    LOCAL_UI_ACTION_CALIBRATE,
    LOCAL_UI_ACTION_ADC_SNAPSHOT,
    LOCAL_UI_ACTION_IDENTIFY,
    LOCAL_UI_ACTION_CLEAR,
    LOCAL_UI_ACTION_STOP
};

struct LocalUiStatus {
    uint8_t node_id;
    uint8_t slave_state;
    bool psoc_connected;
    bool config_busy;
    bool sd_present;
    uint8_t wifi_channel;
    uint16_t sample_rate_hz;
    uint16_t batches_stored;
    uint16_t batches_target;
};

void localUiBegin();
LocalUiAction localUiService(const LocalUiStatus &status, bool criticalWindow);
void localUiNotify(const char *message, bool ok);

