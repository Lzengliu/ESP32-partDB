#include "qr_scanner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_config.h"
#include "camera_mgr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "img_converters.h"
#include "nfc_service.h"
#include "quirc.h"
#include "zxing_qr_decoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "qr_scanner";
static qr_scanner_status_t s_status = {
    .last_err = ESP_ERR_INVALID_STATE,
};

#define QR_SCANNER_MAX_ATTEMPTS 2
#define QR_SCANNER_RETRY_DELAY_MS 120
#define QR_SCANNER_ENABLE_QUIRC_FALLBACK 1
#define QR_SCANNER_SAVE_DEBUG_PGM 0
#define QR_SCANNER_FAST_PIPELINE 1
#define QR_SCANNER_FAST_STRETCH_FALLBACK 0

static void copy_payload(char *dst, size_t dst_len, const uint8_t *src, size_t src_len)
{
    if (!dst || dst_len == 0) {
        return;
    }
    size_t n = src_len;
    if (n >= dst_len) {
        n = dst_len - 1;
    }
    if (src && n > 0) {
        memcpy(dst, src, n);
    }
    dst[n] = '\0';
}

static void copy_error_text(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strnlen(src, dst_len - 1);
    if (n > 0) {
        memcpy(dst, src, n);
    }
    dst[n] = '\0';
}

static void remember_result(const qr_scanner_result_t *result)
{
    if (!result) {
        return;
    }

    s_status.scan_count++;
    s_status.last_found = result->found;
    s_status.last_code_count = result->code_count;
    s_status.last_width = result->width;
    s_status.last_height = result->height;
    s_status.last_elapsed_ms = result->elapsed_ms;
    s_status.last_err = result->err;
    snprintf(s_status.last_decode_error, sizeof(s_status.last_decode_error), "%s",
             result->decode_error);
    snprintf(s_status.last_text, sizeof(s_status.last_text), "%s", result->text);
    if (result->found) {
        s_status.found_count++;
    }
}

static void *scanner_malloc(size_t size)
{
    if (size == 0) {
        return NULL;
    }
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return p;
}

static void scanner_yield(void)
{
    vTaskDelay(pdMS_TO_TICKS(1));
}

static void save_debug_pgm(const uint8_t *luma, uint16_t width, uint16_t height, int row_stride)
{
#if QR_SCANNER_SAVE_DEBUG_PGM
    if (!luma || width == 0 || height == 0 || row_stride < width) {
        return;
    }
    FILE *f = fopen(BOARD_SD_CACHE_DIR "/qr_last.pgm", "wb");
    if (!f) {
        return;
    }

    uint16_t scale = 1;
    while ((width / scale) > 240 || (height / scale) > 180) {
        scale++;
    }
    uint16_t out_w = width / scale;
    uint16_t out_h = height / scale;
    if (out_w == 0 || out_h == 0) {
        fclose(f);
        return;
    }

    fprintf(f, "P5\n%u %u\n255\n", (unsigned)out_w, (unsigned)out_h);
    uint8_t row[240];
    for (uint16_t y = 0; y < out_h; y++) {
        for (uint16_t x = 0; x < out_w; x++) {
            uint32_t sum = 0;
            uint32_t count = 0;
            uint16_t src_y0 = y * scale;
            uint16_t src_x0 = x * scale;
            for (uint16_t yy = 0; yy < scale && (src_y0 + yy) < height; yy++) {
                const uint8_t *src = luma + (size_t)(src_y0 + yy) * row_stride + src_x0;
                for (uint16_t xx = 0; xx < scale && (src_x0 + xx) < width; xx++) {
                    sum += src[xx];
                    count++;
                }
            }
            row[x] = count ? (uint8_t)(sum / count) : 0;
        }
        fwrite(row, 1, out_w, f);
    }
    fclose(f);
    ESP_LOGI(TAG, "saved QR debug frame %ux%u scale=%u to %s",
             (unsigned)out_w, (unsigned)out_h, (unsigned)scale,
             BOARD_SD_CACHE_DIR "/qr_last.pgm");
#else
    (void)luma;
    (void)width;
    (void)height;
    (void)row_stride;
#endif
}

