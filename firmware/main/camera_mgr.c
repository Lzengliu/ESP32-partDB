#include "camera_mgr.h"

#include <stdlib.h>
#include <string.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "img_converters.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nfc_service.h"
#include "peripheral_arbiter.h"

static const char *TAG = "camera";
static bool s_active;
static bool s_prewarm_started;
static pixformat_t s_pixel_format = PIXFORMAT_JPEG;
static framesize_t s_frame_size = FRAMESIZE_INVALID;
static SemaphoreHandle_t s_mutex;
static int64_t s_last_deinit_us;
static bool s_runtime_keep_online;

#define CAMERA_WEB_FRAME_SIZE FRAMESIZE_QVGA
#define CAMERA_WEB_LOW_MEM_FRAME_SIZE FRAMESIZE_QQVGA
#define CAMERA_STILL_LOW_MEM_FRAME_SIZE FRAMESIZE_QQVGA
#define CAMERA_PREVIEW_FRAME_SIZE FRAMESIZE_QVGA
#define CAMERA_PREVIEW_LOW_MEM_FRAME_SIZE FRAMESIZE_QCIF
#define CAMERA_RGB565_LOW_MEM_FRAME_SIZE FRAMESIZE_QQVGA
#define CAMERA_SCAN_FRAME_SIZE FRAMESIZE_SVGA
#define CAMERA_SCAN_JPEG_FRAME_SIZE FRAMESIZE_VGA
#define CAMERA_SCAN_RGB_FRAME_SIZE FRAMESIZE_VGA
#define CAMERA_SCAN_LOW_MEM_FRAME_SIZE FRAMESIZE_QCIF
#define CAMERA_WEB_JPEG_QUALITY 14
#define CAMERA_WEB_LOW_MEM_JPEG_QUALITY 18
#define CAMERA_RGB_TO_JPEG_QUALITY 55
#define CAMERA_REINIT_GUARD_MS 500
#define CAMERA_POST_INIT_SETTLE_MS 80

static void camera_apply_sensor_profile(sensor_t *sensor, pixformat_t pixel_format,
                                        framesize_t frame_size)
{
    if (!sensor) {
        return;
    }

    sensor->set_vflip(sensor, 1);

    bool scan_profile = (pixel_format == PIXFORMAT_JPEG &&
                         frame_size == CAMERA_SCAN_JPEG_FRAME_SIZE) ||
                        pixel_format == PIXFORMAT_GRAYSCALE ||
                        (pixel_format == PIXFORMAT_RGB565 &&
                         frame_size == CAMERA_SCAN_RGB_FRAME_SIZE);
    if (scan_profile) {
        sensor->set_brightness(sensor, BOARD_CAMERA_SCAN_BRIGHTNESS);
        sensor->set_contrast(sensor, BOARD_CAMERA_SCAN_CONTRAST);
        sensor->set_saturation(sensor, BOARD_CAMERA_SCAN_SATURATION);
        sensor->set_sharpness(sensor, BOARD_CAMERA_SCAN_SHARPNESS);
        sensor->set_denoise(sensor, BOARD_CAMERA_SCAN_DENOISE);
        if (sensor->set_special_effect) {
            sensor->set_special_effect(sensor, 0);
        }
        if (sensor->set_whitebal) {
            sensor->set_whitebal(sensor, 1);
        }
        if (sensor->set_awb_gain) {
            sensor->set_awb_gain(sensor, 1);
        }
        if (sensor->set_wb_mode) {
            sensor->set_wb_mode(sensor, 0);
        }
        if (sensor->set_exposure_ctrl) {
            sensor->set_exposure_ctrl(sensor, 1);
        }
        if (sensor->set_gain_ctrl) {
            sensor->set_gain_ctrl(sensor, 1);
        }
        if (sensor->set_aec2) {
            sensor->set_aec2(sensor, 1);
        }
        if (sensor->set_ae_level) {
            sensor->set_ae_level(sensor, BOARD_CAMERA_SCAN_AE_LEVEL);
        }
        if (sensor->set_gainceiling) {
            sensor->set_gainceiling(sensor, (gainceiling_t)BOARD_CAMERA_SCAN_GAINCEILING);
        }
        if (sensor->set_bpc) {
            sensor->set_bpc(sensor, 1);
        }
        if (sensor->set_wpc) {
            sensor->set_wpc(sensor, 1);
        }
        if (sensor->set_raw_gma) {
            sensor->set_raw_gma(sensor, 1);
        }
        if (sensor->set_lenc) {
            sensor->set_lenc(sensor, 1);
        }
        if (sensor->set_dcw) {
            sensor->set_dcw(sensor, 1);
        }
        ESP_LOGI(TAG,
                 "camera scan profile format=%d frame=%d brightness=%d contrast=%d saturation=%d sharpness=%d ae=%d gainceiling=%d",
                 (int)pixel_format, (int)frame_size,
                 BOARD_CAMERA_SCAN_BRIGHTNESS, BOARD_CAMERA_SCAN_CONTRAST,
                 BOARD_CAMERA_SCAN_SATURATION, BOARD_CAMERA_SCAN_SHARPNESS,
                 BOARD_CAMERA_SCAN_AE_LEVEL, BOARD_CAMERA_SCAN_GAINCEILING);
        return;
    }

    sensor->set_brightness(sensor, BOARD_CAMERA_BRIGHTNESS);
    sensor->set_contrast(sensor, BOARD_CAMERA_CONTRAST);
    sensor->set_saturation(sensor, BOARD_CAMERA_SATURATION);
    sensor->set_sharpness(sensor, BOARD_CAMERA_SHARPNESS);
    sensor->set_denoise(sensor, BOARD_CAMERA_DENOISE);
}

