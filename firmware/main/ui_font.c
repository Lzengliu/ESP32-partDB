#include "ui_font.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "calendar_font_data.h"
#include "ui_font_sizes_data.h"

#define UI_FONT_PATH_MAX 96

static char s_active_font_path[UI_FONT_PATH_MAX];

typedef struct {
    uint32_t cp;
    uint16_t rows[16];
} ui_symbol_glyph_t;

static const ui_symbol_glyph_t s_symbol_glyphs[] = {
    {0x00B2, {0x0000, 0x3C00, 0x6600, 0x0600, 0x0C00, 0x1800, 0x3000, 0x7E00,
              0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000}},
    {0x00B3, {0x0000, 0x3C00, 0x6600, 0x0600, 0x1C00, 0x0600, 0x6600, 0x3C00,
              0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000}},
    {0x00B0, {0x0000, 0x1C00, 0x2200, 0x4100, 0x4100, 0x2200, 0x1C00, 0x0000,
              0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000}},
    {0x00B1, {0x0000, 0x0000, 0x0180, 0x0180, 0x0180, 0x7FFE, 0x7FFE, 0x0180,
              0x0180, 0x0180, 0x0000, 0x0000, 0x7FFE, 0x7FFE, 0x0000, 0x0000}},
    {0x00D8, {0x0000, 0x07E6, 0x1FFC, 0x3838, 0x3070, 0x60E6, 0x61C6, 0x6386,
              0x6706, 0x6E06, 0x3C0C, 0x381C, 0x3FF8, 0x67E0, 0x0000, 0x0000}},
    {0x00D7, {0x0000, 0x0000, 0x6006, 0x300C, 0x1818, 0x0C30, 0x0660, 0x03C0,
              0x03C0, 0x0660, 0x0C30, 0x1818, 0x300C, 0x6006, 0x0000, 0x0000}},
    {0x00F7, {0x0000, 0x0000, 0x0000, 0x0180, 0x0180, 0x0000, 0x7FFE, 0x7FFE,
              0x0000, 0x0180, 0x0180, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000}},
    {0x03A3, {0x0000, 0x7FFE, 0x7FFE, 0x6000, 0x3000, 0x1800, 0x0C00, 0x0600,
              0x0300, 0x0600, 0x0C00, 0x1800, 0x3000, 0x7FFE, 0x7FFE, 0x0000}},
    {0x03A6, {0x0000, 0x0180, 0x0180, 0x0FF0, 0x1FF8, 0x399C, 0x718E, 0x718E,
              0x718E, 0x399C, 0x1FF8, 0x0FF0, 0x0180, 0x0180, 0x0000, 0x0000}},
    {0x03A9, {0x0000, 0x07E0, 0x1FF8, 0x381C, 0x300C, 0x6006, 0x6006, 0x6006,
              0x6006, 0x300C, 0x381C, 0x1818, 0x7FFE, 0x7FFE, 0x0000, 0x0000}},
    {0x0394, {0x0000, 0x0180, 0x03C0, 0x0660, 0x0660, 0x0C30, 0x0C30, 0x1818,
              0x1818, 0x300C, 0x300C, 0x6006, 0x6006, 0x7FFE, 0x7FFE, 0x0000}},
    {0x03B1, {0x0000, 0x0000, 0x0000, 0x0F70, 0x1FF8, 0x381C, 0x300C, 0x6006,
              0x6006, 0x6006, 0x300E, 0x383E, 0x1FF6, 0x0F86, 0x0000, 0x0000}},
    {0x03B2, {0x0000, 0x0FE0, 0x1FF0, 0x3018, 0x3018, 0x3018, 0x3FF0, 0x3FF8,
              0x301C, 0x300C, 0x300C, 0x301C, 0x3FF8, 0x3FF0, 0x3000, 0x3000}},
    {0x03B3, {0x0000, 0x0000, 0x0000, 0x6006, 0x300C, 0x3018, 0x1830, 0x0C60,
              0x06C0, 0x0380, 0x0180, 0x0180, 0x0300, 0x0600, 0x0000, 0x0000}},
    {0x03B8, {0x0000, 0x07E0, 0x1FF8, 0x381C, 0x300C, 0x6006, 0x6006, 0x7FFE,
              0x7FFE, 0x6006, 0x6006, 0x300C, 0x381C, 0x1FF8, 0x07E0, 0x0000}},
    {0x03BB, {0x0000, 0x0C00, 0x0600, 0x0600, 0x0300, 0x0300, 0x0180, 0x0180,
              0x03C0, 0x0660, 0x0C30, 0x1818, 0x300C, 0x6006, 0x0000, 0x0000}},
    {0x03BC, {0x0000, 0x0000, 0x0000, 0x300C, 0x300C, 0x300C, 0x300C, 0x300C,
              0x300C, 0x301C, 0x303C, 0x3FEC, 0x3FC0, 0x3000, 0x3000, 0x0000}},
    {0x03C0, {0x0000, 0x0000, 0x0000, 0x7FFE, 0x7FFE, 0x0660, 0x0660, 0x0660,
              0x0660, 0x0660, 0x0660, 0x0666, 0x063C, 0x0000, 0x0000, 0x0000}},
    {0x03C1, {0x0000, 0x0000, 0x0FE0, 0x1FF0, 0x3018, 0x600C, 0x600C, 0x600C,
              0x3018, 0x1FF0, 0x0FE0, 0x6000, 0x6000, 0x6000, 0x0000, 0x0000}},
    {0x03C3, {0x0000, 0x0000, 0x0000, 0x3FFC, 0x7FFE, 0x6186, 0x6180, 0x6180,
              0x6180, 0x3180, 0x1F80, 0x0F00, 0x0000, 0x0000, 0x0000, 0x0000}},
    {0x03C4, {0x0000, 0x0000, 0x0000, 0x7FFE, 0x7FFE, 0x0180, 0x0180, 0x0180,
              0x0180, 0x0180, 0x018C, 0x00F8, 0x0000, 0x0000, 0x0000, 0x0000}},
    {0x03C6, {0x0000, 0x0180, 0x0180, 0x07E0, 0x1FF8, 0x318C, 0x6186, 0x6186,
              0x318C, 0x1FF8, 0x07E0, 0x0180, 0x0180, 0x0180, 0x0000, 0x0000}},
    {0x207B, {0x0000, 0x0000, 0x7E00, 0x7E00, 0x0000, 0x0000, 0x0000, 0x0000,
              0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000}},
    {0x2103, {0x0000, 0x7000, 0x8800, 0x8800, 0x7000, 0x0000, 0x0F80, 0x30C0,
              0x6000, 0x6000, 0x6000, 0x6000, 0x30C0, 0x0F80, 0x0000, 0x0000}},
    {0x2109, {0x0000, 0x7000, 0x8800, 0x8800, 0x7000, 0x0000, 0x7FC0, 0x6000,
              0x6000, 0x7F00, 0x6000, 0x6000, 0x6000, 0x6000, 0x0000, 0x0000}},
    {0x2127, {0x0000, 0x7FFE, 0x7FFE, 0x1818, 0x381C, 0x300C, 0x6006, 0x6006,
              0x6006, 0x6006, 0x300C, 0x381C, 0x1FF8, 0x07E0, 0x0000, 0x0000}},
    {0x2202, {0x0000, 0x0780, 0x0FC0, 0x1860, 0x0060, 0x0F60, 0x1FF0, 0x3078,
              0x6038, 0x6038, 0x3070, 0x1FE0, 0x0F80, 0x0000, 0x0000, 0x0000}},
    {0x2206, {0x0000, 0x0180, 0x03C0, 0x0660, 0x0660, 0x0C30, 0x0C30, 0x1818,
              0x1818, 0x300C, 0x300C, 0x6006, 0x6006, 0x7FFE, 0x7FFE, 0x0000}},
    {0x2211, {0x0000, 0x7FFE, 0x7FFE, 0x6000, 0x3000, 0x1800, 0x0C00, 0x0600,
              0x0300, 0x0600, 0x0C00, 0x1800, 0x3000, 0x7FFE, 0x7FFE, 0x0000}},
    {0x2212, {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x7FFE, 0x7FFE,
              0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000}},
    {0x221A, {0x0000, 0x0000, 0x000E, 0x001C, 0x0038, 0x0070, 0x60E0, 0x71C0,
              0x3B80, 0x1F00, 0x0E00, 0x0C00, 0x0C00, 0x0FFE, 0x0FFE, 0x0000}},
    {0x221D, {0x0000, 0x0000, 0x0000, 0x0F70, 0x1FF8, 0x38DC, 0x306C, 0x306C,
              0x38DC, 0x1FF8, 0x0F70, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000}},
    {0x221E, {0x0000, 0x0000, 0x0000, 0x0F78, 0x1FFC, 0x38CE, 0x3066, 0x3066,
              0x38CE, 0x1FFC, 0x0F78, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000}},
    {0x2220, {0x0000, 0x0000, 0x0006, 0x000C, 0x0018, 0x0030, 0x0060, 0x00C0,
              0x0180, 0x0300, 0x0600, 0x0C00, 0x1FFE, 0x1FFE, 0x0000, 0x0000}},
    {0x222B, {0x00F0, 0x01F8, 0x0318, 0x0300, 0x0300, 0x0180, 0x0180, 0x0180,
              0x0180, 0x00C0, 0x00C0, 0x00C0, 0x18C0, 0x1F80, 0x0F00, 0x0000}},
    {0x2234, {0x0000, 0x0000, 0x0180, 0x0180, 0x0000, 0x0000, 0x0000, 0x0C30,
              0x0C30, 0x0000, 0x0000, 0x0180, 0x0180, 0x0000, 0x0000, 0x0000}},
    {0x223F, {0x0000, 0x0000, 0x0F00, 0x19C0, 0x3060, 0x6030, 0x6030, 0x3060,
              0x19C0, 0x0F00, 0x0180, 0x0300, 0x0600, 0x0C00, 0x0000, 0x0000}},
    {0x2248, {0x0000, 0x0000, 0x0000, 0x1E0C, 0x3F1E, 0x7336, 0x61E6, 0x0000,
              0x1E0C, 0x3F1E, 0x7336, 0x61E6, 0x0000, 0x0000, 0x0000, 0x0000}},
    {0x2260, {0x0000, 0x0000, 0x000C, 0x0018, 0x7FFE, 0x7FFE, 0x0060, 0x00C0,
              0x0180, 0x0300, 0x7FFE, 0x7FFE, 0x0C00, 0x1800, 0x0000, 0x0000}},
    {0x2264, {0x0000, 0x0000, 0x001E, 0x00F0, 0x0780, 0x3C00, 0xF000, 0x3C00,
              0x0780, 0x00F0, 0x001E, 0x0000, 0x7FFE, 0x7FFE, 0x0000, 0x0000}},
    {0x2265, {0x0000, 0x0000, 0xF000, 0x3C00, 0x0780, 0x00F0, 0x001E, 0x00F0,
              0x0780, 0x3C00, 0xF000, 0x0000, 0x7FFE, 0x7FFE, 0x0000, 0x0000}},
    {0x2300, {0x0000, 0x07E6, 0x1FFC, 0x3838, 0x3070, 0x60E6, 0x61C6, 0x6386,
              0x6706, 0x6E06, 0x3C0C, 0x381C, 0x3FF8, 0x67E0, 0x0000, 0x0000}},
    {0x2393, {0x0000, 0x0000, 0x7FFE, 0x7FFE, 0x0000, 0x0000, 0x6666, 0x6666,
              0x0000, 0x0000, 0x7FFE, 0x7FFE, 0x0000, 0x0000, 0x0000, 0x0000}},
    {0x23DA, {0x0000, 0x0180, 0x0180, 0x0180, 0x0180, 0x0180, 0x7FFE, 0x3FFC,
              0x0000, 0x1FF8, 0x0000, 0x0FF0, 0x0000, 0x03C0, 0x0000, 0x0000}},
    {0x23E6, {0x0000, 0x7FFE, 0x0000, 0x0F00, 0x19C0, 0x3060, 0x6030, 0x6030,
              0x3060, 0x19C0, 0x0F00, 0x0000, 0x7FFE, 0x0000, 0x0000, 0x0000}},
    {0x26A1, {0x0180, 0x0380, 0x0700, 0x0E00, 0x1FFC, 0x3FF8, 0x0300, 0x0600,
              0x0C00, 0x1800, 0x3FFE, 0x7FFC, 0x00C0, 0x0180, 0x0000, 0x0000}},
};

