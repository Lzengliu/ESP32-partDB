#include "camera_mgr.h"

#include "board_config.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "peripheral_arbiter.h"

static const char *TAG = "camera";
static bool s_active;
static bool s_prewarm_started;
static pixformat_t s_pixel_format = PIXFORMAT_JPEG;
static framesize_t s_frame_size = FRAMESIZE_INVALID;
static SemaphoreHandle_t s_mutex;

#define CAMERA_WEB_FRAME_SIZE FRAMESIZE_QVGA
#define CAMERA_SCAN_FRAME_SIZE FRAMESIZE_240X240
#define CAMERA_WEB_JPEG_QUALITY 14

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

static esp_err_t camera_mgr_init_for(pixformat_t pixel_format, framesize_t frame_size)
{
    ESP_RETURN_ON_ERROR(ensure_mutex(), TAG, "mutex init failed");
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_active) {
        if (s_pixel_format == pixel_format && s_frame_size == frame_size) {
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

    bool has_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    int64_t start_us = esp_timer_get_time();
    camera_config_t cfg = {
        .pin_pwdn = BOARD_CAM_PWDN_GPIO,
        .pin_reset = BOARD_CAM_RESET_GPIO,
        .pin_xclk = BOARD_CAM_XCLK_GPIO,
        .pin_sscb_sda = BOARD_CAM_SIOD_GPIO,
        .pin_sscb_scl = BOARD_CAM_SIOC_GPIO,
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
        .frame_size = frame_size,
        .jpeg_quality = CAMERA_WEB_JPEG_QUALITY,
        .fb_count = has_psram ? 2 : 1,
        .fb_location = has_psram ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM,
        .grab_mode = has_psram ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY,
    };

    err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        peripheral_arbiter_release(PERIPHERAL_USER_CAMERA);
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "camera init failed: %s", esp_err_to_name(err));
        return err;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor) {
        sensor->set_vflip(sensor, 1);
        sensor->set_brightness(sensor, 1);
        sensor->set_saturation(sensor, 0);
        ESP_LOGI(TAG, "camera PID=0x%04x", sensor->id.PID);
    }

    s_active = true;
    s_pixel_format = pixel_format;
    s_frame_size = frame_size;
    peripheral_arbiter_release(PERIPHERAL_USER_CAMERA);
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "camera init done in %lld ms format=%d frame=%d",
             (long long)((esp_timer_get_time() - start_us) / 1000),
             (int)pixel_format, (int)frame_size);
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
    esp_err_t err = camera_mgr_init();
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
    (void)peripheral_arbiter_acquire(PERIPHERAL_USER_CAMERA, 3000);
    esp_camera_deinit();
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
    ESP_RETURN_ON_ERROR(camera_mgr_init_for(PIXFORMAT_JPEG, CAMERA_SCAN_FRAME_SIZE),
                        TAG, "init scan jpeg failed");
    int64_t start_us = esp_timer_get_time();
    camera_fb_t *frame = esp_camera_fb_get();
    if (!frame) {
        ESP_LOGE(TAG, "camera scan fb_get failed after %lld ms",
                 (long long)((esp_timer_get_time() - start_us) / 1000));
        return ESP_FAIL;
    }
    *fb = frame;
    ESP_LOGI(TAG, "camera scan fb_get done in %lld ms, %ux%u len=%u",
             (long long)((esp_timer_get_time() - start_us) / 1000),
             (unsigned)frame->width, (unsigned)frame->height, (unsigned)frame->len);
    return ESP_OK;
}

esp_err_t camera_mgr_capture_grayscale(camera_fb_t **fb)
{
    if (!fb) {
        return ESP_ERR_INVALID_ARG;
    }
    *fb = NULL;
    ESP_RETURN_ON_ERROR(camera_mgr_init_for(PIXFORMAT_GRAYSCALE, CAMERA_SCAN_FRAME_SIZE),
                        TAG, "init gray failed");
    int64_t start_us = esp_timer_get_time();
    camera_fb_t *frame = esp_camera_fb_get();
    if (!frame) {
        ESP_LOGE(TAG, "camera gray fb_get failed after %lld ms",
                 (long long)((esp_timer_get_time() - start_us) / 1000));
        return ESP_FAIL;
    }
    if (frame->format != PIXFORMAT_GRAYSCALE) {
        esp_camera_fb_return(frame);
        ESP_LOGE(TAG, "camera gray frame wrong format=%d", (int)frame->format);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *fb = frame;
    ESP_LOGI(TAG, "camera gray fb_get done in %lld ms, %ux%u len=%u",
             (long long)((esp_timer_get_time() - start_us) / 1000),
             (unsigned)frame->width, (unsigned)frame->height, (unsigned)frame->len);
    return ESP_OK;
}

void camera_mgr_release_frame(camera_fb_t *fb)
{
    if (fb) {
        esp_camera_fb_return(fb);
    }
}

bool camera_mgr_is_active(void)
{
    return s_active;
}
