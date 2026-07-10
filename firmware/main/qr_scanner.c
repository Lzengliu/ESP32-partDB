#include "qr_scanner.h"

#include <stdio.h>
#include <string.h>

#include "camera_mgr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "img_converters.h"
#include "quirc.h"

static const char *TAG = "qr_scanner";
static qr_scanner_status_t s_status;

static void copy_text(char *dst, size_t dst_len, const uint8_t *src, size_t src_len)
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

static void *scanner_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return p;
}

esp_err_t qr_scanner_scan(qr_scanner_result_t *result)
{
    if (!result) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    result->err = ESP_FAIL;
    int64_t start_us = esp_timer_get_time();

    camera_fb_t *fb = NULL;
    uint8_t *rgb565 = NULL;
    struct quirc *qr = NULL;
    struct quirc_code *code = NULL;
    struct quirc_data *data = NULL;
    esp_err_t err = camera_mgr_capture_scan_jpeg(&fb);
    if (err != ESP_OK || !fb) {
        result->err = err;
        result->elapsed_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
        remember_result(result);
        camera_mgr_deinit();
        return err;
    }

    uint16_t scan_w = fb->width / 2;
    uint16_t scan_h = fb->height / 2;
    result->width = scan_w;
    result->height = scan_h;
    size_t pixels = (size_t)scan_w * scan_h;
    size_t rgb_len = pixels * 2;
    rgb565 = scanner_alloc(rgb_len);
    if (!rgb565) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    if (!jpg2rgb565(fb->buf, fb->len, rgb565, JPEG_IMAGE_SCALE_1_2)) {
        err = ESP_ERR_INVALID_RESPONSE;
        snprintf(result->decode_error, sizeof(result->decode_error), "jpeg_decode_failed");
        goto done;
    }
    camera_mgr_release_frame(fb);
    fb = NULL;
    camera_mgr_deinit();

    qr = quirc_new();
    if (!qr) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    if (quirc_resize(qr, scan_w, scan_h) < 0) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }

    int qr_w = 0;
    int qr_h = 0;
    uint8_t *image = quirc_begin(qr, &qr_w, &qr_h);
    size_t needed = (size_t)qr_w * qr_h;
    if (!image || qr_w != scan_w || qr_h != scan_h) {
        err = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    if (needed > pixels) {
        err = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    const uint16_t *src = (const uint16_t *)rgb565;
    for (size_t i = 0; i < pixels; i++) {
        uint16_t c = src[i];
        uint8_t r = (uint8_t)(((c >> 11) & 0x1f) << 3);
        uint8_t g = (uint8_t)(((c >> 5) & 0x3f) << 2);
        uint8_t b = (uint8_t)((c & 0x1f) << 3);
        image[i] = (uint8_t)(((uint16_t)r * 77 + (uint16_t)g * 150 + (uint16_t)b * 29) >> 8);
    }
    quirc_end(qr);

    result->code_count = quirc_count(qr);
    if (result->code_count <= 0) {
        err = ESP_ERR_NOT_FOUND;
        snprintf(result->decode_error, sizeof(result->decode_error), "no_qr");
        goto done;
    }

    err = ESP_ERR_NOT_FOUND;
    code = scanner_alloc(sizeof(*code));
    data = scanner_alloc(sizeof(*data));
    if (!code || !data) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    for (int i = 0; i < result->code_count; i++) {
        quirc_extract(qr, i, code);
        quirc_decode_error_t dec_err = quirc_decode(code, data);
        if (dec_err == QUIRC_SUCCESS) {
            copy_text(result->text, sizeof(result->text), data->payload, data->payload_len);
            result->found = true;
            result->decode_error[0] = '\0';
            err = ESP_OK;
            break;
        }
        snprintf(result->decode_error, sizeof(result->decode_error), "%s",
                 quirc_strerror(dec_err));
    }

done:
    if (code) {
        free(code);
    }
    if (data) {
        free(data);
    }
    if (qr) {
        quirc_destroy(qr);
    }
    if (rgb565) {
        free(rgb565);
    }
    camera_mgr_release_frame(fb);
    camera_mgr_deinit();
    result->elapsed_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
    result->err = err;
    remember_result(result);
    ESP_LOGI(TAG, "scan err=%s found=%d count=%d size=%dx%d ms=%lu text=%s",
             esp_err_to_name(err), result->found, result->code_count,
             result->width, result->height, (unsigned long)result->elapsed_ms,
             result->found ? result->text : "-");
    return err;
}

qr_scanner_status_t qr_scanner_get_status(void)
{
    return s_status;
}
