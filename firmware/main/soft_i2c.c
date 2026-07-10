#include "soft_i2c.h"

#include <stdbool.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "soft_i2c";

static soft_i2c_config_t s_cfg;
static SemaphoreHandle_t s_mutex;
static bool s_ready;
static uint32_t s_half_period_us = 10;

static void delay_half(void)
{
    esp_rom_delay_us(s_half_period_us);
}

static void set_sda(int level)
{
    if (level) {
        (void)gpio_set_direction(s_cfg.sda_gpio, GPIO_MODE_INPUT);
        (void)gpio_pullup_en(s_cfg.sda_gpio);
    } else {
        gpio_set_level(s_cfg.sda_gpio, 0);
        (void)gpio_set_direction(s_cfg.sda_gpio, GPIO_MODE_OUTPUT_OD);
    }
}

static void set_scl(int level)
{
    if (level) {
        (void)gpio_set_direction(s_cfg.scl_gpio, GPIO_MODE_INPUT);
        (void)gpio_pullup_en(s_cfg.scl_gpio);
    } else {
        gpio_set_level(s_cfg.scl_gpio, 0);
        (void)gpio_set_direction(s_cfg.scl_gpio, GPIO_MODE_OUTPUT_OD);
    }
}

static esp_err_t wait_scl_high(unsigned timeout_ms)
{
    uint32_t timeout_us = timeout_ms > 0 ? timeout_ms * 1000U : 1000U;
    uint32_t waited = 0;
    while (!gpio_get_level(s_cfg.scl_gpio)) {
        if (waited >= timeout_us) {
            return ESP_ERR_TIMEOUT;
        }
        esp_rom_delay_us(2);
        waited += 2;
    }
    return ESP_OK;
}

static esp_err_t clock_high(unsigned timeout_ms)
{
    set_scl(1);
    esp_err_t err = wait_scl_high(timeout_ms);
    delay_half();
    return err;
}

static esp_err_t lock_bus(unsigned timeout_ms)
{
    TickType_t ticks = timeout_ms == 0 ? 0 : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(s_mutex, ticks) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void i2c_start(unsigned timeout_ms)
{
    set_sda(1);
    set_scl(1);
    (void)wait_scl_high(timeout_ms);
    delay_half();
    set_sda(0);
    delay_half();
    set_scl(0);
    delay_half();
}

static void i2c_stop(unsigned timeout_ms)
{
    set_sda(0);
    delay_half();
    set_scl(1);
    (void)wait_scl_high(timeout_ms);
    delay_half();
    set_sda(1);
    delay_half();
}

static esp_err_t write_bit(bool bit, unsigned timeout_ms)
{
    set_sda(bit ? 1 : 0);
    delay_half();
    ESP_RETURN_ON_ERROR(clock_high(timeout_ms), TAG, "clock stretch timeout");
    set_scl(0);
    delay_half();
    return ESP_OK;
}

static esp_err_t read_bit(bool *bit, unsigned timeout_ms)
{
    if (!bit) {
        return ESP_ERR_INVALID_ARG;
    }
    set_sda(1);
    delay_half();
    ESP_RETURN_ON_ERROR(clock_high(timeout_ms), TAG, "clock stretch timeout");
    *bit = gpio_get_level(s_cfg.sda_gpio) != 0;
    set_scl(0);
    delay_half();
    return ESP_OK;
}

static esp_err_t write_byte(uint8_t byte, unsigned timeout_ms)
{
    for (int bit = 7; bit >= 0; bit--) {
        ESP_RETURN_ON_ERROR(write_bit((byte >> bit) & 0x01, timeout_ms), TAG, "write bit failed");
    }

    bool nack = true;
    ESP_RETURN_ON_ERROR(read_bit(&nack, timeout_ms), TAG, "read ack failed");
    return nack ? ESP_ERR_NOT_FOUND : ESP_OK;
}

static esp_err_t read_byte(uint8_t *byte, bool ack, unsigned timeout_ms)
{
    if (!byte) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value = 0;
    for (int bit = 7; bit >= 0; bit--) {
        bool one = false;
        ESP_RETURN_ON_ERROR(read_bit(&one, timeout_ms), TAG, "read bit failed");
        if (one) {
            value |= (uint8_t)(1U << bit);
        }
    }
    ESP_RETURN_ON_ERROR(write_bit(!ack, timeout_ms), TAG, "write ack failed");
    *byte = value;
    return ESP_OK;
}

static esp_err_t write_address(uint8_t addr, bool read, unsigned timeout_ms)
{
    return write_byte((uint8_t)((addr << 1) | (read ? 1 : 0)), timeout_ms);
}

esp_err_t soft_i2c_init(const soft_i2c_config_t *cfg)
{
    if (!cfg || cfg->sda_gpio == GPIO_NUM_NC || cfg->scl_gpio == GPIO_NUM_NC ||
        cfg->sda_gpio == cfg->scl_gpio) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ready && s_cfg.sda_gpio == cfg->sda_gpio && s_cfg.scl_gpio == cfg->scl_gpio &&
        s_cfg.hz == cfg->hz) {
        return ESP_OK;
    }

    s_cfg = *cfg;
    if (s_cfg.hz == 0) {
        s_cfg.hz = 50000;
    }
    s_half_period_us = 1000000U / (s_cfg.hz * 2U);
    if (s_half_period_us < 2) {
        s_half_period_us = 2;
    }

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << s_cfg.sda_gpio) | (1ULL << s_cfg.scl_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio config failed");
    set_sda(1);
    set_scl(1);

    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    s_ready = true;
    ESP_LOGI(TAG, "software I2C ready SDA=%d SCL=%d hz=%lu",
             s_cfg.sda_gpio, s_cfg.scl_gpio, (unsigned long)s_cfg.hz);
    return ESP_OK;
}

