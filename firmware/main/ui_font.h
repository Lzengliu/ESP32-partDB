#pragma once

#include <stdint.h>

void ui_font_set_active_path(const char *path);
const char *ui_font_get_active_path(void);
int ui_font_draw_text_maxw(uint16_t *buf, int bw, int bh, int x, int y,
                           const char *text, uint8_t scale, uint16_t fg,
                           int max_w);
int ui_font_draw_text_small_maxw(uint16_t *buf, int bw, int bh, int x, int y,
                                 const char *text, uint16_t fg, int max_w);
int ui_font_draw_text_tiny_maxw(uint16_t *buf, int bw, int bh, int x, int y,
                                const char *text, uint16_t fg, int max_w);
