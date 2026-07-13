#include "app_config.h"

#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

static const char *TAG = "app_config";

#define CFG_NS                  "app_cfg"
#define KEY_DEVICE_NAME         "dev_name"
#define KEY_WIFI_SSID           "wifi_ssid"
#define KEY_WIFI_PASS           "wifi_pass"
#define KEY_PARTDB_URL          "partdb_url"
#define KEY_PARTDB_TOKEN        "partdb_token"
#define KEY_CAMERA_UPLOAD_URL   "cam_up_url"
#define KEY_DEVICE_SECRET       "dev_secret"
#define KEY_BOOT_IMAGE          "boot_img"
#define KEY_FONT_DIR            "font_dir"
#define KEY_FONT_PATH           "font_path"
#define KEY_SCREEN_BG           "screen_bg"
#define KEY_BOOT_ANIM           "boot_anim"
#define KEY_LOCK_BG             "lock_bg"
#define KEY_UI_LANGUAGE         "ui_lang"
#define KEY_DISPLAY_DRIVER      "disp_driver"
#define KEY_DISPLAY_ORIENT      "disp_orient"
#define KEY_DISPLAY_FLIP        "disp_flip"
#define KEY_TOUCH_ROTATION      "touch_rot"
#define KEY_TOUCH_SWAP_XY       "touch_swap"
#define KEY_TOUCH_FLIP_X        "touch_flip_x"
#define KEY_TOUCH_FLIP_Y        "touch_flip_y"
#define KEY_DISPLAY_WIDTH       "disp_width"
#define KEY_DISPLAY_HEIGHT      "disp_height"
#define KEY_TOUCH_RAW_WIDTH     "touch_raw_w"
#define KEY_TOUCH_RAW_HEIGHT    "touch_raw_h"
#define KEY_AP_ENABLED          "ap_enabled"
#define KEY_WIFI_PROVISIONED    "wifi_done"
#define KEY_NFC_READ_CONFIRM    "nfc_read_cf"
#define KEY_DISPLAY_BRIGHTNESS  "disp_bright"
#define KEY_SCREEN_SLEEP_MIN    "scr_sleep"

void app_config_copy_string(char *dst, unsigned dst_len, const char *src)
{
    if (!dst || dst_len == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_len, "%s", src);
}

static void generate_secret(char *out, size_t len)
{
    if (!out || len < 33) {
        return;
    }
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        uint8_t b = (uint8_t)(esp_random() & 0xff);
        out[i * 2] = hex[b >> 4];
        out[i * 2 + 1] = hex[b & 0x0f];
    }
    out[32] = '\0';
}

void app_config_set_defaults(app_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    app_config_copy_string(cfg->device_name, sizeof(cfg->device_name), BOARD_DEVICE_NAME);
    app_config_copy_string(cfg->boot_image_path, sizeof(cfg->boot_image_path), BOARD_SD_BOOT_DIR "/boot.jpg");
    app_config_copy_string(cfg->font_dir, sizeof(cfg->font_dir), BOARD_SD_FONT_DIR);
    cfg->font_path[0] = '\0';
    cfg->screen_bg_path[0] = '\0';
    cfg->boot_anim_path[0] = '\0';
    cfg->lock_bg_path[0] = '\0';
    app_config_copy_string(cfg->ui_language, sizeof(cfg->ui_language), "zh-CN");
    app_config_copy_string(cfg->display_driver, sizeof(cfg->display_driver), "ili9488");
    app_config_copy_string(cfg->display_orientation, sizeof(cfg->display_orientation), "portrait");
    cfg->display_flip = true;
    cfg->touch_rotation = 2;
    cfg->touch_swap_xy = false;
    cfg->touch_flip_x = false;
    cfg->touch_flip_y = false;
    cfg->ap_enabled = true;
    cfg->wifi_provisioned = false;
    cfg->nfc_read_confirm = false;
    cfg->display_brightness = 80;
    cfg->screen_sleep_minutes = 5;
    cfg->display_width = 320;
    cfg->display_height = 480;
    cfg->touch_raw_width = BOARD_TOUCH_RAW_WIDTH;
    cfg->touch_raw_height = BOARD_TOUCH_RAW_HEIGHT;
    generate_secret(cfg->device_secret, sizeof(cfg->device_secret));
}