static void sccb_release_line(gpio_num_t gpio)
{
    (void)gpio_set_direction(gpio, GPIO_MODE_INPUT);
    (void)gpio_pullup_en(gpio);
}

static void sccb_drive_low(gpio_num_t gpio)
{
    gpio_set_level(gpio, 0);
    (void)gpio_set_direction(gpio, GPIO_MODE_OUTPUT_OD);
}

static void sccb_delay(void)
{
    esp_rom_delay_us(8);
}

static esp_err_t camera_recover_sccb_bus(void)
{
    sccb_release_line(BOARD_CAM_SIOD_GPIO);
    sccb_release_line(BOARD_CAM_SIOC_GPIO);
    sccb_delay();

    for (int i = 0; i < 9 && !gpio_get_level(BOARD_CAM_SIOC_GPIO); i++) {
        sccb_drive_low(BOARD_CAM_SIOC_GPIO);
        sccb_delay();
        sccb_release_line(BOARD_CAM_SIOC_GPIO);
        sccb_delay();
    }

    sccb_drive_low(BOARD_CAM_SIOD_GPIO);
    sccb_delay();
    sccb_release_line(BOARD_CAM_SIOC_GPIO);
    sccb_delay();
    sccb_release_line(BOARD_CAM_SIOD_GPIO);
    sccb_delay();

    int sda = gpio_get_level(BOARD_CAM_SIOD_GPIO);
    int scl = gpio_get_level(BOARD_CAM_SIOC_GPIO);
    if (!sda || !scl) {
        ESP_LOGW(TAG, "camera SCCB recovery failed: SDA=%d SCL=%d", sda, scl);
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "camera SCCB recovery succeeded");
    return ESP_OK;
}

static esp_err_t camera_prepare_sccb_bus(void)
{
    if (BOARD_CAM_SIOD_GPIO == GPIO_NUM_NC || BOARD_CAM_SIOC_GPIO == GPIO_NUM_NC ||
        BOARD_CAM_SIOD_GPIO == BOARD_CAM_SIOC_GPIO) {
        return ESP_ERR_INVALID_ARG;
    }
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BOARD_CAM_SIOD_GPIO) | (1ULL << BOARD_CAM_SIOC_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    int sda = gpio_get_level(BOARD_CAM_SIOD_GPIO);
    int scl = gpio_get_level(BOARD_CAM_SIOC_GPIO);
    if (!sda || !scl) {
        ESP_LOGW(TAG, "camera SCCB bus held low before recovery: SDA=%d SCL=%d", sda, scl);
        esp_err_t recover_err = camera_recover_sccb_bus();
        if (recover_err != ESP_OK) {
            ESP_LOGE(TAG, "camera SCCB bus held low before init: SDA=%d SCL=%d",
                     gpio_get_level(BOARD_CAM_SIOD_GPIO),
                     gpio_get_level(BOARD_CAM_SIOC_GPIO));
            return recover_err;
        }
    }
    return ESP_OK;
}

