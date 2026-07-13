#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool found;
    bool inverted;
    bool mirrored;
    int orientation;
    int width;
    int height;
    int elapsed_ms;
    char format[24];
    char error[64];
} zxing_qr_result_t;

esp_err_t zxing_qr_decode_luma(const uint8_t *luma,
                               int width,
                               int height,
                               int row_stride,
                               char *text,
                               size_t text_len,
                               zxing_qr_result_t *result);

esp_err_t zxing_qr_decode_luma_ex(const uint8_t *luma,
                                  int width,
                                  int height,
                                  int row_stride,
                                  char *text,
                                  size_t text_len,
                                  zxing_qr_result_t *result,
                                  bool robust_fallback);

#ifdef __cplusplus
}
#endif
