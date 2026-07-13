#include "nfc_service.h"

#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nfc_i2c.h"
#include "nfc_pn532.h"
#include "peripheral_arbiter.h"

static const char *TAG = "nfc_service";
#define NFC_SCAN_IDLE_DELAY_MS 50
#define NFC_POLL_PRESENT_DELAY_MS 150
#define NFC_TAG_STALE_MS 10000
#define NFC_TEXT_RETRY_MS 800
#define NFC_NDEF_TIMEOUT_MS 900
#define NFC_RECOVER_ERROR_THRESHOLD 3
#define NFC_SCAN_PENDING_TIMEOUT_MS 2500

static nfc_service_status_t s_status;
static TaskHandle_t s_task;
static volatile uint32_t s_camera_suspend_count;
static volatile uint32_t s_request_suspend_count;
static TickType_t s_next_init_retry;
static nfc_tag_t s_active_tag;
static bool s_active_tag_valid;
static int64_t s_active_tag_seen_ms;
static int64_t s_scan_started_ms;
static bool s_scan_timeout_enabled;
static volatile bool s_force_text_refresh;

static void clear_active_tag(bool clear_uid)
{
    s_status.tag_present = false;
    if (clear_uid) {
        s_status.uid[0] = '\0';
        s_status.text[0] = '\0';
    }
    memset(&s_active_tag, 0, sizeof(s_active_tag));
    s_active_tag_valid = false;
    s_active_tag_seen_ms = 0;
}

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

static void remember_active_tag(const nfc_tag_t *tag)
{
    if (!tag || tag->uid_len == 0 || tag->target_number == 0) {
        return;
    }
    char uid[sizeof(s_status.uid)] = {0};
    uid_to_hex(tag, uid, sizeof(uid));
    snprintf(s_status.uid, sizeof(s_status.uid), "%s", uid);
    s_status.tag_present = true;
    s_status.ready = true;
    s_status.last_err = ESP_OK;
    s_status.last_seen_ms = esp_timer_get_time() / 1000;
    s_active_tag = *tag;
    s_active_tag_valid = true;
    s_active_tag_seen_ms = s_status.last_seen_ms;
}