static uint8_t histogram_percentile(const uint32_t hist[256], uint32_t pixels, uint32_t percent)
{
    if (!hist || pixels == 0) {
        return 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    uint32_t target = (pixels * percent + 99) / 100;
    if (target == 0) {
        target = 1;
    }
    uint32_t acc = 0;
    for (uint32_t i = 0; i < 256; i++) {
        acc += hist[i];
        if (acc >= target) {
            return (uint8_t)i;
        }
    }
    return 255;
}

#if !QR_SCANNER_FAST_PIPELINE || QR_SCANNER_FAST_STRETCH_FALLBACK
static bool build_contrast_stretch(const uint8_t *src, uint8_t *dst, size_t count,
                                   uint8_t low, uint8_t high)
{
    if (!src || !dst || count == 0 || high <= low ||
        ((uint32_t)high - (uint32_t)low) <= 10) {
        return false;
    }
    uint32_t range = (uint32_t)high - (uint32_t)low;
    for (size_t i = 0; i < count; i++) {
        uint8_t v = src[i];
        if (v <= low) {
            dst[i] = 0;
        } else if (v >= high) {
            dst[i] = 255;
        } else {
            dst[i] = (uint8_t)((((uint32_t)v - (uint32_t)low) * 255U + range / 2U) / range);
        }
    }
    return true;
}
#endif

static uint8_t otsu_threshold(const uint32_t hist[256], uint32_t pixels)
{
    if (!hist || pixels == 0) {
        return 127;
    }

    double sum = 0.0;
    for (uint32_t i = 0; i < 256; i++) {
        sum += (double)i * (double)hist[i];
    }

    double sum_b = 0.0;
    uint32_t w_b = 0;
    double max_between = -1.0;
    uint8_t threshold = 127;

    for (uint32_t i = 0; i < 256; i++) {
        w_b += hist[i];
        if (w_b == 0) {
            continue;
        }
        uint32_t w_f = pixels - w_b;
        if (w_f == 0) {
            break;
        }
        sum_b += (double)i * (double)hist[i];
        double mean_b = sum_b / (double)w_b;
        double mean_f = (sum - sum_b) / (double)w_f;
        double diff = mean_b - mean_f;
        double between = (double)w_b * (double)w_f * diff * diff;
        if (between > max_between) {
            max_between = between;
            threshold = (uint8_t)i;
        }
    }

    return threshold;
}

#if !QR_SCANNER_FAST_PIPELINE
static void build_binary_threshold(const uint8_t *src, uint8_t *dst, size_t count,
                                   uint8_t threshold)
{
    if (!src || !dst) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        dst[i] = src[i] > threshold ? 255 : 0;
    }
}

static void build_block_threshold(const uint8_t *src, uint8_t *dst,
                                  uint16_t width, uint16_t height, int row_stride,
                                  uint8_t global_threshold)
{
    if (!src || !dst || width == 0 || height == 0 || row_stride < width) {
        return;
    }

    const int block = 32;
    const int bias = 7;
    for (int by = 0; by < height; by += block) {
        int bh = height - by;
        if (bh > block) {
            bh = block;
        }
        for (int bx = 0; bx < width; bx += block) {
            int bw = width - bx;
            if (bw > block) {
                bw = block;
            }

            uint32_t sum = 0;
            uint32_t count = 0;
            for (int y = 0; y < bh; y++) {
                const uint8_t *row = src + (size_t)(by + y) * row_stride + bx;
                for (int x = 0; x < bw; x++) {
                    sum += row[x];
                }
                count += (uint32_t)bw;
            }
            uint8_t mean = count ? (uint8_t)(sum / count) : global_threshold;
            uint8_t threshold = mean > bias ? (uint8_t)(mean - bias) : mean;
            threshold = (uint8_t)(((uint16_t)threshold + (uint16_t)global_threshold) / 2U);

            for (int y = 0; y < bh; y++) {
                const uint8_t *in = src + (size_t)(by + y) * row_stride + bx;
                uint8_t *out = dst + (size_t)(by + y) * width + bx;
                for (int x = 0; x < bw; x++) {
                    out[x] = in[x] > threshold ? 255 : 0;
                }
            }
        }
    }
}
#endif

