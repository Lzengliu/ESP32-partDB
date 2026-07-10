#include "touch_ft6336.h"

#include "board_config.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"

static const char *TAG = "touch";
static bool s_ready;

#define FT_REG_NUM_FINGER 0x02
#define FT_TP1_REG        0x03

esp_err_t touch_ft6336_init(void)
{
    ESP_RETURN_ON_ERROR(i2c_bus_init(), TAG, "i2c init failed");

    if (BOARD_TOUCH_RST_GPIO != GPIO_NUM_NC) {
        gpio_config_t rst = {
            .pin_bit_mask = 1ULL << BOARD_TOUCH_RST_GPIO,
            .mode = GPIO_MODE_OUTPUT,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&rst), TAG, "rst config failed");
        gpio_set_level(BOARD_TOUCH_RST_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(BOARD_TOUCH_RST_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(80));
    }

    if (BOARD_TOUCH_INT_GPIO != GPIO_NUM_NC) {
        gpio_config_t intr = {
            .pin_bit_mask = 1ULL << BOARD_TOUCH_INT_GPIO,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&intr), TAG, "int config failed");
    }

    uint8_t fingers = 0;
    esp_err_t err = i2c_bus_write_read(BOARD_TOUCH_FT6336_ADDR, (uint8_t[]){FT_REG_NUM_FINGER}, 1,
                                       &fingers, 1, 100);
    if (err == ESP_OK) {
        s_ready = true;
        ESP_LOGI(TAG, "FT6336 ready, current points=%u", fingers & 0x0f);
    } else {
        s_ready = false;
    }
    return err;
}

esp_err_t touch_ft6336_read(touch_point_t *point)
{
    if (!point) {
        return ESP_ERR_INVALID_ARG;
    }
    *point = (touch_point_t){0};

    uint8_t fingers = 0;
    esp_err_t err = i2c_bus_write_read(BOARD_TOUCH_FT6336_ADDR, (uint8_t[]){FT_REG_NUM_FINGER}, 1,
                                       &fingers, 1, 50);
    if (err != ESP_OK) {
        s_ready = false;
        return err;
    }
    s_ready = true;

    uint8_t count = fingers & 0x0f;
    point->points = count;
    if (count == 0) {
        return ESP_OK;
    }

    uint8_t raw[4] = {0};
    err = i2c_bus_write_read(BOARD_TOUCH_FT6336_ADDR, (uint8_t[]){FT_TP1_REG}, 1, raw, sizeof(raw), 50);
    if (err != ESP_OK) {
        return err;
    }

    point->touched = true;
    point->x = ((raw[0] & 0x0f) << 8) | raw[1];
    point->y = ((raw[2] & 0x0f) << 8) | raw[3];
    return ESP_OK;
}

static uint16_t scale_coord(uint32_t value, uint16_t in_size, uint16_t out_size)
{
    if (out_size <= 1 || in_size <= 1) {
        return 0;
    }
    if (value >= in_size) {
        value = in_size - 1;
    }
    return (uint16_t)((value * (out_size - 1)) / (in_size - 1));
}

static uint32_t clamp_coord(uint32_t value, uint16_t size)
{
    if (size <= 1) {
        return 0;
    }
    return value >= size ? (uint32_t)(size - 1) : value;
}

void touch_ft6336_transform_to_display(touch_point_t *point, bool swap_xy,
                                       uint16_t raw_width,
                                       uint16_t raw_height,
                                       uint8_t rotation,
                                       bool flip_x, bool flip_y,
                                       uint16_t display_width,
                                       uint16_t display_height)
{
    if (!point || !point->touched) {
        return;
    }

    uint16_t raw_w = raw_width >= 2 ? raw_width : BOARD_TOUCH_RAW_WIDTH;
    uint16_t raw_h = raw_height >= 2 ? raw_height : BOARD_TOUCH_RAW_HEIGHT;
    uint32_t src_x = point->x;
    uint32_t src_y = point->y;
    uint16_t src_w = raw_w;
    uint16_t src_h = raw_h;
    if (swap_xy) {
        uint32_t tmp = src_x;
        src_x = src_y;
        src_y = tmp;
        uint16_t tmp_size = src_w;
        src_w = src_h;
        src_h = tmp_size;
    }

    src_x = clamp_coord(src_x, src_w);
    src_y = clamp_coord(src_y, src_h);
    uint32_t tx = src_x;
    uint32_t ty = src_y;
    uint16_t tx_size = src_w;
    uint16_t ty_size = src_h;
    rotation &= 0x03;

    if (rotation == 1) {
        tx_size = src_h;
        ty_size = src_w;
        tx = src_h - 1 - src_y;
        ty = src_x;
    } else if (rotation == 2) {
        tx = src_w - 1 - src_x;
        ty = src_h - 1 - src_y;
    } else if (rotation == 3) {
        tx_size = src_h;
        ty_size = src_w;
        tx = src_y;
        ty = src_w - 1 - src_x;
    }

    if (flip_x) {
        tx = tx_size - 1 - clamp_coord(tx, tx_size);
    }
    if (flip_y) {
        ty = ty_size - 1 - clamp_coord(ty, ty_size);
    }

    point->x = scale_coord(tx, tx_size, display_width);
    point->y = scale_coord(ty, ty_size, display_height);
}

bool touch_ft6336_is_ready(void)
{
    return s_ready;
}
