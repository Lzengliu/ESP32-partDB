#include "hardware_diag.h"

#include "camera_mgr.h"
#include "display_ili9488.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nfc_pn532.h"
#include "nfc_service.h"
#include "storage_sd.h"
#include "touch_ft6336.h"

static const char *TAG = "hw_diag";
static hardware_diag_status_t s_status;
static app_config_t s_async_cfg;

static void log_result(const char *name, esp_err_t err)
{
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s OK", name);
    } else {
        ESP_LOGW(TAG, "%s failed: %s", name, esp_err_to_name(err));
    }
}

esp_err_t hardware_diag_run(const app_config_t *cfg)
{
    int64_t start_us = esp_timer_get_time();
    s_status.started = true;
    s_status.running = true;
    s_status.finished = false;
    s_status.display_err = ESP_ERR_INVALID_STATE;
    s_status.touch_err = ESP_ERR_INVALID_STATE;
    s_status.nfc_err = ESP_ERR_INVALID_STATE;
    s_status.sd_err = ESP_ERR_INVALID_STATE;
    s_status.camera_err = ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "Hardware diagnostics started");

    if (cfg) {
        (void)display_ili9488_configure(cfg->display_driver, cfg->display_width,
                                        cfg->display_height, cfg->display_orientation,
                                        cfg->display_flip);
    }
    s_status.display_err = display_ili9488_set_brightness(cfg ? cfg->display_brightness : 80);
    if (s_status.display_err == ESP_OK) {
        s_status.display_err = display_ili9488_init();
    }
    log_result("display", s_status.display_err);

    nfc_service_suspend_for_camera();
    vTaskDelay(pdMS_TO_TICKS(180));
    s_status.camera_err = camera_mgr_init();
    if (s_status.camera_err == ESP_OK && !camera_mgr_should_keep_online()) {
        camera_mgr_deinit();
    }
    nfc_service_resume_after_camera();
    log_result("camera", s_status.camera_err);

    s_status.nfc_err = nfc_pn532_init();
    log_result("nfc", s_status.nfc_err);

    s_status.touch_err = touch_ft6336_init();
    log_result("touch", s_status.touch_err);

    s_status.sd_err = storage_sd_prepare_paths();
    log_result("tf_card", s_status.sd_err);

    s_status.last_run_ms = (esp_timer_get_time() - start_us) / 1000;
    s_status.running = false;
    s_status.finished = true;
    ESP_LOGI(TAG, "Hardware diagnostics finished in %lld ms",
             (long long)s_status.last_run_ms);
    return ESP_OK;
}

static void hardware_diag_task(void *arg)
{
    (void)arg;
    (void)hardware_diag_run(&s_async_cfg);
    vTaskDelete(NULL);
}

esp_err_t hardware_diag_start_async(const app_config_t *cfg)
{
    if (s_status.running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (cfg) {
        s_async_cfg = *cfg;
    }
    BaseType_t ok = xTaskCreate(hardware_diag_task, "hw_diag", 6144, NULL, 3, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

hardware_diag_status_t hardware_diag_get_status(void)
{
    return s_status;
}