esp_err_t soft_i2c_probe(uint8_t addr, unsigned timeout_ms)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(lock_bus(timeout_ms), TAG, "lock failed");
    i2c_start(timeout_ms);
    esp_err_t err = write_address(addr, false, timeout_ms);
    i2c_stop(timeout_ms);
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t soft_i2c_write(uint8_t addr, const uint8_t *data, size_t len, unsigned timeout_ms)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len > 0 && !data) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(lock_bus(timeout_ms), TAG, "lock failed");
    i2c_start(timeout_ms);
    esp_err_t err = write_address(addr, false, timeout_ms);
    for (size_t i = 0; err == ESP_OK && i < len; i++) {
        err = write_byte(data[i], timeout_ms);
    }
    i2c_stop(timeout_ms);
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t soft_i2c_read(uint8_t addr, uint8_t *data, size_t len, unsigned timeout_ms)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(lock_bus(timeout_ms), TAG, "lock failed");
    i2c_start(timeout_ms);
    esp_err_t err = write_address(addr, true, timeout_ms);
    for (size_t i = 0; err == ESP_OK && i < len; i++) {
        err = read_byte(&data[i], i + 1 < len, timeout_ms);
    }
    i2c_stop(timeout_ms);
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t soft_i2c_write_read(uint8_t addr, const uint8_t *wr, size_t wr_len,
                              uint8_t *rd, size_t rd_len, unsigned timeout_ms)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!wr || wr_len == 0 || !rd || rd_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(lock_bus(timeout_ms), TAG, "lock failed");
    i2c_start(timeout_ms);
    esp_err_t err = write_address(addr, false, timeout_ms);
    for (size_t i = 0; err == ESP_OK && i < wr_len; i++) {
        err = write_byte(wr[i], timeout_ms);
    }
    if (err == ESP_OK) {
        i2c_start(timeout_ms);
        err = write_address(addr, true, timeout_ms);
    }
    for (size_t i = 0; err == ESP_OK && i < rd_len; i++) {
        err = read_byte(&rd[i], i + 1 < rd_len, timeout_ms);
    }
    i2c_stop(timeout_ms);
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t soft_i2c_recover(unsigned timeout_ms)
{
    if (!s_ready || !s_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = lock_bus(timeout_ms);
    if (err != ESP_OK) {
        return err;
    }

    set_sda(1);
    set_scl(1);
    err = wait_scl_high(timeout_ms);
    delay_half();

    for (int i = 0; err == ESP_OK && i < 9 && !gpio_get_level(s_cfg.sda_gpio); i++) {
        set_scl(0);
        delay_half();
        set_scl(1);
        err = wait_scl_high(timeout_ms);
        delay_half();
    }

    i2c_stop(timeout_ms);
    if (err == ESP_OK && (!gpio_get_level(s_cfg.scl_gpio) || !gpio_get_level(s_cfg.sda_gpio))) {
        err = ESP_ERR_TIMEOUT;
    }

    xSemaphoreGive(s_mutex);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "software I2C recovery failed: %s", esp_err_to_name(err));
    }
    return err;
}