void ui_font_set_active_path(const char *path)
{
    if (!path || path[0] == '\0') {
        s_active_font_path[0] = '\0';
        return;
    }
    snprintf(s_active_font_path, sizeof(s_active_font_path), "%s", path);
}

const char *ui_font_get_active_path(void)
{
    return s_active_font_path;
}

static bool utf8_cont(uint8_t c)
{
    return (c & 0xC0) == 0x80;
}

static uint32_t decode_utf8(const char **pp)
{
    const uint8_t *p = (const uint8_t *)*pp;
    uint32_t cp = '?';
    if (p[0] < 0x80) {
        cp = p[0];
        *pp += 1;
    } else if ((p[0] & 0xE0) == 0xC0) {
        if (p[1] && utf8_cont(p[1])) {
            cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            *pp += 2;
        } else {
            *pp += 1;
        }
    } else if ((p[0] & 0xF0) == 0xE0) {
        if (p[1] && utf8_cont(p[1]) && p[2] && utf8_cont(p[2])) {
            cp = ((uint32_t)(p[0] & 0x0F) << 12) |
                 ((uint32_t)(p[1] & 0x3F) << 6) |
                 (p[2] & 0x3F);
            *pp += 3;
        } else {
            *pp += 1;
        }
    } else if ((p[0] & 0xF8) == 0xF0) {
        if (p[1] && utf8_cont(p[1]) && p[2] && utf8_cont(p[2]) &&
            p[3] && utf8_cont(p[3])) {
            cp = ((uint32_t)(p[0] & 0x07) << 18) |
                 ((uint32_t)(p[1] & 0x3F) << 12) |
                 ((uint32_t)(p[2] & 0x3F) << 6) |
                 (p[3] & 0x3F);
            *pp += 4;
        } else {
            *pp += 1;
        }
    } else {
        *pp += 1;
    }
    if (cp == 0x3000) {
        return ' ';
    }
    if (cp >= 0xFF01 && cp <= 0xFF5E) {
        return cp - 0xFEE0;
    }
    if (cp == 0x00B5) {
        return 0x03BC;
    }
    if (cp == 0x00F8) {
        return 0x00D8;
    }
    if (cp == 0x2126) {
        return 0x03A9;
    }
    if (cp == 0x03D5 || cp == 0x03D6) {
        return 0x03C6;
    }
    if (cp == 0x03D1 || cp == 0x03F4) {
        return 0x03B8;
    }
    if (cp == 0x2205) {
        return 0x2300;
    }
    if (cp == 0x223C) {
        return 0x223F;
    }
    return cp;
}

