#pragma once

#include <stdbool.h>

#include "app_config.h"
#include "esp_err.h"

typedef struct {
    bool ap_started;
    bool sta_started;
    bool sta_connected;
    bool ap_enabled;
    bool wifi_provisioned;
    char ip[20];
    char ssid[APP_CONFIG_WIFI_SSID_LEN];
} wifi_portal_status_t;

esp_err_t wifi_portal_init(app_config_t *cfg);
esp_err_t wifi_portal_reconfigure(app_config_t *cfg);
esp_err_t wifi_portal_set_ap_enabled(bool enabled);
wifi_portal_status_t wifi_portal_get_status(void);
