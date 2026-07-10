#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool mounted;
    uint64_t card_size_bytes;
    uint64_t total_bytes;
    uint64_t free_bytes;
} storage_sd_status_t;

esp_err_t storage_sd_init(void);
esp_err_t storage_sd_prepare_paths(void);
esp_err_t storage_sd_format_and_prepare(void);
storage_sd_status_t storage_sd_get_status(void);