static const uint16_t *find_symbol_glyph(uint32_t cp)
{
    for (int i = 0; i < (int)(sizeof(s_symbol_glyphs) / sizeof(s_symbol_glyphs[0])); i++) {
        if (s_symbol_glyphs[i].cp == cp) {
            return s_symbol_glyphs[i].rows;
        }
    }
    return NULL;
}

static const uint8_t *find_font12_cjk_glyph(uint32_t cp)
{
    if (cp < UI_FONT12_CJK_START || cp > UI_FONT12_CJK_END) {
        return NULL;
    }
    return ui_font12_cjk[cp - UI_FONT12_CJK_START];
}

static const uint8_t *find_font12_symbol_glyph(uint32_t cp)
{
    for (int i = 0; i < UI_FONT12_SYMBOL_COUNT; i++) {
        if (ui_font12_symbols[i].cp == cp) {
            return ui_font12_symbols[i].bmp;
        }
    }
    return NULL;
}

static const uint8_t *find_font8_symbol_glyph(uint32_t cp)
{
    for (int i = 0; i < UI_FONT8_SYMBOL_COUNT; i++) {
        if (ui_font8_symbols[i].cp == cp) {
            return ui_font8_symbols[i].bmp;
        }
    }
    return NULL;
}

static const uint8_t *find_zh_glyph(uint32_t cp)
{
    int lo = 0;
    int hi = CAL_FONT_ZH_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cal_font_zh[mid].cp == cp) {
            return cal_font_zh[mid].bmp;
        }
        if (cal_font_zh[mid].cp < cp) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

