#include "nfc_pn532.h"

#include <stdbool.h>
#include <string.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nfc_i2c.h"
#include "peripheral_arbiter.h"

static const char *TAG = "pn532";

#define PN532_PREAMBLE      0x00
#define PN532_STARTCODE1    0x00
#define PN532_STARTCODE2    0xFF
#define PN532_POSTAMBLE     0x00
#define PN532_HOSTTOPN532   0xD4
#define PN532_PN532TOHOST   0xD5
#define PN532_ACK_WAIT_MS   100
#define PN532_INIT_ATTEMPTS 3
#define PN532_TYPE2_USER_MAX 512
#define PN532_TYPE2_ERASE_BYTES 4

static bool s_ready;
static bool s_hw_reset_done;
static bool s_force_hw_reset;
static bool s_passive_scan_pending;

static uint64_t gpio_pin_mask_or_zero(gpio_num_t gpio)
{
    return gpio == GPIO_NUM_NC ? 0 : (1ULL << gpio);
}

static esp_err_t pn532_configure_irq(void)
{
    if (BOARD_NFC_IRQ_GPIO == GPIO_NUM_NC) {
        return ESP_OK;
    }
    gpio_config_t irq = {
        .pin_bit_mask = gpio_pin_mask_or_zero(BOARD_NFC_IRQ_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&irq);
}

static esp_err_t pn532_hw_reset_if_needed(void)
{
    if (BOARD_NFC_RST_GPIO == GPIO_NUM_NC || (s_hw_reset_done && !s_force_hw_reset)) {
        return ESP_OK;
    }

    gpio_config_t rst = {
        .pin_bit_mask = gpio_pin_mask_or_zero(BOARD_NFC_RST_GPIO),
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rst), TAG, "rst gpio config failed");
    gpio_set_level(BOARD_NFC_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(40));
    (void)gpio_set_direction(BOARD_NFC_RST_GPIO, GPIO_MODE_INPUT);
    (void)gpio_pullup_en(BOARD_NFC_RST_GPIO);
    vTaskDelay(pdMS_TO_TICKS(420));
    if (gpio_get_level(BOARD_NFC_RST_GPIO) == 0) {
        ESP_LOGW(TAG, "PN532 reset GPIO%d is still low after release",
                 BOARD_NFC_RST_GPIO);
    }
    s_hw_reset_done = true;
    s_force_hw_reset = false;
    ESP_LOGI(TAG, "PN532 reset pulse on GPIO%d", BOARD_NFC_RST_GPIO);
    return ESP_OK;
}

static void pn532_recover_after_fault(esp_err_t err)
{
    if (err == ESP_OK || err == ESP_ERR_NOT_FOUND ||
        err == ESP_ERR_NOT_SUPPORTED || err == ESP_ERR_INVALID_ARG ||
        err == ESP_ERR_INVALID_SIZE || err == ESP_ERR_INVALID_RESPONSE) {
        return;
    }
    s_ready = false;
    s_passive_scan_pending = false;
    ESP_LOGW(TAG, "PN532 marked not ready after %s", esp_err_to_name(err));
    (void)nfc_i2c_recover(200);
}

static uint8_t checksum(const uint8_t *data, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return (uint8_t)(~sum + 1);
}

static esp_err_t pn532_write_cmd(const uint8_t *cmd, size_t cmd_len)
{
    if (!cmd || cmd_len == 0 || cmd_len > 250) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t frame[270];
    size_t n = 0;
    uint8_t len = (uint8_t)(cmd_len + 1);
    frame[n++] = PN532_PREAMBLE;
    frame[n++] = PN532_STARTCODE1;
    frame[n++] = PN532_STARTCODE2;
    frame[n++] = len;
    frame[n++] = (uint8_t)(~len + 1);
    frame[n++] = PN532_HOSTTOPN532;
    memcpy(&frame[n], cmd, cmd_len);
    n += cmd_len;
    frame[n++] = checksum(&frame[5], cmd_len + 1);
    frame[n++] = PN532_POSTAMBLE;
    return nfc_i2c_write(BOARD_NFC_PN532_ADDR, frame, n, 100);
}

