#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t i2c_bus_init(void);
esp_err_t i2c_bus_reset(void);
esp_err_t i2c_bus_probe(uint8_t addr, unsigned timeout_ms);
esp_err_t i2c_bus_write(uint8_t addr, const uint8_t *data, size_t len, unsigned timeout_ms);
esp_err_t i2c_bus_read(uint8_t addr, uint8_t *data, size_t len, unsigned timeout_ms);
esp_err_t i2c_bus_write_read(uint8_t addr, const uint8_t *wr, size_t wr_len,
                             uint8_t *rd, size_t rd_len, unsigned timeout_ms);