static bool glyph_empty16(const uint8_t *glyph)
{
    if (!glyph) {
        return true;
    }
    for (int i = 0; i < 32; i++) {
        if (glyph[i] != 0) {
            return false;
        }
    }
    return true;
}

static bool glyph_empty12(const uint8_t *glyph)
{
    if (!glyph) {
        return true;
    }
    for (int i = 0; i < 24; i++) {
        if (glyph[i] != 0) {
            return false;
        }
    }
    return true;
}

static bool glyph_empty8(const uint8_t *glyph)
{
    if (!glyph) {
        return true;
    }
    for (int i = 0; i < 8; i++) {
        if (glyph[i] != 0) {
            return false;
        }
    }
    return true;
}

static int glyph_advance(uint32_t cp, uint8_t scale)
{
    if (scale == 0) {
        scale = 1;
    }
    if (cp == 0x00B0 || cp == 0x00B2 || cp == 0x00B3 || cp == 0x207B) {
        return 8 * scale;
    }
    return (cp < 0x80 ? 8 : 16) * scale;
}

static int glyph_advance_small(uint32_t cp)
{
    return cp < 0x80 ? UI_FONT12_ASCII_ADV : UI_FONT12_CJK_ADV;
}

static int glyph_advance_tiny(uint32_t cp)
{
    return cp < 0x80 ? UI_FONT8_ASCII_ADV : UI_FONT8_SYMBOL_ADV;
}

