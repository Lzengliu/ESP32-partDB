#include "peripheral_arbiter.h"

#include <stdbool.h>

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_cam_nfc_mutex;
static peripheral_user_t s_owner;
static bool s_owned;

esp_err_t peripheral_arbiter_init(void)
{
    if (s_cam_nfc_mutex) {
        return ESP_OK;
    }
    s_cam_nfc_mutex = xSemaphoreCreateMutex();
    return s_cam_nfc_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t peripheral_arbiter_acquire(peripheral_user_t user, unsigned timeout_ms)
{
    if (!s_cam_nfc_mutex) {
        ESP_RETURN_ON_ERROR(peripheral_arbiter_init(), "arbiter", "init failed");
    }
    TickType_t ticks = timeout_ms == 0 ? 0 : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(s_cam_nfc_mutex, ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_owner = user;
    s_owned = true;
    return ESP_OK;
}

void peripheral_arbiter_release(peripheral_user_t user)
{
    if (!s_cam_nfc_mutex || !s_owned || s_owner != user) {
        return;
    }
    s_owned = false;
    xSemaphoreGive(s_cam_nfc_mutex);
}