#if QR_SCANNER_ENABLE_QUIRC_FALLBACK
static esp_err_t quirc_decode_attempt(const char *label, const uint8_t *luma,
                                      uint16_t width, uint16_t height, int row_stride,
                                      qr_scanner_result_t *result, bool half_scale)
{
    if (!label) {
        label = half_scale ? "half" : "full";
    }
    if (!luma || !result || width == 0 || height == 0 || row_stride < width) {
        return ESP_ERR_INVALID_ARG;
    }

    int q_w = half_scale ? (int)width / 2 : (int)width;
    int q_h = half_scale ? (int)height / 2 : (int)height;
    if (q_w <= 0 || q_h <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    int64_t start_us = esp_timer_get_time();
    struct quirc *qr = quirc_new();
    struct quirc_code *code = NULL;
    struct quirc_data *data = NULL;
    esp_err_t err = ESP_ERR_NOT_FOUND;
    const char *last_error = "no_qr";

    if (!qr) {
        err = ESP_ERR_NO_MEM;
        last_error = "quirc_new";
        goto done;
    }
    if (quirc_resize(qr, q_w, q_h) < 0) {
        err = ESP_ERR_NO_MEM;
        last_error = "quirc_resize";
        goto done;
    }

    int img_w = 0;
    int img_h = 0;
    uint8_t *image = quirc_begin(qr, &img_w, &img_h);
    if (!image || img_w != q_w || img_h != q_h) {
        err = ESP_ERR_INVALID_SIZE;
        last_error = "quirc_begin";
        goto done;
    }

    if (half_scale) {
        for (int y = 0; y < q_h; y++) {
            const uint8_t *src = luma + (size_t)(y * 2) * row_stride;
            uint8_t *dst = image + (size_t)y * q_w;
            for (int x = 0; x < q_w; x++) {
                dst[x] = src[x * 2];
            }
        }
    } else {
        for (int y = 0; y < q_h; y++) {
            memcpy(image + (size_t)y * q_w, luma + (size_t)y * row_stride, (size_t)q_w);
        }
    }
    quirc_end(qr);

    result->code_count = quirc_count(qr);
    if (result->code_count <= 0) {
        err = ESP_ERR_NOT_FOUND;
        last_error = "no_qr";
        goto done;
    }

    code = scanner_malloc(sizeof(*code));
    data = scanner_malloc(sizeof(*data));
    if (!code || !data) {
        err = ESP_ERR_NO_MEM;
        last_error = "quirc_decode_alloc";
        goto done;
    }

    for (int i = 0; i < result->code_count; i++) {
        quirc_extract(qr, i, code);
        quirc_decode_error_t dec_err = quirc_decode(code, data);
        if (dec_err == QUIRC_ERROR_DATA_ECC) {
            quirc_flip(code);
            dec_err = quirc_decode(code, data);
        }
        if (dec_err == QUIRC_SUCCESS) {
            copy_payload(result->text, sizeof(result->text), data->payload, data->payload_len);
            result->found = true;
            result->err = ESP_OK;
            result->decode_error[0] = '\0';
            err = ESP_OK;
            last_error = "";
            break;
        }
        last_error = quirc_strerror(dec_err);
        err = ESP_ERR_NOT_FOUND;
    }

done:
    if (err != ESP_OK) {
        snprintf(result->decode_error, sizeof(result->decode_error), "%s",
                 last_error ? last_error : esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "quirc %s decode err=%s found=%d count=%d size=%dx%d ms=%lld detail=%s",
             label, esp_err_to_name(err), result->found, result->code_count,
             q_w, q_h, (long long)((esp_timer_get_time() - start_us) / 1000),
             result->decode_error[0] ? result->decode_error : "-");
    scanner_yield();
    if (code) {
        heap_caps_free(code);
    }
    if (data) {
        heap_caps_free(data);
    }
    if (qr) {
        quirc_destroy(qr);
    }
    return err;
}
#endif

static inline uint16_t normalize_rgb565(uint16_t pixel)
{
#if BOARD_CAMERA_PREVIEW_SWAP_BYTES
    pixel = (uint16_t)((pixel >> 8) | (pixel << 8));
#endif
#if BOARD_CAMERA_PREVIEW_SWAP_RB
    pixel = (uint16_t)(((pixel & 0xF800) >> 11) |
                       (pixel & 0x07E0) |
                       ((pixel & 0x001F) << 11));
#endif
    return pixel;
}

static inline uint8_t rgb565_luma(uint16_t pixel)
{
    pixel = normalize_rgb565(pixel);
    uint8_t r = (uint8_t)(((pixel >> 11) & 0x1f) << 3);
    uint8_t g = (uint8_t)(((pixel >> 5) & 0x3f) << 2);
    uint8_t b = (uint8_t)((pixel & 0x1f) << 3);
    return (uint8_t)(((uint16_t)r * 77 + (uint16_t)g * 150 + (uint16_t)b * 29) >> 8);
}

static inline uint8_t rgb565_luma_raw(uint16_t pixel)
{
    uint8_t r = (uint8_t)(((pixel >> 11) & 0x1f) << 3);
    uint8_t g = (uint8_t)(((pixel >> 5) & 0x3f) << 2);
    uint8_t b = (uint8_t)((pixel & 0x1f) << 3);
    return (uint8_t)(((uint16_t)r * 77 + (uint16_t)g * 150 + (uint16_t)b * 29) >> 8);
}

typedef uint8_t (*luma_reader_t)(const void *pixels, size_t index);

static uint8_t rgb565_luma_reader(const void *pixels, size_t index)
{
    const uint16_t *rgb565 = (const uint16_t *)pixels;
    return rgb565_luma(rgb565[index]);
}

static uint8_t rgb565_luma_raw_reader(const void *pixels, size_t index)
{
    const uint16_t *rgb565 = (const uint16_t *)pixels;
    return rgb565_luma_raw(rgb565[index]);
}

static uint8_t grayscale_luma_reader(const void *pixels, size_t index)
{
    const uint8_t *gray = (const uint8_t *)pixels;
    return gray[index];
}

static uint8_t rgb888_luma_reader(const void *pixels, size_t index)
{
    const uint8_t *rgb = (const uint8_t *)pixels + (index * 3);
    return (uint8_t)(((uint16_t)rgb[0] * 77 +
                      (uint16_t)rgb[1] * 150 +
                      (uint16_t)rgb[2] * 29) >> 8);
}

static esp_err_t finish_result(qr_scanner_result_t *result, int64_t start_us, bool remember)
{
    result->elapsed_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
    if (result->err != ESP_OK && !result->decode_error[0]) {
        snprintf(result->decode_error, sizeof(result->decode_error), "%s",
                 esp_err_to_name(result->err));
    }
    if (remember) {
        remember_result(result);
    }
    ESP_LOGI(TAG, "decode err=%s found=%d count=%d size=%dx%d ms=%lu text=%s",
             esp_err_to_name(result->err), result->found, result->code_count,
             result->width, result->height, (unsigned long)result->elapsed_ms,
             result->found ? result->text : "-");
    return result->err;
}

static esp_err_t zxing_decode_attempt(const char *label, const uint8_t *luma,
                                      uint16_t width, uint16_t height, int row_stride,
                                      qr_scanner_result_t *result,
                                      zxing_qr_result_t *zxing,
                                      bool robust_fallback)
{
    if (!label) {
        label = "full";
    }
    memset(zxing, 0, sizeof(*zxing));
    esp_err_t err = zxing_qr_decode_luma_ex(luma, width, height, row_stride,
                                            result->text, sizeof(result->text),
                                            zxing, robust_fallback);
    ESP_LOGI(TAG,
             "zxing %s decode err=%s found=%d format=%s orient=%d inv=%d mir=%d ms=%d detail=%s",
             label, esp_err_to_name(err), zxing->found,
             zxing->format[0] ? zxing->format : "-",
             zxing->orientation, zxing->inverted, zxing->mirrored,
             zxing->elapsed_ms, zxing->error[0] ? zxing->error : "-");
    scanner_yield();
    if (err == ESP_OK && result->text[0]) {
        result->found = true;
        result->code_count = 1;
        result->err = ESP_OK;
        result->decode_error[0] = '\0';
    }
    return err;
}

static esp_err_t decode_luma_internal(const void *pixels, uint16_t width,
                                      uint16_t height, qr_scanner_result_t *result,
                                      int64_t start_us, luma_reader_t luma_reader,
                                      bool remember)
{
    uint8_t *luma_buf = NULL;

    if (!result) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));
    result->width = width;
    result->height = height;
    result->err = ESP_FAIL;

    if (!pixels || !luma_reader || width == 0 || height == 0) {
        result->err = ESP_ERR_INVALID_ARG;
        snprintf(result->decode_error, sizeof(result->decode_error), "invalid_frame");
        goto done_no_qr;
    }

    uint8_t min_luma = 255;
    uint8_t max_luma = 0;
    uint64_t sum_luma = 0;
    uint32_t pixels_count = (uint32_t)width * (uint32_t)height;
    uint32_t hist[256] = {0};
    luma_buf = scanner_malloc((size_t)pixels_count);
    if (!luma_buf) {
        result->err = ESP_ERR_NO_MEM;
        snprintf(result->decode_error, sizeof(result->decode_error), "luma_alloc");
        goto done_no_qr;
    }
    for (uint16_t y = 0; y < height; y++) {
        for (uint16_t x = 0; x < width; x++) {
            size_t src_index = (size_t)y * width + x;
            uint8_t luma = luma_reader(pixels, src_index);
            luma_buf[src_index] = luma;
            if (luma < min_luma) {
                min_luma = luma;
            }
            if (luma > max_luma) {
                max_luma = luma;
            }
            sum_luma += luma;
            hist[luma]++;
        }
    }
    uint8_t p02 = histogram_percentile(hist, pixels_count, 2);
    uint8_t p98 = histogram_percentile(hist, pixels_count, 98);
    uint8_t p05 = histogram_percentile(hist, pixels_count, 5);
    uint8_t p95 = histogram_percentile(hist, pixels_count, 95);
    uint8_t otsu = otsu_threshold(hist, pixels_count);
    ESP_LOGI(TAG, "luma stats min=%u max=%u mean=%u p02=%u p98=%u p05=%u p95=%u spread=%u otsu=%u",
             min_luma, max_luma,
             pixels_count ? (unsigned)(sum_luma / pixels_count) : 0,
             p02, p98, p05, p95, (unsigned)(p95 - p05), otsu);
    save_debug_pgm(luma_buf, width, height, width);

#if QR_SCANNER_FAST_PIPELINE
    zxing_qr_result_t zxing = {0};
    esp_err_t last_err = ESP_ERR_NOT_FOUND;
    char last_error[sizeof(result->decode_error)] = {0};
    copy_error_text(last_error, sizeof(last_error), "no_qr");
    bool quirc_saw_code = false;

    esp_err_t zxing_err = zxing_decode_attempt("fast", luma_buf, width, height, width,
                                               result, &zxing, false);
    if (result->found) {
        heap_caps_free(luma_buf);
        return finish_result(result, start_us, remember);
    }
    last_err = zxing_err == ESP_OK ? ESP_ERR_NOT_FOUND : zxing_err;
    copy_error_text(last_error, sizeof(last_error),
                    zxing.error[0] ? zxing.error : "no_qr");

#if QR_SCANNER_ENABLE_QUIRC_FALLBACK
    esp_err_t quirc_err = quirc_decode_attempt("fast", luma_buf, width, height, width,
                                               result, false);
    if (result->found) {
        heap_caps_free(luma_buf);
        return finish_result(result, start_us, remember);
    }
    quirc_saw_code = result->code_count > 0;
    if (quirc_err != ESP_ERR_NOT_FOUND || quirc_saw_code) {
        last_err = quirc_err == ESP_OK ? ESP_ERR_NOT_FOUND : quirc_err;
        copy_error_text(last_error, sizeof(last_error),
                        result->decode_error[0] ? result->decode_error : esp_err_to_name(last_err));
    }

#endif

#if QR_SCANNER_FAST_STRETCH_FALLBACK
    if (!quirc_saw_code) {
        uint8_t *work_buf = scanner_malloc((size_t)pixels_count);
        if (work_buf && build_contrast_stretch(luma_buf, work_buf, (size_t)pixels_count, p02, p98)) {
            zxing_err = zxing_decode_attempt("fast-stretch", work_buf, width, height, width,
                                             result, &zxing, false);
            if (result->found) {
                heap_caps_free(work_buf);
                heap_caps_free(luma_buf);
                return finish_result(result, start_us, remember);
            }
            if (zxing_err != ESP_ERR_NOT_FOUND || !last_error[0]) {
                last_err = zxing_err == ESP_OK ? ESP_ERR_NOT_FOUND : zxing_err;
                copy_error_text(last_error, sizeof(last_error),
                                zxing.error[0] ? zxing.error : "stretch_no_qr");
            }
        }
        if (work_buf) {
            heap_caps_free(work_buf);
        }
    }
#endif

    result->err = last_err == ESP_OK ? ESP_ERR_NOT_FOUND : last_err;
    snprintf(result->decode_error, sizeof(result->decode_error), "%.*s",
             (int)sizeof(result->decode_error) - 1,
             last_error[0] ? last_error : "no_qr");
    heap_caps_free(luma_buf);
    return finish_result(result, start_us, remember);
#else
    zxing_qr_result_t zxing = {0};
    esp_err_t zxing_err = zxing_decode_attempt("full", luma_buf, width, height, width,
                                               result, &zxing, true);
    if (result->found) {
        heap_caps_free(luma_buf);
        return finish_result(result, start_us, remember);
    }

    esp_err_t last_err = zxing_err == ESP_OK ? ESP_ERR_NOT_FOUND : zxing_err;
    char last_error[sizeof(result->decode_error)] = {0};
    copy_error_text(last_error, sizeof(last_error), zxing.error[0] ? zxing.error : "no_qr");

    uint8_t *work_buf = scanner_malloc((size_t)pixels_count);
    if (work_buf) {
        if (build_contrast_stretch(luma_buf, work_buf, (size_t)pixels_count, p02, p98)) {
            zxing_err = zxing_decode_attempt("stretch", work_buf, width, height, width,
                                             result, &zxing, true);
            if (result->found) {
                heap_caps_free(work_buf);
                heap_caps_free(luma_buf);
                return finish_result(result, start_us, remember);
            }
            if (zxing_err != ESP_ERR_NOT_FOUND || !last_error[0]) {
                last_err = zxing_err == ESP_OK ? ESP_ERR_NOT_FOUND : zxing_err;
                copy_error_text(last_error, sizeof(last_error),
                                zxing.error[0] ? zxing.error : "stretch_no_qr");
            }
        }

        build_binary_threshold(luma_buf, work_buf, (size_t)pixels_count, otsu);
        zxing_err = zxing_decode_attempt("binary", work_buf, width, height, width,
                                         result, &zxing, true);
        if (result->found) {
            heap_caps_free(work_buf);
            heap_caps_free(luma_buf);
            return finish_result(result, start_us, remember);
        }
        if (zxing_err != ESP_ERR_NOT_FOUND || !last_error[0]) {
            last_err = zxing_err == ESP_OK ? ESP_ERR_NOT_FOUND : zxing_err;
            copy_error_text(last_error, sizeof(last_error),
                            zxing.error[0] ? zxing.error : "binary_no_qr");
        }
    } else {
        ESP_LOGW(TAG, "preprocess buffer alloc failed, using raw decode only");
        last_err = ESP_ERR_NO_MEM;
        copy_error_text(last_error, sizeof(last_error), "preprocess_alloc");
    }

#if QR_SCANNER_ENABLE_QUIRC_FALLBACK
    esp_err_t quirc_err = quirc_decode_attempt("full", luma_buf, width, height, width,
                                               result, false);
    if (result->found) {
        if (work_buf) {
            heap_caps_free(work_buf);
        }
        heap_caps_free(luma_buf);
        return finish_result(result, start_us, remember);
    }
    if (quirc_err != ESP_ERR_NOT_FOUND || result->code_count > 0) {
        last_err = quirc_err == ESP_OK ? ESP_ERR_NOT_FOUND : quirc_err;
        copy_error_text(last_error, sizeof(last_error),
                        result->decode_error[0] ? result->decode_error : esp_err_to_name(last_err));
    }
    quirc_err = quirc_decode_attempt("half", luma_buf, width, height, width,
                                     result, true);
    if (result->found) {
        if (work_buf) {
            heap_caps_free(work_buf);
        }
        heap_caps_free(luma_buf);
        return finish_result(result, start_us, remember);
    }
    if (quirc_err != ESP_ERR_NOT_FOUND || result->code_count > 0) {
        last_err = quirc_err == ESP_OK ? ESP_ERR_NOT_FOUND : quirc_err;
        copy_error_text(last_error, sizeof(last_error),
                        result->decode_error[0] ? result->decode_error : esp_err_to_name(last_err));
    }
    if (work_buf) {
        quirc_err = quirc_decode_attempt("binary-full", work_buf, width, height, width,
                                         result, false);
        if (result->found) {
            heap_caps_free(work_buf);
            heap_caps_free(luma_buf);
            return finish_result(result, start_us, remember);
        }
        if (quirc_err != ESP_ERR_NOT_FOUND || result->code_count > 0) {
            last_err = quirc_err == ESP_OK ? ESP_ERR_NOT_FOUND : quirc_err;
            copy_error_text(last_error, sizeof(last_error),
                            result->decode_error[0] ? result->decode_error : esp_err_to_name(last_err));
        }

        quirc_err = quirc_decode_attempt("binary-half", work_buf, width, height, width,
                                         result, true);
        if (result->found) {
            heap_caps_free(work_buf);
            heap_caps_free(luma_buf);
            return finish_result(result, start_us, remember);
        }
        if (quirc_err != ESP_ERR_NOT_FOUND || result->code_count > 0) {
            last_err = quirc_err == ESP_OK ? ESP_ERR_NOT_FOUND : quirc_err;
            copy_error_text(last_error, sizeof(last_error),
                            result->decode_error[0] ? result->decode_error : esp_err_to_name(last_err));
        }

        build_block_threshold(luma_buf, work_buf, width, height, width, otsu);
        zxing_err = zxing_decode_attempt("block", work_buf, width, height, width,
                                         result, &zxing, true);
        if (result->found) {
            heap_caps_free(work_buf);
            heap_caps_free(luma_buf);
            return finish_result(result, start_us, remember);
        }
        if (zxing_err != ESP_ERR_NOT_FOUND || !last_error[0]) {
            last_err = zxing_err == ESP_OK ? ESP_ERR_NOT_FOUND : zxing_err;
            copy_error_text(last_error, sizeof(last_error),
                            zxing.error[0] ? zxing.error : "block_no_qr");
        }

        quirc_err = quirc_decode_attempt("block-full", work_buf, width, height, width,
                                         result, false);
        if (result->found) {
            heap_caps_free(work_buf);
            heap_caps_free(luma_buf);
            return finish_result(result, start_us, remember);
        }
        if (quirc_err != ESP_ERR_NOT_FOUND || result->code_count > 0) {
            last_err = quirc_err == ESP_OK ? ESP_ERR_NOT_FOUND : quirc_err;
            copy_error_text(last_error, sizeof(last_error),
                            result->decode_error[0] ? result->decode_error : esp_err_to_name(last_err));
        }

        quirc_err = quirc_decode_attempt("block-half", work_buf, width, height, width,
                                         result, true);
        if (result->found) {
            heap_caps_free(work_buf);
            heap_caps_free(luma_buf);
            return finish_result(result, start_us, remember);
        }
        if (quirc_err != ESP_ERR_NOT_FOUND || result->code_count > 0) {
            last_err = quirc_err == ESP_OK ? ESP_ERR_NOT_FOUND : quirc_err;
            copy_error_text(last_error, sizeof(last_error),
                            result->decode_error[0] ? result->decode_error : esp_err_to_name(last_err));
        }
    }
#endif

    result->err = last_err == ESP_OK ? ESP_ERR_NOT_FOUND : last_err;
    snprintf(result->decode_error, sizeof(result->decode_error), "%.*s",
             (int)sizeof(result->decode_error) - 1,
             last_error[0] ? last_error : "no_qr");
    if (work_buf) {
        heap_caps_free(work_buf);
    }
    heap_caps_free(luma_buf);
    return finish_result(result, start_us, remember);
#endif

done_no_qr:
    if (luma_buf) {
        heap_caps_free(luma_buf);
    }
    return finish_result(result, start_us, remember);
}

