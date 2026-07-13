#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app_config.h"
#include "board_config.h"
#include "button_input.h"
#include "camera_mgr.h"
#include "device_ui.h"
#include "display_ili9488.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "jpeg_decoder.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "hardware_diag.h"
#include "nvs_flash.h"
#include "nfc_pn532.h"
#include "nfc_service.h"
#include "partdb_client.h"
#include "peripheral_arbiter.h"
#include "psram_diag.h"
#include "storage_sd.h"
#include "touch_ft6336.h"
#include "ui_font.h"
#include "wifi_portal.h"
#include "http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app";
static app_config_t s_cfg;

#define BOOT_IMAGE_MAX_BYTES (512 * 1024)

static void prepare_nfc_spi_chip_select(void)
{
#if BOARD_NFC_ENABLED && BOARD_NFC_USE_SPI
    if (BOARD_NFC_SPI_CS_GPIO == GPIO_NUM_NC) {
        return;
    }

    gpio_set_level(BOARD_NFC_SPI_CS_GPIO, 1);
    gpio_config_t cs = {
        .pin_bit_mask = 1ULL << BOARD_NFC_SPI_CS_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cs);
    if (err == ESP_OK) {
        gpio_set_level(BOARD_NFC_SPI_CS_GPIO, 1);
    } else {
        ESP_LOGW(TAG, "Failed to prepare PN532 CS GPIO%d: %s",
                 BOARD_NFC_SPI_CS_GPIO, esp_err_to_name(err));
    }
#endif
}

static bool is_jpeg_path(const char *path)
{
    const char *ext = path ? strrchr(path, '.') : NULL;
    if (!ext) {
        return false;
    }
    return strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0;
}

