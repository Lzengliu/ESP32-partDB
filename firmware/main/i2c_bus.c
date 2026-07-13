#include "i2c_bus.h"

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "i2c_bus";
static i2c_master_bus_handle_t s_bus;
static SemaphoreHandle_t s_mutex;
static bool s_installed;

typedef struct {
    uint8_t addr;
    i2c_master_dev_handle_t handle;
    bool used;
} i2c_device_slot_t;

static i2c_device_slot_t s_devices[8];

static esp_err_t install_bus_handle(void)
{
    if (BOARD_I2C_SDA_GPIO == GPIO_NUM_NC || BOARD_I2C_SCL_GPIO == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "I2C disabled: SDA=%d SCL=%d", BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO);
        return ESP_ERR_INVALID_STATE;
    }

    i2c_master_bus_config_t cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA_GPIO,
        .scl_io_num = BOARD_I2C_SCL_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err == ESP_ERR_INVALID_STATE) {
        err = i2c_master_get_bus_handle(BOARD_I2C_PORT, &s_bus);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "I2C ready SDA=%d SCL=%d", BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO);
    }
    return err;
}

static void clear_device_slots_locked(void)
{
    for (size_t i = 0; i < sizeof(s_devices) / sizeof(s_devices[0]); i++) {
        if (s_devices[i].used && s_devices[i].handle) {
            (void)i2c_master_bus_rm_device(s_devices[i].handle);
        }
        s_devices[i] = (i2c_device_slot_t){0};
    }
}

esp_err_t i2c_bus_init(void)
{
    if (s_installed) {
        return ESP_OK;
    }
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = install_bus_handle();
    if (err == ESP_OK) {
        s_installed = true;
    }
    return err;
}

static esp_err_t get_device_locked(uint8_t addr, i2c_master_dev_handle_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < sizeof(s_devices) / sizeof(s_devices[0]); i++) {
        if (s_devices[i].used && s_devices[i].addr == addr) {
            *out = s_devices[i].handle;
            return ESP_OK;
        }
    }
    for (size_t i = 0; i < sizeof(s_devices) / sizeof(s_devices[0]); i++) {
        if (!s_devices[i].used) {
            i2c_device_config_t dev_cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = addr,
                .scl_speed_hz = BOARD_I2C_HZ,
            };
            ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_devices[i].handle),
                                TAG, "add device 0x%02x failed", addr);
            s_devices[i].addr = addr;
            s_devices[i].used = true;
            *out = s_devices[i].handle;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

static esp_err_t lock_bus(unsigned timeout_ms)
{
    TickType_t ticks = timeout_ms == 0 ? 0 : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(s_mutex, ticks) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static bool i2c_lines_are_low(void)
{
    if (BOARD_I2C_SDA_GPIO == GPIO_NUM_NC || BOARD_I2C_SCL_GPIO == GPIO_NUM_NC) {
        return false;
    }
    return gpio_get_level(BOARD_I2C_SDA_GPIO) == 0 || gpio_get_level(BOARD_I2C_SCL_GPIO) == 0;
}

static void recover_bus_locked(esp_err_t err)
{
    if (!s_bus || (err != ESP_ERR_TIMEOUT && !(err == ESP_ERR_INVALID_STATE && i2c_lines_are_low()))) {
        return;
    }
    clear_device_slots_locked();
    esp_err_t reset_err = i2c_master_bus_reset(s_bus);
    if (reset_err != ESP_OK) {
        ESP_LOGW(TAG, "bus reset after %s failed: %s",
                 esp_err_to_name(err), esp_err_to_name(reset_err));
    } else {
        ESP_LOGW(TAG, "bus reset after %s", esp_err_to_name(err));
    }
}

esp_err_t i2c_bus_reset(void)
{
    if (!s_installed) {
        return i2c_bus_init();
    }
    ESP_RETURN_ON_ERROR(lock_bus(1000), TAG, "lock failed");
    clear_device_slots_locked();
    esp_err_t err = i2c_master_bus_reset(s_bus);
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t i2c_bus_reinstall(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_RETURN_ON_ERROR(lock_bus(1000), TAG, "lock failed");
    clear_device_slots_locked();
    if (s_bus) {
        (void)i2c_master_bus_reset(s_bus);
        esp_err_t del_err = i2c_del_master_bus(s_bus);
        if (del_err != ESP_OK) {
            ESP_LOGW(TAG, "delete bus before reinstall failed: %s", esp_err_to_name(del_err));
        } else {
            s_bus = NULL;
            s_installed = false;
        }
    }
    esp_err_t err = install_bus_handle();
    if (err == ESP_OK) {
        s_installed = true;
        ESP_LOGI(TAG, "I2C reinstalled");
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t i2c_bus_probe(uint8_t addr, unsigned timeout_ms)
{
    if (!s_installed) {
        ESP_RETURN_ON_ERROR(i2c_bus_init(), TAG, "init failed");
    }
    ESP_RETURN_ON_ERROR(lock_bus(timeout_ms), TAG, "lock failed");
    esp_err_t err = i2c_master_probe(s_bus, addr, (int)timeout_ms);
    recover_bus_locked(err);
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t i2c_bus_write(uint8_t addr, const uint8_t *data, size_t len, unsigned timeout_ms)
{
    if (!s_installed) {
        ESP_RETURN_ON_ERROR(i2c_bus_init(), TAG, "init failed");
    }
    ESP_RETURN_ON_ERROR(lock_bus(timeout_ms), TAG, "lock failed");
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = get_device_locked(addr, &dev);
    if (err == ESP_OK) {
        err = i2c_master_transmit(dev, data, len, (int)timeout_ms);
    }
    recover_bus_locked(err);
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t i2c_bus_read(uint8_t addr, uint8_t *data, size_t len, unsigned timeout_ms)
{
    if (!s_installed) {
        ESP_RETURN_ON_ERROR(i2c_bus_init(), TAG, "init failed");
    }
    ESP_RETURN_ON_ERROR(lock_bus(timeout_ms), TAG, "lock failed");
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = get_device_locked(addr, &dev);
    if (err == ESP_OK) {
        err = i2c_master_receive(dev, data, len, (int)timeout_ms);
    }
    recover_bus_locked(err);
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t i2c_bus_write_read(uint8_t addr, const uint8_t *wr, size_t wr_len,
                             uint8_t *rd, size_t rd_len, unsigned timeout_ms)
{
    if (!s_installed) {
        ESP_RETURN_ON_ERROR(i2c_bus_init(), TAG, "init failed");
    }
    ESP_RETURN_ON_ERROR(lock_bus(timeout_ms), TAG, "lock failed");
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = get_device_locked(addr, &dev);
    if (err == ESP_OK) {
        err = i2c_master_transmit_receive(dev, wr, wr_len, rd, rd_len, (int)timeout_ms);
    }
    recover_bus_locked(err);
    xSemaphoreGive(s_mutex);
    return err;
}
