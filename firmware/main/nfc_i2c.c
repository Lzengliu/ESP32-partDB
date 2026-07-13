#include "nfc_i2c.h"

#include <stdio.h>

#include "board_config.h"
#include "display_ili9488.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "soft_i2c.h"

#if BOARD_NFC_USE_UART_HSU
static const char *TAG = "nfc_transport";
#endif

#define PN532_SPI_DATAWRITE 0x01
#define PN532_SPI_STATREAD  0x02
#define PN532_SPI_DATAREAD  0x03

#if BOARD_NFC_ENABLED && BOARD_NFC_USE_SPI
static spi_device_handle_t s_spi;
static uint8_t s_spi_mode;
static bool s_spi_lsb_first = true;
#endif
static char s_spi_variant[16] = "mode0-lsb";

static bool use_spi(void)
{
#if !BOARD_NFC_ENABLED
    return false;
#endif
#if BOARD_NFC_USE_SPI
    return BOARD_NFC_SPI_HOST != SPI_HOST_MAX &&
           BOARD_NFC_SPI_SCLK_GPIO != GPIO_NUM_NC &&
           BOARD_NFC_SPI_MOSI_GPIO != GPIO_NUM_NC &&
           BOARD_NFC_SPI_MISO_GPIO != GPIO_NUM_NC &&
           BOARD_NFC_SPI_CS_GPIO != GPIO_NUM_NC;
#else
    return false;
#endif
}

static bool use_uart_hsu(void)
{
#if !BOARD_NFC_ENABLED
    return false;
#endif
#if BOARD_NFC_USE_UART_HSU
    return BOARD_NFC_UART_PORT != UART_NUM_MAX &&
           BOARD_NFC_UART_TX_GPIO != GPIO_NUM_NC &&
           BOARD_NFC_UART_RX_GPIO != GPIO_NUM_NC &&
           BOARD_NFC_UART_TX_GPIO != BOARD_NFC_UART_RX_GPIO;
#else
    return false;
#endif
}

static esp_err_t release_soft_i2c_pins(void)
{
#if !BOARD_NFC_ENABLED || !BOARD_NFC_USE_SOFT_I2C
    return ESP_OK;
#else
    if (BOARD_NFC_SOFT_SDA_GPIO == GPIO_NUM_NC ||
        BOARD_NFC_SOFT_SCL_GPIO == GPIO_NUM_NC ||
        BOARD_NFC_SOFT_SDA_GPIO == BOARD_NFC_SOFT_SCL_GPIO) {
        return ESP_ERR_INVALID_ARG;
    }
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BOARD_NFC_SOFT_SDA_GPIO) | (1ULL << BOARD_NFC_SOFT_SCL_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&io);
#endif
}

static bool use_soft_i2c(void)
{
#if !BOARD_NFC_ENABLED
    return false;
#endif
#if BOARD_NFC_USE_SPI
    return false;
#endif
#if BOARD_NFC_USE_UART_HSU
    return false;
#endif
#if BOARD_NFC_USE_SOFT_I2C
    return BOARD_NFC_SOFT_SDA_GPIO != GPIO_NUM_NC &&
           BOARD_NFC_SOFT_SCL_GPIO != GPIO_NUM_NC &&
           BOARD_NFC_SOFT_SDA_GPIO != BOARD_NFC_SOFT_SCL_GPIO;
#else
    return false;
#endif
}

static esp_err_t nfc_spi_transfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
#if !BOARD_NFC_ENABLED || !BOARD_NFC_USE_SPI
    (void)tx;
    (void)rx;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!s_spi || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    return spi_device_polling_transmit(s_spi, &t);
#endif
}

