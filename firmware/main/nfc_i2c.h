#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t nfc_i2c_init(void);
esp_err_t nfc_i2c_probe(unsigned timeout_ms);
esp_err_t nfc_i2c_write(uint8_t addr, const uint8_t *data, size_t len, unsigned timeout_ms);
esp_err_t nfc_i2c_read(uint8_t addr, uint8_t *data, size_t len, unsigned timeout_ms);
esp_err_t nfc_i2c_write_read(uint8_t addr, const uint8_t *wr, size_t wr_len,
                             uint8_t *rd, size_t rd_len, unsigned timeout_ms);
esp_err_t nfc_i2c_recover(unsigned timeout_ms);
const char *nfc_i2c_backend(void);