static esp_err_t ensure_mutex(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static void camera_wait_after_deinit_locked(void)
{
    if (s_last_deinit_us <= 0) {
        return;
    }
    int64_t guard_us = (int64_t)CAMERA_REINIT_GUARD_MS * 1000;
    int64_t elapsed_us = esp_timer_get_time() - s_last_deinit_us;
    if (elapsed_us >= guard_us) {
        return;
    }
    uint32_t wait_ms = (uint32_t)((guard_us - elapsed_us + 999) / 1000);
    ESP_LOGI(TAG, "camera reinit guard wait %u ms", (unsigned)wait_ms);
    vTaskDelay(pdMS_TO_TICKS(wait_ms));
}

static esp_err_t camera_mgr_init_for(pixformat_t pixel_format, framesize_t frame_size)
{
    ESP_RETURN_ON_ERROR(ensure_mutex(), TAG, "mutex init failed");
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    bool has_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    framesize_t effective_frame_size = frame_size;
    if (!has_psram) {
        if (pixel_format == PIXFORMAT_JPEG && frame_size != CAMERA_WEB_LOW_MEM_FRAME_SIZE) {
            effective_frame_size = CAMERA_WEB_LOW_MEM_FRAME_SIZE;
            ESP_LOGW(TAG, "PSRAM unavailable; JPEG frame downgraded from %d to %d",
                     (int)frame_size, (int)effective_frame_size);
        } else if (pixel_format == PIXFORMAT_RGB565 &&
                   frame_size > CAMERA_RGB565_LOW_MEM_FRAME_SIZE) {
            effective_frame_size = CAMERA_RGB565_LOW_MEM_FRAME_SIZE;
            ESP_LOGW(TAG, "PSRAM unavailable; preview frame downgraded from %d to %d",
                     (int)frame_size, (int)effective_frame_size);
        } else if (pixel_format == PIXFORMAT_GRAYSCALE &&
                   frame_size > CAMERA_SCAN_LOW_MEM_FRAME_SIZE) {
            effective_frame_size = CAMERA_SCAN_LOW_MEM_FRAME_SIZE;
            ESP_LOGW(TAG, "PSRAM unavailable; grayscale frame downgraded from %d to %d",
                     (int)frame_size, (int)effective_frame_size);
        }
    }

    if (s_active) {
        if (s_pixel_format == pixel_format && s_frame_size == effective_frame_size) {
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
        esp_err_t deinit_err = peripheral_arbiter_acquire(PERIPHERAL_USER_CAMERA, 3000);
        if (deinit_err != ESP_OK) {
            xSemaphoreGive(s_mutex);
            ESP_LOGE(TAG, "camera reconfigure busy: %s", esp_err_to_name(deinit_err));
            return deinit_err;
        }
        esp_camera_deinit();
        s_last_deinit_us = esp_timer_get_time();
        s_active = false;
        s_frame_size = FRAMESIZE_INVALID;
        peripheral_arbiter_release(PERIPHERAL_USER_CAMERA);
    }

    esp_err_t err = peripheral_arbiter_acquire(PERIPHERAL_USER_CAMERA, 3000);
    if (err != ESP_OK) {
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "camera resource busy: %s", esp_err_to_name(err));
        return err;
    }

    int64_t start_us = esp_timer_get_time();
    camera_wait_after_deinit_locked();

    err = camera_prepare_sccb_bus();
    if (err != ESP_OK) {
        peripheral_arbiter_release(PERIPHERAL_USER_CAMERA);
        xSemaphoreGive(s_mutex);
        return err;
    }

    camera_config_t cfg = {
        .pin_pwdn = BOARD_CAM_PWDN_GPIO,
        .pin_reset = BOARD_CAM_RESET_GPIO,
        .pin_xclk = BOARD_CAM_XCLK_GPIO,
        .pin_sccb_sda = BOARD_CAM_SIOD_GPIO,
        .pin_sccb_scl = BOARD_CAM_SIOC_GPIO,
        .pin_d7 = BOARD_CAM_D7_GPIO,
        .pin_d6 = BOARD_CAM_D6_GPIO,
        .pin_d5 = BOARD_CAM_D5_GPIO,
        .pin_d4 = BOARD_CAM_D4_GPIO,
        .pin_d3 = BOARD_CAM_D3_GPIO,
        .pin_d2 = BOARD_CAM_D2_GPIO,
        .pin_d1 = BOARD_CAM_D1_GPIO,
        .pin_d0 = BOARD_CAM_D0_GPIO,
        .pin_vsync = BOARD_CAM_VSYNC_GPIO,
        .pin_href = BOARD_CAM_HREF_GPIO,
        .pin_pclk = BOARD_CAM_PCLK_GPIO,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = pixel_format,
        .frame_size = effective_frame_size,
        .jpeg_quality = has_psram ? CAMERA_WEB_JPEG_QUALITY : CAMERA_WEB_LOW_MEM_JPEG_QUALITY,
        .fb_count = has_psram ? 2 : 1,
        .fb_location = has_psram ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM,
        .grab_mode = has_psram ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY,
    };

    ESP_LOGI(TAG, "camera init begin format=%d frame=%d requested=%d psram=%d psram_free=%u internal_largest=%u",
             (int)pixel_format, (int)effective_frame_size, (int)frame_size, has_psram,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        (void)esp_camera_deinit();
        s_last_deinit_us = esp_timer_get_time();
        peripheral_arbiter_release(PERIPHERAL_USER_CAMERA);
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "camera init failed: %s", esp_err_to_name(err));
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(CAMERA_POST_INIT_SETTLE_MS));

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor) {
        camera_apply_sensor_profile(sensor, pixel_format, effective_frame_size);
        ESP_LOGI(TAG, "camera PID=0x%04x", sensor->id.PID);
    }

    s_active = true;
    s_pixel_format = pixel_format;
    s_frame_size = effective_frame_size;
    peripheral_arbiter_release(PERIPHERAL_USER_CAMERA);
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "camera init done in %lld ms format=%d frame=%d",
             (long long)((esp_timer_get_time() - start_us) / 1000),
             (int)pixel_format, (int)effective_frame_size);
    return ESP_OK;
}

