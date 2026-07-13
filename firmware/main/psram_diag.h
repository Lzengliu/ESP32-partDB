#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool initialized;
    size_t chip_size;
    size_t heap_total;
    size_t heap_free;
    size_t heap_largest;
    uintptr_t mapped_start;
    uintptr_t mapped_end;
    size_t mapped_size;
    size_t heap_candidate_size;
    bool mem_test_ran;
    bool mem_test_ok;
    esp_err_t last_mem_test_err;
    esp_err_t last_add_heap_err;
} psram_diag_status_t;

void psram_diag_collect(psram_diag_status_t *out);
void psram_diag_log_boot(void);
esp_err_t psram_diag_run_read_probe(void);
esp_err_t psram_diag_run_heap_probe(void);
esp_err_t psram_diag_run_mem_test(void);
esp_err_t psram_diag_add_to_heap(void);
