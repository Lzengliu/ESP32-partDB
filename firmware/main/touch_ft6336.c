#include "touch_ft6336.h"

#include "board_config.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "soft_i2c.h"

static const char *TAG = "touch";
static bool s_ready;
static bool s_disabled_logged;
static TickType_t s_next_scan_log_tick;

#define FT_REG_NUM_FINGER 0x02
#define FT_TP1_REG        0x03
#define TOUCH_SCAN_LOG_INTERVAL_MS 30000

static uint64_t gpio_pin_mask_or_zero(gpio_num_t gpio)
{
    return gpio == GPIO_NUM_NC ? 0 : (1ULL << gpio);
}

static void touch_i2c_delay(void)
{
    esp_rom_delay_us(8);
}

static void touch_i2c_release_line(gpio_num_t gpio)
{
    if (gpio == GPIO_NUM_NC) {
        return;
    }
    (void)gpio_set_direction(gpio, GPIO_MODE_INPUT);
    (void)gpio_pullup_en(gpio);
}

static void touch_i2c_drive_low(gpio_num_t gpio)
{
    if (gpio == GPIO_NUM_NC) {
        return;
    }
    gpio_set_level(gpio, 0);
    (void)gpio_set_direction(gpio, GPIO_MODE_OUTPUT_OD);
}

static int gpio_level_or_nc(gpio_num_t gpio)
{
    return gpio == GPIO_NUM_NC ? -1 : gpio_get_level(gpio);
}

static esp_err_t touch_i2c_recover_lines(const char *reason)
{
    if (BOARD_TOUCH_SDA_GPIO == GPIO_NUM_NC || BOARD_TOUCH_SCL_GPIO == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "touch I2C recovery skipped reason=%s SDA=%d SCL=%d",
                 reason ? reason : "?", BOARD_TOUCH_SDA_GPIO, BOARD_TOUCH_SCL_GPIO);
        return ESP_ERR_INVALID_STATE;
    }
    gpio_config_t io = {
        .pin_bit_mask = gpio_pin_mask_or_zero(BOARD_TOUCH_SDA_GPIO) |
                        gpio_pin_mask_or_zero(BOARD_TOUCH_SCL_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "recover gpio config failed");
    touch_i2c_release_line(BOARD_TOUCH_SDA_GPIO);
    touch_i2c_release_line(BOARD_TOUCH_SCL_GPIO);
    touch_i2c_delay();

    for (int i = 0; i < 9 && !gpio_get_level(BOARD_TOUCH_SDA_GPIO); i++) {
        touch_i2c_drive_low(BOARD_TOUCH_SCL_GPIO);
        touch_i2c_delay();
        touch_i2c_release_line(BOARD_TOUCH_SCL_GPIO);
        touch_i2c_delay();
    }

    touch_i2c_drive_low(BOARD_TOUCH_SDA_GPIO);
    touch_i2c_delay();
    touch_i2c_release_line(BOARD_TOUCH_SCL_GPIO);
    touch_i2c_delay();
    touch_i2c_release_line(BOARD_TOUCH_SDA_GPIO);
    touch_i2c_delay();

    int sda = gpio_level_or_nc(BOARD_TOUCH_SDA_GPIO);
    int scl = gpio_level_or_nc(BOARD_TOUCH_SCL_GPIO);
    if (!sda || !scl) {
        ESP_LOGW(TAG, "touch I2C recovery failed reason=%s SDA=%d SCL=%d",
                 reason ? reason : "?", sda, scl);
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "touch I2C recovery succeeded reason=%s", reason ? reason : "?");
    return ESP_OK;
}

static esp_err_t touch_i2c_init_bus(void)
{
#if BOARD_TOUCH_USE_SOFT_I2C
    soft_i2c_config_t cfg = {
        .sda_gpio = BOARD_TOUCH_SDA_GPIO,
        .scl_gpio = BOARD_TOUCH_SCL_GPIO,
        .hz = BOARD_TOUCH_SOFT_I2C_HZ,
    };
    return soft_i2c_init(&cfg);
#else
    return i2c_bus_init();
#endif
}

static esp_err_t touch_i2c_reset_bus(void)
{
#if BOARD_TOUCH_USE_SOFT_I2C
    return soft_i2c_recover(100);
#else
    return i2c_bus_reset();
#endif
}

static esp_err_t touch_i2c_reinstall_bus(void)
{
#if BOARD_TOUCH_USE_SOFT_I2C
    (void)soft_i2c_release();
    return touch_i2c_init_bus();
#else
    return i2c_bus_reinstall();
#endif
}

static esp_err_t touch_i2c_probe_addr(uint8_t addr, unsigned timeout_ms)
{
#if BOARD_TOUCH_USE_SOFT_I2C
    return soft_i2c_probe(addr, timeout_ms);
#else
    return i2c_bus_probe(addr, timeout_ms);
#endif
}

static esp_err_t touch_i2c_write_read_addr(uint8_t addr, const uint8_t *wr, size_t wr_len,
                                           uint8_t *rd, size_t rd_len, unsigned timeout_ms)
{
#if BOARD_TOUCH_USE_SOFT_I2C
    return soft_i2c_write_read(addr, wr, wr_len, rd, rd_len, timeout_ms);
#else
    return i2c_bus_write_read(addr, wr, wr_len, rd, rd_len, timeout_ms);
#endif
}