static esp_err_t nfc_spi_init(void)
{
#if !BOARD_NFC_ENABLED || !BOARD_NFC_USE_SPI
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_spi) {
        return ESP_OK;
    }

    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_NFC_SPI_MOSI_GPIO,
        .miso_io_num = BOARD_NFC_SPI_MISO_GPIO,
        .sclk_io_num = BOARD_NFC_SPI_SCLK_GPIO,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = 300,
    };

    bool shares_ready_lcd_bus =
        BOARD_NFC_SPI_HOST == BOARD_LCD_SPI_HOST &&
        BOARD_NFC_SPI_SCLK_GPIO == BOARD_LCD_SCLK_GPIO &&
        BOARD_NFC_SPI_MOSI_GPIO == BOARD_LCD_MOSI_GPIO &&
        BOARD_NFC_SPI_MISO_GPIO == BOARD_LCD_MISO_GPIO &&
        display_ili9488_is_ready();
    if (!shares_ready_lcd_bus) {
        esp_err_t err = spi_bus_initialize(BOARD_NFC_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = BOARD_NFC_SPI_HZ,
        .mode = s_spi_mode,
        .spics_io_num = BOARD_NFC_SPI_CS_GPIO,
        .queue_size = 1,
        .flags = s_spi_lsb_first ? SPI_DEVICE_BIT_LSBFIRST : 0,
    };
    return spi_bus_add_device(BOARD_NFC_SPI_HOST, &dev, &s_spi);
#endif
}

static esp_err_t nfc_spi_configure_variant(uint8_t mode, bool lsb_first)
{
#if !BOARD_NFC_ENABLED || !BOARD_NFC_USE_SPI
    (void)mode;
    (void)lsb_first;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (mode > 3) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_spi && s_spi_mode == mode && s_spi_lsb_first == lsb_first) {
        return ESP_OK;
    }
    if (s_spi) {
        esp_err_t err = spi_bus_remove_device(s_spi);
        s_spi = NULL;
        if (err != ESP_OK) {
            return err;
        }
    }
    s_spi_mode = mode;
    s_spi_lsb_first = lsb_first;
    snprintf(s_spi_variant, sizeof(s_spi_variant), "mode%u-%s",
             (unsigned)mode, lsb_first ? "lsb" : "msb");
    return nfc_spi_init();
#endif
}

static esp_err_t nfc_spi_read_status(uint8_t *status)
{
#if !BOARD_NFC_ENABLED || !BOARD_NFC_USE_SPI
    (void)status;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t tx[2] = {PN532_SPI_STATREAD, 0x00};
    uint8_t rx[2] = {0};
    esp_err_t err = nfc_spi_transfer(tx, rx, sizeof(tx));
    if (err != ESP_OK) {
        return err;
    }
    *status = rx[1];
    return ESP_OK;
#endif
}