static void draw_ascii(uint16_t *buf, int bw, int bh, int x, int y,
                       uint32_t cp, uint8_t scale, uint16_t fg)
{
    int idx = (int)cp - 32;
    if (idx < 0 || idx >= 95) {
        idx = 0;
    }
    const uint8_t *glyph = cal_font_ascii[idx];
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if ((bits & (0x80 >> col)) == 0) {
                continue;
            }
            int px = x + col * scale;
            int py = y + row * scale;
            for (int sy = 0; sy < scale; sy++) {
                int yy = py + sy;
                if (yy < 0 || yy >= bh) {
                    continue;
                }
                for (int sx = 0; sx < scale; sx++) {
                    int xx = px + sx;
                    if (xx >= 0 && xx < bw) {
                        buf[yy * bw + xx] = fg;
                    }
                }
            }
        }
    }
}

static void draw_ascii_small(uint16_t *buf, int bw, int bh, int x, int y,
                             uint32_t cp, uint16_t fg)
{
    int idx = (int)cp - 32;
    if (idx < 0 || idx >= 95) {
        idx = 0;
    }
    const uint8_t *glyph = ui_font12_ascii[idx];
    for (int row = 0; row < UI_FONT12_H; row++) {
        int yy = y + row;
        if (yy < 0 || yy >= bh) {
            continue;
        }
        uint8_t bits = glyph[row];
        for (int col = 0; col < UI_FONT12_ASCII_W; col++) {
            if ((bits & (0x80 >> col)) == 0) {
                continue;
            }
            int xx = x + col;
            if (xx >= 0 && xx < bw) {
                buf[yy * bw + xx] = fg;
            }
        }
    }
}

static void draw_ascii_tiny(uint16_t *buf, int bw, int bh, int x, int y,
                            uint32_t cp, uint16_t fg)
{
    int idx = (int)cp - 32;
    if (idx < 0 || idx >= 95) {
        idx = 0;
    }
    const uint8_t *glyph = ui_font8_ascii[idx];
    for (int row = 0; row < UI_FONT8_H; row++) {
        int yy = y + row;
        if (yy < 0 || yy >= bh) {
            continue;
        }
        uint8_t bits = glyph[row];
        for (int col = 0; col < UI_FONT8_ASCII_W; col++) {
            if ((bits & (0x80 >> col)) == 0) {
                continue;
            }
            int xx = x + col;
            if (xx >= 0 && xx < bw) {
                buf[yy * bw + xx] = fg;
            }
        }
    }
}

static void draw_bitmap16(uint16_t *buf, int bw, int bh, int x, int y,
                          const uint8_t *glyph, uint8_t scale, uint16_t fg)
{
    for (int row = 0; row < 16; row++) {
        uint16_t bits = ((uint16_t)glyph[row * 2] << 8) | glyph[row * 2 + 1];
        for (int col = 0; col < 16; col++) {
            if ((bits & (0x8000 >> col)) == 0) {
                continue;
            }
            int px = x + col * scale;
            int py = y + row * scale;
            for (int sy = 0; sy < scale; sy++) {
                int yy = py + sy;
                if (yy < 0 || yy >= bh) {
                    continue;
                }
                for (int sx = 0; sx < scale; sx++) {
                    int xx = px + sx;
                    if (xx >= 0 && xx < bw) {
                        buf[yy * bw + xx] = fg;
                    }
                }
            }
        }
    }
}

