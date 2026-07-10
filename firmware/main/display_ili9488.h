#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t display_ili9488_init(void);
esp_err_t display_ili9488_configure(const char *driver, uint16_t width, uint16_t height,
                                    const char *orientation, bool flip);
esp_err_t display_ili9488_clear(uint16_t rgb565);
esp_err_t display_ili9488_fill_rect(int x, int y, int w, int h, uint16_t rgb565);
esp_err_t display_ili9488_draw_bitmap565(int x, int y, int w, int h, const uint16_t *pixels);
esp_err_t display_ili9488_set_brightness(uint8_t percent);
esp_err_t display_ili9488_set_awake(bool awake);
uint8_t display_ili9488_get_brightness(void);
uint16_t display_ili9488_get_width(void);
uint16_t display_ili9488_get_height(void);
const char *display_ili9488_get_driver(void);
const char *display_ili9488_get_orientation(void);
bool display_ili9488_get_flip(void);
bool display_ili9488_is_awake(void);
bool display_ili9488_is_ready(void);
