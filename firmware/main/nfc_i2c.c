#include "nfc_i2c.h"

#include "board_config.h"
#include "driver/gpio.h"
#include "i2c_bus.h"
#include "soft_i2c.h"

static bool use_soft_i2c(void)
{
#if BOARD_NFC_USE_SOFT_I2C
    return BOARD_NFC_SOFT_SDA_GPIO != GPIO_NUM_NC &&
           BOARD_NFC_SOFT_SCL_GPIO != GPIO_NUM_NC &&
           BOARD_NFC_SOFT_SDA_GPIO != BOARD_NFC_SOFT_SCL_GPIO;
#else
    return false;
#endif
}

esp_err_t nfc_i2c_init(void)
{
    if (use_soft_i2c()) {
        soft_i2c_config_t cfg = {
            .sda_gpio = BOARD_NFC_SOFT_SDA_GPIO,
            .scl_gpio = BOARD_NFC_SOFT_SCL_GPIO,
            .hz = BOARD_NFC_SOFT_I2C_HZ,
        };
        return soft_i2c_init(&cfg);
    }
    return i2c_bus_init();
}

esp_err_t nfc_i2c_probe(unsigned timeout_ms)
{
    if (use_soft_i2c()) {
        return soft_i2c_probe(BOARD_NFC_PN532_ADDR, timeout_ms);
    }
    return i2c_bus_probe(BOARD_NFC_PN532_ADDR, timeout_ms);
}

esp_err_t nfc_i2c_write(uint8_t addr, const uint8_t *data, size_t len, unsigned timeout_ms)
{
    if (use_soft_i2c()) {
        return soft_i2c_write(addr, data, len, timeout_ms);
    }
    return i2c_bus_write(addr, data, len, timeout_ms);
}

esp_err_t nfc_i2c_read(uint8_t addr, uint8_t *data, size_t len, unsigned timeout_ms)
{
    if (use_soft_i2c()) {
        return soft_i2c_read(addr, data, len, timeout_ms);
    }
    return i2c_bus_read(addr, data, len, timeout_ms);
}

esp_err_t nfc_i2c_write_read(uint8_t addr, const uint8_t *wr, size_t wr_len,
                             uint8_t *rd, size_t rd_len, unsigned timeout_ms)
{
    if (use_soft_i2c()) {
        return soft_i2c_write_read(addr, wr, wr_len, rd, rd_len, timeout_ms);
    }
    return i2c_bus_write_read(addr, wr, wr_len, rd, rd_len, timeout_ms);
}

esp_err_t nfc_i2c_recover(unsigned timeout_ms)
{
    if (use_soft_i2c()) {
        return soft_i2c_recover(timeout_ms);
    }
    (void)timeout_ms;
    return i2c_bus_reset();
}

const char *nfc_i2c_backend(void)
{
    return use_soft_i2c() ? "software" : "hardware";
}