static esp_err_t decode_rgb565_internal(const uint16_t *rgb565, uint16_t width,
                                        uint16_t height, qr_scanner_result_t *result,
                                        int64_t start_us, bool remember)
{
    qr_scanner_result_t normal = {0};
    esp_err_t err = decode_luma_internal(rgb565, width, height, &normal, start_us,
                                         rgb565_luma_reader, false);
    if (err == ESP_OK && normal.found) {
        *result = normal;
        if (remember) {
            remember_result(result);
        }
        return err;
    }

    ESP_LOGI(TAG, "retrying RGB565 decode with raw byte order after %s",
             normal.decode_error[0] ? normal.decode_error : esp_err_to_name(err));
    qr_scanner_result_t raw = {0};
    esp_err_t raw_err = decode_luma_internal(rgb565, width, height, &raw, start_us,
                                             rgb565_luma_raw_reader, false);
    if (raw_err == ESP_OK && raw.found) {
        ESP_LOGI(TAG, "RGB565 decode succeeded with raw byte order");
        *result = raw;
        if (remember) {
            remember_result(result);
        }
        return raw_err;
    }

    *result = raw.code_count > normal.code_count ? raw : normal;
    if (remember) {
        remember_result(result);
    }
    return result->err;
}

static esp_err_t decode_grayscale_internal(const uint8_t *gray, uint16_t width,
                                           uint16_t height, qr_scanner_result_t *result,
                                           int64_t start_us, bool remember)
{
    return decode_luma_internal(gray, width, height, result, start_us,
                                grayscale_luma_reader, remember);
}

