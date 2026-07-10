#pragma once

#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

typedef struct {
    gpio_num_t sda_gpio;
    gpio_num_t scl_gpio;
    uint32_t hz;
} soft_i2c_config_t;

esp_err_t soft_i2c_init(const soft_i2c_config_t *cfg);
esp_err_t soft_i2c_probe(uint8_t addr, unsigned timeout_ms);
esp_err_t soft_i2c_write(uint8_t addr, const uint8_t *data, size_t len, unsigned timeout_ms);
esp_err_t soft_i2c_read(uint8_t addr, uint8_t *data, size_t len, unsigned timeout_ms);
esp_err_t soft_i2c_write_read(uint8_t addr, const uint8_t *wr, size_t wr_len,
                              uint8_t *rd, size_t rd_len, unsigned timeout_ms);
esp_err_t soft_i2c_recover(unsigned timeout_ms);
