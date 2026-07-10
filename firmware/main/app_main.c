#include <stdio.h>

#include "app_config.h"
#include "board_config.h"
#include "button_input.h"
#include "camera_mgr.h"
#include "device_ui.h"
#include "display_ili9488.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "hardware_diag.h"
#include "nvs_flash.h"
#include "nfc_pn532.h"
#include "nfc_service.h"
#include "partdb_client.h"
#include "peripheral_arbiter.h"
#include "storage_sd.h"
#include "touch_ft6336.h"
#include "ui_font.h"
#include "wifi_portal.h"
#include "http_server.h"

static const char *TAG = "app";
static app_config_t s_cfg;

static void boot_ui_progress(uint8_t percent, uint16_t color)
{
    if (!display_ili9488_is_ready()) {
        return;
    }
    uint16_t width = display_ili9488_get_width();
    uint16_t height = display_ili9488_get_height();
    uint16_t bar_h = height > 240 ? 12 : 8;
    uint16_t fill_w = (uint16_t)(((uint32_t)width * percent) / 100);
    display_ili9488_fill_rect(0, height - bar_h, width, bar_h, 0x2104);
    if (fill_w > 0) {
        display_ili9488_fill_rect(0, height - bar_h, fill_w, bar_h, color);
    }
}

static void boot_ui_start(const app_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    (void)display_ili9488_set_brightness(cfg->display_brightness);
    esp_err_t err = display_ili9488_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Boot UI display init skipped: %s", esp_err_to_name(err));
        return;
    }

    uint16_t width = display_ili9488_get_width();
    uint16_t height = display_ili9488_get_height();
    (void)display_ili9488_clear(0x0000);
    (void)display_ili9488_fill_rect(0, 0, width, height / 3, 0x18E3);
    (void)display_ili9488_fill_rect(0, height / 3, width, height / 3, 0x0208);
    (void)display_ili9488_fill_rect(0, (height / 3) * 2, width, height - (height / 3) * 2, 0x2104);
    boot_ui_progress(10, 0x07E0);
    if (cfg->boot_anim_path[0]) {
        ESP_LOGI(TAG, "Boot animation selected for UI renderer: %s", cfg->boot_anim_path);
    }
}

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting ESP32 Part-DB terminal");
    init_nvs();

    ESP_ERROR_CHECK(peripheral_arbiter_init());
    ESP_ERROR_CHECK(app_config_load(&s_cfg));
    ui_font_set_active_path(s_cfg.font_path);
    esp_err_t err = button_input_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Button service not started: %s", esp_err_to_name(err));
    }
    (void)display_ili9488_configure(s_cfg.display_driver, s_cfg.display_width,
                                    s_cfg.display_height, s_cfg.display_orientation,
                                    s_cfg.display_flip);
    boot_ui_start(&s_cfg);
    ESP_ERROR_CHECK(partdb_client_init(&s_cfg));
    boot_ui_progress(25, 0x07E0);

    err = wifi_portal_init(&s_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(err));
    }
    boot_ui_progress(45, err == ESP_OK ? 0x07E0 : 0xFFE0);

    err = http_server_start(&s_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed: %s", esp_err_to_name(err));
    }
    boot_ui_progress(60, err == ESP_OK ? 0x07E0 : 0xFFE0);

#if BOARD_HARDWARE_DIAG_AT_BOOT
    boot_ui_progress(75, 0x07E0);
    err = hardware_diag_run(&s_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Hardware diagnostics failed to run: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Hardware diagnostics completed");
    }
    boot_ui_progress(90, err == ESP_OK ? 0x07E0 : 0xFFE0);
    err = nfc_service_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NFC service not started: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "NFC service queued");
    }
    boot_ui_progress(96, err == ESP_OK ? 0x07E0 : 0xFFE0);
#elif BOARD_CAMERA_PREWARM_AT_BOOT
    err = nfc_service_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NFC service not started: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "NFC service queued");
    }
    boot_ui_progress(75, err == ESP_OK ? 0x07E0 : 0xFFE0);
    err = camera_mgr_prewarm_async();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Camera prewarm not started: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Camera prewarm queued");
    }
#endif

#if BOARD_INIT_OPTIONAL_HARDWARE_AT_BOOT && !BOARD_HARDWARE_DIAG_AT_BOOT
    esp_err_t hw_err;

    hw_err = storage_sd_init();
    if (hw_err != ESP_OK) {
        ESP_LOGW(TAG, "SD disabled until card/wiring is ready: %s", esp_err_to_name(hw_err));
    }

    hw_err = display_ili9488_init();
    if (hw_err != ESP_OK) {
        ESP_LOGW(TAG, "Display disabled until wiring is ready: %s", esp_err_to_name(hw_err));
    }

    hw_err = touch_ft6336_init();
    if (hw_err != ESP_OK) {
        ESP_LOGW(TAG, "Touch disabled until wiring is ready: %s", esp_err_to_name(hw_err));
    }

    hw_err = nfc_pn532_init();
    if (hw_err != ESP_OK) {
        ESP_LOGW(TAG, "NFC disabled until wiring/module is ready: %s", esp_err_to_name(hw_err));
    }
#elif !BOARD_HARDWARE_DIAG_AT_BOOT
    ESP_LOGI(TAG, "Other optional hardware init skipped at boot; use web diagnostics to probe peripherals");
#endif
#if !BOARD_HARDWARE_DIAG_AT_BOOT
    boot_ui_progress(100, 0x07E0);
#endif
    err = device_ui_start(&s_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Device UI not started: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Device UI started");
    }
    ESP_LOGI(TAG, "Boot complete");
}