static void nfc_service_task(void *arg)
{
    (void)arg;
    s_status.started = true;
    uint32_t consecutive_errors = 0;
    char last_uid[sizeof(s_status.uid)] = {0};
    ESP_LOGI(TAG, "NFC service started");

    while (true) {
        s_status.paused_for_camera = s_camera_suspend_count > 0;
        s_status.paused_for_request = s_request_suspend_count > 0;

        if (s_status.paused_for_camera || s_status.paused_for_request) {
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }

#if BOARD_NFC_SHARES_TOUCH_I2C
        /*
         * PN532 transactions can hold the shared touch bus during retries or clock
         * stretching. Keep the background service passive on this wiring; explicit
         * read/write requests temporarily pause the service and own the bus.
         */
        s_status.ready = nfc_pn532_is_ready();
        s_status.tag_present = false;
        s_status.last_err = ESP_ERR_INVALID_STATE;
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
#endif

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
        }
        s_status.ready = true;

#if !BOARD_NFC_BACKGROUND_POLL
        s_status.tag_present = false;
        s_status.last_err = ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
#endif

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (s_status.tag_present && s_active_tag_valid &&
            now_ms - s_status.last_seen_ms <= NFC_TAG_STALE_MS) {
            bool retry_missing_text = s_status.text[0] == '\0' &&
                                      now_ms - s_status.last_seen_ms >= NFC_TEXT_RETRY_MS;
            if (retry_missing_text) {
                last_uid[0] = '\0';
            } else {
                s_status.ready = true;
                s_status.last_err = ESP_OK;
                vTaskDelay(pdMS_TO_TICKS(NFC_POLL_PRESENT_DELAY_MS));
                continue;
            }
        }

        if (!nfc_pn532_passive_scan_pending()) {
            err = nfc_pn532_start_passive_scan();
            s_status.last_err = err;
            if (err != ESP_OK) {
                s_scan_started_ms = 0;
                s_scan_timeout_enabled = false;
                consecutive_errors++;
                s_status.error_count++;
                ESP_LOGW(TAG, "NFC scan start failed: %s", esp_err_to_name(err));
                if (consecutive_errors >= NFC_RECOVER_ERROR_THRESHOLD) {
                    TickType_t now = xTaskGetTickCount();
                    s_status.ready = false;
                    clear_active_tag(true);
                    s_next_init_retry = now + pdMS_TO_TICKS(1000);
                    consecutive_errors = 0;
                    ESP_LOGW(TAG, "NFC recovering after repeated scan start errors");
                    nfc_pn532_mark_not_ready();
                }
                vTaskDelay(pdMS_TO_TICKS(NFC_SCAN_IDLE_DELAY_MS));
                continue;
            }
            s_scan_started_ms = esp_timer_get_time() / 1000;
            s_scan_timeout_enabled = s_status.tag_present || s_active_tag_valid;
        }

        if (!nfc_pn532_passive_scan_ready()) {
            consecutive_errors = 0;
            s_status.ready = true;
            s_status.last_err = ESP_OK;
            now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (s_scan_timeout_enabled && s_scan_started_ms > 0 &&
                (int64_t)now_ms - s_scan_started_ms > NFC_SCAN_PENDING_TIMEOUT_MS) {
                s_status.ready = false;
                s_status.last_err = ESP_ERR_TIMEOUT;
                s_status.error_count++;
                s_scan_started_ms = 0;
                s_scan_timeout_enabled = false;
                ESP_LOGW(TAG, "NFC passive scan pending timeout");
                (void)nfc_service_restart(2500);
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }
            if (s_status.tag_present &&
                now_ms - s_status.last_seen_ms > NFC_TAG_STALE_MS) {
                clear_active_tag(true);
                last_uid[0] = '\0';
            }
            vTaskDelay(pdMS_TO_TICKS(NFC_SCAN_IDLE_DELAY_MS));
            continue;
        }

        nfc_tag_t tag = {0};
        err = nfc_pn532_read_passive_scan_result(&tag, 300);
        s_scan_started_ms = 0;
        s_scan_timeout_enabled = false;
        if (err == ESP_OK && tag.uid_len == 0) {
            err = ESP_ERR_NOT_FOUND;
        }
        s_status.last_err = err;
        if (err == ESP_OK && tag.uid_len > 0) {
            consecutive_errors = 0;
            char uid[sizeof(s_status.uid)] = {0};
            uid_to_hex(&tag, uid, sizeof(uid));
            bool changed = strcmp(uid, s_status.uid) != 0;
            bool should_read_text = s_force_text_refresh || strcmp(uid, last_uid) != 0;
            snprintf(s_status.uid, sizeof(s_status.uid), "%s", uid);
            s_status.tag_present = true;
            s_status.last_seen_ms = esp_timer_get_time() / 1000;
            s_active_tag = tag;
            s_active_tag_valid = true;
            s_active_tag_seen_ms = s_status.last_seen_ms;
            s_status.read_count++;
            if (should_read_text) {
                char text[sizeof(s_status.text)] = {0};
                esp_err_t text_err = nfc_pn532_read_ndef_text_from_tag(&tag, text, sizeof(text),
                                                                       NFC_NDEF_TIMEOUT_MS);
                if (text_err == ESP_OK && text[0]) {
                    snprintf(s_status.text, sizeof(s_status.text), "%s", text);
                    s_status.text_read_count++;
                    snprintf(last_uid, sizeof(last_uid), "%s", uid);
                    s_force_text_refresh = false;
                } else if (changed || text_err == ESP_ERR_NOT_FOUND ||
                           s_status.text[0] == '\0') {
                    s_status.text[0] = '\0';
                    if (text_err == ESP_ERR_TIMEOUT || text_err == ESP_ERR_INVALID_RESPONSE) {
                        last_uid[0] = '\0';
                    } else {
                        snprintf(last_uid, sizeof(last_uid), "%s", uid);
                        s_force_text_refresh = false;
                    }
                } else {
                    snprintf(last_uid, sizeof(last_uid), "%s", uid);
                    s_force_text_refresh = false;
                }
                if (!nfc_pn532_is_ready()) {
                    s_status.ready = false;
                    s_status.last_err = text_err;
                    clear_active_tag(false);
                    last_uid[0] = '\0';
                    ESP_LOGW(TAG, "NFC text read invalidated PN532: %s",
                             esp_err_to_name(text_err));
                    vTaskDelay(pdMS_TO_TICKS(NFC_SCAN_IDLE_DELAY_MS));
                    continue;
                }
            }
            if (changed) {
                ESP_LOGI(TAG, "NFC tag detected uid=%s", s_status.uid);
            }
            vTaskDelay(pdMS_TO_TICKS(NFC_POLL_PRESENT_DELAY_MS));
        } else {
            if (err == ESP_ERR_NOT_FOUND) {
                consecutive_errors = 0;
                s_status.ready = true;
                clear_active_tag(true);
                last_uid[0] = '\0';
            } else {
                consecutive_errors++;
                s_status.error_count++;
                ESP_LOGW(TAG, "NFC poll failed: %s", esp_err_to_name(err));
                if (consecutive_errors >= NFC_RECOVER_ERROR_THRESHOLD) {
                    TickType_t now = xTaskGetTickCount();
                    s_status.ready = false;
                    clear_active_tag(true);
                    s_next_init_retry = now + pdMS_TO_TICKS(1000);
                    consecutive_errors = 0;
                    ESP_LOGW(TAG, "NFC recovering after repeated poll errors");
                    nfc_pn532_mark_not_ready();
                    vTaskDelay(pdMS_TO_TICKS(250));
                    continue;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(NFC_SCAN_IDLE_DELAY_MS));
        }
    }
}

