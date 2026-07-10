#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "esp_err.h"

typedef enum {
    DEVICE_UI_PAGE_HOME = 0,
    DEVICE_UI_PAGE_SHORTCUTS,
    DEVICE_UI_PAGE_RESULTS,
    DEVICE_UI_PAGE_DETAIL,
    DEVICE_UI_PAGE_INFO,
    DEVICE_UI_PAGE_SETTINGS,
    DEVICE_UI_PAGE_COUNT,
    DEVICE_UI_PAGE_PARTDB = DEVICE_UI_PAGE_DETAIL,
    DEVICE_UI_PAGE_NFC = DEVICE_UI_PAGE_SHORTCUTS,
    DEVICE_UI_PAGE_CAMERA = DEVICE_UI_PAGE_SHORTCUTS,
    DEVICE_UI_PAGE_HARDWARE = DEVICE_UI_PAGE_INFO,
} device_ui_page_t;

typedef struct {
    bool started;
    bool awake;
    uint8_t page;
    const char *page_name;
    uint32_t redraw_count;
    uint32_t handled_button_count;
    const char *last_button_event;
    uint32_t touch_event_count;
    const char *last_touch_event;
    bool touch_range_valid;
    uint16_t last_touch_raw_x;
    uint16_t last_touch_raw_y;
    uint16_t last_touch_x;
    uint16_t last_touch_y;
    uint16_t touch_raw_min_x;
    uint16_t touch_raw_max_x;
    uint16_t touch_raw_min_y;
    uint16_t touch_raw_max_y;
    uint16_t touch_min_x;
    uint16_t touch_max_x;
    uint16_t touch_min_y;
    uint16_t touch_max_y;
} device_ui_status_t;

esp_err_t device_ui_start(app_config_t *cfg);
esp_err_t device_ui_set_page(device_ui_page_t page);
esp_err_t device_ui_next_page(void);
esp_err_t device_ui_prev_page(void);
esp_err_t device_ui_request_redraw(void);
device_ui_status_t device_ui_get_status(void);
const char *device_ui_page_name(device_ui_page_t page);
