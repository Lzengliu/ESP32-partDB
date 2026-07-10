#include "nfc_pn532.h"

#include <stdbool.h>
#include <string.h>

#include "board_config.h"
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

static bool s_ready;

static void pn532_recover_after_fault(esp_err_t err)
{
    if (err == ESP_OK || err == ESP_ERR_NOT_FOUND) {
        return;
    }
    s_ready = false;
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
    uint8_t status = 0;
    unsigned waited = 0;
    while (waited <= timeout_ms) {
        esp_err_t err = nfc_i2c_read(BOARD_NFC_PN532_ADDR, &status, 1, 20);
        if (err == ESP_OK && status == 0x01) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t pn532_read_frame(uint8_t *out, size_t out_len, size_t *actual, unsigned timeout_ms)
{
    esp_err_t err = pn532_wait_ready(timeout_ms);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t raw[128] = {0};
    err = nfc_i2c_read(BOARD_NFC_PN532_ADDR, raw, sizeof(raw), 100);
    if (err != ESP_OK) {
        return err;
    }

    /* I2C read includes one leading status byte before the PN532 frame. */
    size_t pos = 1;
    while (pos + 5 < sizeof(raw) && !(raw[pos] == 0x00 && raw[pos + 1] == 0x00 && raw[pos + 2] == 0xff)) {
        pos++;
    }
    if (pos + 6 >= sizeof(raw)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t len = raw[pos + 3];
    if (((uint8_t)(len + raw[pos + 4])) != 0) {
        return ESP_ERR_INVALID_CRC;
    }
    if (pos + 5 + len >= sizeof(raw)) {
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

static esp_err_t pn532_read_ack(unsigned timeout_ms)
{
    esp_err_t err = pn532_wait_ready(timeout_ms);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t raw[8] = {0};
    err = nfc_i2c_read(BOARD_NFC_PN532_ADDR, raw, sizeof(raw), 100);
    if (err != ESP_OK) {
        return err;
    }
    const uint8_t ack[] = {0x00, 0x00, 0xff, 0x00, 0xff, 0x00};
    for (size_t offset = 1; offset + sizeof(ack) <= sizeof(raw); offset++) {
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
    if (err == ESP_OK && len >= 3 && resp[0] == PN532_PN532TOHOST && resp[1] == 0x4B && resp[2] == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err == ESP_OK && len >= 12 && resp[0] == PN532_PN532TOHOST && resp[1] == 0x4B && resp[2] >= 1) {
        tag->target_number = resp[3];
        uint8_t uid_len = resp[7];
        if (uid_len > sizeof(tag->uid)) {
            uid_len = sizeof(tag->uid);
        }
        tag->uid_len = uid_len;
        memcpy(tag->uid, &resp[8], uid_len);
        return ESP_OK;
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

esp_err_t nfc_pn532_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

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
    uint8_t get_fw[] = {0x02};
    err = ESP_FAIL;
    for (int attempt = 1; attempt <= PN532_INIT_ATTEMPTS; attempt++) {
        err = pn532_cmd_response(get_fw, sizeof(get_fw), resp, sizeof(resp), &len, 1500);
        if (err == ESP_OK && len >= 6 && resp[0] == PN532_PN532TOHOST && resp[1] == 0x03) {
            ESP_LOGI(TAG, "PN532 firmware IC=0x%02x ver=%u.%u", resp[2], resp[3], resp[4]);
            break;
        }
        if (err == ESP_OK) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
        ESP_LOGW(TAG, "GetFirmwareVersion attempt %d failed: %s", attempt, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (err != ESP_OK) {
        s_ready = false;
        peripheral_arbiter_release(PERIPHERAL_USER_NFC);
        return err;
    }

    uint8_t sam[] = {0x14, 0x01, 0x14, 0x01};
    err = pn532_cmd_response(sam, sizeof(sam), resp, sizeof(resp), &len, 1000);
    if (err == ESP_OK) {
        uint8_t retries[] = {0x32, 0x05, 0xff, 0x01, 0x03};
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
        size_t capacity = 0;
        err = pn532_type2_capacity_locked(tag.target_number, &capacity, timeout_ms);
        if (err == ESP_OK) {
            uint8_t data[PN532_TYPE2_USER_MAX] = {0};
            for (size_t offset = 0; offset < capacity; offset += 16) {
                uint8_t pages[16] = {0};
                uint8_t page = (uint8_t)(4 + offset / 4);
                err = pn532_type2_read_page(tag.target_number, page, pages, timeout_ms);
                if (err != ESP_OK) {
                    break;
                }
                size_t copy = capacity - offset;
                if (copy > sizeof(pages)) {
                    copy = sizeof(pages);
                }
                memcpy(&data[offset], pages, copy);
            }
            if (err == ESP_OK) {
                err = parse_ndef_text_record(data, capacity, out, out_len);
            }
        }
    }

    peripheral_arbiter_release(PERIPHERAL_USER_NFC);
    if (err == ESP_ERR_NOT_FOUND) {
        (void)nfc_pn532_ping(300);
    } else {
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
    if (err == ESP_OK) {
        size_t capacity = 0;
        err = pn532_type2_capacity_locked(tag.target_number, &capacity, timeout_ms);
        if (err == ESP_OK) {
            uint8_t payload[256] = {0};
            size_t payload_len = 0;
            err = build_ndef_text_record(text, payload, sizeof(payload), &payload_len);
            if (err == ESP_OK && payload_len > capacity) {
                err = ESP_ERR_INVALID_SIZE;
            }
            for (size_t offset = 0; err == ESP_OK && offset < payload_len; offset += 4) {
                uint8_t page_data[4] = {0};
                size_t copy = payload_len - offset;
                if (copy > sizeof(page_data)) {
                    copy = sizeof(page_data);
                }
                memcpy(page_data, &payload[offset], copy);
                uint8_t page = (uint8_t)(4 + offset / 4);
                err = pn532_type2_write_page(tag.target_number, page, page_data, timeout_ms);
            }
        }
    }

    peripheral_arbiter_release(PERIPHERAL_USER_NFC);
    if (err == ESP_ERR_NOT_FOUND) {
        (void)nfc_pn532_ping(300);
    } else {
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
    (void)nfc_i2c_recover(200);
}

bool nfc_pn532_is_ready(void)
{
    return s_ready;
}