static void draw_bitmap12(uint16_t *buf, int bw, int bh, int x, int y,
                          const uint8_t *glyph, uint16_t fg)
{
    for (int row = 0; row < UI_FONT12_H; row++) {
        uint16_t bits = ((uint16_t)glyph[row * 2] << 8) | glyph[row * 2 + 1];
        int yy = y + row;
        if (yy < 0 || yy >= bh) {
            continue;
        }
        for (int col = 0; col < 12; col++) {
            if ((bits & (0x8000 >> col)) == 0) {
                continue;
            }
            int xx = x + col;
            if (xx >= 0 && xx < bw) {
                buf[yy * bw + xx] = fg;
            }
        }
    }
}

static void draw_bitmap8(uint16_t *buf, int bw, int bh, int x, int y,
                         const uint8_t *glyph, uint16_t fg)
{
    for (int row = 0; row < UI_FONT8_H; row++) {
        uint8_t bits = glyph[row];
        int yy = y + row;
        if (yy < 0 || yy >= bh) {
            continue;
        }
        for (int col = 0; col < 8; col++) {
            if ((bits & (0x80 >> col)) == 0) {
                continue;
            }
            int xx = x + col;
            if (xx >= 0 && xx < bw) {
                buf[yy * bw + xx] = fg;
            }
        }
    }
}

static void draw_symbol16(uint16_t *buf, int bw, int bh, int x, int y,
                          const uint16_t *rows, uint8_t scale, uint16_t fg)
{
    for (int row = 0; row < 16; row++) {
        uint16_t bits = rows[row];
        for (int col = 0; col < 16; col++) {
            if ((bits & (0x8000 >> col)) == 0) {
                continue;
            }
            int px = x + col * scale;
            int py = y + row * scale;
            for (int sy = 0; sy < scale; sy++) {
                int yy = py + sy;
                if (yy < 0 || yy >= bh) {
                    continue;
                }
                for (int sx = 0; sx < scale; sx++) {
                    int xx = px + sx;
                    if (xx >= 0 && xx < bw) {
                        buf[yy * bw + xx] = fg;
                    }
                }
            }
        }
    }
}

static void draw_missing(uint16_t *buf, int bw, int bh, int x, int y,
                         uint8_t scale, uint16_t fg)
{
    int size = 16 * scale;
    for (int i = 0; i < size; i++) {
        int xx = x + i;
        int yy0 = y;
        int yy1 = y + size - 1;
        if (xx >= 0 && xx < bw) {
            if (yy0 >= 0 && yy0 < bh) {
                buf[yy0 * bw + xx] = fg;
            }
            if (yy1 >= 0 && yy1 < bh) {
                buf[yy1 * bw + xx] = fg;
            }
        }
        int yy = y + i;
        int xx1 = x + size - 1;
        if (yy >= 0 && yy < bh) {
            if (x >= 0 && x < bw) {
                buf[yy * bw + x] = fg;
            }
            if (xx1 >= 0 && xx1 < bw) {
                buf[yy * bw + xx1] = fg;
            }
        }
    }
}

static void draw_missing_small(uint16_t *buf, int bw, int bh, int x, int y,
                               uint16_t fg)
{
    int size = 12;
    for (int i = 0; i < size; i++) {
        int xx = x + i;
        int yy0 = y;
        int yy1 = y + size - 1;
        if (xx >= 0 && xx < bw) {
            if (yy0 >= 0 && yy0 < bh) {
                buf[yy0 * bw + xx] = fg;
            }
            if (yy1 >= 0 && yy1 < bh) {
                buf[yy1 * bw + xx] = fg;
            }
        }
        int yy = y + i;
        int xx1 = x + size - 1;
        if (yy >= 0 && yy < bh) {
            if (x >= 0 && x < bw) {
                buf[yy * bw + x] = fg;
            }
            if (xx1 >= 0 && xx1 < bw) {
                buf[yy * bw + xx1] = fg;
            }
        }
    }
}

