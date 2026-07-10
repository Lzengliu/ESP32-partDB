#include "nfc_service.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nfc_pn532.h"

static const char *TAG = "nfc_service";
#define NFC_POLL_TIMEOUT_MS 300
#define NFC_RECOVER_ERROR_THRESHOLD 3
#define NFC_IDLE_HEALTH_MISSES 25
#define NFC_HEALTH_CHECK_INTERVAL_MS 10000

static nfc_service_status_t s_status;
static TaskHandle_t s_task;
static volatile uint32_t s_request_suspend_count;
static TickType_t s_next_init_retry;

static void uid_to_hex(const nfc_tag_t *tag, char *out, size_t out_len)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!tag || !out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    size_t needed = (size_t)tag->uid_len * 2 + 1;
    if (out_len < needed) {
        return;
    }
    for (uint8_t i = 0; i < tag->uid_len; i++) {
        out[i * 2] = hex[tag->uid[i] >> 4];
        out[i * 2 + 1] = hex[tag->uid[i] & 0x0f];
    }
    out[tag->uid_len * 2] = '\0';
}

static void nfc_service_task(void *arg)
{
    (void)arg;
    s_status.started = true;
    uint32_t consecutive_errors = 0;
    uint32_t consecutive_not_found = 0;
    TickType_t next_health_check = 0;
    char last_uid[sizeof(s_status.uid)] = {0};
    ESP_LOGI(TAG, "NFC service started");

    while (true) {
        s_status.paused_for_camera = false;

        if (s_request_suspend_count > 0) {
            s_status.paused_for_request = true;
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }
        s_status.paused_for_request = false;

        esp_err_t err = ESP_OK;
        if (!nfc_pn532_is_ready()) {
            TickType_t now = xTaskGetTickCount();
            if (now < s_next_init_retry) {
                s_status.ready = false;
                vTaskDelay(pdMS_TO_TICKS(120));
                continue;
            }
            err = nfc_pn532_init();
            s_status.ready = err == ESP_OK;
            s_status.last_err = err;
            if (err != ESP_OK) {
                s_status.error_count++;
                s_next_init_retry = now + pdMS_TO_TICKS(5000);
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            consecutive_errors = 0;
            consecutive_not_found = 0;
            next_health_check = now + pdMS_TO_TICKS(NFC_HEALTH_CHECK_INTERVAL_MS);
        }
        s_status.ready = true;

        nfc_tag_t tag = {0};
        err = nfc_pn532_read_passive(&tag, NFC_POLL_TIMEOUT_MS);
        if (err == ESP_OK && tag.uid_len == 0) {
            err = ESP_ERR_NOT_FOUND;
        }
        s_status.last_err = err;
        if (err == ESP_OK && tag.uid_len > 0) {
            consecutive_errors = 0;
            consecutive_not_found = 0;
            next_health_check = xTaskGetTickCount() + pdMS_TO_TICKS(NFC_HEALTH_CHECK_INTERVAL_MS);
            char uid[sizeof(s_status.uid)] = {0};
            uid_to_hex(&tag, uid, sizeof(uid));
            bool changed = strcmp(uid, s_status.uid) != 0;
            snprintf(s_status.uid, sizeof(s_status.uid), "%s", uid);
            s_status.tag_present = true;
            s_status.last_seen_ms = esp_timer_get_time() / 1000;
            s_status.read_count++;
            if (changed || strcmp(uid, last_uid) != 0 || s_status.text[0] == '\0') {
                char text[sizeof(s_status.text)] = {0};
                esp_err_t text_err = nfc_pn532_read_ndef_text(text, sizeof(text), 1200);
                if (text_err == ESP_OK && text[0]) {
                    snprintf(s_status.text, sizeof(s_status.text), "%s", text);
                    s_status.text_read_count++;
                } else if (changed || strcmp(uid, last_uid) != 0) {
                    s_status.text[0] = '\0';
                }
                snprintf(last_uid, sizeof(last_uid), "%s", uid);
            }
            if (changed) {
                ESP_LOGI(TAG, "NFC tag detected uid=%s", s_status.uid);
            }
            vTaskDelay(pdMS_TO_TICKS(80));
        } else {
            if (err == ESP_ERR_NOT_FOUND) {
                consecutive_errors = 0;
                consecutive_not_found++;
                s_status.tag_present = false;
                s_status.text[0] = '\0';
                TickType_t now = xTaskGetTickCount();
                if (consecutive_not_found >= NFC_IDLE_HEALTH_MISSES && now >= next_health_check) {
                    esp_err_t ping_err = nfc_pn532_ping(500);
                    if (ping_err != ESP_OK) {
                        s_status.error_count++;
                        s_status.last_err = ping_err;
                        s_status.ready = false;
                        s_status.tag_present = false;
                        s_next_init_retry = now + pdMS_TO_TICKS(1000);
                        consecutive_errors = 0;
                        consecutive_not_found = 0;
                        ESP_LOGW(TAG, "NFC health check failed: %s", esp_err_to_name(ping_err));
                        nfc_pn532_mark_not_ready();
                        vTaskDelay(pdMS_TO_TICKS(250));
                        continue;
                    }
                    consecutive_not_found = 0;
                    next_health_check = now + pdMS_TO_TICKS(NFC_HEALTH_CHECK_INTERVAL_MS);
                }
            } else {
                consecutive_errors++;
                s_status.error_count++;
                ESP_LOGW(TAG, "NFC poll failed: %s", esp_err_to_name(err));
                if (consecutive_errors >= NFC_RECOVER_ERROR_THRESHOLD) {
                    TickType_t now = xTaskGetTickCount();
                    s_status.ready = false;
                    s_status.tag_present = false;
                    s_next_init_retry = now + pdMS_TO_TICKS(1000);
                    consecutive_errors = 0;
                    consecutive_not_found = 0;
                    ESP_LOGW(TAG, "NFC recovering after repeated poll errors");
                    nfc_pn532_mark_not_ready();
                    vTaskDelay(pdMS_TO_TICKS(250));
                    continue;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(120));
        }
    }
}

esp_err_t nfc_service_start(void)
{
    if (s_task) {
        return ESP_OK;
    }
    BaseType_t ok = xTaskCreate(nfc_service_task, "nfc_service", 4096, NULL, 4, &s_task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

nfc_service_status_t nfc_service_get_status(void)
{
    return s_status;
}

void nfc_service_suspend_for_request(void)
{
    s_request_suspend_count++;
}

void nfc_service_resume_after_request(void)
{
    if (s_request_suspend_count > 0) {
        s_request_suspend_count--;
    }
}