static esp_err_t decode_rgb888_internal(const uint8_t *rgb888, uint16_t width,
                                        uint16_t height, qr_scanner_result_t *result,
                                        int64_t start_us, bool remember)
{
    return decode_luma_internal(rgb888, width, height, result, start_us,
                                rgb888_luma_reader, remember);
}

static esp_err_t decode_jpeg_internal(const camera_fb_t *fb, qr_scanner_result_t *result,
                                      int64_t start_us, bool remember)
{
    if (!fb || !fb->buf || fb->format != PIXFORMAT_JPEG || fb->width == 0 || fb->height == 0) {
        if (result) {
            memset(result, 0, sizeof(*result));
            result->err = ESP_ERR_INVALID_ARG;
            snprintf(result->decode_error, sizeof(result->decode_error), "invalid_jpeg");
        }
        return ESP_ERR_INVALID_ARG;
    }
    if ((size_t)fb->width * (size_t)fb->height > SIZE_MAX / 3) {
        if (result) {
            memset(result, 0, sizeof(*result));
            result->err = ESP_ERR_INVALID_SIZE;
            snprintf(result->decode_error, sizeof(result->decode_error), "jpeg_too_large");
        }
        return ESP_ERR_INVALID_SIZE;
    }

    size_t rgb_len = (size_t)fb->width * (size_t)fb->height * 3;
    uint8_t *rgb = heap_caps_malloc(rgb_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rgb) {
        rgb = heap_caps_malloc(rgb_len, MALLOC_CAP_8BIT);
    }
    if (!rgb) {
        memset(result, 0, sizeof(*result));
        result->width = fb->width;
        result->height = fb->height;
        result->err = ESP_ERR_NO_MEM;
        snprintf(result->decode_error, sizeof(result->decode_error), "jpeg_rgb_alloc");
        return finish_result(result, start_us, remember);
    }

    bool ok = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, rgb);
    if (!ok) {
        heap_caps_free(rgb);
        memset(result, 0, sizeof(*result));
        result->width = fb->width;
        result->height = fb->height;
        result->err = ESP_ERR_INVALID_RESPONSE;
        snprintf(result->decode_error, sizeof(result->decode_error), "jpeg_decode");
        return finish_result(result, start_us, remember);
    }

    esp_err_t err = decode_rgb888_internal(rgb, fb->width, fb->height, result, start_us, remember);
    heap_caps_free(rgb);
    return err;
}

