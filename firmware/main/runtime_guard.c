#include "runtime_guard.h"

#include "camera_mgr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "qr_scanner.h"

static const char *TAG = "runtime_guard";
static TaskHandle_t s_task;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static runtime_guard_status_t s_status = {
    .last_err = ESP_ERR_INVALID_STATE,
};

#define RUNTIME_GUARD_START_DELAY_MS       15000
#define RUNTIME_GUARD_INTERVAL_MS          30000
#define RUNTIME_GUARD_CAMERA_IDLE_MS       30000
#define RUNTIME_GUARD_INTERNAL_FREE_MIN    (64 * 1024)
#define RUNTIME_GUARD_INTERNAL_LARGEST_MIN (24 * 1024)
#define RUNTIME_GUARD_PSRAM_FREE_MIN       (512 * 1024)
#define RUNTIME_GUARD_PSRAM_LARGEST_MIN    (256 * 1024)
#define RUNTIME_GUARD_INTEGRITY_PERIOD     10
#define RUNTIME_GUARD_TASK_STACK_BYTES     4096

static void runtime_guard_task(void *arg)
{
    (void)arg;
    bool previous_pressure = false;
    vTaskDelay(pdMS_TO_TICKS(RUNTIME_GUARD_START_DELAY_MS));

    while (true) {
        uint32_t next_check;
        taskENTER_CRITICAL(&s_status_lock);
        next_check = s_status.check_count + 1;
        taskEXIT_CRITICAL(&s_status_lock);

        size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t internal_largest =
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        bool pressure = internal_free < RUNTIME_GUARD_INTERNAL_FREE_MIN ||
                        internal_largest < RUNTIME_GUARD_INTERNAL_LARGEST_MIN ||
                        (psram_total > 0 &&
                         (psram_free < RUNTIME_GUARD_PSRAM_FREE_MIN ||
                          psram_largest < RUNTIME_GUARD_PSRAM_LARGEST_MIN));

        bool cache_trimmed = qr_scanner_trim_cache(pressure);
        bool camera_cleaned = camera_mgr_maintenance(
            pressure ? 1000 : RUNTIME_GUARD_CAMERA_IDLE_MS);
        bool heap_ok = true;
        if (next_check % RUNTIME_GUARD_INTEGRITY_PERIOD == 0) {
            heap_ok = heap_caps_check_integrity_all(false);
        }

        internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        internal_largest =
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        size_t internal_min =
            heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t psram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);

        taskENTER_CRITICAL(&s_status_lock);
        s_status.uptime_seconds = (uint32_t)(esp_timer_get_time() / 1000000);
        s_status.check_count = next_check;
        s_status.low_memory_count += pressure ? 1 : 0;
        s_status.cache_trim_count += cache_trimmed ? 1 : 0;
        s_status.camera_cleanup_count += camera_cleaned ? 1 : 0;
        s_status.heap_integrity_fail_count += heap_ok ? 0 : 1;
        s_status.internal_free = internal_free;
        s_status.internal_min = internal_min;
        s_status.internal_largest = internal_largest;
        s_status.psram_free = psram_free;
        s_status.psram_min = psram_min;
        s_status.psram_largest = psram_largest;
        s_status.last_err = !heap_ok ? ESP_FAIL : (pressure ? ESP_ERR_NO_MEM : ESP_OK);
        taskEXIT_CRITICAL(&s_status_lock);

        if (!heap_ok) {
            ESP_LOGE(TAG, "heap integrity check failed at check %lu",
                     (unsigned long)next_check);
        } else if (pressure != previous_pressure ||
                   next_check % RUNTIME_GUARD_INTEGRITY_PERIOD == 0) {
            ESP_LOGI(TAG,
                     "check=%lu pressure=%d internal=%u largest=%u min=%u psram=%u largest=%u min=%u trim=%d camera=%d",
                     (unsigned long)next_check, pressure,
                     (unsigned)internal_free, (unsigned)internal_largest,
                     (unsigned)internal_min, (unsigned)psram_free,
                     (unsigned)psram_largest, (unsigned)psram_min,
                     cache_trimmed, camera_cleaned);
        }
        previous_pressure = pressure;
        vTaskDelay(pdMS_TO_TICKS(RUNTIME_GUARD_INTERVAL_MS));
    }
}

esp_err_t runtime_guard_start(void)
{
    if (s_task) {
        return ESP_OK;
    }
    taskENTER_CRITICAL(&s_status_lock);
    s_status.started = true;
    s_status.last_err = ESP_OK;
    taskEXIT_CRITICAL(&s_status_lock);

    BaseType_t created = xTaskCreate(runtime_guard_task, "runtime_guard",
                                    RUNTIME_GUARD_TASK_STACK_BYTES,
                                    NULL, 1, &s_task);
    if (created != pdPASS) {
        taskENTER_CRITICAL(&s_status_lock);
        s_status.started = false;
        s_status.last_err = ESP_ERR_NO_MEM;
        taskEXIT_CRITICAL(&s_status_lock);
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "runtime maintenance started; no periodic reboot is used");
    return ESP_OK;
}

runtime_guard_status_t runtime_guard_get_status(void)
{
    runtime_guard_status_t status;
    taskENTER_CRITICAL(&s_status_lock);
    status = s_status;
    taskEXIT_CRITICAL(&s_status_lock);
    status.uptime_seconds = (uint32_t)(esp_timer_get_time() / 1000000);
    return status;
}