esp_err_t camera_mgr_init(void)
{
    return camera_mgr_init_for(PIXFORMAT_JPEG, CAMERA_WEB_FRAME_SIZE);
}

static void camera_prewarm_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(250));
    int64_t start_us = esp_timer_get_time();
    nfc_service_suspend_for_camera();
    vTaskDelay(pdMS_TO_TICKS(BOARD_CAMERA_NFC_QUIET_MS));
    esp_err_t err = camera_mgr_init();
    nfc_service_resume_after_camera();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "camera prewarm ready in %lld ms",
                 (long long)((esp_timer_get_time() - start_us) / 1000));
    } else {
        ESP_LOGW(TAG, "camera prewarm failed: %s", esp_err_to_name(err));
    }
    s_prewarm_started = false;
    vTaskDelete(NULL);
}

esp_err_t camera_mgr_prewarm_async(void)
{
    ESP_RETURN_ON_ERROR(ensure_mutex(), TAG, "mutex init failed");
    if (s_active || s_prewarm_started) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(camera_prewarm_task, "cam_prewarm", 4096, NULL, 4, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_prewarm_started = true;
    return ESP_OK;
}

void camera_mgr_deinit(void)
{
    if (ensure_mutex() != ESP_OK) {
        return;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return;
    }
    if (!s_active) {
        xSemaphoreGive(s_mutex);
        return;
    }
    esp_err_t arb_err = peripheral_arbiter_acquire(PERIPHERAL_USER_CAMERA, 3000);
    if (arb_err != ESP_OK) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "camera deinit skipped, resource busy: %s",
                 esp_err_to_name(arb_err));
        return;
    }
    esp_camera_deinit();
    s_last_deinit_us = esp_timer_get_time();
    s_active = false;
    s_frame_size = FRAMESIZE_INVALID;
    peripheral_arbiter_release(PERIPHERAL_USER_CAMERA);
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "camera deinit done");
}