static esp_err_t pn532_wait_ready(unsigned timeout_ms)
{
    if (!nfc_i2c_requires_ready_poll()) {
        (void)timeout_ms;
        return ESP_OK;
    }

    if (s_ready && BOARD_NFC_IRQ_GPIO != GPIO_NUM_NC) {
        unsigned waited = 0;
        while (waited <= timeout_ms) {
            if (gpio_get_level(BOARD_NFC_IRQ_GPIO) == 0) {
                return ESP_OK;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            waited += 5;
        }
        ESP_LOGW(TAG, "PN532 IRQ timeout after %u ms, irq=%d",
                 timeout_ms, gpio_get_level(BOARD_NFC_IRQ_GPIO));
        return ESP_ERR_TIMEOUT;
    }

    uint8_t status = 0;
    uint8_t last_status = 0;
    esp_err_t last_err = ESP_ERR_TIMEOUT;
    unsigned waited = 0;
    while (waited <= timeout_ms) {
        esp_err_t err = nfc_i2c_read_status(&status, 20);
        if (err == ESP_OK && status == 0x01) {
            return ESP_OK;
        }
        if (err == ESP_OK) {
            last_status = status;
        }
        last_err = err;
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
    ESP_LOGW(TAG, "PN532 ready timeout after %u ms, last_status=0x%02x last_err=%s",
             timeout_ms, last_status, esp_err_to_name(last_err));
    return ESP_ERR_TIMEOUT;
}

static esp_err_t pn532_parse_frame(const uint8_t *raw, size_t raw_len, size_t pos,
                                   uint8_t *out, size_t out_len, size_t *actual)
{
    while (pos + 5 < raw_len && !(raw[pos] == 0x00 && raw[pos + 1] == 0x00 && raw[pos + 2] == 0xff)) {
        pos++;
    }
    if (pos + 6 >= raw_len) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t len = raw[pos + 3];
    if (((uint8_t)(len + raw[pos + 4])) != 0) {
        return ESP_ERR_INVALID_CRC;
    }
    if (pos + 5 + len >= raw_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t *payload = &raw[pos + 5];
    uint8_t sum = 0;
    for (int i = 0; i < len; i++) {
        sum += payload[i];
    }
    sum += raw[pos + 5 + len];
    if (sum != 0) {
        return ESP_ERR_INVALID_CRC;
    }

    size_t copy = len < out_len ? len : out_len;
    memcpy(out, payload, copy);
    if (actual) {
        *actual = copy;
    }
    return ESP_OK;
}

static esp_err_t pn532_read_uart_frame(uint8_t *out, size_t out_len, size_t *actual,
                                       unsigned timeout_ms)
{
    uint8_t raw[128] = {0};
    size_t pos = 0;
    unsigned waited = 0;

    while (waited <= timeout_ms && pos < sizeof(raw)) {
        uint8_t b = 0;
        esp_err_t err = nfc_i2c_read(BOARD_NFC_PN532_ADDR, &b, 1, 20);
        if (err == ESP_ERR_TIMEOUT) {
            waited += 20;
            continue;
        }
        if (err != ESP_OK) {
            return err;
        }

        if (pos == 0 && b != 0x00) {
            continue;
        }
        raw[pos++] = b;

        if (pos >= 3 && raw[pos - 3] == 0x00 && raw[pos - 2] == 0x00 && raw[pos - 1] == 0xff) {
            raw[0] = 0x00;
            raw[1] = 0x00;
            raw[2] = 0xff;
            pos = 3;
            break;
        }
    }
    if (pos < 3) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = nfc_i2c_read(BOARD_NFC_PN532_ADDR, &raw[3], 3, 100);
    if (err != ESP_OK) {
        return err;
    }
    pos = 6;
    uint8_t len = raw[3];
    if (((uint8_t)(len + raw[4])) != 0) {
        return ESP_ERR_INVALID_CRC;
    }
    if (len == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (pos + len + 1 > sizeof(raw)) {
        return ESP_ERR_INVALID_SIZE;
    }
    err = nfc_i2c_read(BOARD_NFC_PN532_ADDR, &raw[pos], (size_t)len + 1, 100);
    if (err != ESP_OK) {
        return err;
    }
    return pn532_parse_frame(raw, pos + len + 1, 0, out, out_len, actual);
}

static esp_err_t pn532_read_frame(uint8_t *out, size_t out_len, size_t *actual, unsigned timeout_ms)
{
    if (!nfc_i2c_requires_ready_poll()) {
        return pn532_read_uart_frame(out, out_len, actual, timeout_ms);
    }

    esp_err_t err = pn532_wait_ready(timeout_ms);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t raw[128] = {0};
    err = nfc_i2c_read(BOARD_NFC_PN532_ADDR, raw, sizeof(raw), 100);
    if (err != ESP_OK) {
        return err;
    }

    size_t offset = nfc_i2c_has_i2c_status_byte() ? 1 : 0;
    return pn532_parse_frame(raw, sizeof(raw), offset, out, out_len, actual);
}

static esp_err_t pn532_read_ack(unsigned timeout_ms)
{
    esp_err_t err = pn532_wait_ready(timeout_ms);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t raw[8] = {0};
    err = nfc_i2c_read(BOARD_NFC_PN532_ADDR, raw,
                       nfc_i2c_has_i2c_status_byte() ? sizeof(raw) : 6, 100);
    if (err != ESP_OK) {
        return err;
    }
    const uint8_t ack[] = {0x00, 0x00, 0xff, 0x00, 0xff, 0x00};
    size_t raw_len = nfc_i2c_has_i2c_status_byte() ? sizeof(raw) : 6;
    size_t offset = nfc_i2c_has_i2c_status_byte() ? 1 : 0;
    for (; offset + sizeof(ack) <= raw_len; offset++) {
        if (memcmp(&raw[offset], ack, sizeof(ack)) == 0) {
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t pn532_cmd_response(const uint8_t *cmd, size_t cmd_len,
                                    uint8_t *resp, size_t resp_len, size_t *actual,
                                    unsigned timeout_ms)
{
    esp_err_t err = pn532_write_cmd(cmd, cmd_len);
    if (err != ESP_OK) {
        return err;
    }
    err = pn532_read_ack(PN532_ACK_WAIT_MS);
    if (err != ESP_OK) {
        return err;
    }
    return pn532_read_frame(resp, resp_len, actual, timeout_ms);
}

static esp_err_t pn532_parse_passive_response(const uint8_t *resp, size_t len, nfc_tag_t *tag)
{
    if (!resp || !tag) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(tag, 0, sizeof(*tag));
    if (len >= 3 && resp[0] == PN532_PN532TOHOST && resp[1] == 0x4B && resp[2] == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (len >= 12 && resp[0] == PN532_PN532TOHOST && resp[1] == 0x4B && resp[2] >= 1) {
        tag->target_number = resp[3];
        uint8_t uid_len = resp[7];
        if (uid_len > sizeof(tag->uid)) {
            uid_len = sizeof(tag->uid);
        }
        tag->uid_len = uid_len;
        memcpy(tag->uid, &resp[8], uid_len);
        return ESP_OK;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t pn532_select_passive_locked(nfc_tag_t *tag, unsigned timeout_ms)
{
    if (!tag) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(tag, 0, sizeof(*tag));
    uint8_t cmd[] = {0x4A, 0x01, 0x00};
    uint8_t resp[64] = {0};
    size_t len = 0;
    esp_err_t err = pn532_cmd_response(cmd, sizeof(cmd), resp, sizeof(resp), &len, timeout_ms);
    if (err == ESP_OK) {
        return pn532_parse_passive_response(resp, len, tag);
    }
    if (err == ESP_ERR_TIMEOUT) {
        return ESP_ERR_NOT_FOUND;
    }
    return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
}

static esp_err_t pn532_type2_read_page(uint8_t target, uint8_t page, uint8_t out[16], unsigned timeout_ms)
{
    uint8_t cmd[] = {0x40, target, 0x30, page};
    uint8_t resp[24] = {0};
    size_t len = 0;
    esp_err_t err = pn532_cmd_response(cmd, sizeof(cmd), resp, sizeof(resp), &len, timeout_ms);
    if (err != ESP_OK) {
        return err;
    }
    if (len < 19 || resp[0] != PN532_PN532TOHOST || resp[1] != 0x41 || resp[2] != 0x00) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    memcpy(out, &resp[3], 16);
    return ESP_OK;
}

static esp_err_t pn532_type2_write_page(uint8_t target, uint8_t page, const uint8_t data[4], unsigned timeout_ms)
{
    uint8_t cmd[] = {0x40, target, 0xA2, page, data[0], data[1], data[2], data[3]};
    uint8_t resp[8] = {0};
    size_t len = 0;
    esp_err_t err = pn532_cmd_response(cmd, sizeof(cmd), resp, sizeof(resp), &len, timeout_ms);
    if (err != ESP_OK) {
        return err;
    }
    if (len < 3 || resp[0] != PN532_PN532TOHOST || resp[1] != 0x41 || resp[2] != 0x00) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t pn532_type2_capacity_locked(uint8_t target, size_t *capacity, unsigned timeout_ms)
{
    uint8_t pages[16] = {0};
    esp_err_t err = pn532_type2_read_page(target, 3, pages, timeout_ms);
    if (err != ESP_OK) {
        return err;
    }
    if (pages[0] != 0xE1 || (pages[1] & 0xF0) != 0x10 || pages[2] == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    size_t cap = (size_t)pages[2] * 8;
    if (cap > PN532_TYPE2_USER_MAX) {
        cap = PN532_TYPE2_USER_MAX;
    }
    if (capacity) {
        *capacity = cap;
    }
    return ESP_OK;
}

static esp_err_t build_ndef_text_record(const char *text, uint8_t *out, size_t out_len, size_t *actual)
{
    size_t text_len = text ? strlen(text) : 0;
    if (!text || text_len == 0 || text_len > 220 || !out || !actual) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t payload_len = text_len + 3; /* status byte + "en" + text */
    size_t record_len = 4 + payload_len;
    size_t tlv_len = 2 + record_len + 1;
    size_t padded_len = (tlv_len + 3) & ~(size_t)3;
    if (record_len > 254 || padded_len > out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    memset(out, 0, padded_len);
    out[0] = 0x03;
    out[1] = (uint8_t)record_len;
    out[2] = 0xD1; /* MB/ME/SR + well-known record */
    out[3] = 0x01;
    out[4] = (uint8_t)payload_len;
    out[5] = 'T';
    out[6] = 0x02;
    out[7] = 'e';
    out[8] = 'n';
    memcpy(&out[9], text, text_len);
    out[9 + text_len] = 0xFE;
    *actual = padded_len;
    return ESP_OK;
}

static esp_err_t parse_ndef_text_record(const uint8_t *data, size_t data_len, char *out, size_t out_len)
{
    if (!data || !out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    size_t pos = 0;
    while (pos < data_len) {
        uint8_t tlv = data[pos++];
        if (tlv == 0x00) {
            continue;
        }
        if (tlv == 0xFE) {
            break;
        }
        if (pos >= data_len) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        size_t ndef_len = data[pos++];
        if (ndef_len == 0xFF) {
            if (pos + 2 > data_len) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            ndef_len = ((size_t)data[pos] << 8) | data[pos + 1];
            pos += 2;
        }
        if (pos + ndef_len > data_len) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (tlv != 0x03) {
            pos += ndef_len;
            continue;
        }
        if (ndef_len == 0) {
            return ESP_ERR_NOT_FOUND;
        }

        const uint8_t *ndef = &data[pos];
        if (ndef_len < 6 || (ndef[0] & 0x10) == 0) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        uint8_t type_len = ndef[1];
        size_t payload_len = ndef[2];
        size_t idx = 3;
        if (ndef[0] & 0x08) {
            if (idx >= ndef_len) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            idx += 1 + ndef[idx];
        }
        if (idx + type_len + payload_len > ndef_len || type_len != 1 || ndef[idx] != 'T') {
            return ESP_ERR_NOT_SUPPORTED;
        }
        const uint8_t *payload = &ndef[idx + type_len];
        if (payload_len < 2) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        uint8_t lang_len = payload[0] & 0x3F;
        if (payload_len <= (size_t)1 + lang_len) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        size_t text_len = payload_len - 1 - lang_len;
        if (text_len >= out_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(out, &payload[1 + lang_len], text_len);
        out[text_len] = '\0';
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t pn532_get_firmware(uint8_t *resp, size_t resp_len, size_t *len)
{
    uint8_t get_fw[] = {0x02};
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= PN532_INIT_ATTEMPTS; attempt++) {
        err = pn532_cmd_response(get_fw, sizeof(get_fw), resp, resp_len, len, 1500);
        if (err == ESP_OK && *len >= 6 && resp[0] == PN532_PN532TOHOST && resp[1] == 0x03) {
            ESP_LOGI(TAG, "PN532 firmware IC=0x%02x ver=%u.%u", resp[2], resp[3], resp[4]);
            return ESP_OK;
        }
        if (err == ESP_OK) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
        ESP_LOGW(TAG, "GetFirmwareVersion attempt %d failed on %s: %s",
                 attempt, nfc_i2c_is_spi() ? nfc_i2c_spi_variant() : nfc_i2c_backend(),
                 esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return err;
}

esp_err_t nfc_pn532_init(void)
{
#if !BOARD_NFC_ENABLED
    s_ready = false;
    return ESP_ERR_NOT_SUPPORTED;
#endif
    if (s_ready) {
        return ESP_OK;
    }
    s_passive_scan_pending = false;

    ESP_RETURN_ON_ERROR(pn532_configure_irq(), TAG, "irq config failed");
    ESP_RETURN_ON_ERROR(pn532_hw_reset_if_needed(), TAG, "hw reset failed");
    ESP_RETURN_ON_ERROR(nfc_i2c_init(), TAG, "i2c init failed");
    ESP_RETURN_ON_ERROR(peripheral_arbiter_acquire(PERIPHERAL_USER_NFC, 3000),
                        TAG, "resource busy");

    esp_err_t err = nfc_i2c_probe(100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PN532 not detected on %s I2C addr=0x%02x: %s",
                 nfc_i2c_backend(), BOARD_NFC_PN532_ADDR, esp_err_to_name(err));
        s_ready = false;
        peripheral_arbiter_release(PERIPHERAL_USER_NFC);
        return err;
    }

    uint8_t resp[32] = {0};
    size_t len = 0;
    err = ESP_FAIL;
    if (nfc_i2c_is_spi()) {
        const struct {
            uint8_t mode;
            bool lsb_first;
        } variants[] = {
            {0, true},
            {1, true},
            {2, true},
            {3, true},
            {0, false},
            {1, false},
            {2, false},
            {3, false},
        };
        for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); i++) {
            memset(resp, 0, sizeof(resp));
            len = 0;
            err = nfc_i2c_configure_spi_variant(variants[i].mode, variants[i].lsb_first);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "configure PN532 SPI mode%u-%s failed: %s",
                         (unsigned)variants[i].mode,
                         variants[i].lsb_first ? "lsb" : "msb",
                         esp_err_to_name(err));
                continue;
            }
            ESP_LOGI(TAG, "probing PN532 SPI %s", nfc_i2c_spi_variant());
            err = pn532_get_firmware(resp, sizeof(resp), &len);
            if (err == ESP_OK) {
                break;
            }
        }
    } else {
        err = pn532_get_firmware(resp, sizeof(resp), &len);
    }
    if (err != ESP_OK) {
        s_ready = false;
        peripheral_arbiter_release(PERIPHERAL_USER_NFC);
        return err;
    }

    uint8_t sam[] = {0x14, 0x01, 0x14, 0x01};
    err = pn532_cmd_response(sam, sizeof(sam), resp, sizeof(resp), &len, 1000);
    if (err == ESP_OK) {
        uint8_t retries[] = {0x32, 0x05, 0xff, 0xff, 0xff};
        err = pn532_cmd_response(retries, sizeof(retries), resp, sizeof(resp), &len, 1000);
    }
    s_ready = err == ESP_OK;
    peripheral_arbiter_release(PERIPHERAL_USER_NFC);
    return err;
}

esp_err_t nfc_pn532_read_passive(nfc_tag_t *tag, unsigned timeout_ms)
{
    if (!tag) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(tag, 0, sizeof(*tag));
    ESP_RETURN_ON_ERROR(peripheral_arbiter_acquire(PERIPHERAL_USER_NFC, timeout_ms),
                        TAG, "resource busy");

    esp_err_t err = pn532_select_passive_locked(tag, timeout_ms);

    peripheral_arbiter_release(PERIPHERAL_USER_NFC);
    pn532_recover_after_fault(err);
    return err;
}

esp_err_t nfc_pn532_start_passive_scan(void)
{
#if !BOARD_NFC_ENABLED
    return ESP_ERR_NOT_SUPPORTED;
#endif
    if (!s_ready) {
        ESP_RETURN_ON_ERROR(nfc_pn532_init(), TAG, "init failed");
    }
    if (s_passive_scan_pending) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(peripheral_arbiter_acquire(PERIPHERAL_USER_NFC, 500),
                        TAG, "resource busy");

    uint8_t cmd[] = {0x4A, 0x01, 0x00};
    esp_err_t err = pn532_write_cmd(cmd, sizeof(cmd));
    if (err == ESP_OK) {
        err = pn532_read_ack(PN532_ACK_WAIT_MS);
    }
    if (err == ESP_OK) {
        s_passive_scan_pending = true;
    }

    peripheral_arbiter_release(PERIPHERAL_USER_NFC);
    pn532_recover_after_fault(err);
    return err;
}

bool nfc_pn532_passive_scan_pending(void)
{
    return s_passive_scan_pending;
}

bool nfc_pn532_passive_scan_ready(void)
{
#if !BOARD_NFC_ENABLED
    return false;
#endif
    if (!s_ready || !s_passive_scan_pending) {
        return false;
    }
    if (BOARD_NFC_IRQ_GPIO == GPIO_NUM_NC) {
        return true;
    }
    return gpio_get_level(BOARD_NFC_IRQ_GPIO) == 0;
}

esp_err_t nfc_pn532_read_passive_scan_result(nfc_tag_t *tag, unsigned timeout_ms)
{
    if (!tag) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(tag, 0, sizeof(*tag));
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_passive_scan_pending) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!nfc_pn532_passive_scan_ready()) {
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_ERROR(peripheral_arbiter_acquire(PERIPHERAL_USER_NFC, timeout_ms),
                        TAG, "resource busy");

    uint8_t resp[64] = {0};
    size_t len = 0;
    esp_err_t err = pn532_read_frame(resp, sizeof(resp), &len, timeout_ms);
    if (err == ESP_OK) {
        err = pn532_parse_passive_response(resp, len, tag);
    }
    s_passive_scan_pending = false;

    peripheral_arbiter_release(PERIPHERAL_USER_NFC);
    if (err != ESP_ERR_NOT_FOUND) {
        pn532_recover_after_fault(err);
    }
    return err;
}

static esp_err_t pn532_read_ndef_text_from_selected_locked(const nfc_tag_t *tag, char *out,
                                                           size_t out_len, unsigned timeout_ms)
{
    if (!tag || tag->target_number == 0 || !out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t capacity = 0;
    esp_err_t err = pn532_type2_capacity_locked(tag->target_number, &capacity, timeout_ms);
    if (err == ESP_OK) {
        uint8_t data[PN532_TYPE2_USER_MAX] = {0};
        for (size_t offset = 0; offset < capacity; offset += 16) {
            uint8_t pages[16] = {0};
            uint8_t page = (uint8_t)(4 + offset / 4);
            err = pn532_type2_read_page(tag->target_number, page, pages, timeout_ms);
            if (err != ESP_OK) {
                break;
            }
            size_t copy = capacity - offset;
            if (copy > sizeof(pages)) {
                copy = sizeof(pages);
            }
            memcpy(&data[offset], pages, copy);
            esp_err_t parse_err = parse_ndef_text_record(data, offset + copy, out, out_len);
            if (parse_err == ESP_OK || parse_err == ESP_ERR_NOT_FOUND) {
                return parse_err;
            }
        }
        if (err == ESP_OK) {
            err = parse_ndef_text_record(data, capacity, out, out_len);
        }
    }
    return err;
}

esp_err_t nfc_pn532_read_ndef_text_from_tag(const nfc_tag_t *tag, char *out,
                                            size_t out_len, unsigned timeout_ms)
{
    if (!out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (!tag || tag->target_number == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        ESP_RETURN_ON_ERROR(nfc_pn532_init(), TAG, "init failed");
    }
    ESP_RETURN_ON_ERROR(peripheral_arbiter_acquire(PERIPHERAL_USER_NFC, timeout_ms),
                        TAG, "resource busy");

    esp_err_t err = pn532_read_ndef_text_from_selected_locked(tag, out, out_len, timeout_ms);

    peripheral_arbiter_release(PERIPHERAL_USER_NFC);
    if (err != ESP_ERR_NOT_FOUND) {
        pn532_recover_after_fault(err);
    }
    return err;
}

esp_err_t nfc_pn532_read_ndef_text(char *out, size_t out_len, unsigned timeout_ms)
{
    if (!out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (!s_ready) {
        ESP_RETURN_ON_ERROR(nfc_pn532_init(), TAG, "init failed");
    }
    ESP_RETURN_ON_ERROR(peripheral_arbiter_acquire(PERIPHERAL_USER_NFC, timeout_ms),
                        TAG, "resource busy");

    nfc_tag_t tag = {0};
    esp_err_t err = pn532_select_passive_locked(&tag, timeout_ms);
    if (err == ESP_OK) {
        err = pn532_read_ndef_text_from_selected_locked(&tag, out, out_len, timeout_ms);
    }

    peripheral_arbiter_release(PERIPHERAL_USER_NFC);
    if (err != ESP_ERR_NOT_FOUND) {
        pn532_recover_after_fault(err);
    }
    return err;
}

static bool pn532_tag_uid_equal(const nfc_tag_t *a, const nfc_tag_t *b)
{
    return a && b && a->uid_len == b->uid_len &&
           memcmp(a->uid, b->uid, a->uid_len) == 0;
}

static esp_err_t pn532_recover_and_reselect(nfc_tag_t *tag, unsigned timeout_ms)
{
    if (!tag) {
        return ESP_ERR_INVALID_ARG;
    }
    nfc_tag_t original = *tag;
    nfc_pn532_mark_not_ready();
    ESP_RETURN_ON_ERROR(nfc_pn532_init(), TAG, "reinit failed");
    nfc_tag_t fresh = {0};
    esp_err_t err = nfc_pn532_read_passive(&fresh, timeout_ms);
    if (err != ESP_OK) {
        return err;
    }
    if (!pn532_tag_uid_equal(&original, &fresh)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *tag = fresh;
    return ESP_OK;
}

static esp_err_t pn532_verify_type2_page(nfc_tag_t *tag, uint8_t page,
                                         const uint8_t data[4], unsigned timeout_ms)
{
    if (!tag || tag->target_number == 0 || !data) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        ESP_RETURN_ON_ERROR(nfc_pn532_init(), TAG, "init failed");
    }
    ESP_RETURN_ON_ERROR(peripheral_arbiter_acquire(PERIPHERAL_USER_NFC, timeout_ms),
                        TAG, "resource busy");
    uint8_t pages[16] = {0};
    esp_err_t err = pn532_type2_read_page(tag->target_number, page, pages, timeout_ms);
    peripheral_arbiter_release(PERIPHERAL_USER_NFC);
    if (err != ESP_OK) {
        return err;
    }
    return memcmp(pages, data, 4) == 0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t pn532_write_ndef_text_with_recovery(nfc_tag_t *tag, const char *text,
                                                     unsigned timeout_ms)
{
    if (!text || text[0] == '\0' || !tag || tag->target_number == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[256] = {0};
    size_t payload_len = 0;
    esp_err_t err = build_ndef_text_record(text, payload, sizeof(payload), &payload_len);
    if (err != ESP_OK) {
        return err;
    }

    for (size_t offset = 0; offset < payload_len; offset += 4) {
        uint8_t page_data[4] = {0};
        size_t copy = payload_len - offset;
        if (copy > sizeof(page_data)) {
            copy = sizeof(page_data);
        }
        memcpy(page_data, &payload[offset], copy);
        uint8_t page = (uint8_t)(4 + offset / 4);

        bool written = false;
        for (int attempt = 0; attempt < 3 && !written; attempt++) {
            if (!s_ready) {
                ESP_RETURN_ON_ERROR(nfc_pn532_init(), TAG, "init failed");
            }
            ESP_RETURN_ON_ERROR(peripheral_arbiter_acquire(PERIPHERAL_USER_NFC, timeout_ms),
                                TAG, "resource busy");
            err = pn532_type2_write_page(tag->target_number, page, page_data, timeout_ms);
            peripheral_arbiter_release(PERIPHERAL_USER_NFC);
            if (err == ESP_OK) {
                written = true;
                break;
            }
            if (err != ESP_ERR_TIMEOUT && err != ESP_ERR_INVALID_RESPONSE) {
                pn532_recover_after_fault(err);
                return err;
            }
            esp_err_t verify_err = pn532_verify_type2_page(tag, page, page_data, timeout_ms);
            if (verify_err == ESP_OK) {
                ESP_LOGW(TAG, "Type 2 page %u write returned %s but readback matched",
                         page, esp_err_to_name(err));
                written = true;
                break;
            }
            ESP_LOGW(TAG, "Type 2 page %u write ended with %s, recovering",
                     page, esp_err_to_name(err));
            esp_err_t recover_err = pn532_recover_and_reselect(tag, 1200);
            if (recover_err != ESP_OK) {
                return recover_err;
            }
        }
        if (!written) {
            return err == ESP_OK ? ESP_ERR_TIMEOUT : err;
        }
    }

    char verify[64] = {0};
    err = nfc_pn532_read_ndef_text_from_tag(tag, verify, sizeof(verify), timeout_ms);
    if (err != ESP_OK || strcmp(verify, text) != 0) {
        ESP_LOGW(TAG, "NDEF verify failed err=%s expected=%s got=%s",
                 esp_err_to_name(err), text, verify);
        esp_err_t recover_err = pn532_recover_and_reselect(tag, 1200);
        if (recover_err == ESP_OK) {
            verify[0] = '\0';
            err = nfc_pn532_read_ndef_text_from_tag(tag, verify, sizeof(verify), timeout_ms);
        }
    }
    if (err == ESP_OK && strcmp(verify, text) == 0) {
        return ESP_OK;
    }
    return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
}

static esp_err_t pn532_erase_ndef_selected_locked(const nfc_tag_t *tag, unsigned timeout_ms)
{
    if (!tag || tag->target_number == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t page_data[4] = {0x03, 0x00, 0xFE, 0x00}; /* Empty NDEF message TLV. */
    return pn532_type2_write_page(tag->target_number, 4, page_data, timeout_ms);
}

static void pn532_fill_type2_info_from_pages(nfc_type2_info_t *info)
{
    if (!info) {
        return;
    }
    memcpy(info->cc, &info->page0[12], sizeof(info->cc));
    info->type2_ndef = info->read0_err == ESP_OK &&
                       info->cc[0] == 0xE1 &&
                       (info->cc[1] & 0xF0) == 0x10 &&
                       info->cc[2] > 0;
    info->capacity = info->type2_ndef ? (size_t)info->cc[2] * 8 : 0;
    if (info->capacity > PN532_TYPE2_USER_MAX) {
        info->capacity = PN532_TYPE2_USER_MAX;
    }
}

esp_err_t nfc_pn532_probe_type2_from_tag(const nfc_tag_t *tag, nfc_type2_info_t *info,
                                         unsigned timeout_ms)
{
    if (!tag || tag->target_number == 0 || !info) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(info, 0, sizeof(*info));
    info->read0_err = ESP_ERR_INVALID_STATE;
    info->read4_err = ESP_ERR_INVALID_STATE;
    if (!s_ready) {
        ESP_RETURN_ON_ERROR(nfc_pn532_init(), TAG, "init failed");
    }
    ESP_RETURN_ON_ERROR(peripheral_arbiter_acquire(PERIPHERAL_USER_NFC, timeout_ms),
                        TAG, "resource busy");

    info->read0_err = pn532_type2_read_page(tag->target_number, 0, info->page0, timeout_ms);
    if (info->read0_err == ESP_OK) {
        pn532_fill_type2_info_from_pages(info);
        info->read4_err = pn532_type2_read_page(tag->target_number, 4, info->page4, timeout_ms);
    }

    peripheral_arbiter_release(PERIPHERAL_USER_NFC);
    if (info->read0_err != ESP_ERR_NOT_FOUND && info->read0_err != ESP_ERR_INVALID_RESPONSE) {
        pn532_recover_after_fault(info->read0_err);
    }
    if (info->read4_err != ESP_ERR_NOT_FOUND && info->read4_err != ESP_ERR_INVALID_RESPONSE) {
        pn532_recover_after_fault(info->read4_err);
    }
    if (info->read0_err != ESP_OK) {
        return info->read0_err;
    }
    return info->type2_ndef ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nfc_pn532_write_ndef_text_to_tag(const nfc_tag_t *tag, const char *text,
                                           unsigned timeout_ms)
{
    if (!text || text[0] == '\0' || !tag || tag->target_number == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    nfc_tag_t active = *tag;
    esp_err_t err = pn532_write_ndef_text_with_recovery(&active, text, timeout_ms);
    if (err != ESP_ERR_NOT_FOUND) {
        pn532_recover_after_fault(err);
    }
    return err;
}

esp_err_t nfc_pn532_write_ndef_text(const char *text, unsigned timeout_ms)
{
    if (!text || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        ESP_RETURN_ON_ERROR(nfc_pn532_init(), TAG, "init failed");
    }
    ESP_RETURN_ON_ERROR(peripheral_arbiter_acquire(PERIPHERAL_USER_NFC, timeout_ms),
                        TAG, "resource busy");

    nfc_tag_t tag = {0};
    esp_err_t err = pn532_select_passive_locked(&tag, timeout_ms);

    peripheral_arbiter_release(PERIPHERAL_USER_NFC);
    if (err == ESP_OK) {
        err = pn532_write_ndef_text_with_recovery(&tag, text, timeout_ms);
    }
    if (err != ESP_ERR_NOT_FOUND) {
        pn532_recover_after_fault(err);
    }
    return err;
}

esp_err_t nfc_pn532_erase_ndef_from_tag(const nfc_tag_t *tag, unsigned timeout_ms)
{
    if (!tag || tag->target_number == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    nfc_tag_t active = *tag;
    if (!s_ready) {
        ESP_RETURN_ON_ERROR(nfc_pn532_init(), TAG, "init failed");
    }
    ESP_RETURN_ON_ERROR(peripheral_arbiter_acquire(PERIPHERAL_USER_NFC, timeout_ms),
                        TAG, "resource busy");

    esp_err_t err = pn532_erase_ndef_selected_locked(&active, timeout_ms);

    peripheral_arbiter_release(PERIPHERAL_USER_NFC);
    uint8_t page_data[4] = {0x03, 0x00, 0xFE, 0x00};
    if (err == ESP_OK || err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_TIMEOUT) {
        esp_err_t verify_err = pn532_verify_type2_page(&active, 4, page_data, timeout_ms);
        if (verify_err == ESP_OK) {
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Type 2 erase returned %s but readback matched",
                         esp_err_to_name(err));
            }
            err = ESP_OK;
        } else if (err == ESP_OK) {
            err = verify_err;
        }
    }
    if (err != ESP_ERR_NOT_FOUND) {
        pn532_recover_after_fault(err);
    }
    return err;
}

esp_err_t nfc_pn532_erase_ndef(unsigned timeout_ms)
{
    if (!s_ready) {
        ESP_RETURN_ON_ERROR(nfc_pn532_init(), TAG, "init failed");
    }
    ESP_RETURN_ON_ERROR(peripheral_arbiter_acquire(PERIPHERAL_USER_NFC, timeout_ms),
                        TAG, "resource busy");

    nfc_tag_t tag = {0};
    esp_err_t err = pn532_select_passive_locked(&tag, timeout_ms);
    if (err == ESP_OK) {
        err = pn532_erase_ndef_selected_locked(&tag, timeout_ms);
    }

    peripheral_arbiter_release(PERIPHERAL_USER_NFC);
    uint8_t page_data[4] = {0x03, 0x00, 0xFE, 0x00};
    if (err == ESP_OK || err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_TIMEOUT) {
        esp_err_t verify_err = pn532_verify_type2_page(&tag, 4, page_data, timeout_ms);
        if (verify_err == ESP_OK) {
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Type 2 erase returned %s but readback matched",
                         esp_err_to_name(err));
            }
            err = ESP_OK;
        } else if (err == ESP_OK) {
            err = verify_err;
        }
    }
    if (err != ESP_ERR_NOT_FOUND) {
        pn532_recover_after_fault(err);
    }
    return err;
}

esp_err_t nfc_pn532_ping(unsigned timeout_ms)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(peripheral_arbiter_acquire(PERIPHERAL_USER_NFC, timeout_ms),
                        TAG, "resource busy");

    uint8_t resp[32] = {0};
    size_t len = 0;
    uint8_t get_fw[] = {0x02};
    esp_err_t err = pn532_cmd_response(get_fw, sizeof(get_fw), resp, sizeof(resp), &len, timeout_ms);
    if (err == ESP_OK && (len < 6 || resp[0] != PN532_PN532TOHOST || resp[1] != 0x03)) {
        err = ESP_ERR_INVALID_RESPONSE;
    }

    peripheral_arbiter_release(PERIPHERAL_USER_NFC);
    pn532_recover_after_fault(err);
    return err;
}

void nfc_pn532_mark_not_ready(void)
{
    if (s_ready) {
        ESP_LOGW(TAG, "PN532 marked not ready by service");
    }
    s_ready = false;
    s_passive_scan_pending = false;
    s_force_hw_reset = BOARD_NFC_RST_GPIO != GPIO_NUM_NC;
    (void)nfc_i2c_recover(200);
}

void nfc_pn532_suspend_for_camera(void)
{
    if (s_ready) {
        ESP_LOGI(TAG, "PN532 suspended for camera");
    }
    s_ready = false;
    s_passive_scan_pending = false;
    s_force_hw_reset = BOARD_NFC_RST_GPIO != GPIO_NUM_NC;
}

bool nfc_pn532_is_ready(void)
{
#if !BOARD_NFC_ENABLED
    return false;
#endif
    return s_ready;
}
