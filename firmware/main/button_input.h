#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool started;
    uint8_t configured_count;
    bool up_pressed;
    bool down_pressed;
    bool ok_pressed;
    bool wake_pressed;
    const char *last_event;
    uint32_t event_count;
} button_input_status_t;

esp_err_t button_input_init(void);
button_input_status_t button_input_get_status(void);
