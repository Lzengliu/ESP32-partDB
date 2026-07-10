#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define APP_CONFIG_WIFI_SSID_LEN       33
#define APP_CONFIG_WIFI_PASS_LEN       65
#define APP_CONFIG_URL_LEN             160
#define APP_CONFIG_TOKEN_LEN           192
#define APP_CONFIG_SECRET_LEN          65
#define APP_CONFIG_PATH_LEN            96
#define APP_CONFIG_DEVICE_NAME_LEN     48
#define APP_CONFIG_DISPLAY_DRIVER_LEN  16
#define APP_CONFIG_DISPLAY_ORIENT_LEN  12

typedef struct {
    char device_name[APP_CONFIG_DEVICE_NAME_LEN];
    char wifi_ssid[APP_CONFIG_WIFI_SSID_LEN];
    char wifi_pass[APP_CONFIG_WIFI_PASS_LEN];
    char partdb_url[APP_CONFIG_URL_LEN];
    char partdb_token[APP_CONFIG_TOKEN_LEN];
    char device_secret[APP_CONFIG_SECRET_LEN];
    char boot_image_path[APP_CONFIG_PATH_LEN];
    char font_dir[APP_CONFIG_PATH_LEN];
    char font_path[APP_CONFIG_PATH_LEN];
    char screen_bg_path[APP_CONFIG_PATH_LEN];
    char boot_anim_path[APP_CONFIG_PATH_LEN];
    char lock_bg_path[APP_CONFIG_PATH_LEN];
    char display_driver[APP_CONFIG_DISPLAY_DRIVER_LEN];
    char display_orientation[APP_CONFIG_DISPLAY_ORIENT_LEN];
    bool display_flip;
    bool touch_swap_xy;
    bool touch_flip_x;
    bool touch_flip_y;
    bool ap_enabled;
    bool wifi_provisioned;
    uint8_t display_brightness;
    uint8_t touch_rotation;
    uint16_t display_width;
    uint16_t display_height;
    uint16_t touch_raw_width;
    uint16_t touch_raw_height;
} app_config_t;

void app_config_set_defaults(app_config_t *cfg);
esp_err_t app_config_load(app_config_t *cfg);
esp_err_t app_config_save(const app_config_t *cfg);
bool app_config_has_wifi(const app_config_t *cfg);
bool app_config_has_partdb(const app_config_t *cfg);
void app_config_copy_string(char *dst, unsigned dst_len, const char *src);
