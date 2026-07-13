#include "display_ili9488.h"

#include <stdbool.h>
#include <string.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ili9488";
static spi_device_handle_t s_lcd;
static bool s_ready;
static bool s_backlight_ready;
static bool s_awake = true;
static uint8_t s_brightness = 80;
static char s_driver[16] = "ili9488";
static char s_orientation[12] = "portrait";
static bool s_flip = true;
static uint16_t s_width = 320;
static uint16_t s_height = 480;
static SemaphoreHandle_t s_draw_mutex;
static uint8_t *s_tx_buf;
static size_t s_tx_buf_len;

#define LCD_BL_LEDC_MODE       LEDC_LOW_SPEED_MODE
#define LCD_BL_LEDC_TIMER      LEDC_TIMER_1
#define LCD_BL_LEDC_CHANNEL    LEDC_CHANNEL_1
#define LCD_BL_LEDC_RES        LEDC_TIMER_10_BIT
#define LCD_BL_LEDC_MAX_DUTY   1023
#define LCD_BL_LEDC_HZ         50000
#define LCD_TX_CHUNK_ROWS      8

static esp_err_t backlight_init(void)
{
    if (s_backlight_ready || BOARD_LCD_BL_GPIO == GPIO_NUM_NC) {
        return ESP_OK;
    }

    ledc_timer_config_t timer = {
        .speed_mode = LCD_BL_LEDC_MODE,
        .duty_resolution = LCD_BL_LEDC_RES,
        .timer_num = LCD_BL_LEDC_TIMER,
        .freq_hz = LCD_BL_LEDC_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer failed");

    ledc_channel_config_t channel = {
        .gpio_num = BOARD_LCD_BL_GPIO,
        .speed_mode = LCD_BL_LEDC_MODE,
        .channel = LCD_BL_LEDC_CHANNEL,
        .timer_sel = LCD_BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "backlight channel failed");
    s_backlight_ready = true;
    return ESP_OK;
}

static esp_err_t apply_backlight(void)
{
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight init failed");
    if (BOARD_LCD_BL_GPIO == GPIO_NUM_NC) {
        return ESP_OK;
    }

    uint8_t percent = s_awake ? s_brightness : 0;
    uint32_t duty = ((uint32_t)LCD_BL_LEDC_MAX_DUTY * percent) / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LCD_BL_LEDC_MODE, LCD_BL_LEDC_CHANNEL, duty),
                        TAG, "backlight duty failed");
    return ledc_update_duty(LCD_BL_LEDC_MODE, LCD_BL_LEDC_CHANNEL);
}

