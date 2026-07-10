#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool started;
    bool ready;
    bool paused_for_camera;
    bool paused_for_request;
    bool tag_present;
    char uid[21];
    char text[128];
    int64_t last_seen_ms;
    uint32_t read_count;
    uint32_t text_read_count;
    uint32_t error_count;
    esp_err_t last_err;
} nfc_service_status_t;

esp_err_t nfc_service_start(void);
nfc_service_status_t nfc_service_get_status(void);
void nfc_service_suspend_for_request(void);
void nfc_service_resume_after_request(void);