static void log_touch_i2c_error(const char *op, esp_err_t err)
{
    ESP_LOGW(TAG, "%s failed: %s SDA=%d SCL=%d ready=%d",
             op ? op : "i2c",
             esp_err_to_name(err),
             gpio_level_or_nc(BOARD_TOUCH_SDA_GPIO),
             gpio_level_or_nc(BOARD_TOUCH_SCL_GPIO),
             s_ready);
}

static bool touch_i2c_lines_are_low(void)
{
    return gpio_level_or_nc(BOARD_TOUCH_SDA_GPIO) == 0 || gpio_level_or_nc(BOARD_TOUCH_SCL_GPIO) == 0;
}

static void touch_i2c_scan_log(void)
{
    TickType_t now = xTaskGetTickCount();
    if (s_next_scan_log_tick != 0 && (int32_t)(now - s_next_scan_log_tick) < 0) {
        return;
    }
    s_next_scan_log_tick = now + pdMS_TO_TICKS(TOUCH_SCAN_LOG_INTERVAL_MS);

    char found[96] = {0};
    size_t used = 0;
    int count = 0;

    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        esp_err_t err = touch_i2c_probe_addr(addr, 40);
        if (err == ESP_OK) {
            int n = snprintf(found + used, sizeof(found) - used,
                             "%s0x%02x", count ? "," : "", addr);
            if (n > 0) {
                size_t add = (size_t)n;
                used = add < sizeof(found) - used ? used + add : sizeof(found) - 1;
            }
            count++;
        } else if (err == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "I2C scan timeout at 0x%02x SDA=%d SCL=%d",
                     addr, gpio_level_or_nc(BOARD_TOUCH_SDA_GPIO),
                     gpio_level_or_nc(BOARD_TOUCH_SCL_GPIO));
            break;
        }
    }

    ESP_LOGW(TAG, "I2C scan found %d device(s): %s", count, count ? found : "none");
}

static esp_err_t touch_ft6336_reset_controller(void)
{
    if (BOARD_TOUCH_RST_GPIO != GPIO_NUM_NC) {
        gpio_config_t rst = {
            .pin_bit_mask = gpio_pin_mask_or_zero(BOARD_TOUCH_RST_GPIO),
            .mode = GPIO_MODE_OUTPUT,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&rst), TAG, "rst config failed");
        gpio_set_level(BOARD_TOUCH_RST_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(BOARD_TOUCH_RST_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    return ESP_OK;
}

static esp_err_t touch_ft6336_probe(uint8_t *fingers)
{
    if (!fingers) {
        return ESP_ERR_INVALID_ARG;
    }
    *fingers = 0;
    esp_err_t err = touch_i2c_probe_addr(BOARD_TOUCH_FT6336_ADDR, 100);
    if (err != ESP_OK) {
        return err;
    }
    return touch_i2c_write_read_addr(BOARD_TOUCH_FT6336_ADDR, (uint8_t[]){FT_REG_NUM_FINGER}, 1,
                                     fingers, 1, 100);
}

esp_err_t touch_ft6336_init(void)
{
    if (BOARD_TOUCH_SDA_GPIO == GPIO_NUM_NC || BOARD_TOUCH_SCL_GPIO == GPIO_NUM_NC) {
        s_ready = false;
        if (!s_disabled_logged) {
            ESP_LOGW(TAG, "FT6336 disabled: touch I2C pins are not configured");
            s_disabled_logged = true;
        }
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(touch_i2c_init_bus(), TAG, "i2c init failed");
    ESP_RETURN_ON_ERROR(touch_ft6336_reset_controller(), TAG, "rst config failed");

    if (BOARD_TOUCH_INT_GPIO != GPIO_NUM_NC) {
        gpio_config_t intr = {
            .pin_bit_mask = gpio_pin_mask_or_zero(BOARD_TOUCH_INT_GPIO),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&intr), TAG, "int config failed");
    }

    uint8_t fingers = 0;
    esp_err_t err = touch_ft6336_probe(&fingers);
    if (err != ESP_OK) {
        log_touch_i2c_error("FT6336 probe/read", err);
        touch_i2c_scan_log();
        if (touch_i2c_lines_are_low()) {
            (void)touch_i2c_recover_lines("ft6336 init retry");
            (void)touch_i2c_reinstall_bus();
        } else {
            (void)touch_i2c_reset_bus();
        }
        (void)touch_ft6336_reset_controller();
        err = touch_ft6336_probe(&fingers);
    }
    if (err == ESP_OK) {
        s_ready = true;
        ESP_LOGI(TAG, "FT6336 ready, current points=%u", fingers & 0x0f);
    } else {
        s_ready = false;
        log_touch_i2c_error("FT6336 init retry read", err);
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
    esp_err_t err = touch_i2c_write_read_addr(BOARD_TOUCH_FT6336_ADDR, (uint8_t[]){FT_REG_NUM_FINGER}, 1,
                                              &fingers, 1, 50);
    if (err != ESP_OK) {
        s_ready = false;
        log_touch_i2c_error("FT6336 read fingers", err);
        return err;
    }
    s_ready = true;

    uint8_t count = fingers & 0x0f;
    point->points = count;
    if (count == 0) {
        return ESP_OK;
    }

    uint8_t raw[4] = {0};
    err = touch_i2c_write_read_addr(BOARD_TOUCH_FT6336_ADDR, (uint8_t[]){FT_TP1_REG}, 1,
                                    raw, sizeof(raw), 50);
    if (err != ESP_OK) {
        log_touch_i2c_error("FT6336 read point", err);
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