static void draw_missing_tiny(uint16_t *buf, int bw, int bh, int x, int y,
                              uint16_t fg)
{
    int size = UI_FONT8_H;
    for (int i = 0; i < size; i++) {
        int xx = x + i;
        int yy0 = y;
        int yy1 = y + size - 1;
        if (xx >= 0 && xx < bw) {
            if (yy0 >= 0 && yy0 < bh) {
                buf[yy0 * bw + xx] = fg;
            }
            if (yy1 >= 0 && yy1 < bh) {
                buf[yy1 * bw + xx] = fg;
            }
        }
        int yy = y + i;
        int xx1 = x + size - 1;
        if (yy >= 0 && yy < bh) {
            if (x >= 0 && x < bw) {
                buf[yy * bw + x] = fg;
            }
            if (xx1 >= 0 && xx1 < bw) {
                buf[yy * bw + xx1] = fg;
            }
        }
    }
}

int ui_font_draw_text_maxw(uint16_t *buf, int bw, int bh, int x, int y,
                           const char *text, uint8_t scale, uint16_t fg,
                           int max_w)
{
    if (!buf || !text || bw <= 0 || bh <= 0 || max_w <= 0) {
        return 0;
    }
    if (scale == 0) {
        scale = 1;
    }
    int cx = x;
    int limit = x + max_w;
    while (*text && cx < limit) {
        const char *before = text;
        uint32_t cp = decode_utf8(&text);
        int adv = glyph_advance(cp, scale);
        if (cx + adv > limit) {
            text = before;
            break;
        }
        if (cp < 0x80) {
            draw_ascii(buf, bw, bh, cx, y, cp, scale, fg);
        } else {
            const uint16_t *symbol = find_symbol_glyph(cp);
            const uint8_t *glyph = symbol ? NULL : find_zh_glyph(cp);
            if (symbol) {
                draw_symbol16(buf, bw, bh, cx, y, symbol, scale, fg);
            } else if (!glyph_empty16(glyph)) {
                draw_bitmap16(buf, bw, bh, cx, y, glyph, scale, fg);
            } else {
                draw_missing(buf, bw, bh, cx, y, scale, fg);
            }
        }
        cx += adv;
    }
    return cx - x;
}

int ui_font_draw_text_small_maxw(uint16_t *buf, int bw, int bh, int x, int y,
                                 const char *text, uint16_t fg, int max_w)
{
    if (!buf || !text || bw <= 0 || bh <= 0 || max_w <= 0) {
        return 0;
    }
    int cx = x;
    int limit = x + max_w;
    while (*text && cx < limit) {
        const char *before = text;
        uint32_t cp = decode_utf8(&text);
        int adv = glyph_advance_small(cp);
        if (cx + adv > limit) {
            text = before;
            break;
        }
        if (cp < 0x80) {
            draw_ascii_small(buf, bw, bh, cx, y, cp, fg);
        } else {
            const uint8_t *glyph = find_font12_symbol_glyph(cp);
            if (!glyph) {
                glyph = find_font12_cjk_glyph(cp);
            }
            if (!glyph_empty12(glyph)) {
                draw_bitmap12(buf, bw, bh, cx, y, glyph, fg);
            } else {
                draw_missing_small(buf, bw, bh, cx, y, fg);
            }
        }
        cx += adv;
    }
    return cx - x;
}

int ui_font_draw_text_tiny_maxw(uint16_t *buf, int bw, int bh, int x, int y,
                                const char *text, uint16_t fg, int max_w)
{
    if (!buf || !text || bw <= 0 || bh <= 0 || max_w <= 0) {
        return 0;
    }
    int cx = x;
    int limit = x + max_w;
    while (*text && cx < limit) {
        const char *before = text;
        uint32_t cp = decode_utf8(&text);
        int adv = glyph_advance_tiny(cp);
        if (cx + adv > limit) {
            text = before;
            break;
        }
        if (cp < 0x80) {
            draw_ascii_tiny(buf, bw, bh, cx, y, cp, fg);
        } else {
            const uint8_t *glyph = find_font8_symbol_glyph(cp);
            if (!glyph_empty8(glyph)) {
                draw_bitmap8(buf, bw, bh, cx, y, glyph, fg);
            } else {
                draw_missing_tiny(buf, bw, bh, cx, y, fg);
            }
        }
        cx += adv;
    }
    return cx - x;
}
