#pragma once

#include <stdbool.h>
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
esp_err_t nfc_i2c_release(void);
bool nfc_i2c_has_i2c_status_byte(void);
bool nfc_i2c_requires_ready_poll(void);
bool nfc_i2c_is_spi(void);
esp_err_t nfc_i2c_configure_spi_variant(uint8_t mode, bool lsb_first);
const char *nfc_i2c_spi_variant(void);
esp_err_t nfc_i2c_read_status(uint8_t *status, unsigned timeout_ms);
const char *nfc_i2c_backend(void);
