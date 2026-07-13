#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool started;
    uint32_t uptime_seconds;
    uint32_t check_count;
    uint32_t low_memory_count;
    uint32_t cache_trim_count;
    uint32_t camera_cleanup_count;
    uint32_t heap_integrity_fail_count;
    size_t internal_free;
    size_t internal_min;
    size_t internal_largest;
    size_t psram_free;
    size_t psram_min;
    size_t psram_largest;
    esp_err_t last_err;
} runtime_guard_status_t;

esp_err_t runtime_guard_start(void);
runtime_guard_status_t runtime_guard_get_status(void);