static esp_err_t boot_resolve_sd_path(const char *path, char *out, size_t out_len)
{
    if (!path || path[0] == '\0' || !out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t mount_len = strlen(BOARD_SD_MOUNT_POINT);
    if (strncmp(path, BOARD_SD_MOUNT_POINT, mount_len) == 0 &&
        (path[mount_len] == '\0' || path[mount_len] == '/')) {
        int written = snprintf(out, out_len, "%s", path);
        return written >= 0 && written < (int)out_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    if (path[0] == '/' && !strstr(path, "..") && !strchr(path, '\\')) {
        int written = snprintf(out, out_len, "%s%s", BOARD_SD_MOUNT_POINT, path);
        return written >= 0 && written < (int)out_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    return ESP_ERR_INVALID_ARG;
}

static uint8_t *boot_read_file(const char *path, size_t max_bytes, size_t *out_len)
{
    if (out_len) {
        *out_len = 0;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size <= 0 || (size_t)size > max_bytes || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = heap_caps_malloc((size_t)size, MALLOC_CAP_8BIT);
    }
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        heap_caps_free(buf);
        return NULL;
    }
    if (out_len) {
        *out_len = got;
    }
    return buf;
}

static esp_err_t boot_ui_draw_jpeg_file(const char *configured_path)
{
    if (!display_ili9488_is_ready() || !is_jpeg_path(configured_path)) {
        return ESP_ERR_INVALID_ARG;
    }
    char abs_path[APP_CONFIG_PATH_LEN * 2];
    ESP_RETURN_ON_ERROR(boot_resolve_sd_path(configured_path, abs_path, sizeof(abs_path)),
                        TAG, "boot image path invalid");

    size_t jpg_len = 0;
    uint8_t *jpg = boot_read_file(abs_path, BOOT_IMAGE_MAX_BYTES, &jpg_len);
    if (!jpg) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = ESP_FAIL;
    uint16_t screen_w = display_ili9488_get_width();
    uint16_t screen_h = display_ili9488_get_height();
    const esp_jpeg_image_scale_t scales[] = {
        JPEG_IMAGE_SCALE_0,
        JPEG_IMAGE_SCALE_1_2,
        JPEG_IMAGE_SCALE_1_4,
        JPEG_IMAGE_SCALE_1_8,
    };
    for (size_t i = 0; i < sizeof(scales) / sizeof(scales[0]); i++) {
        esp_jpeg_image_cfg_t info_cfg = {
            .indata = jpg,
            .indata_size = jpg_len,
            .out_format = JPEG_IMAGE_FORMAT_RGB565,
            .out_scale = scales[i],
            .flags.swap_color_bytes = 0,
        };
        esp_jpeg_image_output_t info = {0};
        err = esp_jpeg_get_image_info(&info_cfg, &info);
        if (err != ESP_OK || info.width == 0 || info.height == 0 ||
            info.width > screen_w || info.height > screen_h) {
            continue;
        }
        uint16_t *pixels = heap_caps_malloc(info.output_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!pixels) {
            pixels = heap_caps_malloc(info.output_len, MALLOC_CAP_8BIT);
        }
        if (!pixels) {
            err = ESP_ERR_NO_MEM;
            continue;
        }
        esp_jpeg_image_cfg_t decode_cfg = info_cfg;
        decode_cfg.outbuf = (uint8_t *)pixels;
        decode_cfg.outbuf_size = info.output_len;
        esp_jpeg_image_output_t decoded = {0};
        err = esp_jpeg_decode(&decode_cfg, &decoded);
        if (err == ESP_OK) {
            int x = (screen_w - decoded.width) / 2;
            int y = (screen_h - decoded.height) / 2;
            (void)display_ili9488_clear(0x0000);
            err = display_ili9488_draw_bitmap565(x, y, decoded.width, decoded.height, pixels);
            ESP_LOGI(TAG, "Boot image rendered %s %ux%u", abs_path, decoded.width, decoded.height);
        }
        heap_caps_free(pixels);
        if (err == ESP_OK) {
            break;
        }
    }
    heap_caps_free(jpg);
    return err;
}

static void boot_ui_message(const char *text, int row, uint16_t color)
{
    if (!display_ili9488_is_ready() || !text) {
        return;
    }
    int width = display_ili9488_get_width();
    int y = 26 + row * 28;
    int h = 24;
    uint16_t *buf = heap_caps_malloc((size_t)width * h * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (!buf) {
        return;
    }
    for (int i = 0; i < width * h; i++) {
        buf[i] = 0x1082;
    }
    (void)ui_font_draw_text_maxw(buf, width, h, 12, 4, text, 1, color, width - 24);
    (void)display_ili9488_draw_bitmap565(0, y, width, h, buf);
    heap_caps_free(buf);
}

static void boot_ui_progress(uint8_t percent, uint16_t color)
{
    if (!display_ili9488_is_ready()) {
        return;
    }
    uint16_t width = display_ili9488_get_width();
    uint16_t height = display_ili9488_get_height();
    uint16_t bar_h = height > 240 ? 12 : 8;
    uint16_t fill_w = (uint16_t)(((uint32_t)width * percent) / 100);
    display_ili9488_fill_rect(0, height - bar_h, width, bar_h, 0x2104);
    if (fill_w > 0) {
        display_ili9488_fill_rect(0, height - bar_h, fill_w, bar_h, color);
    }
}

static void boot_ui_start(const app_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    (void)display_ili9488_set_brightness(cfg->display_brightness);
    esp_err_t err = display_ili9488_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Boot UI display init skipped: %s", esp_err_to_name(err));
        return;
    }

    uint16_t width = display_ili9488_get_width();
    uint16_t height = display_ili9488_get_height();
    bool image_shown = false;
    if (cfg->boot_anim_path[0]) {
        ESP_LOGI(TAG, "Boot animation selected: %s", cfg->boot_anim_path);
        ESP_LOGW(TAG, "GIF/WebP boot animation playback is not implemented yet");
    }
    if (cfg->boot_image_path[0]) {
        esp_err_t sd_err = storage_sd_init();
        if (sd_err == ESP_OK) {
            esp_err_t img_err = boot_ui_draw_jpeg_file(cfg->boot_image_path);
            image_shown = img_err == ESP_OK;
            if (img_err != ESP_OK) {
                ESP_LOGW(TAG, "Boot image not rendered from %s: %s",
                         cfg->boot_image_path, esp_err_to_name(img_err));
            }
        } else {
            ESP_LOGW(TAG, "Boot image skipped, SD not mounted: %s", esp_err_to_name(sd_err));
        }
    }
    if (!image_shown) {
        (void)display_ili9488_clear(0x0000);
        (void)display_ili9488_fill_rect(0, 0, width, 2, 0xFFFF);
        (void)display_ili9488_fill_rect(0, height - 2, width, 2, 0xFFFF);
    }
    boot_ui_message("Part-DB Terminal", 0, 0xFFFF);
    boot_ui_message("启动硬件流程检查", 1, 0xBDF7);
    boot_ui_progress(10, 0xFFFF);
}

static void boot_ui_wait_until(int64_t boot_start_us)
{
    int64_t elapsed_ms = (esp_timer_get_time() - boot_start_us) / 1000;
    if (elapsed_ms >= BOARD_BOOT_MIN_MS) {
        return;
    }
    boot_ui_message("启动检查完成", 6, 0xFFFF);
    vTaskDelay(pdMS_TO_TICKS((uint32_t)(BOARD_BOOT_MIN_MS - elapsed_ms)));
}

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static void confirm_pending_ota(bool boot_healthy)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    if (!boot_healthy) {
        ESP_LOGE(TAG, "OTA image remains pending because required services failed");
        return;
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA image confirmed valid");
    } else {
        ESP_LOGE(TAG, "Failed to confirm OTA image: %s", esp_err_to_name(err));
    }
}

void app_main(void)
{
    int64_t boot_start_us = esp_timer_get_time();
    bool boot_healthy = true;
    ESP_LOGI(TAG, "Starting ESP32 Part-DB terminal");
    init_nvs();
    ESP_LOGI(TAG, "Memory psram_chip=%u psram_total=%u psram_free=%u internal_free=%u internal_largest=%u",
             (unsigned)esp_psram_get_size(),
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    psram_diag_log_boot();
    if (BOARD_NFC_SHARES_CAMERA_SCCB) {
        ESP_LOGW(TAG, "NFC pins currently overlap camera SCCB: NFC SDA/SCL=%d/%d camera SIOD/SIOC=%d/%d",
                 BOARD_NFC_SDA_GPIO, BOARD_NFC_SCL_GPIO,
                 BOARD_CAM_SIOD_GPIO, BOARD_CAM_SIOC_GPIO);
    } else if (BOARD_NFC_SHARES_TOUCH_I2C) {
        ESP_LOGI(TAG, "NFC shares touch I2C: SDA/SCL=%d/%d addr=0x%02x",
                 BOARD_NFC_SDA_GPIO, BOARD_NFC_SCL_GPIO, BOARD_NFC_PN532_ADDR);
    } else if (BOARD_NFC_USE_UART_HSU) {
        ESP_LOGI(TAG, "NFC uses isolated PN532 HSU/UART: tx=%d rx=%d baud=%d",
                 BOARD_NFC_UART_TX_GPIO, BOARD_NFC_UART_RX_GPIO, BOARD_NFC_UART_BAUD);
    } else if (BOARD_NFC_USE_SPI) {
        ESP_LOGI(TAG, "NFC uses PN532 SPI: sclk=%d mosi=%d miso=%d cs=%d hz=%d",
                 BOARD_NFC_SPI_SCLK_GPIO, BOARD_NFC_SPI_MOSI_GPIO,
                 BOARD_NFC_SPI_MISO_GPIO, BOARD_NFC_SPI_CS_GPIO, BOARD_NFC_SPI_HZ);
    }
    prepare_nfc_spi_chip_select();

    ESP_ERROR_CHECK(peripheral_arbiter_init());
    ESP_ERROR_CHECK(app_config_load(&s_cfg));
    ui_font_set_active_path(s_cfg.font_path);
    esp_err_t err = button_input_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Button service not started: %s", esp_err_to_name(err));
    }
    (void)display_ili9488_configure(s_cfg.display_driver, s_cfg.display_width,
                                    s_cfg.display_height, s_cfg.display_orientation,
                                    s_cfg.display_flip);
    boot_ui_start(&s_cfg);
    ESP_ERROR_CHECK(partdb_client_init(&s_cfg));
    boot_ui_message("配置和缓存策略就绪", 2, 0xFFFF);
    boot_ui_progress(18, 0xFFFF);

#if BOARD_BOOT_HARDWARE_CHECK_AT_BOOT || BOARD_HARDWARE_DIAG_AT_BOOT
    boot_ui_message("检查摄像头/NFC/触摸/TF", 3, 0xBDF7);
    boot_ui_progress(30, 0xFFFF);
    err = hardware_diag_run(&s_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Boot hardware check failed to run: %s", esp_err_to_name(err));
    } else {
        hardware_diag_status_t diag = hardware_diag_get_status();
        ESP_LOGI(TAG, "Boot hardware check completed camera=%s nfc=%s touch=%s tf=%s",
                 esp_err_to_name(diag.camera_err), esp_err_to_name(diag.nfc_err),
                 esp_err_to_name(diag.touch_err), esp_err_to_name(diag.sd_err));
    }
    boot_ui_progress(62, err == ESP_OK ? 0xFFFF : 0xBDF7);
#endif

#if BOARD_NFC_SERVICE_AT_BOOT
    boot_ui_message("启动 NFC 常驻服务", 4, 0xBDF7);
    err = nfc_service_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NFC service not started: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "NFC service queued");
    }
    boot_ui_progress(70, err == ESP_OK ? 0xFFFF : 0xBDF7);