static esp_err_t nfc_uart_init(void)
{
#if !BOARD_NFC_ENABLED || !BOARD_NFC_USE_UART_HSU
    return ESP_ERR_NOT_SUPPORTED;
#else
    uart_config_t cfg = {
        .baud_rate = BOARD_NFC_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(BOARD_NFC_UART_PORT, 512, 512, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    ESP_RETURN_ON_ERROR(uart_param_config(BOARD_NFC_UART_PORT, &cfg), TAG,
                        "uart config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(BOARD_NFC_UART_PORT, BOARD_NFC_UART_TX_GPIO,
                                     BOARD_NFC_UART_RX_GPIO, UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE),
                        TAG, "uart pin config failed");
    ESP_RETURN_ON_ERROR(uart_flush_input(BOARD_NFC_UART_PORT), TAG, "uart flush failed");

    static const uint8_t wake[] = {0x55, 0x55, 0x00, 0x00, 0x00};
    int n = uart_write_bytes(BOARD_NFC_UART_PORT, wake, sizeof(wake));
    if (n < 0 || (size_t)n != sizeof(wake)) {
        return ESP_FAIL;
    }
    (void)uart_wait_tx_done(BOARD_NFC_UART_PORT, pdMS_TO_TICKS(50));
    vTaskDelay(pdMS_TO_TICKS(10));
    (void)uart_flush_input(BOARD_NFC_UART_PORT);
    return ESP_OK;
#endif
}

esp_err_t nfc_i2c_init(void)
{
#if !BOARD_NFC_ENABLED
    return ESP_ERR_NOT_SUPPORTED;
#endif
    if (use_spi()) {
        return nfc_spi_init();
    }
    if (use_uart_hsu()) {
        return nfc_uart_init();
    }
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
#if !BOARD_NFC_ENABLED
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
    if (use_spi()) {
        (void)timeout_ms;
        return nfc_spi_init();
    }
    if (use_uart_hsu()) {
        (void)timeout_ms;
        return ESP_OK;
    }
    if (use_soft_i2c()) {
        return soft_i2c_probe(BOARD_NFC_PN532_ADDR, timeout_ms);
    }
    return i2c_bus_probe(BOARD_NFC_PN532_ADDR, timeout_ms);
}

esp_err_t nfc_i2c_write(uint8_t addr, const uint8_t *data, size_t len, unsigned timeout_ms)
{
#if !BOARD_NFC_ENABLED
    (void)addr;
    (void)data;
    (void)len;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
    if (use_spi()) {
        (void)addr;
        (void)timeout_ms;
        if (!data || len == 0 || len + 1 > 300) {
            return ESP_ERR_INVALID_ARG;
        }
        uint8_t tx[300] = {0};
        tx[0] = PN532_SPI_DATAWRITE;
        for (size_t i = 0; i < len; i++) {
            tx[i + 1] = data[i];
        }
        return nfc_spi_transfer(tx, NULL, len + 1);
    }
    if (use_uart_hsu()) {
        (void)addr;
        TickType_t ticks = pdMS_TO_TICKS(timeout_ms ? timeout_ms : 1);
        int n = uart_write_bytes(BOARD_NFC_UART_PORT, data, len);
        if (n < 0) {
            return ESP_FAIL;
        }
        if ((size_t)n != len) {
            return ESP_ERR_TIMEOUT;
        }
        esp_err_t err = uart_wait_tx_done(BOARD_NFC_UART_PORT, ticks);
        return err == ESP_OK ? ESP_OK : err;
    }
    if (use_soft_i2c()) {
        return soft_i2c_write(addr, data, len, timeout_ms);
    }
    return i2c_bus_write(addr, data, len, timeout_ms);
}

esp_err_t nfc_i2c_read(uint8_t addr, uint8_t *data, size_t len, unsigned timeout_ms)
{
#if !BOARD_NFC_ENABLED
    (void)addr;
    (void)data;
    (void)len;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
    if (use_spi()) {
        (void)addr;
        (void)timeout_ms;
        if (!data || len == 0 || len + 1 > 300) {
            return ESP_ERR_INVALID_ARG;
        }
        uint8_t tx[300] = {0};
        uint8_t rx[300] = {0};
        tx[0] = PN532_SPI_DATAREAD;
        esp_err_t err = nfc_spi_transfer(tx, rx, len + 1);
        if (err != ESP_OK) {
            return err;
        }
        for (size_t i = 0; i < len; i++) {
            data[i] = rx[i + 1];
        }
        return ESP_OK;
    }
    if (use_uart_hsu()) {
        (void)addr;
        if (!data || len == 0) {
            return ESP_ERR_INVALID_ARG;
        }
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms ? timeout_ms : 1);
        size_t got = 0;
        while (got < len) {
            TickType_t now = xTaskGetTickCount();
            if ((int32_t)(deadline - now) <= 0) {
                return ESP_ERR_TIMEOUT;
            }
            TickType_t ticks = deadline - now;
            if (ticks > pdMS_TO_TICKS(20)) {
                ticks = pdMS_TO_TICKS(20);
            }
            int n = uart_read_bytes(BOARD_NFC_UART_PORT, data + got, len - got, ticks);
            if (n < 0) {
                return ESP_FAIL;
            }
            if (n > 0) {
                got += (size_t)n;
            }
        }
        return ESP_OK;
    }
    if (use_soft_i2c()) {
        return soft_i2c_read(addr, data, len, timeout_ms);
    }
    return i2c_bus_read(addr, data, len, timeout_ms);
}