static esp_err_t lcd_tx(const uint8_t *data, int len)
{
    if (!s_lcd || !data || len <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    return spi_device_polling_transmit(s_lcd, &t);
}

static esp_err_t lcd_cmd(uint8_t cmd)
{
    gpio_set_level(BOARD_LCD_DC_GPIO, 0);
    return lcd_tx(&cmd, 1);
}

static esp_err_t lcd_data(const uint8_t *data, int len)
{
    gpio_set_level(BOARD_LCD_DC_GPIO, 1);
    return lcd_tx(data, len);
}

static esp_err_t lcd_cmd_data(uint8_t cmd, const uint8_t *data, int len)
{
    ESP_RETURN_ON_ERROR(lcd_cmd(cmd), TAG, "cmd failed");
    if (data && len > 0) {
        ESP_RETURN_ON_ERROR(lcd_data(data, len), TAG, "data failed");
    }
    return ESP_OK;
}

static bool driver_supported(void)
{
    return strcmp(s_driver, "ili9488") == 0;
}

static esp_err_t ensure_draw_mutex(void)
{
    if (!s_draw_mutex) {
        s_draw_mutex = xSemaphoreCreateMutex();
        if (!s_draw_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static esp_err_t lock_lcd(void)
{
    ESP_RETURN_ON_ERROR(ensure_draw_mutex(), TAG, "draw mutex failed");
    return xSemaphoreTake(s_draw_mutex, pdMS_TO_TICKS(5000)) == pdTRUE ?
           ESP_OK : ESP_ERR_TIMEOUT;
}

static void unlock_lcd(void)
{
    if (s_draw_mutex) {
        xSemaphoreGive(s_draw_mutex);
    }
}

static uint8_t madctl_for_orientation(void)
{
    if (strcmp(s_orientation, "landscape") == 0) {
        return s_flip ? 0xA8 : 0x28;
    }
    return s_flip ? 0xC8 : 0x08;
}

static esp_err_t apply_orientation(void)
{
    uint8_t madctl = madctl_for_orientation();
    return lcd_cmd_data(0x36, &madctl, 1);
}

static esp_err_t apply_orientation_locked(void)
{
    ESP_RETURN_ON_ERROR(lock_lcd(), TAG, "lcd lock failed");
    esp_err_t err = apply_orientation();
    unlock_lcd();
    return err;
}

static esp_err_t lcd_cmd_locked(uint8_t cmd)
{
    ESP_RETURN_ON_ERROR(lock_lcd(), TAG, "lcd lock failed");
    esp_err_t err = lcd_cmd(cmd);
    unlock_lcd();
    return err;
}

static void rgb565_to_rgb666_bytes(uint16_t rgb565, uint8_t out[3])
{
    uint8_t r5 = (rgb565 >> 11) & 0x1f;
    uint8_t g6 = (rgb565 >> 5) & 0x3f;
    uint8_t b5 = rgb565 & 0x1f;
    uint8_t r6 = (r5 << 1) | (r5 >> 4);
    uint8_t b6 = (b5 << 1) | (b5 >> 4);
    out[0] = r6 << 2;
    out[1] = g6 << 2;
    out[2] = b6 << 2;
}

static esp_err_t ensure_draw_resources(void)
{
    ESP_RETURN_ON_ERROR(ensure_draw_mutex(), TAG, "draw mutex failed");

    size_t needed = (size_t)s_width * LCD_TX_CHUNK_ROWS * 3;
    if (needed == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_tx_buf && s_tx_buf_len >= needed) {
        return ESP_OK;
    }

    uint8_t *buf = heap_caps_malloc(needed, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) {
        ESP_LOGE(TAG, "display DMA buffer alloc failed bytes=%u internal_largest=%u",
                 (unsigned)needed,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        return ESP_ERR_NO_MEM;
    }
    if (s_tx_buf) {
        heap_caps_free(s_tx_buf);
    }
    s_tx_buf = buf;
    s_tx_buf_len = needed;
    return ESP_OK;
}

esp_err_t display_ili9488_configure(const char *driver, uint16_t width, uint16_t height,
                                    const char *orientation, bool flip)
{
    if (driver && driver[0] && strcmp(driver, "ili9488") != 0) {
        ESP_LOGW(TAG, "display driver %s is not supported", driver);
        return ESP_ERR_NOT_SUPPORTED;
    }
    snprintf(s_driver, sizeof(s_driver), "ili9488");
    if (orientation && strcmp(orientation, "landscape") == 0) {
        snprintf(s_orientation, sizeof(s_orientation), "landscape");
    } else {
        snprintf(s_orientation, sizeof(s_orientation), "portrait");
    }
    s_flip = flip;
    if (width >= 64 && width <= 1024) {
        s_width = width;
    }
    if (height >= 64 && height <= 1024) {
        s_height = height;
    }

    if (s_ready) {
        ESP_RETURN_ON_ERROR(apply_orientation_locked(), TAG, "orientation failed");
        return display_ili9488_clear(0x0000);
    }
    return ESP_OK;
}

static esp_err_t lcd_reset(void)
{
    if (BOARD_LCD_RST_GPIO == GPIO_NUM_NC) {
        return ESP_OK;
    }
    gpio_set_level(BOARD_LCD_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_LCD_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

static esp_err_t lcd_set_window(int x, int y, int w, int h)
{
    uint16_t x0 = (uint16_t)x;
    uint16_t y0 = (uint16_t)y;
    uint16_t x1 = (uint16_t)(x + w - 1);
    uint16_t y1 = (uint16_t)(y + h - 1);
    uint8_t col[] = {x0 >> 8, x0 & 0xff, x1 >> 8, x1 & 0xff};
    uint8_t row[] = {y0 >> 8, y0 & 0xff, y1 >> 8, y1 & 0xff};
    ESP_RETURN_ON_ERROR(lcd_cmd_data(0x2A, col, sizeof(col)), TAG, "set col failed");
    ESP_RETURN_ON_ERROR(lcd_cmd_data(0x2B, row, sizeof(row)), TAG, "set row failed");
    ESP_RETURN_ON_ERROR(lcd_cmd(0x2C), TAG, "ramwr failed");
    gpio_set_level(BOARD_LCD_DC_GPIO, 1);
    return ESP_OK;
}

esp_err_t display_ili9488_init(void)
{
    if (!driver_supported()) {
        ESP_LOGW(TAG, "display driver %s is not supported by this firmware", s_driver);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_ready) {
        return ESP_OK;
    }

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << BOARD_LCD_DC_GPIO) |
                        (1ULL << BOARD_LCD_RST_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out), TAG, "gpio config failed");
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight init failed");

    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_LCD_MOSI_GPIO,
        .miso_io_num = BOARD_LCD_MISO_GPIO,
        .sclk_io_num = BOARD_LCD_SCLK_GPIO,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = (s_width > s_height ? s_width : s_height) * 80 * 3,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO),
                        TAG, "spi bus init failed");

    spi_device_interface_config_t dev = {
        .clock_speed_hz = BOARD_LCD_SPI_HZ,
        .mode = 0,
        .spics_io_num = BOARD_LCD_CS_GPIO,
        .queue_size = 4,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(BOARD_LCD_SPI_HOST, &dev, &s_lcd),
                        TAG, "spi add device failed");

    ESP_RETURN_ON_ERROR(lcd_reset(), TAG, "reset failed");

    const struct {
        uint8_t cmd;
        uint8_t data[16];
        uint8_t len;
        uint16_t delay_ms;
    } seq[] = {
        {0xF7, {0xA9, 0x51, 0x2C, 0x82}, 4, 0},
        {0xEC, {0x00, 0x02, 0x03, 0x7A}, 4, 0},
        {0xC0, {0x13, 0x13}, 2, 0},
        {0xC1, {0x41}, 1, 0},
        {0xC5, {0x00, 0x28, 0x80}, 3, 0},
        {0xB1, {0xB0, 0x11}, 2, 0},
        {0xB4, {0x02}, 1, 0},
        {0xB6, {0x02, 0x22}, 2, 0},
        {0xB7, {0xC6}, 1, 0},
        {0xBE, {0x00, 0x04}, 2, 0},
        {0xE9, {0x00}, 1, 0},
        {0xF4, {0x00, 0x00, 0x0F}, 3, 0},
        {0xE0, {0x00, 0x04, 0x0E, 0x08, 0x17, 0x0A, 0x40, 0x79, 0x4D, 0x07, 0x0E, 0x0A, 0x1A, 0x1D, 0x0F}, 15, 0},
        {0xE1, {0x00, 0x1B, 0x1F, 0x02, 0x10, 0x05, 0x32, 0x34, 0x43, 0x02, 0x0A, 0x09, 0x33, 0x37, 0x0F}, 15, 0},
        {0xF4, {0x00, 0x00, 0x0F}, 3, 0},
        {0x3A, {0x66}, 1, 0},
        {0x21, {0}, 0, 0},
        {0x11, {0}, 0, 120},
        {0x29, {0}, 0, 20},
    };

    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        ESP_RETURN_ON_ERROR(lcd_cmd_data(seq[i].cmd, seq[i].data, seq[i].len),
                            TAG, "init seq failed");
        if (seq[i].delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(seq[i].delay_ms));
        }
    }
    ESP_RETURN_ON_ERROR(apply_orientation(), TAG, "orientation failed");

    s_ready = true;
    s_awake = true;
    ESP_LOGI(TAG, "ILI9488 ready %ux%u %s", s_width, s_height, s_orientation);
    ESP_RETURN_ON_ERROR(apply_backlight(), TAG, "backlight set failed");
    return display_ili9488_clear(0x0000);
}

esp_err_t display_ili9488_fill_rect(int x, int y, int w, int h, uint16_t rgb565)
{
    if (!s_ready || w <= 0 || h <= 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > s_width) {
        w = s_width - x;
    }
    if (y + h > s_height) {
        h = s_height - y;
    }
    if (w <= 0 || h <= 0) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(ensure_draw_resources(), TAG, "draw resources failed");
    if (xSemaphoreTake(s_draw_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = lcd_set_window(x, y, w, h);
    if (err != ESP_OK) {
        xSemaphoreGive(s_draw_mutex);
        ESP_LOGE(TAG, "window failed: %s", esp_err_to_name(err));
        return err;
    }

    int pixels_per_chunk = (int)(s_tx_buf_len / 3);
    uint8_t color[3];
    rgb565_to_rgb666_bytes(rgb565, color);
    for (int i = 0; i < pixels_per_chunk; i++) {
        s_tx_buf[i * 3] = color[0];
        s_tx_buf[i * 3 + 1] = color[1];
        s_tx_buf[i * 3 + 2] = color[2];
    }

    int remaining = w * h;
    while (remaining > 0 && err == ESP_OK) {
        int chunk = remaining > pixels_per_chunk ? pixels_per_chunk : remaining;
        err = lcd_tx(s_tx_buf, chunk * 3);
        remaining -= chunk;
    }
    xSemaphoreGive(s_draw_mutex);
    return err;
}

esp_err_t display_ili9488_draw_bitmap565(int x, int y, int w, int h, const uint16_t *pixels)
{
    if (!s_ready || !pixels || w <= 0 || h <= 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (x < 0 || y < 0 || x + w > s_width || y + h > s_height) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(ensure_draw_resources(), TAG, "draw resources failed");
    if (xSemaphoreTake(s_draw_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = lcd_set_window(x, y, w, h);
    if (err != ESP_OK) {
        xSemaphoreGive(s_draw_mutex);
        ESP_LOGE(TAG, "window failed: %s", esp_err_to_name(err));
        return err;
    }

    int pixels_per_chunk = (int)(s_tx_buf_len / 3);
    int total = w * h;
    int done = 0;
    while (done < total && err == ESP_OK) {
        int chunk = total - done;
        if (chunk > pixels_per_chunk) {
            chunk = pixels_per_chunk;
        }
        for (int i = 0; i < chunk; i++) {
            uint8_t color[3];
            rgb565_to_rgb666_bytes(pixels[done + i], color);
            s_tx_buf[i * 3] = color[0];
            s_tx_buf[i * 3 + 1] = color[1];
            s_tx_buf[i * 3 + 2] = color[2];
        }
        err = lcd_tx(s_tx_buf, chunk * 3);
        done += chunk;
    }
    xSemaphoreGive(s_draw_mutex);
    return err;
}

esp_err_t display_ili9488_clear(uint16_t rgb565)
{
    return display_ili9488_fill_rect(0, 0, s_width, s_height, rgb565);
}

bool display_ili9488_is_ready(void)
{
    return s_ready;
}

esp_err_t display_ili9488_set_brightness(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    s_brightness = percent;
    return apply_backlight();
}

esp_err_t display_ili9488_set_awake(bool awake)
{
    if (awake && !s_ready) {
        return display_ili9488_init();
    }

    if (s_ready && s_awake != awake) {
        esp_err_t err = lcd_cmd_locked(awake ? 0x29 : 0x28);
        if (err != ESP_OK) {
            return err;
        }
    }
    s_awake = awake;
    return apply_backlight();
}

uint8_t display_ili9488_get_brightness(void)
{
    return s_brightness;
}

uint16_t display_ili9488_get_width(void)
{
    return s_width;
}

uint16_t display_ili9488_get_height(void)
{
    return s_height;
}

const char *display_ili9488_get_driver(void)
{
    return s_driver;
}

const char *display_ili9488_get_orientation(void)
{
    return s_orientation;
}

bool display_ili9488_get_flip(void)
{
    return s_flip;
}

bool display_ili9488_is_awake(void)
{
    return s_awake;
}
