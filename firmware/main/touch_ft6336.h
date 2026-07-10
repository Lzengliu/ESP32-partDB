#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool touched;
    uint16_t x;
    uint16_t y;
    uint8_t points;
} touch_point_t;

esp_err_t touch_ft6336_init(void);
esp_err_t touch_ft6336_read(touch_point_t *point);
void touch_ft6336_transform_to_display(touch_point_t *point, bool swap_xy,
                                       uint16_t raw_width,
                                       uint16_t raw_height,
                                       uint8_t rotation,
                                       bool flip_x, bool flip_y,
                                       uint16_t display_width,
                                       uint16_t display_height);
bool touch_ft6336_is_ready(void);
