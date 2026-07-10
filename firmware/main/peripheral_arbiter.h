#pragma once

#include "esp_err.h"

typedef enum {
    PERIPHERAL_USER_CAMERA = 0,
    PERIPHERAL_USER_NFC,
} peripheral_user_t;

esp_err_t peripheral_arbiter_init(void);
esp_err_t peripheral_arbiter_acquire(peripheral_user_t user, unsigned timeout_ms);
void peripheral_arbiter_release(peripheral_user_t user);