#else
    ESP_LOGI(TAG, "NFC background service skipped at boot; manual NFC requests initialize PN532 on demand");
#endif

    boot_ui_message("启动 WiFi 和后台", 5, 0xBDF7);
    err = wifi_portal_init(&s_cfg);
    if (err != ESP_OK) {
        boot_healthy = false;
        ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(err));
    }
    boot_ui_progress(84, err == ESP_OK ? 0xFFFF : 0xBDF7);

    err = http_server_start(&s_cfg);
    if (err != ESP_OK) {
        boot_healthy = false;
        ESP_LOGE(TAG, "HTTP server failed: %s", esp_err_to_name(err));
    }
    boot_ui_progress(92, err == ESP_OK ? 0xFFFF : 0xBDF7);

#if BOARD_CAMERA_PREWARM_AT_BOOT
    err = camera_mgr_prewarm_async();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Camera prewarm not started: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Camera prewarm queued");
    }
#endif

#if BOARD_INIT_OPTIONAL_HARDWARE_AT_BOOT && !BOARD_HARDWARE_DIAG_AT_BOOT
    esp_err_t hw_err;

    hw_err = storage_sd_init();
    if (hw_err != ESP_OK) {
        ESP_LOGW(TAG, "SD disabled until card/wiring is ready: %s", esp_err_to_name(hw_err));
    }

    hw_err = display_ili9488_init();
    if (hw_err != ESP_OK) {
        ESP_LOGW(TAG, "Display disabled until wiring is ready: %s", esp_err_to_name(hw_err));
    }

    hw_err = touch_ft6336_init();
    if (hw_err != ESP_OK) {
        ESP_LOGW(TAG, "Touch disabled until wiring is ready: %s", esp_err_to_name(hw_err));
    }

    hw_err = nfc_pn532_init();
    if (hw_err != ESP_OK) {
        ESP_LOGW(TAG, "NFC disabled until wiring/module is ready: %s", esp_err_to_name(hw_err));
    }
#elif !BOARD_HARDWARE_DIAG_AT_BOOT
    ESP_LOGI(TAG, "Other optional hardware init skipped at boot; use web diagnostics to probe peripherals");
#endif
#if !BOARD_HARDWARE_DIAG_AT_BOOT
    boot_ui_progress(100, 0xFFFF);
#endif
    boot_ui_wait_until(boot_start_us);
    err = device_ui_start(&s_cfg);
    if (err != ESP_OK) {
        boot_healthy = false;
        ESP_LOGW(TAG, "Device UI not started: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Device UI started");
    }
    confirm_pending_ota(boot_healthy);
    ESP_LOGI(TAG, "Boot complete");
}