esp_err_t nfc_i2c_write_read(uint8_t addr, const uint8_t *wr, size_t wr_len,
                             uint8_t *rd, size_t rd_len, unsigned timeout_ms)
{
#if !BOARD_NFC_ENABLED
    (void)addr;
    (void)wr;
    (void)wr_len;
    (void)rd;
    (void)rd_len;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
    if (use_spi()) {
        esp_err_t err = nfc_i2c_write(addr, wr, wr_len, timeout_ms);
        if (err != ESP_OK) {
            return err;
        }
        return nfc_i2c_read(addr, rd, rd_len, timeout_ms);
    }
    if (use_uart_hsu()) {
        (void)addr;
        esp_err_t err = nfc_i2c_write(addr, wr, wr_len, timeout_ms);
        if (err != ESP_OK) {
            return err;
        }
        return nfc_i2c_read(addr, rd, rd_len, timeout_ms);
    }
    if (use_soft_i2c()) {
        return soft_i2c_write_read(addr, wr, wr_len, rd, rd_len, timeout_ms);
    }
    return i2c_bus_write_read(addr, wr, wr_len, rd, rd_len, timeout_ms);
}

esp_err_t nfc_i2c_recover(unsigned timeout_ms)
{
#if !BOARD_NFC_ENABLED
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
    if (use_spi()) {
        (void)timeout_ms;
        return ESP_OK;
    }
    if (use_uart_hsu()) {
        (void)timeout_ms;
        return uart_flush_input(BOARD_NFC_UART_PORT);
    }
    if (use_soft_i2c()) {
        return soft_i2c_recover(timeout_ms);
    }
    (void)timeout_ms;
    return i2c_bus_reset();
}

esp_err_t nfc_i2c_release(void)
{
#if !BOARD_NFC_ENABLED
    return ESP_OK;
#endif
    if (use_spi()) {
        return ESP_OK;
    }
    if (use_uart_hsu()) {
        return uart_flush_input(BOARD_NFC_UART_PORT);
    }
    if (use_soft_i2c()) {
        esp_err_t err = soft_i2c_release();
        esp_err_t pin_err = release_soft_i2c_pins();
        return err == ESP_OK ? pin_err : err;
    }
    return ESP_OK;
}

bool nfc_i2c_has_i2c_status_byte(void)
{
#if !BOARD_NFC_ENABLED
    return false;
#endif
    return !use_uart_hsu() && !use_spi();
}

bool nfc_i2c_requires_ready_poll(void)
{
#if !BOARD_NFC_ENABLED
    return false;
#endif
    return !use_uart_hsu();
}

bool nfc_i2c_is_spi(void)
{
    return use_spi();
}

esp_err_t nfc_i2c_configure_spi_variant(uint8_t mode, bool lsb_first)
{
    return nfc_spi_configure_variant(mode, lsb_first);
}

const char *nfc_i2c_spi_variant(void)
{
    return s_spi_variant;
}

esp_err_t nfc_i2c_read_status(uint8_t *status, unsigned timeout_ms)
{
#if !BOARD_NFC_ENABLED
    (void)status;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }
    if (use_spi()) {
        (void)timeout_ms;
        return nfc_spi_read_status(status);
    }
    if (use_uart_hsu()) {
        *status = 0x01;
        return ESP_OK;
    }
    return nfc_i2c_read(BOARD_NFC_PN532_ADDR, status, 1, timeout_ms);
}

const char *nfc_i2c_backend(void)
{
#if !BOARD_NFC_ENABLED
    return "disabled";
#endif
    if (use_uart_hsu()) {
        return "uart-hsu";
    }
    if (use_spi()) {
        return "spi";
    }
    return use_soft_i2c() ? "software" : "hardware";
}