static esp_err_t decode_camera_frame(const camera_fb_t *fb,
                                     qr_scanner_result_t *result,
                                     int64_t start_us)
{
    if (!fb || !fb->buf || fb->width == 0 || fb->height == 0) {
        memset(result, 0, sizeof(*result));
        result->err = ESP_FAIL;
        result->elapsed_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
        snprintf(result->decode_error, sizeof(result->decode_error), "invalid_frame");
        return result->err;
    }
    if (fb->format == PIXFORMAT_JPEG) {
        return decode_jpeg_internal(fb, result, start_us, false);
    }
    if (fb->format == PIXFORMAT_GRAYSCALE) {
        return decode_grayscale_internal(fb->buf, fb->width, fb->height, result,
                                         start_us, false);
    }
    if (fb->format == PIXFORMAT_RGB565) {
        return decode_rgb565_internal((const uint16_t *)fb->buf, fb->width, fb->height,
                                      result, start_us, false);
    }

    memset(result, 0, sizeof(*result));
    result->width = fb->width;
    result->height = fb->height;
    result->err = ESP_ERR_INVALID_RESPONSE;
    result->elapsed_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
    snprintf(result->decode_error, sizeof(result->decode_error), "frame_format_%d",
             (int)fb->format);
    return result->err;
}

