#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "esp_err.h"

typedef struct {
    bool started;
    bool running;
    bool finished;
    esp_err_t display_err;
    esp_err_t touch_err;
    esp_err_t nfc_err;
    esp_err_t sd_err;
    esp_err_t camera_err;
    int64_t last_run_ms;
} hardware_diag_status_t;

esp_err_t hardware_diag_start_async(const app_config_t *cfg);
esp_err_t hardware_diag_run(const app_config_t *cfg);
hardware_diag_status_t hardware_diag_get_status(void);