esp_err_t nfc_service_start(void)
{
#if !BOARD_NFC_ENABLED
    s_status.started = false;
    s_status.ready = false;
    s_status.last_err = ESP_ERR_NOT_SUPPORTED;
    return ESP_ERR_NOT_SUPPORTED;
#endif
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

bool nfc_service_get_active_tag(nfc_tag_t *out, int64_t max_age_ms)
{
    if (!out || !s_status.tag_present || !s_active_tag_valid ||
        s_active_tag.uid_len == 0 || s_active_tag.target_number == 0 ||
        !nfc_pn532_is_ready()) {
        return false;
    }
    int64_t now_ms = esp_timer_get_time() / 1000;
    int64_t age_ms = now_ms - s_active_tag_seen_ms;
    if (max_age_ms >= 0 && age_ms > max_age_ms) {
        return false;
    }
    *out = s_active_tag;
    return true;
}

esp_err_t nfc_service_claim_tag(nfc_tag_t *out, unsigned timeout_ms)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    int64_t deadline = esp_timer_get_time() / 1000 + timeout_ms;

    while (esp_timer_get_time() / 1000 <= deadline) {
        nfc_tag_t tag = {0};
        if (nfc_service_get_active_tag(&tag, NFC_TAG_STALE_MS)) {
            nfc_service_suspend_for_request();
            vTaskDelay(pdMS_TO_TICKS(40));
            if (!nfc_pn532_is_ready()) {
                clear_active_tag(false);
                nfc_service_resume_after_request();
                vTaskDelay(pdMS_TO_TICKS(80));
                continue;
            }

            if (nfc_pn532_passive_scan_pending()) {
                int64_t pending_deadline = esp_timer_get_time() / 1000 + 1200;
                while (nfc_pn532_passive_scan_pending() &&
                       esp_timer_get_time() / 1000 <= pending_deadline) {
                    if (nfc_pn532_passive_scan_ready()) {
                        nfc_tag_t fresh = {0};
                        esp_err_t scan_err = nfc_pn532_read_passive_scan_result(&fresh, 500);
                        if (scan_err == ESP_OK && fresh.uid_len > 0) {
                            remember_active_tag(&fresh);
                            *out = fresh;
                            return ESP_OK;
                        }
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(30));
                }
            }

            if (!nfc_pn532_passive_scan_pending() &&
                nfc_service_get_active_tag(&tag, NFC_TAG_STALE_MS)) {
                *out = tag;
                return ESP_OK;
            }

            /*
             * If a background scan is still pending after a card was already
             * observed, take ownership of PN532 by resetting the pending scan
             * and selecting the card again while the background service is
             * suspended. Otherwise a fresh background scan can keep the chip
             * busy and make UI write/erase report "card not found".
             */
            if (nfc_pn532_passive_scan_pending()) {
                s_scan_started_ms = 0;
                s_scan_timeout_enabled = false;
                nfc_pn532_mark_not_ready();
                clear_active_tag(false);
                esp_err_t init_err = nfc_pn532_init();
                if (init_err == ESP_OK) {
                    nfc_tag_t fresh = {0};
                    esp_err_t scan_err = nfc_pn532_read_passive(&fresh, 1500);
                    if (scan_err == ESP_OK && fresh.uid_len > 0) {
                        remember_active_tag(&fresh);
                        *out = fresh;
                        ESP_LOGI(TAG, "NFC claim recovered pending scan uid=%s",
                                 s_status.uid);
                        return ESP_OK;
                    }
                    s_status.last_err = scan_err;
                } else {
                    s_status.last_err = init_err;
                }
            }
            nfc_service_resume_after_request();
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    return ESP_ERR_NOT_FOUND;
}

void nfc_service_release_tag(void)
{
    nfc_service_resume_after_request();
}

void nfc_service_note_tag_payload(const nfc_tag_t *tag, const char *text)
{
    if (!tag || tag->uid_len == 0 || tag->target_number == 0) {
        return;
    }
    char uid[sizeof(s_status.uid)] = {0};
    uid_to_hex(tag, uid, sizeof(uid));
    snprintf(s_status.uid, sizeof(s_status.uid), "%s", uid);
    if (text && text[0]) {
        snprintf(s_status.text, sizeof(s_status.text), "%s", text);
    } else {
        s_status.text[0] = '\0';
    }
    s_status.tag_present = true;
    s_status.ready = true;
    s_status.last_err = ESP_OK;
    s_status.last_seen_ms = esp_timer_get_time() / 1000;
    s_active_tag = *tag;
    s_active_tag_valid = true;
    s_active_tag_seen_ms = s_status.last_seen_ms;
    s_force_text_refresh = false;
}

esp_err_t nfc_service_restart(unsigned timeout_ms)
{
    nfc_service_suspend_for_request();
    vTaskDelay(pdMS_TO_TICKS(80));

    esp_err_t wait_err = peripheral_arbiter_acquire(PERIPHERAL_USER_NFC, timeout_ms);
    if (wait_err == ESP_OK) {
        nfc_pn532_mark_not_ready();
        (void)nfc_i2c_release();
        peripheral_arbiter_release(PERIPHERAL_USER_NFC);
    } else {
        ESP_LOGW(TAG, "NFC restart could not take bus: %s", esp_err_to_name(wait_err));
        nfc_pn532_mark_not_ready();
    }

    clear_active_tag(true);
    s_scan_started_ms = 0;
    s_scan_timeout_enabled = false;
    s_next_init_retry = 0;
    esp_err_t err = nfc_pn532_init();
    s_status.ready = err == ESP_OK;
    s_status.last_err = err;
    if (err != ESP_OK) {
        s_status.error_count++;
    }
    nfc_service_resume_after_request();
    ESP_LOGI(TAG, "NFC restart: %s", esp_err_to_name(err));
    return err;
}

void nfc_service_suspend_for_camera(void)
{
    if (!BOARD_NFC_SHARES_CAMERA_SCCB) {
        s_camera_suspend_count = 0;
        s_status.paused_for_camera = false;
        return;
    }
    s_camera_suspend_count++;
    s_status.paused_for_camera = true;
    if (s_camera_suspend_count == 1) {
        esp_err_t err = peripheral_arbiter_acquire(PERIPHERAL_USER_NFC, 1500);
        if (err == ESP_OK) {
            nfc_pn532_suspend_for_camera();
            (void)nfc_i2c_recover(100);
            (void)nfc_i2c_release();
            peripheral_arbiter_release(PERIPHERAL_USER_NFC);
            s_status.ready = false;
            s_status.last_err = ESP_ERR_INVALID_STATE;
            ESP_LOGI(TAG, "NFC suspended for camera");
        } else {
            ESP_LOGW(TAG, "NFC suspend waiting for current operation timed out: %s",
                     esp_err_to_name(err));
        }
    }
}

void nfc_service_resume_after_camera(void)
{
    if (!BOARD_NFC_SHARES_CAMERA_SCCB) {
        s_camera_suspend_count = 0;
        s_status.paused_for_camera = false;
        return;
    }
    if (s_camera_suspend_count > 0) {
        s_camera_suspend_count--;
    }
    s_status.paused_for_camera = s_camera_suspend_count > 0;
    if (!s_status.paused_for_camera && s_task && !s_status.paused_for_request) {
#if BOARD_NFC_SHARES_TOUCH_I2C
        s_status.ready = nfc_pn532_is_ready();
        s_status.last_err = ESP_ERR_INVALID_STATE;
        ESP_LOGI(TAG, "NFC left passive after camera on shared touch I2C");
        return;
#endif
        esp_err_t err = nfc_pn532_init();
        s_status.ready = err == ESP_OK;
        s_status.last_err = err;
        ESP_LOGI(TAG, "NFC resumed after camera: %s", esp_err_to_name(err));
    }
}

void nfc_service_suspend_for_request(void)
{
    s_request_suspend_count++;
    s_status.paused_for_request = true;
}

void nfc_service_resume_after_request(void)
{
    if (s_request_suspend_count > 0) {
        s_request_suspend_count--;
    }
    s_status.paused_for_request = s_request_suspend_count > 0;
}