esp_err_t qr_scanner_scan(qr_scanner_result_t *result)
{
    if (!result) {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t start_us = esp_timer_get_time();
    camera_fb_t *fb = NULL;
    esp_err_t err = ESP_OK;

    nfc_service_suspend_for_camera();
    vTaskDelay(pdMS_TO_TICKS(BOARD_CAMERA_NFC_QUIET_MS));

    memset(result, 0, sizeof(*result));
    result->err = ESP_ERR_NOT_FOUND;
    snprintf(result->decode_error, sizeof(result->decode_error), "no_qr");
    bool saw_code = false;

    for (int attempt = 0; attempt < QR_SCANNER_MAX_ATTEMPTS; attempt++) {
        fb = NULL;
        err = camera_mgr_capture_scan_grayscale(&fb);
        if (err != ESP_OK || !fb) {
            ESP_LOGW(TAG, "grayscale scan capture failed, falling back to jpeg: %s",
                     esp_err_to_name(err));
            if (fb) {
                camera_mgr_release_frame(fb);
                fb = NULL;
            }
            camera_mgr_deinit();
            err = camera_mgr_capture_scan_jpeg(&fb);
        }
        if (err == ESP_OK) {
            err = decode_camera_frame(fb, result, start_us);
        } else {
            memset(result, 0, sizeof(*result));
            result->err = err;
            result->elapsed_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
            snprintf(result->decode_error, sizeof(result->decode_error), "%s",
                     esp_err_to_name(result->err));
        }
        saw_code = saw_code || result->code_count > 0;

        if (fb) {
            camera_mgr_release_frame(fb);
            fb = NULL;
        }

        if (err == ESP_OK && result->found) {
            break;
        }
        if (!saw_code) {
            break;
        }
        if (attempt + 1 < QR_SCANNER_MAX_ATTEMPTS) {
            ESP_LOGI(TAG, "scan attempt %d/%d not found: %s",
                     attempt + 1, QR_SCANNER_MAX_ATTEMPTS,
                     result->decode_error[0] ? result->decode_error : esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(QR_SCANNER_RETRY_DELAY_MS));
        }
    }

    if (!camera_mgr_should_keep_online()) {
        camera_mgr_deinit();
    }
    nfc_service_resume_after_camera();
    remember_result(result);
    return err;
}

esp_err_t qr_scanner_decode_rgb565(const uint16_t *rgb565, uint16_t width, uint16_t height,
                                   qr_scanner_result_t *result)
{
    return decode_rgb565_internal(rgb565, width, height, result, esp_timer_get_time(), true);
}

esp_err_t qr_scanner_decode_grayscale(const uint8_t *gray, uint16_t width, uint16_t height,
                                      qr_scanner_result_t *result)
{
    return decode_grayscale_internal(gray, width, height, result, esp_timer_get_time(), true);
}

qr_scanner_status_t qr_scanner_get_status(void)
{
    return s_status;
}