static esp_err_t load_str(nvs_handle_t h, const char *key, char *dst, size_t dst_len)
{
    size_t len = dst_len;
    esp_err_t err = nvs_get_str(h, key, dst, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to read %s: %s", key, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t save_str(nvs_handle_t h, const char *key, const char *value)
{
    if (!value || value[0] == '\0') {
        esp_err_t err = nvs_erase_key(h, key);
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }
    return nvs_set_str(h, key, value);
}

static void load_u8(nvs_handle_t h, const char *key, uint8_t *dst)
{
    uint8_t value = 0;
    esp_err_t err = nvs_get_u8(h, key, &value);
    if (err == ESP_OK) {
        *dst = value;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to read %s: %s", key, esp_err_to_name(err));
    }
}

static void load_u16(nvs_handle_t h, const char *key, uint16_t *dst)
{
    uint16_t value = 0;
    esp_err_t err = nvs_get_u16(h, key, &value);
    if (err == ESP_OK) {
        *dst = value;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to read %s: %s", key, esp_err_to_name(err));
    }
}

static void load_bool(nvs_handle_t h, const char *key, bool *dst)
{
    uint8_t value = *dst ? 1 : 0;
    load_u8(h, key, &value);
    *dst = value != 0;
}

esp_err_t app_config_load(app_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    app_config_set_defaults(cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open(CFG_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return app_config_save(cfg);
    }
    if (err != ESP_OK) {
        return err;
    }

    load_str(h, KEY_WIFI_SSID, cfg->wifi_ssid, sizeof(cfg->wifi_ssid));
    load_str(h, KEY_DEVICE_NAME, cfg->device_name, sizeof(cfg->device_name));
    load_str(h, KEY_WIFI_PASS, cfg->wifi_pass, sizeof(cfg->wifi_pass));
    load_str(h, KEY_PARTDB_URL, cfg->partdb_url, sizeof(cfg->partdb_url));
    load_str(h, KEY_PARTDB_TOKEN, cfg->partdb_token, sizeof(cfg->partdb_token));
    load_str(h, KEY_CAMERA_UPLOAD_URL, cfg->camera_upload_url, sizeof(cfg->camera_upload_url));
    esp_err_t secret_err = load_str(h, KEY_DEVICE_SECRET, cfg->device_secret,
                                    sizeof(cfg->device_secret));
    load_str(h, KEY_BOOT_IMAGE, cfg->boot_image_path, sizeof(cfg->boot_image_path));
    load_str(h, KEY_FONT_DIR, cfg->font_dir, sizeof(cfg->font_dir));
    load_str(h, KEY_FONT_PATH, cfg->font_path, sizeof(cfg->font_path));
    load_str(h, KEY_SCREEN_BG, cfg->screen_bg_path, sizeof(cfg->screen_bg_path));
    load_str(h, KEY_BOOT_ANIM, cfg->boot_anim_path, sizeof(cfg->boot_anim_path));
    load_str(h, KEY_LOCK_BG, cfg->lock_bg_path, sizeof(cfg->lock_bg_path));
    load_str(h, KEY_UI_LANGUAGE, cfg->ui_language, sizeof(cfg->ui_language));
    load_str(h, KEY_DISPLAY_DRIVER, cfg->display_driver, sizeof(cfg->display_driver));
    load_str(h, KEY_DISPLAY_ORIENT, cfg->display_orientation, sizeof(cfg->display_orientation));
    load_bool(h, KEY_DISPLAY_FLIP, &cfg->display_flip);
    load_bool(h, KEY_TOUCH_SWAP_XY, &cfg->touch_swap_xy);
    load_bool(h, KEY_TOUCH_FLIP_X, &cfg->touch_flip_x);
    load_bool(h, KEY_TOUCH_FLIP_Y, &cfg->touch_flip_y);
    load_bool(h, KEY_AP_ENABLED, &cfg->ap_enabled);
    load_bool(h, KEY_WIFI_PROVISIONED, &cfg->wifi_provisioned);
    load_bool(h, KEY_NFC_READ_CONFIRM, &cfg->nfc_read_confirm);
    load_u8(h, KEY_DISPLAY_BRIGHTNESS, &cfg->display_brightness);
    load_u8(h, KEY_SCREEN_SLEEP_MIN, &cfg->screen_sleep_minutes);
    load_u8(h, KEY_TOUCH_ROTATION, &cfg->touch_rotation);
    load_u16(h, KEY_DISPLAY_WIDTH, &cfg->display_width);
    load_u16(h, KEY_DISPLAY_HEIGHT, &cfg->display_height);
    load_u16(h, KEY_TOUCH_RAW_WIDTH, &cfg->touch_raw_width);
    load_u16(h, KEY_TOUCH_RAW_HEIGHT, &cfg->touch_raw_height);
    nvs_close(h);

    if (cfg->display_brightness > 100) {
        cfg->display_brightness = 80;
    }
    switch (cfg->screen_sleep_minutes) {
    case 5:
    case 10:
    case 15:
    case 30:
    case 60:
        break;
    default:
        cfg->screen_sleep_minutes = 5;
        break;
    }
    if (cfg->display_width < 64 || cfg->display_width > 1024) {
        cfg->display_width = 320;
    }
    if (cfg->display_height < 64 || cfg->display_height > 1024) {
        cfg->display_height = 480;
    }
    if (cfg->touch_raw_width < 64 || cfg->touch_raw_width > 1024) {
        cfg->touch_raw_width = BOARD_TOUCH_RAW_WIDTH;
    }
    if (cfg->touch_raw_height < 64 || cfg->touch_raw_height > 1024) {
        cfg->touch_raw_height = BOARD_TOUCH_RAW_HEIGHT;
    }
    if (strcmp(cfg->display_driver, "ili9488") != 0) {
        app_config_copy_string(cfg->display_driver, sizeof(cfg->display_driver), "ili9488");
    }
    if (strcmp(cfg->display_orientation, "landscape") != 0) {
        app_config_copy_string(cfg->display_orientation, sizeof(cfg->display_orientation), "portrait");
    }
    if (cfg->touch_rotation > 3) {
        cfg->touch_rotation = 2;
    }
    if (cfg->device_name[0] == '\0') {
        app_config_copy_string(cfg->device_name, sizeof(cfg->device_name), BOARD_DEVICE_NAME);
    }
    if (strcmp(cfg->ui_language, "zh-CN") != 0) {
        app_config_copy_string(cfg->ui_language, sizeof(cfg->ui_language), "zh-CN");
    }
    app_config_copy_string(cfg->boot_image_path, sizeof(cfg->boot_image_path), BOARD_SD_BOOT_DIR "/boot.jpg");
    app_config_copy_string(cfg->font_dir, sizeof(cfg->font_dir), BOARD_SD_FONT_DIR);
    if (secret_err == ESP_ERR_NVS_NOT_FOUND || cfg->device_secret[0] == '\0') {
        generate_secret(cfg->device_secret, sizeof(cfg->device_secret));
        err = app_config_save(cfg);
    }
    return err;
}

esp_err_t app_config_save(const app_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(CFG_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    if (err == ESP_OK) err = save_str(h, KEY_WIFI_SSID, cfg->wifi_ssid);
    if (err == ESP_OK) err = save_str(h, KEY_DEVICE_NAME, cfg->device_name);
    if (err == ESP_OK) err = save_str(h, KEY_WIFI_PASS, cfg->wifi_pass);
    if (err == ESP_OK) err = save_str(h, KEY_PARTDB_URL, cfg->partdb_url);
    if (err == ESP_OK) err = save_str(h, KEY_PARTDB_TOKEN, cfg->partdb_token);
    if (err == ESP_OK) err = save_str(h, KEY_CAMERA_UPLOAD_URL, cfg->camera_upload_url);
    if (err == ESP_OK) err = save_str(h, KEY_DEVICE_SECRET, cfg->device_secret);
    if (err == ESP_OK) err = save_str(h, KEY_BOOT_IMAGE, cfg->boot_image_path);
    if (err == ESP_OK) err = save_str(h, KEY_FONT_DIR, cfg->font_dir);
    if (err == ESP_OK) err = save_str(h, KEY_FONT_PATH, cfg->font_path);
    if (err == ESP_OK) err = save_str(h, KEY_SCREEN_BG, cfg->screen_bg_path);
    if (err == ESP_OK) err = save_str(h, KEY_BOOT_ANIM, cfg->boot_anim_path);
    if (err == ESP_OK) err = save_str(h, KEY_LOCK_BG, cfg->lock_bg_path);
    if (err == ESP_OK) err = save_str(h, KEY_UI_LANGUAGE, cfg->ui_language);
    if (err == ESP_OK) err = save_str(h, KEY_DISPLAY_DRIVER, cfg->display_driver);
    if (err == ESP_OK) err = save_str(h, KEY_DISPLAY_ORIENT, cfg->display_orientation);
    if (err == ESP_OK) err = nvs_set_u8(h, KEY_DISPLAY_FLIP, cfg->display_flip ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, KEY_TOUCH_ROTATION, cfg->touch_rotation);
    if (err == ESP_OK) err = nvs_set_u8(h, KEY_TOUCH_SWAP_XY, cfg->touch_swap_xy ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, KEY_TOUCH_FLIP_X, cfg->touch_flip_x ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, KEY_TOUCH_FLIP_Y, cfg->touch_flip_y ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, KEY_AP_ENABLED, cfg->ap_enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, KEY_WIFI_PROVISIONED, cfg->wifi_provisioned ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, KEY_NFC_READ_CONFIRM, cfg->nfc_read_confirm ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, KEY_DISPLAY_BRIGHTNESS, cfg->display_brightness);
    if (err == ESP_OK) err = nvs_set_u8(h, KEY_SCREEN_SLEEP_MIN, cfg->screen_sleep_minutes);
    if (err == ESP_OK) err = nvs_set_u16(h, KEY_DISPLAY_WIDTH, cfg->display_width);
    if (err == ESP_OK) err = nvs_set_u16(h, KEY_DISPLAY_HEIGHT, cfg->display_height);
    if (err == ESP_OK) err = nvs_set_u16(h, KEY_TOUCH_RAW_WIDTH, cfg->touch_raw_width);
    if (err == ESP_OK) err = nvs_set_u16(h, KEY_TOUCH_RAW_HEIGHT, cfg->touch_raw_height);
    if (err == ESP_OK) err = nvs_commit(h);

    nvs_close(h);
    return err;
}

bool app_config_has_wifi(const app_config_t *cfg)
{
    return cfg && cfg->wifi_ssid[0] != '\0';
}

bool app_config_has_partdb(const app_config_t *cfg)
{
    return cfg && cfg->partdb_url[0] != '\0' && cfg->partdb_token[0] != '\0';
}
