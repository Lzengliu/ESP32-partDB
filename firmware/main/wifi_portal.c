#include "wifi_portal.h"

#include <string.h>

#include "board_config.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "wifi";

static wifi_portal_status_t s_status;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static bool s_initialized;
static bool s_wifi_started;
static app_config_t *s_cfg;
static int s_sta_disconnect_count;
static bool s_recovery_ap_started;

static esp_err_t configure_ap(void);

static bool should_start_ap(const app_config_t *cfg)
{
    return !cfg || cfg->ap_enabled || !cfg->wifi_provisioned ||
           !app_config_has_wifi(cfg) || cfg->wifi_pass[0] == '\0' || s_recovery_ap_started;
}

static bool should_start_sta(const app_config_t *cfg)
{
    return cfg && app_config_has_wifi(cfg) && cfg->wifi_pass[0] != '\0';
}

static void start_recovery_ap(void)
{
    if (s_recovery_ap_started || s_status.ap_started) {
        return;
    }
    s_recovery_ap_started = true;
    s_status.ap_enabled = true;
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        err = configure_ap();
    }
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "STA reconnect failed repeatedly; recovery AP enabled");
    } else {
        ESP_LOGW(TAG, "Failed to enable recovery AP: %s", esp_err_to_name(err));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        s_status.sta_started = true;
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_status.sta_connected = false;
        s_status.ip[0] = '\0';
        if (should_start_sta(s_cfg)) {
            s_sta_disconnect_count++;
            if (!should_start_ap(s_cfg) && s_sta_disconnect_count >= 3) {
                start_recovery_ap();
            }
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_STOP) {
        s_status.sta_started = false;
        s_status.sta_connected = false;
        s_status.ip[0] = '\0';
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP) {
        s_status.ap_started = false;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_status.sta_connected = true;
        s_sta_disconnect_count = 0;
        ESP_LOGI(TAG, "STA IP: %s", s_status.ip);
        if (s_cfg && app_config_has_wifi(s_cfg) && !s_cfg->wifi_provisioned) {
            s_cfg->wifi_provisioned = true;
            s_cfg->ap_enabled = false;
            s_status.wifi_provisioned = true;
            s_status.ap_enabled = should_start_ap(s_cfg);
            esp_err_t err = app_config_save(s_cfg);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "WiFi provisioned; AP will be disabled on next reboot");
            } else {
                ESP_LOGW(TAG, "Failed to persist WiFi provision state: %s", esp_err_to_name(err));
            }
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        s_status.ap_started = true;
        ESP_LOGI(TAG, "AP started: ssid=\"%s\" password=\"%s\" url=http://192.168.4.1/",
                 BOARD_AP_DEFAULT_SSID, BOARD_AP_DEFAULT_PASS);
    }
}

static esp_err_t ensure_wifi_stack(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop failed");

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_sta_netif || !s_ap_netif) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL),
                        TAG, "wifi handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL),
                        TAG, "ip handler failed");

    s_initialized = true;
    return ESP_OK;
}

static esp_err_t configure_ap(void)
{
    wifi_config_t ap_cfg = {0};
    strlcpy((char *)ap_cfg.ap.ssid, BOARD_AP_DEFAULT_SSID, sizeof(ap_cfg.ap.ssid));
    strlcpy((char *)ap_cfg.ap.password, BOARD_AP_DEFAULT_PASS, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.ssid_len = strlen((char *)ap_cfg.ap.ssid);
    ap_cfg.ap.channel = 6;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    if (strlen((char *)ap_cfg.ap.password) < 8) {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }
    return esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
}

static esp_err_t configure_sta(const app_config_t *cfg)
{
    wifi_config_t sta_cfg = {0};
    if (cfg && app_config_has_wifi(cfg)) {
        strlcpy((char *)sta_cfg.sta.ssid, cfg->wifi_ssid, sizeof(sta_cfg.sta.ssid));
        strlcpy((char *)sta_cfg.sta.password, cfg->wifi_pass, sizeof(sta_cfg.sta.password));
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        strlcpy(s_status.ssid, cfg->wifi_ssid, sizeof(s_status.ssid));
    }
    return esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
}

static esp_err_t apply_wifi_config(app_config_t *cfg)
{
    bool start_ap = should_start_ap(cfg);
    bool start_sta = should_start_sta(cfg);
    wifi_mode_t mode = WIFI_MODE_NULL;

    if (start_ap && start_sta) {
        mode = WIFI_MODE_APSTA;
    } else if (start_ap) {
        mode = WIFI_MODE_AP;
    } else {
        mode = WIFI_MODE_STA;
    }

    s_status.ap_enabled = start_ap;
    s_status.wifi_provisioned = cfg ? cfg->wifi_provisioned : false;
    if (!start_ap) {
        s_status.ap_started = false;
    }
    if (!start_sta) {
        s_status.sta_started = false;
        s_status.sta_connected = false;
        s_status.ip[0] = '\0';
        s_status.ssid[0] = '\0';
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(mode), TAG, "mode failed");
    if (start_ap) {
        ESP_RETURN_ON_ERROR(configure_ap(), TAG, "ap config failed");
    }
    if (start_sta) {
        ESP_RETURN_ON_ERROR(configure_sta(cfg), TAG, "sta config failed");
    }
    ESP_LOGI(TAG, "WiFi mode=%s ap=%d sta=%d provisioned=%d",
             mode == WIFI_MODE_APSTA ? "APSTA" : (mode == WIFI_MODE_AP ? "AP" : "STA"),
             start_ap, start_sta, cfg ? cfg->wifi_provisioned : 0);
    return ESP_OK;
}

esp_err_t wifi_portal_init(app_config_t *cfg)
{
    s_cfg = cfg;
    ESP_RETURN_ON_ERROR(ensure_wifi_stack(), TAG, "stack failed");
    ESP_RETURN_ON_ERROR(apply_wifi_config(cfg), TAG, "wifi config failed");

    if (!s_wifi_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
        ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "disable wifi ps failed");
        s_wifi_started = true;
    } else {
        esp_wifi_disconnect();
        esp_wifi_connect();
    }
    return ESP_OK;
}

esp_err_t wifi_portal_reconfigure(app_config_t *cfg)
{
    if (!s_initialized) {
        return wifi_portal_init(cfg);
    }
    s_cfg = cfg;
    s_status.sta_connected = false;
    s_status.ip[0] = '\0';
    esp_wifi_disconnect();
    ESP_RETURN_ON_ERROR(apply_wifi_config(cfg), TAG, "wifi reconfig failed");
    if (should_start_sta(cfg)) {
        return esp_wifi_connect();
    }
    return ESP_OK;
}

esp_err_t wifi_portal_set_ap_enabled(bool enabled)
{
    if (!s_cfg) {
        return ESP_ERR_INVALID_STATE;
    }
    s_cfg->ap_enabled = enabled;
    esp_err_t err = app_config_save(s_cfg);
    if (err != ESP_OK) {
        return err;
    }
    if (!s_initialized) {
        return ESP_OK;
    }
    err = apply_wifi_config(s_cfg);
    if (err == ESP_OK && should_start_sta(s_cfg) && !s_status.sta_connected) {
        err = esp_wifi_connect();
    }
    return err;
}

wifi_portal_status_t wifi_portal_get_status(void)
{
    return s_status;
}
