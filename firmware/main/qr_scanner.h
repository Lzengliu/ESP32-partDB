#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define QR_SCANNER_TEXT_MAX 192

typedef struct {
    bool found;
    char text[QR_SCANNER_TEXT_MAX];
    int code_count;
    int width;
    int height;
    uint32_t elapsed_ms;
    esp_err_t err;
    char decode_error[48];
} qr_scanner_result_t;

typedef struct {
    bool last_found;
    char last_text[QR_SCANNER_TEXT_MAX];
    int last_code_count;
    int last_width;
    int last_height;
    uint32_t last_elapsed_ms;
    uint32_t scan_count;
    uint32_t found_count;
    esp_err_t last_err;
    char last_decode_error[48];
} qr_scanner_status_t;

esp_err_t qr_scanner_scan(qr_scanner_result_t *result);
qr_scanner_status_t qr_scanner_get_status(void);