esp_err_t camera_mgr_capture_jpeg(camera_fb_t **fb)
{
    if (!fb) {
        return ESP_ERR_INVALID_ARG;
    }
    *fb = NULL;
    ESP_RETURN_ON_ERROR(camera_mgr_init(), TAG, "init failed");
    int64_t start_us = esp_timer_get_time();
    camera_fb_t *frame = esp_camera_fb_get();
    if (!frame) {
        ESP_LOGE(TAG, "camera fb_get failed after %lld ms",
                 (long long)((esp_timer_get_time() - start_us) / 1000));
        return ESP_FAIL;
    }
    *fb = frame;
    ESP_LOGI(TAG, "camera fb_get done in %lld ms, len=%u",
             (long long)((esp_timer_get_time() - start_us) / 1000), (unsigned)frame->len);
    return ESP_OK;
}

esp_err_t camera_mgr_capture_scan_jpeg(camera_fb_t **fb)
{
    if (!fb) {
        return ESP_ERR_INVALID_ARG;
    }
    *fb = NULL;
    ESP_RETURN_ON_ERROR(camera_mgr_init_for(PIXFORMAT_JPEG, CAMERA_SCAN_JPEG_FRAME_SIZE),
                        TAG, "init scan jpeg failed");
    if (BOARD_CAMERA_SCAN_SETTLE_MS > 0) {
        vTaskDelay(pdMS_TO_TICKS(BOARD_CAMERA_SCAN_SETTLE_MS));
    }
    for (int i = 0; i < BOARD_CAMERA_SCAN_DROP_FRAMES; i++) {
        camera_fb_t *drop = esp_camera_fb_get();
        if (!drop) {
            ESP_LOGW(TAG, "camera scan jpeg warmup drop %d failed", i + 1);
            break;
        }
        esp_camera_fb_return(drop);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    int64_t start_us = esp_timer_get_time();
    camera_fb_t *frame = esp_camera_fb_get();
    if (!frame) {
        ESP_LOGE(TAG, "camera scan jpeg fb_get failed after %lld ms",
                 (long long)((esp_timer_get_time() - start_us) / 1000));
        return ESP_FAIL;
    }
    if (frame->format != PIXFORMAT_JPEG) {
        esp_camera_fb_return(frame);
        ESP_LOGE(TAG, "camera scan jpeg frame wrong format=%d", (int)frame->format);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *fb = frame;
    ESP_LOGI(TAG, "camera scan jpeg fb_get done in %lld ms, %ux%u len=%u",
             (long long)((esp_timer_get_time() - start_us) / 1000),
             frame->width, frame->height, (unsigned)frame->len);
    return ESP_OK;
}

static esp_err_t camera_mgr_capture_rgb565_frame(framesize_t frame_size, const char *label,
                                                 camera_fb_t **fb)
{
    if (!fb) {
        return ESP_ERR_INVALID_ARG;
    }
    *fb = NULL;
    ESP_RETURN_ON_ERROR(camera_mgr_init_for(PIXFORMAT_RGB565, frame_size),
                        TAG, "init %s rgb565 failed", label ? label : "frame");
    bool scan_frame = label && strstr(label, "scan");
    if (scan_frame) {
        if (BOARD_CAMERA_SCAN_SETTLE_MS > 0) {
            vTaskDelay(pdMS_TO_TICKS(BOARD_CAMERA_SCAN_SETTLE_MS));
        }
        for (int i = 0; i < BOARD_CAMERA_SCAN_DROP_FRAMES; i++) {
            camera_fb_t *drop = esp_camera_fb_get();
            if (!drop) {
                ESP_LOGW(TAG, "camera %s warmup drop %d failed",
                         label ? label : "rgb565", i + 1);
                break;
            }
            esp_camera_fb_return(drop);
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }
    int64_t start_us = esp_timer_get_time();
    camera_fb_t *frame = esp_camera_fb_get();
    if (!frame) {
        ESP_LOGE(TAG, "camera %s fb_get failed after %lld ms",
                 label ? label : "rgb565",
                 (long long)((esp_timer_get_time() - start_us) / 1000));
        return ESP_FAIL;
    }
    if (frame->format != PIXFORMAT_RGB565) {
        esp_camera_fb_return(frame);
        ESP_LOGE(TAG, "camera %s frame wrong format=%d",
                 label ? label : "rgb565", (int)frame->format);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *fb = frame;
    ESP_LOGI(TAG, "camera %s fb_get done in %lld ms, %ux%u len=%u",
             label ? label : "rgb565",
             (long long)((esp_timer_get_time() - start_us) / 1000),
             frame->width, frame->height, (unsigned)frame->len);
    return ESP_OK;
}

static bool camera_grayscale_frame_usable(const camera_fb_t *frame,
                                          uint8_t *out_min, uint8_t *out_max,
                                          uint8_t *out_mean, uint8_t *out_clipped_pct)
{
    if (!frame || !frame->buf || frame->format != PIXFORMAT_GRAYSCALE ||
        frame->width == 0 || frame->height == 0) {
        return false;
    }

    size_t pixels = (size_t)frame->width * frame->height;
    if (pixels > frame->len) {
        pixels = frame->len;
    }
    size_t step = pixels > 8192 ? pixels / 8192 : 1;
    uint8_t min_luma = 255;
    uint8_t max_luma = 0;
    uint64_t sum = 0;
    size_t count = 0;
    size_t clipped = 0;
    for (size_t i = 0; i < pixels; i += step) {
        uint8_t value = frame->buf[i];
        if (value < min_luma) min_luma = value;
        if (value > max_luma) max_luma = value;
        sum += value;
        if (value <= 5 || value >= 250) clipped++;
        count++;
    }
    uint8_t mean = count ? (uint8_t)(sum / count) : 0;
    uint8_t clipped_pct = count ? (uint8_t)((clipped * 100) / count) : 100;
    if (out_min) *out_min = min_luma;
    if (out_max) *out_max = max_luma;
    if (out_mean) *out_mean = mean;
    if (out_clipped_pct) *out_clipped_pct = clipped_pct;
    return count > 0 && (int)max_luma - (int)min_luma >= 24 &&
           mean > 7 && mean < 248 && clipped_pct < 30;
}

static esp_err_t camera_mgr_capture_grayscale_frame(framesize_t frame_size, const char *label,
                                                    camera_fb_t **fb)
{
    if (!fb) {
        return ESP_ERR_INVALID_ARG;
    }
    *fb = NULL;
    ESP_RETURN_ON_ERROR(camera_mgr_init_for(PIXFORMAT_GRAYSCALE, frame_size),
                        TAG, "init %s grayscale failed", label ? label : "frame");
    bool scan_frame = label && strstr(label, "scan");
    int64_t start_us = esp_timer_get_time();
    if (scan_frame) {
        if (BOARD_CAMERA_SCAN_SETTLE_MS > 0) {
            vTaskDelay(pdMS_TO_TICKS(BOARD_CAMERA_SCAN_SETTLE_MS));
        }
        for (int i = 0; i < BOARD_CAMERA_SCAN_DROP_FRAMES; i++) {
            camera_fb_t *drop = esp_camera_fb_get();
            if (!drop) {
                ESP_LOGW(TAG, "camera %s warmup drop %d failed",
                         label ? label : "gray", i + 1);
                break;
            }
            uint8_t min_luma = 0;
            uint8_t max_luma = 0;
            uint8_t mean_luma = 0;
            uint8_t clipped_pct = 0;
            if (camera_grayscale_frame_usable(drop, &min_luma, &max_luma,
                                              &mean_luma, &clipped_pct)) {
                *fb = drop;
                ESP_LOGI(TAG,
                         "camera %s warmup ready frame=%d min=%u max=%u mean=%u clipped=%u%% total=%lld ms",
                         label ? label : "gray", i + 1, min_luma, max_luma, mean_luma,
                         clipped_pct,
                         (long long)((esp_timer_get_time() - start_us) / 1000));
                return ESP_OK;
            }
            ESP_LOGI(TAG,
                     "camera %s warmup discard frame=%d min=%u max=%u mean=%u clipped=%u%%",
                     label ? label : "gray", i + 1, min_luma, max_luma,
                     mean_luma, clipped_pct);
            esp_camera_fb_return(drop);
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }
    camera_fb_t *frame = esp_camera_fb_get();
    if (!frame) {
        ESP_LOGE(TAG, "camera %s fb_get failed after %lld ms",
                 label ? label : "gray",
                 (long long)((esp_timer_get_time() - start_us) / 1000));
        return ESP_FAIL;
    }
    if (frame->format != PIXFORMAT_GRAYSCALE) {
        esp_camera_fb_return(frame);
        ESP_LOGE(TAG, "camera %s frame wrong format=%d",
                 label ? label : "gray", (int)frame->format);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *fb = frame;
    if (!label || !strstr(label, "preview")) {
        ESP_LOGI(TAG, "camera %s fb_get done in %lld ms, %ux%u len=%u",
                 label ? label : "gray",
                 (long long)((esp_timer_get_time() - start_us) / 1000),
                 frame->width, frame->height, (unsigned)frame->len);
    }
    return ESP_OK;
}

esp_err_t camera_mgr_capture_jpeg_bytes(uint8_t **out, size_t *out_len)
{
    if (!out || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    *out_len = 0;

    bool has_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    if (has_psram) {
        camera_fb_t *fb = NULL;
        esp_err_t err = camera_mgr_capture_jpeg(&fb);
        if (err != ESP_OK) {
            return err;
        }
        if (!fb || !fb->buf || fb->len == 0) {
            if (fb) {
                camera_mgr_release_frame(fb);
            }
            return ESP_FAIL;
        }
        uint8_t *copy = heap_caps_malloc(fb->len, MALLOC_CAP_8BIT);
        if (!copy) {
            camera_mgr_release_frame(fb);
            ESP_LOGE(TAG, "JPEG copy alloc failed len=%u", (unsigned)fb->len);
            return ESP_ERR_NO_MEM;
        }
        memcpy(copy, fb->buf, fb->len);
        *out = copy;
        *out_len = fb->len;
        camera_mgr_release_frame(fb);
        return ESP_OK;
    }

    camera_fb_t *rgb = NULL;
    esp_err_t err = camera_mgr_capture_rgb565_frame(CAMERA_STILL_LOW_MEM_FRAME_SIZE,
                                                    "still lowmem", &rgb);
    if (err != ESP_OK) {
        return err;
    }
    if (!rgb || !rgb->buf || rgb->len == 0 || rgb->width == 0 || rgb->height == 0) {
        if (rgb) {
            camera_mgr_release_frame(rgb);
        }
        return ESP_FAIL;
    }

    uint8_t *jpg = NULL;
    size_t jpg_len = 0;
    bool ok = fmt2jpg(rgb->buf, rgb->len, rgb->width, rgb->height, rgb->format,
                      CAMERA_RGB_TO_JPEG_QUALITY, &jpg, &jpg_len);
    camera_mgr_release_frame(rgb);
    if (!ok || !jpg || jpg_len == 0) {
        if (jpg) {
            free(jpg);
        }
        ESP_LOGE(TAG, "RGB565 to JPEG conversion failed");
        return ESP_ERR_NO_MEM;
    }

    *out = jpg;
    *out_len = jpg_len;
    ESP_LOGI(TAG, "RGB565 frame converted to JPEG len=%u quality=%d",
             (unsigned)jpg_len, CAMERA_RGB_TO_JPEG_QUALITY);
    return ESP_OK;
}

void camera_mgr_free_jpeg_bytes(uint8_t *buf)
{
    free(buf);
}

esp_err_t camera_mgr_capture_still_rgb565(camera_fb_t **fb)
{
    return camera_mgr_capture_rgb565_frame(CAMERA_STILL_LOW_MEM_FRAME_SIZE,
                                          "still lowmem", fb);
}

esp_err_t camera_mgr_capture_scan_grayscale(camera_fb_t **fb)
{
    esp_err_t err = camera_mgr_capture_grayscale_frame(CAMERA_SCAN_FRAME_SIZE,
                                                       "scan gray", fb);
    if (err == ESP_OK) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "grayscale scan failed, falling back to RGB565 lowmem: %s",
             esp_err_to_name(err));
    camera_mgr_deinit();
    return camera_mgr_capture_rgb565_frame(CAMERA_RGB565_LOW_MEM_FRAME_SIZE,
                                           "scan rgb565 fallback", fb);
}

esp_err_t camera_mgr_capture_scan_rgb565(camera_fb_t **fb)
{
    return camera_mgr_capture_rgb565_frame(CAMERA_SCAN_RGB_FRAME_SIZE, "scan rgb565", fb);
}

esp_err_t camera_mgr_capture_preview_lowmem(camera_fb_t **fb)
{
    esp_err_t err = camera_mgr_capture_grayscale_frame(CAMERA_PREVIEW_LOW_MEM_FRAME_SIZE,
                                                       "preview gray", fb);
    if (err == ESP_OK) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "grayscale preview failed, falling back to RGB565 lowmem: %s",
             esp_err_to_name(err));
    camera_mgr_deinit();
    return camera_mgr_capture_rgb565_frame(CAMERA_RGB565_LOW_MEM_FRAME_SIZE,
                                           "preview rgb565 lowmem", fb);
}

esp_err_t camera_mgr_capture_preview_rgb565(camera_fb_t **fb)
{
    if (!fb) {
        return ESP_ERR_INVALID_ARG;
    }
    *fb = NULL;
    ESP_RETURN_ON_ERROR(camera_mgr_init_for(PIXFORMAT_RGB565, CAMERA_PREVIEW_FRAME_SIZE),
                        TAG, "init preview rgb565 failed");
    int64_t start_us = esp_timer_get_time();
    camera_fb_t *frame = esp_camera_fb_get();
    if (!frame) {
        ESP_LOGE(TAG, "camera preview fb_get failed after %lld ms",
                 (long long)((esp_timer_get_time() - start_us) / 1000));
        return ESP_FAIL;
    }
    if (frame->format != PIXFORMAT_RGB565) {
        esp_camera_fb_return(frame);
        ESP_LOGE(TAG, "camera preview frame wrong format=%d", (int)frame->format);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *fb = frame;
    return ESP_OK;
}

esp_err_t camera_mgr_capture_preview_rgb565_lowmem(camera_fb_t **fb)
{
    if (!fb) {
        return ESP_ERR_INVALID_ARG;
    }
    *fb = NULL;
    ESP_RETURN_ON_ERROR(camera_mgr_init_for(PIXFORMAT_RGB565, CAMERA_RGB565_LOW_MEM_FRAME_SIZE),
                        TAG, "init lowmem preview rgb565 failed");
    int64_t start_us = esp_timer_get_time();
    camera_fb_t *frame = esp_camera_fb_get();
    if (!frame) {
        ESP_LOGE(TAG, "camera lowmem preview fb_get failed after %lld ms",
                 (long long)((esp_timer_get_time() - start_us) / 1000));
        return ESP_FAIL;
    }
    if (frame->format != PIXFORMAT_RGB565) {
        esp_camera_fb_return(frame);
        ESP_LOGE(TAG, "camera lowmem preview frame wrong format=%d", (int)frame->format);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *fb = frame;
    return ESP_OK;
}

void camera_mgr_release_frame(camera_fb_t *fb)
{
    if (fb) {
        esp_camera_fb_return(fb);
    }
}

bool camera_mgr_should_keep_online(void)
{
    return BOARD_CAMERA_KEEP_ONLINE || s_runtime_keep_online;
}

void camera_mgr_set_keep_online(bool keep_online)
{
    s_runtime_keep_online = keep_online;
}

bool camera_mgr_is_active(void)
{
    return s_active;
}
