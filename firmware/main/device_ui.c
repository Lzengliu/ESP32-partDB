#include "device_ui.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "button_input.h"
#include "board_config.h"
#include "camera_mgr.h"
#include "cJSON.h"
#include "display_ili9488.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hardware_diag.h"
#include "nfc_pn532.h"
#include "nfc_service.h"
#include "partdb_client.h"
#include "qr_scanner.h"
#include "storage_sd.h"
#include "touch_ft6336.h"
#include "ui_font.h"
#include "wifi_portal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "device_ui";

#define UI_STATUS_POLL_MS  1000
#define UI_POLL_MS         25
#define UI_TOUCH_POLL_MS   25
#define UI_TOUCH_RETRY_MS  3000
#define UI_TOUCH_SWIPE_PX  60
#define UI_TOUCH_TAP_PX    40
#define UI_TOUCH_MIN_SAMPLES 1
#define UI_SEARCH_MAX      32
#define UI_PART_FIELD_MAX  32
#define UI_DETAIL_TEXT_MAX 80
#define UI_PARTDB_BODY_MAX 16384
#define UI_SEARCH_RESULT_MAX 8
#define UI_SEARCH_RESULT_VISIBLE 4
#define UI_PARTS_LIST_MAX 5
#define UI_RESULTS_Y       112
#define UI_RESULTS_ROW_H   77
#define UI_RESULTS_ITEM_H  74
#define UI_KEYBOARD_H      240
#define UI_KEYBOARD_MARGIN 6
#define UI_KEYBOARD_GAP    3
#define UI_KEYBOARD_ROW_H  25
#define UI_KEYBOARD_TOP    6
#define UI_NAV_H           58
#define UI_TOP_H           102
#define UI_DETAIL_FIELD_X  12
#define UI_DETAIL_NAME_Y   106
#define UI_DETAIL_NAME_H   54
#define UI_DETAIL_FIELD_ROW_Y 166
#define UI_DETAIL_FIELD_H  34
#define UI_DETAIL_INFO_Y   208
#define UI_DETAIL_GAP      8
#define UI_DETAIL_ACTION_H 64
#define UI_DETAIL_BOTTOM_MARGIN 10
#define UI_DETAIL_ACTION_BUTTON_Y 30
#define UI_DETAIL_ACTION_BUTTON_H 29
#define UI_CAMERA_FRAME_W  296
#define UI_CAMERA_FRAME_H  222
#define UI_CAMERA_FRAME_LOW_W 160
#define UI_CAMERA_FRAME_LOW_H 120
#define UI_CAMERA_FRAME_Y  108
#define UI_CAMERA_BUTTON_H 46
#define UI_CAMERA_STOP_WAIT_MS 1800
#define UI_CAMERA_PREVIEW_DELAY_MS 30
#define UI_CAMERA_TASK_STACK_BYTES 24576
#define UI_SLEEP_DOUBLE_TAP_MS 500
#define UI_NFC_TAG_WAIT_MS 9000
#define UI_BG              0x0841
#define UI_PANEL           0x18E3
#define UI_PANEL_DARK      0x1082
#define UI_LINE            0x39E7
#define UI_TEXT            0xFFFF
#define UI_MUTED           0xBDF7
#define UI_GREEN           0x07E0
#define UI_RED             0xF800
#define UI_YELLOW          0xFFE0
#define UI_BLUE            0x051F
#define UI_CYAN            0x07FF
#define UI_ORANGE          0xFD20
#define UI_MAGENTA         0xF81F
#define UI_METAL           0x2945
#define UI_METAL_HI        0x6B6D
#define UI_SEG_DIM         0x1124
#define UI_HOME_ACCENT     0x05BF
#define UI_HOME_ACCENT_DIM 0x0351

static app_config_t *s_cfg;
static TaskHandle_t s_task;
static TaskHandle_t s_inventory_task;
static volatile bool s_inventory_refresh_requested;
static bool s_dirty = true;
static bool s_input_dirty;
static bool s_full_repaint = true;
static bool s_have_view_hash;
static uint32_t s_last_view_hash;
static uint32_t s_header_time_bucket;
static int64_t s_last_activity_ms;
static int64_t s_next_sleep_allowed_ms;
static int64_t s_sleep_touch_tap_ms;
static uint16_t s_sleep_touch_tap_x;
static uint16_t s_sleep_touch_tap_y;
static bool s_auto_sleep_applied;
static bool s_screen_sleeping;
static device_ui_status_t s_status = {
    .page = DEVICE_UI_PAGE_HOME,
    .page_name = "HOME",
    .last_button_event = "none",
    .last_touch_event = "none",
};

static uint16_t *ui_alloc_pixels_impl(size_t pixel_count, const char *owner)
{
    if (pixel_count == 0) {
        return NULL;
    }
    size_t bytes = pixel_count * sizeof(uint16_t);
    uint16_t *buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }
    if (!buf) {
        ESP_LOGW(TAG,
                 "ui pixel alloc failed owner=%s pixels=%u bytes=%u internal_largest=%u psram_largest=%u",
                 owner ? owner : "?",
                 (unsigned)pixel_count,
                 (unsigned)bytes,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    return buf;
}

#define ui_alloc_pixels(pixel_count) ui_alloc_pixels_impl((pixel_count), __func__)

static void ui_free_pixels(uint16_t *buf)
{
    if (buf) {
        heap_caps_free(buf);
    }
}

typedef enum {
    UI_INPUT_NONE = 0,
    UI_INPUT_SEARCH,
    UI_INPUT_IPN,
    UI_INPUT_QTY,
} ui_input_target_t;

typedef struct {
    bool valid;
    bool refreshing;
    uint32_t parts;
    uint32_t lots;
    uint32_t locations;
    uint32_t categories;
    uint32_t suppliers;
    uint32_t manufacturers;
    uint32_t footprints;
    uint32_t attachments;
    uint32_t projects;
    int last_http_status;
    esp_err_t last_err;
    uint32_t refresh_count;
    uint32_t updated_ms;
} ui_inventory_stats_t;

typedef struct {
    int id;
    float amount;
    bool amount_valid;
    char name[UI_DETAIL_TEXT_MAX];
    char ipn[UI_PART_FIELD_MAX + 1];
    char category[48];
} ui_search_result_item_t;

typedef struct {
    bool loading;
    bool valid;
    int result_count;
    int http_status;
    esp_err_t last_err;
    uint32_t search_count;
    uint32_t updated_ms;
    char query[UI_DETAIL_TEXT_MAX];
    ui_search_result_item_t items[UI_SEARCH_RESULT_MAX];
} ui_search_results_t;

typedef struct {
    int id;
    float amount;
    bool amount_valid;
    char name[UI_DETAIL_TEXT_MAX];
    char ipn[UI_PART_FIELD_MAX + 1];
    char category[48];
    char footprint[48];
    char description[UI_DETAIL_TEXT_MAX];
} ui_parts_list_item_t;

typedef struct {
    bool loading;
    bool valid;
    int page;
    int item_count;
    uint32_t total_items;
    int http_status;
    esp_err_t last_err;
    uint32_t fetch_count;
    uint32_t updated_ms;
    ui_parts_list_item_t items[UI_PARTS_LIST_MAX];
} ui_parts_list_t;

static char s_search[UI_SEARCH_MAX + 1];
static char s_detail_ipn[UI_PART_FIELD_MAX + 1] = "P0001";
static char s_detail_qty[UI_PART_FIELD_MAX + 1] = "1";
static bool s_keyboard_open;
static uint8_t s_keyboard_page;
static ui_input_target_t s_input_target;
static ui_inventory_stats_t s_inventory = {
    .last_err = ESP_ERR_INVALID_STATE,
};

typedef struct {
    bool loading;
    bool valid;
    bool found;
    bool is_lot;
    bool cache_hit;
    int id;
    int part_id;
    int lot_id;
    int lot_count;
    int http_status;
    float amount;
    bool amount_valid;
    bool amount_unknown;
    esp_err_t last_err;
    uint32_t fetch_count;
    uint32_t updated_ms;
    char query[UI_DETAIL_TEXT_MAX];
    char name[UI_DETAIL_TEXT_MAX];
    char ipn[UI_PART_FIELD_MAX + 1];
    char description[UI_DETAIL_TEXT_MAX];
    char comment[UI_DETAIL_TEXT_MAX];
    char category[48];
    char manufacturer[48];
    char mpn[48];
    char footprint[48];
    char location[48];
    char barcode[48];
    char parameters[UI_DETAIL_TEXT_MAX];
    int order_count;
    int attachment_count;
    int association_count;
    char supplier[48];
    char supplier_partnr[48];
    char attachment[48];
    char modules[UI_DETAIL_TEXT_MAX];
    char cache_source[12];
} ui_partdb_detail_t;

static ui_partdb_detail_t s_detail = {
    .last_err = ESP_ERR_INVALID_STATE,
};
static TaskHandle_t s_detail_task;
static ui_search_results_t s_results = {
    .last_err = ESP_ERR_INVALID_STATE,
};
static TaskHandle_t s_results_task;
static ui_parts_list_t s_parts_list = {
    .page = 1,
    .last_err = ESP_ERR_INVALID_STATE,
};
static TaskHandle_t s_parts_task;
static int s_results_scroll;
static int s_results_selected;
static int s_detail_info_page;

typedef enum {
    UI_STOCK_OP_NONE = 0,
    UI_STOCK_OP_IN,
    UI_STOCK_OP_OUT,
} ui_stock_op_t;

typedef enum {
    UI_NFC_ACTION_NONE = 0,
    UI_NFC_ACTION_WRITE_DETAIL,
    UI_NFC_ACTION_ERASE,
} ui_nfc_action_t;

typedef struct {
    char prefix;
    int id;
} ui_partdb_target_t;

typedef struct {
    bool busy;
    bool done;
    bool ok;
    esp_err_t last_err;
    int http_status;
    int lot_id;
    float qty;
    float old_amount;
    float new_amount;
    uint32_t apply_count;
    uint32_t updated_ms;
    char message[UI_DETAIL_TEXT_MAX];
} ui_stock_state_t;

static ui_stock_op_t s_stock_op = UI_STOCK_OP_NONE;
static ui_stock_state_t s_stock = {
    .last_err = ESP_ERR_INVALID_STATE,
};
static TaskHandle_t s_stock_task;

typedef struct {
    bool busy;
    bool done;
    bool ok;
    ui_nfc_action_t action;
    esp_err_t last_err;
    uint32_t write_count;
    uint32_t updated_ms;
    char payload[128];
    char message[UI_DETAIL_TEXT_MAX];
} ui_nfc_write_state_t;

static ui_nfc_write_state_t s_nfc_write = {
    .last_err = ESP_ERR_INVALID_STATE,
};
static TaskHandle_t s_nfc_write_task;
static bool s_nfc_confirm_open;
static ui_nfc_action_t s_nfc_confirm_action;
static bool s_nfc_result_holds_service;
static bool s_nfc_confirm_tag_valid;
static nfc_tag_t s_nfc_confirm_tag;

typedef struct {
    bool open;
    bool matched;
    uint32_t read_count;
    ui_partdb_target_t target;
    char uid[32];
    char raw[128];
    char query[32];
    char title[32];
    char message[96];
} ui_nfc_read_prompt_t;

static ui_nfc_read_prompt_t s_nfc_read_prompt;
static char s_last_nfc_prompt_key[192];

typedef enum {
    UI_SETTINGS_ACTION_NONE = 0,
    UI_SETTINGS_ACTION_AP_TOGGLE,
    UI_SETTINGS_ACTION_FONT_NEXT,
    UI_SETTINGS_ACTION_SCREEN_BG_NEXT,
    UI_SETTINGS_ACTION_LOCK_BG_NEXT,
    UI_SETTINGS_ACTION_BOOT_ANIM_NEXT,
    UI_SETTINGS_ACTION_NFC_READ_MODE,
    UI_SETTINGS_ACTION_NFC_RESTART,
    UI_SETTINGS_ACTION_SLEEP_TIMEOUT_NEXT,
} ui_settings_action_t;

typedef struct {
    bool active;
    bool action_sent;
    uint16_t start_x;
    uint16_t start_y;
    uint16_t last_x;
    uint16_t last_y;
    uint8_t samples;
    TickType_t started_at;
} touch_gesture_t;

static touch_gesture_t s_touch;
static TickType_t s_last_touch_poll;
static TickType_t s_next_touch_retry;
static uint32_t s_seen_nfc_read_count;
static TaskHandle_t s_settings_task;
static bool s_settings_busy;

typedef enum {
    UI_CAMERA_STOPPED = 0,
    UI_CAMERA_STARTING,
    UI_CAMERA_LIVE,
    UI_CAMERA_CAPTURING,
    UI_CAMERA_RESULT,
    UI_CAMERA_ERROR,
} ui_camera_mode_t;

typedef struct {
    volatile ui_camera_mode_t mode;
    volatile bool stop_requested;
    volatile bool capture_requested;
    volatile bool result_ready;
    esp_err_t last_err;
    uint32_t frame_seq;
    uint32_t capture_count;
    uint16_t frame_w;
    uint16_t frame_h;
    char message[UI_DETAIL_TEXT_MAX];
    qr_scanner_result_t result;
} ui_camera_state_t;

static ui_camera_state_t s_camera_ui = {
    .mode = UI_CAMERA_STOPPED,
    .last_err = ESP_ERR_INVALID_STATE,
};
static TaskHandle_t s_camera_task;
static uint16_t *s_camera_frame;
static uint16_t s_camera_frame_buf_w;
static uint16_t s_camera_frame_buf_h;
static bool s_camera_preview_low_mem;

static void enter_detail_with_object(const char *value);
static void enter_results_with_query(const char *query);
static bool enter_selected_result(void);
static void open_keyboard(ui_input_target_t target);
static void clear_stock_result(void);
static void clear_nfc_write_result(void);
static void release_nfc_result_hold(bool restart);
static void close_nfc_read_prompt(void);
static bool detail_nfc_payload(const ui_partdb_detail_t *detail, char *out, size_t out_len);
static void begin_nfc_write_from_detail(void);
static void begin_nfc_erase_from_settings(void);
static void detail_lookup_task(void *arg);
static void set_page_internal(device_ui_page_t page);
static void begin_camera_preview(void);
static void stop_camera_preview(void);
static void schedule_parts_list(int page);
static bool move_parts_page(int delta);
static bool enter_parts_list_item(int index);
static int main_page_index(device_ui_page_t page);

static const device_ui_page_t s_main_pages[] = {
    DEVICE_UI_PAGE_HOME,
    DEVICE_UI_PAGE_SHORTCUTS,
    DEVICE_UI_PAGE_INFO,
    DEVICE_UI_PAGE_SETTINGS,
};

#define UI_MAIN_PAGE_COUNT ((int)(sizeof(s_main_pages) / sizeof(s_main_pages[0])))

const char *device_ui_page_name(device_ui_page_t page)
{
    switch (page) {
    case DEVICE_UI_PAGE_HOME:
        return "HOME";
    case DEVICE_UI_PAGE_SHORTCUTS:
        return "SHORTCUTS";
    case DEVICE_UI_PAGE_RESULTS:
        return "RESULTS";
    case DEVICE_UI_PAGE_DETAIL:
        return "DETAIL";
    case DEVICE_UI_PAGE_INFO:
        return "PARTS";
    case DEVICE_UI_PAGE_SETTINGS:
        return "SETTINGS";
    case DEVICE_UI_PAGE_SYSTEM:
        return "SYSTEM";
    case DEVICE_UI_PAGE_NFC_SETTINGS:
        return "NFC_SETTINGS";
    case DEVICE_UI_PAGE_CAMERA:
        return "CAMERA";
    default:
        return "UNKNOWN";
    }
}

static void buffer_fill(uint16_t *buf, int w, int h, uint16_t color)
{
    if (!buf || w <= 0 || h <= 0) {
        return;
    }
    for (int i = 0; i < w * h; i++) {
        buf[i] = color;
    }
}

static void buffer_fill_rect(uint16_t *buf, int bw, int bh, int x, int y, int w, int h, uint16_t color)
{
    if (!buf || bw <= 0 || bh <= 0 || w <= 0 || h <= 0) {
        return;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > bw) {
        w = bw - x;
    }
    if (y + h > bh) {
        h = bh - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    for (int row = y; row < y + h; row++) {
        for (int col = x; col < x + w; col++) {
            buf[row * bw + col] = color;
        }
    }
}

static void buffer_text(uint16_t *buf, int bw, int bh, int x, int y,
                        const char *text, uint8_t scale, uint16_t fg)
{
    if (!buf || !text || scale == 0 || x >= bw || y >= bh) {
        return;
    }
    int char_h = 16 * scale;
    if (y + char_h <= 0 || x < 0) {
        return;
    }
    (void)ui_font_draw_text_maxw(buf, bw, bh, x, y, text, scale, fg, bw - x);
}

static void buffer_text_small(uint16_t *buf, int bw, int bh, int x, int y,
                              const char *text, uint16_t fg)
{
    if (!buf || !text || x >= bw || y >= bh) {
        return;
    }
    if (y + 12 <= 0 || x < 0) {
        return;
    }
    (void)ui_font_draw_text_small_maxw(buf, bw, bh, x, y, text, fg, bw - x);
}

static void draw_text_patch(int x, int y, int w, int h, const char *text,
                            uint16_t fg, uint16_t bg)
{
    if (!text || !text[0] || w <= 0 || h <= 0) {
        return;
    }
    size_t bytes = (size_t)w * h * sizeof(uint16_t);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (internal > largest) {
        largest = internal;
    }
    if (largest > 512) {
        largest -= 512;
    }
    if (bytes > largest) {
        int max_w = (int)(largest / ((size_t)h * sizeof(uint16_t)));
        if (max_w < 16) {
            return;
        }
        if (max_w < w) {
            w = max_w;
        }
    }
    uint16_t *buf = ui_alloc_pixels((size_t)w * h);
    if (!buf) {
        return;
    }
    buffer_fill(buf, w, h, bg);
    buffer_text(buf, w, h, 0, (h - 16) / 2, text, 1, fg);
    (void)display_ili9488_draw_bitmap565(x, y, w, h, buf);
    ui_free_pixels(buf);
}

static void draw_text_patch_small(int x, int y, int w, int h, const char *text,
                                  uint16_t fg, uint16_t bg)
{
    if (!text || !text[0] || w <= 0 || h <= 0) {
        return;
    }
    size_t bytes = (size_t)w * h * sizeof(uint16_t);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (internal > largest) {
        largest = internal;
    }
    if (largest > 512) {
        largest -= 512;
    }
    if (bytes > largest) {
        int max_w = (int)(largest / ((size_t)h * sizeof(uint16_t)));
        if (max_w < 16) {
            return;
        }
        if (max_w < w) {
            w = max_w;
        }
    }
    uint16_t *buf = ui_alloc_pixels((size_t)w * h);
    if (!buf) {
        return;
    }
    buffer_fill(buf, w, h, bg);
    buffer_text_small(buf, w, h, 0, (h - 12) / 2, text, fg);
    (void)display_ili9488_draw_bitmap565(x, y, w, h, buf);
    ui_free_pixels(buf);
}

static uint16_t page_accent(device_ui_page_t page)
{
    switch (page) {
    case DEVICE_UI_PAGE_RESULTS:
        return UI_CYAN;
    case DEVICE_UI_PAGE_SHORTCUTS:
        return UI_ORANGE;
    case DEVICE_UI_PAGE_DETAIL:
        return UI_GREEN;
    case DEVICE_UI_PAGE_INFO:
        return UI_YELLOW;
    case DEVICE_UI_PAGE_SETTINGS:
        return UI_BLUE;
    case DEVICE_UI_PAGE_NFC_SETTINGS:
        return UI_MAGENTA;
    case DEVICE_UI_PAGE_CAMERA:
        return UI_CYAN;
    case DEVICE_UI_PAGE_HOME:
    default:
        return UI_CYAN;
    }
}

static void draw_search_field_direct(int sx, int sy, int sw, int sh, uint16_t accent)
{
    (void)display_ili9488_fill_rect(sx, sy, sw, sh, UI_BG);
    (void)display_ili9488_fill_rect(sx, sy, sw, 1, accent);
    (void)display_ili9488_fill_rect(sx, sy + sh - 1, sw, 1, UI_LINE);
    (void)display_ili9488_fill_rect(sx, sy, 1, sh, UI_LINE);
    (void)display_ili9488_fill_rect(sx + sw - 1, sy, 1, sh, UI_LINE);

    char search_line[64];
    const char *text = "模糊搜索元件/IPN/条码";
    uint16_t color = UI_MUTED;
    if (s_search[0]) {
        snprintf(search_line, sizeof(search_line), "搜索 %s", s_search);
        text = search_line;
        color = UI_TEXT;
    }
    draw_text_patch(sx + 10, sy + 6, sw - 20, sh - 12, text, color, UI_BG);
}

static bool ui_wall_time_valid(const struct tm *tm)
{
    return tm && tm->tm_year + 1900 >= 2024;
}

static void header_time_text(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    time_t now = time(NULL);
    struct tm tm_now = {0};
    localtime_r(&now, &tm_now);
    if (ui_wall_time_valid(&tm_now)) {
        snprintf(out, out_len, "%02d/%02d %02d:%02d",
                 tm_now.tm_mon + 1, tm_now.tm_mday, tm_now.tm_hour, tm_now.tm_min);
        return;
    }

    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    uint32_t hours = uptime_s / 3600;
    uint32_t minutes = (uptime_s / 60) % 60;
    if (hours > 99) {
        hours = 99;
    }
    snprintf(out, out_len, "UP %02lu:%02lu",
             (unsigned long)hours, (unsigned long)minutes);
}

static uint32_t header_time_bucket(void)
{
    time_t now = time(NULL);
    struct tm tm_now = {0};
    localtime_r(&now, &tm_now);
    if (ui_wall_time_valid(&tm_now)) {
        return (uint32_t)(now / 60);
    }
    return (uint32_t)(esp_timer_get_time() / 60000000);
}

static void draw_header(const char *title, uint16_t accent)
{
    int w = display_ili9488_get_width();
    (void)display_ili9488_fill_rect(0, 0, w, 51, UI_PANEL_DARK);
    (void)display_ili9488_fill_rect(0, 0, w, 3, accent);
    (void)display_ili9488_fill_rect(0, 50, w, 1, UI_LINE);

    bool main_page = s_status.page == DEVICE_UI_PAGE_HOME ||
                     s_status.page == DEVICE_UI_PAGE_SHORTCUTS ||
                     s_status.page == DEVICE_UI_PAGE_INFO ||
                     s_status.page == DEVICE_UI_PAGE_SETTINGS;
    if (main_page) {
        const char *name = (s_cfg && s_cfg->device_name[0]) ? s_cfg->device_name : "Part-DB Terminal";
        draw_text_patch(12, 9, 140, 24, name, UI_TEXT, UI_PANEL_DARK);
    } else {
        (void)display_ili9488_fill_rect(10, 9, 58, 28, UI_PANEL);
        (void)display_ili9488_fill_rect(78, 9, 58, 28, UI_PANEL);
        draw_text_patch(18, 15, 44, 18, "返回", UI_TEXT, UI_PANEL);
        draw_text_patch(86, 15, 44, 18, "主页", UI_TEXT, UI_PANEL);
        draw_text_patch(150, 10, 110, 24, title && title[0] ? title : s_status.page_name,
                        UI_MUTED, UI_PANEL_DARK);
    }
    char time_line[16];
    header_time_text(time_line, sizeof(time_line));
    int text_w = (int)strlen(time_line) * 8;
    int tx = w - text_w - 12;
    if (tx < 160) {
        tx = 160;
    }
    if (main_page && time_line[0] && w > 180) {
        draw_text_patch(tx, 9, w - tx - 12, 24, time_line, UI_MUTED, UI_PANEL_DARK);
    }

    int sx = 12;
    int sy = 58;
    int sw = w - 24;
    int sh = 34;
    draw_search_field_direct(sx, sy, sw, sh, accent);
    (void)display_ili9488_fill_rect(0, UI_TOP_H - 1, w, 1, UI_LINE);
}

static void redraw_header_time_only(void)
{
    if (!display_ili9488_is_ready() || !display_ili9488_is_awake() ||
        s_keyboard_open || main_page_index((device_ui_page_t)s_status.page) < 0) {
        return;
    }
    int w = display_ili9488_get_width();
    if (w <= 180) {
        return;
    }
    char time_line[16];
    header_time_text(time_line, sizeof(time_line));
    int clear_x = 160;
    (void)display_ili9488_fill_rect(clear_x, 9, w - clear_x - 12, 24, UI_PANEL_DARK);
    int text_w = (int)strlen(time_line) * 8;
    int tx = w - text_w - 12;
    if (tx < clear_x) {
        tx = clear_x;
    }
    if (time_line[0]) {
        draw_text_patch(tx, 9, w - tx - 12, 24, time_line, UI_MUTED, UI_PANEL_DARK);
    }
}

static int main_page_index(device_ui_page_t page)
{
    for (int i = 0; i < UI_MAIN_PAGE_COUNT; i++) {
        if (s_main_pages[i] == page) {
            return i;
        }
    }
    return -1;
}

static void draw_footer(void)
{
    int w = display_ili9488_get_width();
    int screen_h = display_ili9488_get_height();
    int h = UI_NAV_H;
    int y0 = screen_h - h;
    (void)display_ili9488_fill_rect(0, y0, w, h, UI_PANEL_DARK);
    (void)display_ili9488_fill_rect(0, y0, w, 1, UI_LINE);
    const char *labels[] = {"主页", "快捷", "元件", "设置"};
    int active_idx = main_page_index((device_ui_page_t)s_status.page);
    int bw = w / UI_MAIN_PAGE_COUNT;
    for (int i = 0; i < UI_MAIN_PAGE_COUNT; i++) {
        int x = i * bw;
        int ww = (i == UI_MAIN_PAGE_COUNT - 1) ? (w - x) : bw;
        bool active = active_idx == i;
        (void)display_ili9488_fill_rect(x + 5, y0 + 8, ww - 10, h - 16,
                                        active ? UI_BLUE : UI_PANEL);
        (void)display_ili9488_fill_rect(x + 5, y0 + 8, ww - 10, 1,
                                        active ? UI_CYAN : UI_LINE);
        draw_text_patch(x + 23, y0 + 18, ww - 32, 24, labels[i],
                        active ? UI_TEXT : UI_MUTED,
                        active ? UI_BLUE : UI_PANEL);
    }
}

static void draw_card(int y, const char *title, const char *line1, const char *line2, uint16_t accent)
{
    int w = display_ili9488_get_width();
    int x = 12;
    int cw = w - 24;
    int ch = 76;
    uint16_t *buf = ui_alloc_pixels((size_t)cw * ch);
    if (!buf) {
        return;
    }
    buffer_fill(buf, cw, ch, UI_PANEL);
    buffer_fill_rect(buf, cw, ch, 0, 0, cw, 1, UI_LINE);
    buffer_fill_rect(buf, cw, ch, 0, ch - 1, cw, 1, UI_BG);
    buffer_fill_rect(buf, cw, ch, 0, 0, 5, ch, accent);
    buffer_text_small(buf, cw, ch, 15, 9, title, UI_MUTED);
    buffer_text(buf, cw, ch, 15, 31, line1, 1, UI_TEXT);
    if (line2 && line2[0]) {
        buffer_text_small(buf, cw, ch, 15, 58, line2, UI_MUTED);
    }
    (void)display_ili9488_draw_bitmap565(x, y, cw, ch, buf);
    ui_free_pixels(buf);
}

static void draw_tile(int x, int y, int cw, int ch, const char *title,
                      const char *value, const char *sub, uint16_t accent)
{
    if (cw <= 0 || ch <= 0) {
        return;
    }
    uint16_t *buf = ui_alloc_pixels((size_t)cw * ch);
    if (!buf) {
        return;
    }
    buffer_fill(buf, cw, ch, UI_PANEL);
    buffer_fill_rect(buf, cw, ch, 0, 0, cw, 1, UI_LINE);
    buffer_fill_rect(buf, cw, ch, 0, 0, 4, ch, accent);
    buffer_fill_rect(buf, cw, ch, 4, ch - 1, cw - 4, 1, UI_BG);
    int title_y = ch <= 56 ? 7 : 8;
    int value_y = ch <= 56 ? 25 : 27;
    int sub_y = ch <= 72 ? 57 : 60;
    bool show_sub = sub && sub[0] && ch >= 66;
    if (cw < 132 && ch < 78) {
        show_sub = false;
    }
    if (title && title[0]) {
        buffer_text_small(buf, cw, ch, 12, title_y, title, UI_MUTED);
    }
    if (value && value[0]) {
        buffer_text(buf, cw, ch, 12, value_y, value, 1, UI_TEXT);
    }
    if (show_sub) {
        buffer_text_small(buf, cw, ch, 12, sub_y, sub, UI_MUTED);
    }
    (void)display_ili9488_draw_bitmap565(x, y, cw, ch, buf);
    ui_free_pixels(buf);
}

static uint16_t ok_color(bool ok)
{
    return ok ? UI_GREEN : UI_RED;
}

static const char *path_basename(const char *path)
{
    if (!path || path[0] == '\0') {
        return "";
    }
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool ext_allowed(const char *name, const char *const *exts, int count)
{
    const char *dot = name ? strrchr(name, '.') : NULL;
    if (!dot || dot[1] == '\0') {
        return false;
    }
    dot++;
    for (int i = 0; i < count; i++) {
        if (strcasecmp(dot, exts[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool font_file_allowed(const char *name)
{
    static const char *const exts[] = {"ttf", "otf", "ttc", "woff", "woff2"};
    return ext_allowed(name, exts, (int)(sizeof(exts) / sizeof(exts[0])));
}

static bool image_file_allowed(const char *name)
{
    static const char *const exts[] = {"jpg", "jpeg", "png", "bmp", "gif", "webp"};
    return ext_allowed(name, exts, (int)(sizeof(exts) / sizeof(exts[0])));
}

static bool anim_file_allowed(const char *name)
{
    static const char *const exts[] = {"gif", "webp"};
    return ext_allowed(name, exts, (int)(sizeof(exts) / sizeof(exts[0])));
}

static esp_err_t choose_next_file_name(const char *abs_dir, const char *current_path,
                                       bool (*allowed)(const char *name),
                                       char *out, size_t out_len)
{
    if (!abs_dir || !allowed || !out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    DIR *dir = opendir(abs_dir);
    if (!dir) {
        return ESP_ERR_NOT_FOUND;
    }

    const char *current = path_basename(current_path);
    char first[APP_CONFIG_PATH_LEN] = {0};
    char next[APP_CONFIG_PATH_LEN] = {0};
    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0 ||
            !allowed(ent->d_name)) {
            continue;
        }
        char abs_path[APP_CONFIG_PATH_LEN * 2];
        int written = snprintf(abs_path, sizeof(abs_path), "%s/%s", abs_dir, ent->d_name);
        if (written < 0 || written >= (int)sizeof(abs_path)) {
            continue;
        }
        struct stat st;
        if (stat(abs_path, &st) != 0 || S_ISDIR(st.st_mode) || st.st_size <= 0) {
            continue;
        }
        if (first[0] == '\0' || strcmp(ent->d_name, first) < 0) {
            app_config_copy_string(first, sizeof(first), ent->d_name);
        }
        if ((current[0] == '\0' || strcmp(ent->d_name, current) > 0) &&
            (next[0] == '\0' || strcmp(ent->d_name, next) < 0)) {
            app_config_copy_string(next, sizeof(next), ent->d_name);
        }
    }
    closedir(dir);

    const char *chosen = next[0] ? next : first;
    if (!chosen || chosen[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }
    snprintf(out, out_len, "%s", chosen);
    return ESP_OK;
}

static esp_err_t save_config_and_redraw(void)
{
    if (!s_cfg) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = app_config_save(s_cfg);
    if (err == ESP_OK) {
        s_have_view_hash = false;
        s_dirty = true;
    }
    return err;
}

static uint8_t normalize_sleep_minutes(uint8_t minutes)
{
    static const uint8_t allowed[] = {5, 10, 15, 30, 60};
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (minutes <= allowed[i]) {
            return allowed[i];
        }
    }
    return allowed[sizeof(allowed) / sizeof(allowed[0]) - 1];
}

static int64_t ui_time_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static int64_t screen_sleep_timeout_ms(void)
{
    uint8_t minutes = s_cfg ? normalize_sleep_minutes(s_cfg->screen_sleep_minutes) : 5;
    return (int64_t)minutes * 60 * 1000;
}

static void mark_ui_activity(void)
{
    int64_t now_ms = ui_time_ms();
    s_last_activity_ms = now_ms;
    s_next_sleep_allowed_ms = now_ms + screen_sleep_timeout_ms();
    s_auto_sleep_applied = false;
}

static void wake_display_for_activity(const char *reason)
{
    esp_err_t err = display_ili9488_set_awake(true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "display wake failed reason=%s err=%s",
                 reason ? reason : "activity", esp_err_to_name(err));
        return;
    }
    s_screen_sleeping = false;
    mark_ui_activity();
    s_full_repaint = true;
    s_have_view_hash = false;
    s_dirty = true;
    ESP_LOGI(TAG, "display wake reason=%s", reason ? reason : "activity");
}

static void sleep_display_now(const char *reason)
{
    if (s_screen_sleeping) {
        return;
    }
    if (s_camera_task || s_camera_ui.mode != UI_CAMERA_STOPPED) {
        stop_camera_preview();
    }
    esp_err_t err = display_ili9488_set_awake(false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "display sleep failed reason=%s err=%s",
                 reason ? reason : "idle", esp_err_to_name(err));
        return;
    }
    s_screen_sleeping = true;
    s_auto_sleep_applied = true;
    s_sleep_touch_tap_ms = 0;
    s_dirty = false;
    s_input_dirty = false;
    int64_t idle_ms = ui_time_ms() - s_last_activity_ms;
    if (idle_ms < 0) {
        idle_ms = 0;
    }
    ESP_LOGI(TAG, "display sleep reason=%s timeout=%u min idle=%lld ms",
             reason ? reason : "idle",
             s_cfg ? normalize_sleep_minutes(s_cfg->screen_sleep_minutes) : 5,
             (long long)idle_ms);
}

static esp_err_t select_next_font(void)
{
    if (!s_cfg) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = storage_sd_prepare_paths();
    char name[APP_CONFIG_PATH_LEN] = {0};
    if (err == ESP_OK) {
        err = choose_next_file_name(BOARD_SD_FONT_DIR, s_cfg->font_path,
                                    font_file_allowed, name, sizeof(name));
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "font select failed: %s", esp_err_to_name(err));
        return err;
    }
    char path[APP_CONFIG_PATH_LEN];
    int written = snprintf(path, sizeof(path), "%s/%s", BOARD_SD_FONT_DIR, name);
    if (written < 0 || written >= (int)sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    app_config_copy_string(s_cfg->font_path, sizeof(s_cfg->font_path), path);
    ui_font_set_active_path(s_cfg->font_path);
    return save_config_and_redraw();
}

static esp_err_t select_next_asset(const char *abs_dir, const char *rel_dir,
                                   bool (*allowed)(const char *name), char *selected,
                                   size_t selected_len)
{
    if (!s_cfg || !abs_dir || !rel_dir || !allowed || !selected || selected_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = storage_sd_prepare_paths();
    char name[APP_CONFIG_PATH_LEN] = {0};
    if (err == ESP_OK) {
        err = choose_next_file_name(abs_dir, selected, allowed, name, sizeof(name));
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "asset select failed dir=%s err=%s", abs_dir, esp_err_to_name(err));
        return err;
    }
    char rel_path[APP_CONFIG_PATH_LEN];
    int written = snprintf(rel_path, sizeof(rel_path), "%s/%s", rel_dir, name);
    if (written < 0 || written >= (int)sizeof(rel_path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    app_config_copy_string(selected, selected_len, rel_path);
    return save_config_and_redraw();
}

static void cycle_brightness(void)
{
    if (!s_cfg) {
        return;
    }
    uint8_t current = s_cfg->display_brightness;
    uint8_t next = current < 40 ? 40 : (current < 60 ? 60 : (current < 80 ? 80 : (current < 100 ? 100 : 40)));
    s_cfg->display_brightness = next;
    (void)display_ili9488_set_brightness(next);
    (void)save_config_and_redraw();
}

static void cycle_sleep_timeout(void)
{
    if (!s_cfg) {
        return;
    }
    uint8_t current = normalize_sleep_minutes(s_cfg->screen_sleep_minutes);
    uint8_t next = current < 10 ? 10 :
                   (current < 15 ? 15 :
                   (current < 30 ? 30 :
                   (current < 60 ? 60 : 5)));
    s_cfg->screen_sleep_minutes = next;
    (void)save_config_and_redraw();
    mark_ui_activity();
}

static void toggle_ap_mode(void)
{
    if (!s_cfg) {
        return;
    }
    esp_err_t err = wifi_portal_set_ap_enabled(!s_cfg->ap_enabled);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AP toggle failed: %s", esp_err_to_name(err));
    }
    s_have_view_hash = false;
    s_dirty = true;
}

static void toggle_nfc_read_mode(void)
{
    if (!s_cfg) {
        return;
    }
    s_cfg->nfc_read_confirm = !s_cfg->nfc_read_confirm;
    s_last_nfc_prompt_key[0] = '\0';
    esp_err_t err = save_config_and_redraw();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NFC read mode save failed: %s", esp_err_to_name(err));
    }
}

static void settings_action_task(void *arg)
{
    ui_settings_action_t action = (ui_settings_action_t)(intptr_t)arg;
    switch (action) {
    case UI_SETTINGS_ACTION_AP_TOGGLE:
        toggle_ap_mode();
        break;
    case UI_SETTINGS_ACTION_FONT_NEXT:
        (void)select_next_font();
        break;
    case UI_SETTINGS_ACTION_SCREEN_BG_NEXT:
        if (s_cfg) {
            (void)select_next_asset(BOARD_SD_SCREEN_BG_DIR, "/backgrounds", image_file_allowed,
                                    s_cfg->screen_bg_path, sizeof(s_cfg->screen_bg_path));
        }
        break;
    case UI_SETTINGS_ACTION_LOCK_BG_NEXT:
        if (s_cfg) {
            (void)select_next_asset(BOARD_SD_LOCK_BG_DIR, "/lockscreen", image_file_allowed,
                                    s_cfg->lock_bg_path, sizeof(s_cfg->lock_bg_path));
        }
        break;
    case UI_SETTINGS_ACTION_BOOT_ANIM_NEXT:
        if (s_cfg) {
            (void)select_next_asset(BOARD_SD_BOOT_ANIM_DIR, "/boot/animation", anim_file_allowed,
                                    s_cfg->boot_anim_path, sizeof(s_cfg->boot_anim_path));
        }
        break;
    case UI_SETTINGS_ACTION_NFC_READ_MODE:
        toggle_nfc_read_mode();
        break;
    case UI_SETTINGS_ACTION_NFC_RESTART:
        (void)nfc_service_restart(2500);
        break;
    case UI_SETTINGS_ACTION_SLEEP_TIMEOUT_NEXT:
        cycle_sleep_timeout();
        break;
    case UI_SETTINGS_ACTION_NONE:
    default:
        break;
    }
    s_settings_busy = false;
    s_settings_task = NULL;
    s_have_view_hash = false;
    s_dirty = true;
    vTaskDelete(NULL);
}

static void schedule_settings_action(ui_settings_action_t action)
{
    if (action == UI_SETTINGS_ACTION_NONE || s_settings_busy) {
        s_dirty = true;
        return;
    }
    s_settings_busy = true;
    s_dirty = true;
    BaseType_t ok = xTaskCreate(settings_action_task, "ui_setting", 4096,
                                (void *)(intptr_t)action, 2, &s_settings_task);
    if (ok != pdPASS) {
        s_settings_busy = false;
        s_settings_task = NULL;
        ESP_LOGW(TAG, "settings action task failed");
    }
}

static void request_inventory_refresh(void)
{
    s_inventory_refresh_requested = true;
    if (s_inventory_task) {
        xTaskNotifyGive(s_inventory_task);
    }
}

static bool json_total_items(const char *body, uint32_t *out)
{
    if (!body || !out) {
        return false;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return false;
    }
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "hydra:totalItems");
    if (!cJSON_IsNumber(item)) {
        item = cJSON_GetObjectItemCaseSensitive(root, "totalItems");
    }
    bool ok = cJSON_IsNumber(item) && item->valuedouble >= 0;
    if (ok) {
        *out = (uint32_t)item->valuedouble;
    }
    cJSON_Delete(root);
    return ok;
}

static esp_err_t fetch_total_items(const char *path, uint32_t *out, int *http_status)
{
    if (!path || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    char *body = heap_caps_malloc(UI_PARTDB_BODY_MAX, MALLOC_CAP_8BIT);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    int status = 0;
    esp_err_t err = partdb_client_get_json(path, body, UI_PARTDB_BODY_MAX, &status);
    if (http_status) {
        *http_status = status;
    }
    if (err == ESP_OK && !json_total_items(body, out)) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    heap_caps_free(body);
    return err;
}

static bool inventory_stats_changed(const ui_inventory_stats_t *old,
                                    const ui_inventory_stats_t *next)
{
    if (!old || !next) {
        return true;
    }
    return old->valid != next->valid ||
           old->refreshing != next->refreshing ||
           old->parts != next->parts ||
           old->lots != next->lots ||
           old->locations != next->locations ||
           old->categories != next->categories ||
           old->suppliers != next->suppliers ||
           old->manufacturers != next->manufacturers ||
           old->footprints != next->footprints ||
           old->attachments != next->attachments ||
           old->projects != next->projects ||
           old->last_http_status != next->last_http_status ||
           old->last_err != next->last_err;
}

static bool s_detail_target_direct;
static ui_partdb_target_t s_detail_direct_target;
static bool s_detail_pending;
static bool s_detail_pending_direct;
static ui_partdb_target_t s_detail_pending_target;
static char s_detail_pending_query[UI_DETAIL_TEXT_MAX];

static bool json_copy_string(cJSON *root, const char *key, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return false;
    }
    out[0] = '\0';
    cJSON *item = root ? cJSON_GetObjectItem(root, key) : NULL;
    if (!cJSON_IsString(item) || !item->valuestring) {
        return false;
    }
    snprintf(out, out_len, "%s", item->valuestring);
    return out[0] != '\0';
}

static bool json_copy_nested_string(cJSON *root, const char *object_key,
                                    const char *string_key,
                                    char *out, size_t out_len)
{
    cJSON *obj = root ? cJSON_GetObjectItem(root, object_key) : NULL;
    return cJSON_IsObject(obj) && json_copy_string(obj, string_key, out, out_len);
}

static void copy_compact_text(const char *in, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!in) {
        return;
    }
    size_t w = 0;
    bool space = false;
    bool in_tag = false;
    for (const unsigned char *p = (const unsigned char *)in; *p && w + 1 < out_len; p++) {
        char c = (char)*p;
        if (c == '<') {
            in_tag = true;
            space = true;
            continue;
        }
        if (in_tag) {
            if (c == '>') {
                in_tag = false;
            }
            continue;
        }
        if (c == '\r' || c == '\n' || c == '\t') {
            space = true;
            continue;
        }
        if (c == ' ') {
            space = true;
            continue;
        }
        if (space && w > 0 && w + 1 < out_len) {
            out[w++] = ' ';
        }
        out[w++] = c;
        space = false;
    }
    out[w] = '\0';
}

static int json_array_object_count(cJSON *array)
{
    if (!cJSON_IsArray(array)) {
        return 0;
    }
    int count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        if (cJSON_IsObject(item)) {
            count++;
        }
    }
    return count;
}

static void append_detail_summary(char *out, size_t out_len, const char *value)
{
    if (!out || out_len == 0 || !value || !value[0]) {
        return;
    }
    char compact[48];
    copy_compact_text(value, compact, sizeof(compact));
    if (!compact[0]) {
        return;
    }
    size_t len = strlen(out);
    const char *sep = len > 0 ? " / " : "";
    int written = snprintf(out + len, out_len - len, "%s%.40s", sep, compact);
    if (written < 0 || written >= (int)(out_len - len)) {
        out[out_len - 1] = '\0';
    }
}

static bool parse_positive_int(const char *s, int *out)
{
    if (!s || !*s || !out) {
        return false;
    }
    int value = 0;
    while (*s) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        value = value * 10 + (*s - '0');
        if (value > 999999) {
            return false;
        }
        s++;
    }
    if (value <= 0) {
        return false;
    }
    *out = value;
    return true;
}

static bool parse_positive_int_until_space_or_end(const char *s, int *out)
{
    if (!s || !*s || !out) {
        return false;
    }
    int value = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9') {
        value = value * 10 + (*s - '0');
        digits++;
        if (value > 999999) {
            return false;
        }
        s++;
    }
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    if (*s != '\0' || digits <= 0 || value <= 0) {
        return false;
    }
    *out = value;
    return true;
}

static bool parse_id_after_marker(const char *s, const char *marker, int *out)
{
    const char *p = (s && marker) ? strstr(s, marker) : NULL;
    if (!p) {
        return false;
    }
    p += strlen(marker);
    int value = 0;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (*p - '0');
        p++;
    }
    if (value <= 0) {
        return false;
    }
    *out = value;
    return true;
}

static bool target_from_text(const char *text, ui_partdb_target_t *target)
{
    if (!text || !target) {
        return false;
    }
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        text++;
    }
    int id = 0;
    if (parse_id_after_marker(text, "/scan/part/", &id) ||
        parse_id_after_marker(text, "/api/parts/", &id) ||
        parse_id_after_marker(text, "/p/", &id) ||
        parse_id_after_marker(text, "/parts/", &id) ||
        parse_id_after_marker(text, "/part/", &id)) {
        target->prefix = 'P';
        target->id = id;
        return true;
    }
    if (parse_id_after_marker(text, "/scan/lot/", &id) ||
        parse_id_after_marker(text, "/api/part_lots/", &id) ||
        parse_id_after_marker(text, "/l/", &id) ||
        parse_id_after_marker(text, "/part_lots/", &id) ||
        parse_id_after_marker(text, "/lot/", &id)) {
        target->prefix = 'L';
        target->id = id;
        return true;
    }
    if ((text[0] == 'p' || text[0] == 'P') && text[1] == '/' &&
        parse_positive_int(text + 2, &id)) {
        target->prefix = 'P';
        target->id = id;
        return true;
    }
    if ((text[0] == 'l' || text[0] == 'L') && text[1] == '/' &&
        parse_positive_int(text + 2, &id)) {
        target->prefix = 'L';
        target->id = id;
        return true;
    }
    char prefix = (char)toupper((unsigned char)text[0]);
    if ((prefix == 'P' || prefix == 'L') && parse_positive_int(text + 1, &id)) {
        target->prefix = prefix;
        target->id = id;
        return true;
    }
    if ((prefix == 'P' || prefix == 'L') && text[1] == '-' &&
        parse_positive_int_until_space_or_end(text + 2, &id)) {
        target->prefix = prefix;
        target->id = id;
        return true;
    }
    size_t len = strlen(text);
    if ((len == 7 || len == 8) && parse_positive_int_until_space_or_end(text, &id)) {
        target->prefix = 'P';
        target->id = id / (len == 8 ? 10 : 1);
        return target->id > 0;
    }
    return false;
}

static void format_target_code(const ui_partdb_target_t *target, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!target || target->id <= 0) {
        return;
    }
    char prefix = target->prefix == 'L' ? 'L' : 'P';
    snprintf(out, out_len, "%c%04d", prefix, target->id);
}

static bool nfc_fast_route_from_text(const char *text, ui_partdb_target_t *target,
                                     char *query, size_t query_len)
{
    if (!text || !target) {
        return false;
    }
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        text++;
    }
    char prefix = (char)tolower((unsigned char)text[0]);
    if ((prefix != 'p' && prefix != 'l') || text[1] != '/') {
        return false;
    }

    const char *digits = text + 2;
    int id = 0;
    int digit_count = 0;
    while (*digits >= '0' && *digits <= '9') {
        id = id * 10 + (*digits - '0');
        digit_count++;
        if (digit_count > 6 || id > 999999) {
            return false;
        }
        digits++;
    }
    while (*digits == ' ' || *digits == '\t' || *digits == '\r' || *digits == '\n') {
        digits++;
    }
    if (*digits != '\0' || digit_count <= 0 || id <= 0) {
        return false;
    }

    target->prefix = prefix == 'p' ? 'P' : 'L';
    target->id = id;
    if (query && query_len > 0) {
        snprintf(query, query_len, "%c/%d", prefix, id);
    }
    return true;
}

static bool current_detail_matches_nfc_query(const char *query)
{
    if (!query || !query[0] || s_status.page != DEVICE_UI_PAGE_DETAIL) {
        return false;
    }
    char current[32] = {0};
    return detail_nfc_payload(&s_detail, current, sizeof(current)) &&
           strcasecmp(current, query) == 0;
}

static bool url_encode_component(const char *in, char *out, size_t out_len)
{
    if (!in || !out || out_len == 0) {
        return false;
    }
    static const char hex[] = "0123456789ABCDEF";
    size_t w = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        unsigned char c = *p;
        bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                    c == '.' || c == '~';
        if (keep) {
            if (w + 1 >= out_len) {
                return false;
            }
            out[w++] = (char)c;
        } else {
            if (w + 3 >= out_len) {
                return false;
            }
            out[w++] = '%';
            out[w++] = hex[c >> 4];
            out[w++] = hex[c & 0x0f];
        }
    }
    out[w] = '\0';
    return true;
}

static bool target_from_collection_body(const char *body, const char *marker,
                                        ui_partdb_target_t *target, char prefix)
{
    cJSON *parsed = body ? cJSON_Parse(body) : NULL;
    cJSON *members = parsed ? cJSON_GetObjectItem(parsed, "hydra:member") : NULL;
    cJSON *first = cJSON_IsArray(members) ? cJSON_GetArrayItem(members, 0) : NULL;
    cJSON *id_item = cJSON_IsObject(first) ? cJSON_GetObjectItem(first, "id") : NULL;
    int id = 0;
    if (cJSON_IsNumber(id_item)) {
        id = id_item->valueint;
    } else {
        cJSON *iri = cJSON_IsObject(first) ? cJSON_GetObjectItem(first, "@id") : NULL;
        if (cJSON_IsString(iri)) {
            (void)parse_id_after_marker(iri->valuestring, marker, &id);
        }
    }
    if (parsed) {
        cJSON_Delete(parsed);
    }
    if (id <= 0 || !target) {
        return false;
    }
    target->prefix = prefix;
    target->id = id;
    return true;
}

static esp_err_t lookup_part_by_like_field(const char *field, const char *encoded,
                                           char *body, size_t body_len,
                                           ui_partdb_target_t *target,
                                           int *http_status)
{
    if (!field || !encoded || !body || body_len == 0 || !target) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[448];
    int written = snprintf(path, sizeof(path),
                           "/api/parts.jsonld?%s=%%25%s%%25&itemsPerPage=1&"
                           "properties[]=id&properties[]=ipn&properties[]=name",
                           field, encoded);
    if (written < 0 || written >= (int)sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    int status = 0;
    esp_err_t err = partdb_client_get_json(path, body, body_len, &status);
    if (http_status) {
        *http_status = status;
    }
    if (err == ESP_OK && status >= 200 && status < 300 &&
        target_from_collection_body(body, "/parts/", target, 'P')) {
        return ESP_OK;
    }
    return err == ESP_OK ? ESP_ERR_NOT_FOUND : err;
}

static esp_err_t lookup_part_by_fuzzy_query(const char *encoded, char *body,
                                            size_t body_len,
                                            ui_partdb_target_t *target,
                                            int *http_status)
{
    static const char *const fields[] = {
        "ipn",
        "name",
        "description",
        "manufacturer_product_number",
    };
    esp_err_t last_err = ESP_ERR_NOT_FOUND;
    for (int i = 0; i < (int)(sizeof(fields) / sizeof(fields[0])); i++) {
        last_err = lookup_part_by_like_field(fields[i], encoded, body, body_len,
                                             target, http_status);
        if (last_err == ESP_OK) {
            return ESP_OK;
        }
    }
    return last_err;
}

static esp_err_t lookup_target_by_query(const char *query, ui_partdb_target_t *target,
                                        int *http_status)
{
    if (!query || !query[0] || !target) {
        return ESP_ERR_INVALID_ARG;
    }
    if (target_from_text(query, target)) {
        if (http_status) {
            *http_status = 0;
        }
        return ESP_OK;
    }

    char encoded[UI_DETAIL_TEXT_MAX * 3 + 1];
    if (!url_encode_component(query, encoded, sizeof(encoded))) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *body = heap_caps_malloc(UI_PARTDB_BODY_MAX, MALLOC_CAP_8BIT);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    char path[384];
    int status = 0;
    snprintf(path, sizeof(path),
             "/api/part_lots.jsonld?user_barcode=%s&itemsPerPage=1&"
             "properties[]=id&properties[]=user_barcode",
             encoded);
    esp_err_t err = partdb_client_get_json(path, body, UI_PARTDB_BODY_MAX, &status);
    if (http_status) {
        *http_status = status;
    }
    if (err == ESP_OK && status >= 200 && status < 300 &&
        target_from_collection_body(body, "/part_lots/", target, 'L')) {
        heap_caps_free(body);
        return ESP_OK;
    }

    err = lookup_part_by_fuzzy_query(encoded, body, UI_PARTDB_BODY_MAX, target, &status);
    if (http_status) {
        *http_status = status;
    }
    bool found = err == ESP_OK;
    heap_caps_free(body);
    return found ? ESP_OK : (err == ESP_OK ? ESP_ERR_NOT_FOUND : err);
}

static bool json_object_api_id(cJSON *obj, const char *marker, int *out)
{
    if (!obj || !out) {
        return false;
    }
    cJSON *id = cJSON_GetObjectItem(obj, "id");
    if (cJSON_IsNumber(id) && id->valueint > 0) {
        *out = id->valueint;
        return true;
    }
    cJSON *iri = cJSON_GetObjectItem(obj, "@id");
    if (cJSON_IsString(iri) && parse_id_after_marker(iri->valuestring, marker, out)) {
        return true;
    }
    return false;
}

static int search_result_index_by_id(const ui_search_results_t *results, int id)
{
    if (!results || id <= 0) {
        return -1;
    }
    for (int i = 0; i < results->result_count && i < UI_SEARCH_RESULT_MAX; i++) {
        if (results->items[i].id == id) {
            return i;
        }
    }
    return -1;
}

static bool add_search_result_from_part(cJSON *part, ui_search_results_t *results)
{
    if (!cJSON_IsObject(part) || !results || results->result_count >= UI_SEARCH_RESULT_MAX) {
        return false;
    }
    int id = 0;
    if (!json_object_api_id(part, "/parts/", &id) || id <= 0 ||
        search_result_index_by_id(results, id) >= 0) {
        return false;
    }
    ui_search_result_item_t *item = &results->items[results->result_count];
    memset(item, 0, sizeof(*item));
    item->id = id;
    (void)json_copy_string(part, "name", item->name, sizeof(item->name));
    (void)json_copy_string(part, "ipn", item->ipn, sizeof(item->ipn));
    cJSON *stock = cJSON_GetObjectItem(part, "total_instock");
    if (cJSON_IsNumber(stock)) {
        item->amount = (float)stock->valuedouble;
        item->amount_valid = true;
    }
    cJSON *category = cJSON_GetObjectItem(part, "category");
    cJSON *cat_name = cJSON_IsObject(category) ? cJSON_GetObjectItem(category, "name") : NULL;
    if (cJSON_IsString(cat_name)) {
        snprintf(item->category, sizeof(item->category), "%s", cat_name->valuestring);
    }
    results->result_count++;
    return true;
}

static esp_err_t add_search_result_for_part_id(int id, ui_search_results_t *results,
                                               char *body, size_t body_len,
                                               int *http_status)
{
    if (id <= 0 || !results || !body || body_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[384];
    snprintf(path, sizeof(path),
             "/api/parts/%d.jsonld?properties[]=id&properties[]=name&properties[]=ipn&"
             "properties[]=total_instock&properties[category][]=name",
             id);
    int status = 0;
    esp_err_t err = partdb_client_get_json(path, body, body_len, &status);
    if (http_status) {
        *http_status = status;
    }
    bool ok = err == ESP_OK && status >= 200 && status < 300;
    cJSON *parsed = ok ? cJSON_Parse(body) : NULL;
    if (parsed) {
        (void)add_search_result_from_part(parsed, results);
        cJSON_Delete(parsed);
    } else if (ok) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    return err;
}

static esp_err_t add_search_result_for_lot_id(int id, ui_search_results_t *results,
                                              char *body, size_t body_len,
                                              int *http_status)
{
    if (id <= 0 || !results || !body || body_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[384];
    snprintf(path, sizeof(path),
             "/api/part_lots/%d.jsonld?properties[]=id&"
             "properties[part][]=id&properties[part][]=name&properties[part][]=ipn",
             id);
    int status = 0;
    esp_err_t err = partdb_client_get_json(path, body, body_len, &status);
    if (http_status) {
        *http_status = status;
    }
    bool ok = err == ESP_OK && status >= 200 && status < 300;
    cJSON *parsed = ok ? cJSON_Parse(body) : NULL;
    cJSON *part = parsed ? cJSON_GetObjectItem(parsed, "part") : NULL;
    if (cJSON_IsObject(part)) {
        (void)add_search_result_from_part(part, results);
    } else if (ok) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (parsed) {
        cJSON_Delete(parsed);
    }
    return err;
}

static esp_err_t fetch_search_results_field(const char *field, const char *encoded,
                                            ui_search_results_t *results,
                                            char *body, size_t body_len,
                                            int *http_status)
{
    if (!field || !encoded || !results || !body || body_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[512];
    int written = snprintf(path, sizeof(path),
                           "/api/parts.jsonld?%s=%%25%s%%25&itemsPerPage=%d&"
                           "properties[]=id&properties[]=name&properties[]=ipn&"
                           "properties[]=total_instock&properties[category][]=name",
                           field, encoded, UI_SEARCH_RESULT_MAX);
    if (written < 0 || written >= (int)sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    int status = 0;
    esp_err_t err = partdb_client_get_json(path, body, body_len, &status);
    if (http_status) {
        *http_status = status;
    }
    if (err != ESP_OK || status < 200 || status >= 300) {
        return err == ESP_OK ? ESP_FAIL : err;
    }
    cJSON *parsed = cJSON_Parse(body);
    cJSON *members = parsed ? cJSON_GetObjectItem(parsed, "hydra:member") : NULL;
    if (!cJSON_IsArray(members)) {
        if (parsed) {
            cJSON_Delete(parsed);
        }
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *part = NULL;
    cJSON_ArrayForEach(part, members) {
        if (results->result_count >= UI_SEARCH_RESULT_MAX) {
            break;
        }
        (void)add_search_result_from_part(part, results);
    }
    cJSON_Delete(parsed);
    return ESP_OK;
}

static void search_results_task(void *arg)
{
    (void)arg;
    ui_search_results_t next = s_results;
    next.loading = true;
    next.valid = false;
    next.result_count = 0;
    next.http_status = 0;
    next.last_err = ESP_ERR_INVALID_STATE;
    memset(next.items, 0, sizeof(next.items));

    char *body = heap_caps_malloc(UI_PARTDB_BODY_MAX, MALLOC_CAP_8BIT);
    esp_err_t err = body ? ESP_OK : ESP_ERR_NO_MEM;
    int status = 0;

    if (err == ESP_OK) {
        ui_partdb_target_t target = {0};
        if (target_from_text(next.query, &target)) {
            if (target.prefix == 'P') {
                err = add_search_result_for_part_id(target.id, &next, body,
                                                    UI_PARTDB_BODY_MAX, &status);
            } else if (target.prefix == 'L') {
                err = add_search_result_for_lot_id(target.id, &next, body,
                                                   UI_PARTDB_BODY_MAX, &status);
            }
            next.http_status = status;
        } else {
            char encoded[UI_DETAIL_TEXT_MAX * 3 + 1];
            if (!url_encode_component(next.query, encoded, sizeof(encoded))) {
                err = ESP_ERR_INVALID_SIZE;
            } else {
                static const char *const fields[] = {
                    "ipn",
                    "name",
                    "description",
                    "manufacturer_product_number",
                };
                err = ESP_ERR_NOT_FOUND;
                for (int i = 0; i < (int)(sizeof(fields) / sizeof(fields[0])); i++) {
                    esp_err_t field_err = fetch_search_results_field(fields[i], encoded,
                                                                     &next, body,
                                                                     UI_PARTDB_BODY_MAX,
                                                                     &status);
                    next.http_status = status;
                    if (field_err == ESP_OK) {
                        err = ESP_OK;
                    } else if (err != ESP_OK) {
                        err = field_err;
                    }
                    if (next.result_count >= UI_SEARCH_RESULT_MAX) {
                        break;
                    }
                }
                if (err == ESP_OK && next.result_count == 0) {
                    err = ESP_ERR_NOT_FOUND;
                }
            }
        }
    }

    if (body) {
        heap_caps_free(body);
    }
    next.loading = false;
    next.valid = err == ESP_OK;
    next.last_err = err;
    next.search_count++;
    next.updated_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_results = next;
    s_results_task = NULL;
    if (s_status.page == DEVICE_UI_PAGE_RESULTS) {
        s_dirty = true;
    }
    vTaskDelete(NULL);
}

static void schedule_search_results(const char *query)
{
    if (!query || !query[0]) {
        return;
    }
    if (s_results_task) {
        return;
    }
    memset(&s_results, 0, sizeof(s_results));
    s_results.loading = true;
    s_results.last_err = ESP_ERR_INVALID_STATE;
    snprintf(s_results.query, sizeof(s_results.query), "%s", query);
    s_results_scroll = 0;
    s_results_selected = 0;
    s_dirty = true;
    BaseType_t ok = xTaskCreate(search_results_task, "ui_search", 7168,
                                NULL, 2, &s_results_task);
    if (ok != pdPASS) {
        s_results_task = NULL;
        s_results.loading = false;
        s_results.valid = false;
        s_results.last_err = ESP_ERR_NO_MEM;
    }
}

static bool add_parts_list_item_from_part(cJSON *part, ui_parts_list_t *list)
{
    if (!cJSON_IsObject(part) || !list || list->item_count >= UI_PARTS_LIST_MAX) {
        return false;
    }
    int id = 0;
    if (!json_object_api_id(part, "/parts/", &id) || id <= 0) {
        return false;
    }
    ui_parts_list_item_t *item = &list->items[list->item_count];
    memset(item, 0, sizeof(*item));
    item->id = id;
    (void)json_copy_string(part, "name", item->name, sizeof(item->name));
    (void)json_copy_string(part, "ipn", item->ipn, sizeof(item->ipn));
    (void)json_copy_string(part, "description", item->description, sizeof(item->description));
    cJSON *stock = cJSON_GetObjectItem(part, "total_instock");
    if (cJSON_IsNumber(stock)) {
        item->amount = (float)stock->valuedouble;
        item->amount_valid = true;
    }
    (void)json_copy_nested_string(part, "category", "name",
                                  item->category, sizeof(item->category));
    (void)json_copy_nested_string(part, "footprint", "name",
                                  item->footprint, sizeof(item->footprint));
    list->item_count++;
    return true;
}

static void parts_list_task(void *arg)
{
    (void)arg;
    ui_parts_list_t next = s_parts_list;
    next.loading = true;
    next.valid = false;
    next.item_count = 0;
    next.http_status = 0;
    next.last_err = ESP_ERR_INVALID_STATE;
    memset(next.items, 0, sizeof(next.items));

    char *body = heap_caps_malloc(UI_PARTDB_BODY_MAX, MALLOC_CAP_8BIT);
    esp_err_t err = body ? ESP_OK : ESP_ERR_NO_MEM;
    int status = 0;
    if (err == ESP_OK) {
        char path[512];
        int written = snprintf(path, sizeof(path),
                               "/api/parts.jsonld?itemsPerPage=%d&page=%d&"
                               "properties[]=id&properties[]=name&properties[]=ipn&"
                               "properties[]=description&properties[]=total_instock&"
                               "properties[category][]=name&properties[footprint][]=name",
                               UI_PARTS_LIST_MAX, next.page <= 0 ? 1 : next.page);
        if (written < 0 || written >= (int)sizeof(path)) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            err = partdb_client_get_json(path, body, UI_PARTDB_BODY_MAX, &status);
            next.http_status = status;
        }
    }

    if (err == ESP_OK && status >= 200 && status < 300) {
        cJSON *parsed = cJSON_Parse(body);
        cJSON *members = parsed ? cJSON_GetObjectItem(parsed, "hydra:member") : NULL;
        if (!cJSON_IsArray(members)) {
            err = ESP_ERR_INVALID_RESPONSE;
        } else {
            uint32_t total = 0;
            if (json_total_items(body, &total)) {
                next.total_items = total;
            }
            cJSON *part = NULL;
            cJSON_ArrayForEach(part, members) {
                if (next.item_count >= UI_PARTS_LIST_MAX) {
                    break;
                }
                (void)add_parts_list_item_from_part(part, &next);
            }
            if (next.item_count == 0 && next.total_items > 0) {
                err = ESP_ERR_NOT_FOUND;
            }
        }
        if (parsed) {
            cJSON_Delete(parsed);
        }
    } else if (err == ESP_OK) {
        err = ESP_FAIL;
    }

    if (body) {
        heap_caps_free(body);
    }
    next.loading = false;
    next.valid = err == ESP_OK;
    next.last_err = err;
    next.fetch_count++;
    next.updated_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_parts_list = next;
    s_parts_task = NULL;
    if (s_status.page == DEVICE_UI_PAGE_INFO) {
        s_dirty = true;
    }
    vTaskDelete(NULL);
}

static void schedule_parts_list(int page)
{
    if (page < 1) {
        page = 1;
    }
    if (s_parts_task) {
        return;
    }
    memset(&s_parts_list, 0, sizeof(s_parts_list));
    s_parts_list.loading = true;
    s_parts_list.page = page;
    s_parts_list.last_err = ESP_ERR_INVALID_STATE;
    s_dirty = true;
    BaseType_t ok = xTaskCreate(parts_list_task, "ui_parts", 7168,
                                NULL, 2, &s_parts_task);
    if (ok != pdPASS) {
        s_parts_task = NULL;
        s_parts_list.loading = false;
        s_parts_list.valid = false;
        s_parts_list.last_err = ESP_ERR_NO_MEM;
    }
}

static bool move_parts_page(int delta)
{
    if (delta == 0 || s_parts_list.loading || s_parts_task) {
        return false;
    }
    int page = s_parts_list.page <= 0 ? 1 : s_parts_list.page;
    int next_page = page + delta;
    if (next_page < 1) {
        next_page = 1;
    }
    if (s_parts_list.total_items > 0 && delta > 0) {
        int max_page = (int)((s_parts_list.total_items + UI_PARTS_LIST_MAX - 1) /
                             UI_PARTS_LIST_MAX);
        if (max_page < 1) {
            max_page = 1;
        }
        if (next_page > max_page) {
            next_page = max_page;
        }
    }
    if (next_page == page && s_parts_list.valid) {
        return false;
    }
    schedule_parts_list(next_page);
    return true;
}

static void parse_single_part_lot_summary(cJSON *lot, ui_partdb_detail_t *detail)
{
    if (!cJSON_IsObject(lot) || !detail) {
        return;
    }
    int lot_id = 0;
    if (!json_object_api_id(lot, "/part_lots/", &lot_id)) {
        return;
    }
    detail->lot_id = lot_id;
    detail->lot_count = 1;
    (void)json_copy_string(lot, "user_barcode", detail->barcode, sizeof(detail->barcode));
    cJSON *amount = cJSON_GetObjectItem(lot, "amount");
    if (cJSON_IsNumber(amount) && !detail->amount_valid) {
        detail->amount = (float)amount->valuedouble;
        detail->amount_valid = true;
    }
    cJSON *unknown = cJSON_GetObjectItem(lot, "instock_unknown");
    detail->amount_unknown = cJSON_IsBool(unknown) && cJSON_IsTrue(unknown);
    cJSON *location = cJSON_GetObjectItem(lot, "storage_location");
    cJSON *loc_name = cJSON_IsObject(location) ? cJSON_GetObjectItem(location, "name") : NULL;
    if (cJSON_IsString(loc_name)) {
        snprintf(detail->location, sizeof(detail->location), "%s", loc_name->valuestring);
    }
}

static void parse_part_lots_summary(cJSON *parsed, ui_partdb_detail_t *detail)
{
    cJSON *part_lots = parsed ? cJSON_GetObjectItem(parsed, "partLots") : NULL;
    if (!cJSON_IsArray(part_lots) || !detail) {
        return;
    }
    int count = 0;
    cJSON *first = NULL;
    cJSON *lot = NULL;
    cJSON_ArrayForEach(lot, part_lots) {
        int lot_id = 0;
        if (!json_object_api_id(lot, "/part_lots/", &lot_id)) {
            continue;
        }
        count++;
        if (!first) {
            first = lot;
        }
    }
    detail->lot_count = count;
    if (count == 1) {
        parse_single_part_lot_summary(first, detail);
    } else {
        detail->lot_id = 0;
        detail->location[0] = '\0';
        detail->barcode[0] = '\0';
    }
}

static void parse_orderdetails_summary(cJSON *parsed, ui_partdb_detail_t *detail)
{
    if (!parsed || !detail) {
        return;
    }
    cJSON *orders = cJSON_GetObjectItem(parsed, "orderdetails");
    if (!cJSON_IsArray(orders)) {
        return;
    }
    detail->order_count = json_array_object_count(orders);
    detail->supplier[0] = '\0';
    detail->supplier_partnr[0] = '\0';

    cJSON *order = NULL;
    cJSON_ArrayForEach(order, orders) {
        if (!cJSON_IsObject(order)) {
            continue;
        }
        if (!detail->supplier[0]) {
            (void)json_copy_nested_string(order, "supplier", "name",
                                          detail->supplier, sizeof(detail->supplier));
        }
        if (!detail->supplier_partnr[0]) {
            (void)json_copy_string(order, "supplierpartnr",
                                   detail->supplier_partnr, sizeof(detail->supplier_partnr));
        }
        if (detail->supplier[0] || detail->supplier_partnr[0]) {
            break;
        }
    }
}

static void parse_attachments_summary(cJSON *parsed, ui_partdb_detail_t *detail)
{
    if (!parsed || !detail) {
        return;
    }
    cJSON *attachments = cJSON_GetObjectItem(parsed, "attachments");
    if (!cJSON_IsArray(attachments)) {
        return;
    }
    detail->attachment_count = json_array_object_count(attachments);
    detail->attachment[0] = '\0';

    cJSON *attachment = NULL;
    cJSON_ArrayForEach(attachment, attachments) {
        if (!cJSON_IsObject(attachment)) {
            continue;
        }
        if (!json_copy_string(attachment, "name", detail->attachment,
                              sizeof(detail->attachment))) {
            (void)json_copy_string(attachment, "original_filename",
                                   detail->attachment, sizeof(detail->attachment));
        }
        if (!detail->attachment[0]) {
            (void)json_copy_nested_string(attachment, "attachment_type", "name",
                                          detail->attachment, sizeof(detail->attachment));
        }
        if (detail->attachment[0]) {
            break;
        }
    }
}

static void append_association_summary(cJSON *assoc, const char *part_key,
                                       char *out, size_t out_len)
{
    if (!cJSON_IsObject(assoc) || !part_key || !out || out_len == 0) {
        return;
    }
    char type[24] = {0};
    if (!json_copy_string(assoc, "other_type", type, sizeof(type))) {
        cJSON *type_item = cJSON_GetObjectItem(assoc, "type");
        if (cJSON_IsString(type_item) && type_item->valuestring) {
            snprintf(type, sizeof(type), "%s", type_item->valuestring);
        } else if (cJSON_IsNumber(type_item)) {
            snprintf(type, sizeof(type), "%d", type_item->valueint);
        }
    }

    char name[48] = {0};
    (void)json_copy_nested_string(assoc, part_key, "name", name, sizeof(name));
    if (!name[0]) {
        cJSON *part = cJSON_GetObjectItem(assoc, part_key);
        int id = 0;
        if (cJSON_IsObject(part) && json_object_api_id(part, "/parts/", &id)) {
            snprintf(name, sizeof(name), "P%04d", id);
        }
    }

    char piece[72];
    if (type[0] && name[0]) {
        snprintf(piece, sizeof(piece), "%.18s:%.42s", type, name);
    } else if (name[0]) {
        snprintf(piece, sizeof(piece), "%.60s", name);
    } else if (type[0]) {
        snprintf(piece, sizeof(piece), "%.60s", type);
    } else {
        return;
    }
    append_detail_summary(out, out_len, piece);
}

static void parse_associations_summary(cJSON *parsed, ui_partdb_detail_t *detail)
{
    if (!parsed || !detail) {
        return;
    }
    cJSON *as_owner = cJSON_GetObjectItem(parsed, "associated_parts_as_owner");
    cJSON *as_other = cJSON_GetObjectItem(parsed, "associated_parts_as_other");
    detail->association_count = json_array_object_count(as_owner) + json_array_object_count(as_other);
    detail->modules[0] = '\0';

    cJSON *assoc = NULL;
    cJSON_ArrayForEach(assoc, as_owner) {
        if (strlen(detail->modules) > 48) {
            break;
        }
        append_association_summary(assoc, "other", detail->modules, sizeof(detail->modules));
    }
    cJSON_ArrayForEach(assoc, as_other) {
        if (strlen(detail->modules) > 48) {
            break;
        }
        append_association_summary(assoc, "owner", detail->modules, sizeof(detail->modules));
    }
}

static void parse_partdb_modules_summary(cJSON *parsed, ui_partdb_detail_t *detail)
{
    parse_orderdetails_summary(parsed, detail);
    parse_attachments_summary(parsed, detail);
    parse_associations_summary(parsed, detail);
}

static bool format_part_parameter_value(cJSON *param, char *out, size_t out_len)
{
    if (!cJSON_IsObject(param) || !out || out_len == 0) {
        return false;
    }
    out[0] = '\0';
    cJSON *formatted = cJSON_GetObjectItem(param, "formatted");
    if (cJSON_IsString(formatted) && formatted->valuestring[0]) {
        snprintf(out, out_len, "%s", formatted->valuestring);
        return true;
    }

    cJSON *value_min = cJSON_GetObjectItem(param, "value_min");
    cJSON *value_typical = cJSON_GetObjectItem(param, "value_typical");
    cJSON *value_max = cJSON_GetObjectItem(param, "value_max");
    cJSON *value_text = cJSON_GetObjectItem(param, "value_text");
    cJSON *unit = cJSON_GetObjectItem(param, "unit");
    const char *u = cJSON_IsString(unit) ? unit->valuestring : "";
    const char *text = cJSON_IsString(value_text) ? value_text->valuestring : "";

    bool have_value = false;
    if (cJSON_IsNumber(value_typical)) {
        snprintf(out, out_len, "%.4g%s", value_typical->valuedouble, u);
        have_value = true;
    } else if (cJSON_IsNumber(value_min) && cJSON_IsNumber(value_max)) {
        snprintf(out, out_len, "%.4g..%.4g%s", value_min->valuedouble,
                 value_max->valuedouble, u);
        have_value = true;
    } else if (cJSON_IsNumber(value_min)) {
        snprintf(out, out_len, "min %.4g%s", value_min->valuedouble, u);
        have_value = true;
    } else if (cJSON_IsNumber(value_max)) {
        snprintf(out, out_len, "max %.4g%s", value_max->valuedouble, u);
        have_value = true;
    }

    if (have_value && text[0]) {
        size_t len = strlen(out);
        snprintf(out + len, out_len - len, " %.20s", text);
    } else if (!have_value && text[0]) {
        snprintf(out, out_len, "%s", text);
    } else if (!have_value && u[0]) {
        snprintf(out, out_len, "%s", u);
    }
    return out[0] != '\0';
}

static void parse_part_parameters(cJSON *parsed, ui_partdb_detail_t *detail)
{
    cJSON *parameters = parsed ? cJSON_GetObjectItem(parsed, "parameters") : NULL;
    if (!cJSON_IsArray(parameters) || !detail) {
        return;
    }
    detail->parameters[0] = '\0';
    int count = 0;
    cJSON *param = NULL;
    cJSON_ArrayForEach(param, parameters) {
        if (count >= 3) {
            break;
        }
        cJSON *name = cJSON_GetObjectItem(param, "name");
        const char *n = cJSON_IsString(name) ? name->valuestring : "";
        char value[64];
        bool have_value = format_part_parameter_value(param, value, sizeof(value));
        const char *v = have_value ? value : "";
        if (!n[0] && !v[0]) {
            continue;
        }
        size_t len = strlen(detail->parameters);
        int written = snprintf(detail->parameters + len, sizeof(detail->parameters) - len,
                               "%s%.24s%s%.36s",
                               len > 0 ? "  " : "",
                               n,
                               (n[0] && v[0]) ? ":" : "",
                               v);
        if (written < 0 || written >= (int)(sizeof(detail->parameters) - len)) {
            detail->parameters[sizeof(detail->parameters) - 1] = '\0';
            break;
        }
        count++;
    }
}

static void parse_part_detail(cJSON *parsed, ui_partdb_detail_t *detail)
{
    if (!parsed || !detail) {
        return;
    }
    cJSON *id = cJSON_GetObjectItem(parsed, "id");
    if (cJSON_IsNumber(id)) {
        detail->id = id->valueint;
        detail->part_id = id->valueint;
    }
    (void)json_copy_string(parsed, "name", detail->name, sizeof(detail->name));
    (void)json_copy_string(parsed, "ipn", detail->ipn, sizeof(detail->ipn));
    (void)json_copy_string(parsed, "description", detail->description, sizeof(detail->description));
    (void)json_copy_string(parsed, "comment", detail->comment, sizeof(detail->comment));
    (void)json_copy_string(parsed, "manufacturer_product_number", detail->mpn, sizeof(detail->mpn));
    (void)json_copy_nested_string(parsed, "manufacturer", "name",
                                  detail->manufacturer, sizeof(detail->manufacturer));
    (void)json_copy_nested_string(parsed, "footprint", "name",
                                  detail->footprint, sizeof(detail->footprint));
    cJSON *category = cJSON_GetObjectItem(parsed, "category");
    cJSON *cat_name = cJSON_IsObject(category) ? cJSON_GetObjectItem(category, "name") : NULL;
    if (cJSON_IsString(cat_name)) {
        snprintf(detail->category, sizeof(detail->category), "%s", cat_name->valuestring);
    }
    cJSON *stock = cJSON_GetObjectItem(parsed, "total_instock");
    if (!cJSON_IsNumber(stock)) {
        stock = cJSON_GetObjectItem(parsed, "amountSum");
    }
    if (cJSON_IsNumber(stock)) {
        detail->amount = (float)stock->valuedouble;
        detail->amount_valid = true;
    }
    parse_part_lots_summary(parsed, detail);
    parse_part_parameters(parsed, detail);
    parse_partdb_modules_summary(parsed, detail);
}

static void parse_lot_detail(cJSON *parsed, ui_partdb_detail_t *detail)
{
    if (!parsed || !detail) {
        return;
    }
    detail->is_lot = true;
    cJSON *id = cJSON_GetObjectItem(parsed, "id");
    if (cJSON_IsNumber(id)) {
        detail->id = id->valueint;
        detail->lot_id = id->valueint;
        detail->lot_count = 1;
    }
    (void)json_copy_string(parsed, "description", detail->description, sizeof(detail->description));
    (void)json_copy_string(parsed, "comment", detail->comment, sizeof(detail->comment));
    (void)json_copy_string(parsed, "user_barcode", detail->barcode, sizeof(detail->barcode));
    cJSON *amount = cJSON_GetObjectItem(parsed, "amount");
    if (cJSON_IsNumber(amount)) {
        detail->amount = (float)amount->valuedouble;
        detail->amount_valid = true;
    }
    cJSON *unknown = cJSON_GetObjectItem(parsed, "instock_unknown");
    detail->amount_unknown = cJSON_IsBool(unknown) && cJSON_IsTrue(unknown);

    cJSON *part = cJSON_GetObjectItem(parsed, "part");
    cJSON *part_id = cJSON_IsObject(part) ? cJSON_GetObjectItem(part, "id") : NULL;
    if (cJSON_IsNumber(part_id)) {
        detail->part_id = part_id->valueint;
    } else if (cJSON_IsString(part)) {
        (void)parse_id_after_marker(part->valuestring, "/parts/", &detail->part_id);
    }
    cJSON *part_name = cJSON_IsObject(part) ? cJSON_GetObjectItem(part, "name") : NULL;
    cJSON *part_ipn = cJSON_IsObject(part) ? cJSON_GetObjectItem(part, "ipn") : NULL;
    cJSON *part_desc = cJSON_IsObject(part) ? cJSON_GetObjectItem(part, "description") : NULL;
    cJSON *part_comment = cJSON_IsObject(part) ? cJSON_GetObjectItem(part, "comment") : NULL;
    cJSON *part_mpn = cJSON_IsObject(part) ? cJSON_GetObjectItem(part, "manufacturer_product_number") : NULL;
    if (cJSON_IsString(part_name)) {
        snprintf(detail->name, sizeof(detail->name), "%s", part_name->valuestring);
    }
    if (cJSON_IsString(part_ipn)) {
        snprintf(detail->ipn, sizeof(detail->ipn), "%s", part_ipn->valuestring);
    }
    if (!detail->description[0] && cJSON_IsString(part_desc)) {
        snprintf(detail->description, sizeof(detail->description), "%s", part_desc->valuestring);
    }
    if (!detail->comment[0] && cJSON_IsString(part_comment)) {
        snprintf(detail->comment, sizeof(detail->comment), "%s", part_comment->valuestring);
    }
    if (cJSON_IsString(part_mpn)) {
        snprintf(detail->mpn, sizeof(detail->mpn), "%s", part_mpn->valuestring);
    }
    cJSON *category = cJSON_IsObject(part) ? cJSON_GetObjectItem(part, "category") : NULL;
    cJSON *cat_name = cJSON_IsObject(category) ? cJSON_GetObjectItem(category, "name") : NULL;
    if (cJSON_IsString(cat_name)) {
        snprintf(detail->category, sizeof(detail->category), "%s", cat_name->valuestring);
    }
    cJSON *manufacturer = cJSON_IsObject(part) ? cJSON_GetObjectItem(part, "manufacturer") : NULL;
    cJSON *man_name = cJSON_IsObject(manufacturer) ? cJSON_GetObjectItem(manufacturer, "name") : NULL;
    if (cJSON_IsString(man_name)) {
        snprintf(detail->manufacturer, sizeof(detail->manufacturer), "%s", man_name->valuestring);
    }
    cJSON *footprint = cJSON_IsObject(part) ? cJSON_GetObjectItem(part, "footprint") : NULL;
    cJSON *fp_name = cJSON_IsObject(footprint) ? cJSON_GetObjectItem(footprint, "name") : NULL;
    if (cJSON_IsString(fp_name)) {
        snprintf(detail->footprint, sizeof(detail->footprint), "%s", fp_name->valuestring);
    }
    cJSON *location = cJSON_GetObjectItem(parsed, "storage_location");
    cJSON *loc_name = cJSON_IsObject(location) ? cJSON_GetObjectItem(location, "name") : NULL;
    if (cJSON_IsString(loc_name)) {
        snprintf(detail->location, sizeof(detail->location), "%s", loc_name->valuestring);
    }
    if (cJSON_IsObject(part)) {
        parse_partdb_modules_summary(part, detail);
    }
}

static esp_err_t fetch_part_parameters_for_detail(int part_id, ui_partdb_detail_t *detail,
                                                  char *body, size_t body_len,
                                                  bool include_formatted)
{
    if (part_id <= 0 || !detail || !body || body_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[448];
    int written = 0;
    if (include_formatted) {
        written = snprintf(path, sizeof(path),
                           "/api/parts/%d.jsonld?properties[]=id&"
                           "properties[parameters][]=name&properties[parameters][]=formatted&"
                           "properties[parameters][]=value_min&properties[parameters][]=value_typical&"
                           "properties[parameters][]=value_max&properties[parameters][]=value_text&"
                           "properties[parameters][]=unit",
                           part_id);
    } else {
        written = snprintf(path, sizeof(path),
                           "/api/parts/%d.jsonld?properties[]=id&"
                           "properties[parameters][]=name&properties[parameters][]=value_min&"
                           "properties[parameters][]=value_typical&properties[parameters][]=value_max&"
                           "properties[parameters][]=value_text&properties[parameters][]=unit",
                           part_id);
    }
    if (written < 0 || written >= (int)sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    int status = 0;
    esp_err_t err = partdb_client_get_json(path, body, body_len, &status);
    if (err != ESP_OK || status < 200 || status >= 300) {
        return err == ESP_OK ? ESP_FAIL : err;
    }
    cJSON *parsed = cJSON_Parse(body);
    if (!parsed) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    parse_part_parameters(parsed, detail);
    cJSON_Delete(parsed);
    return ESP_OK;
}

static esp_err_t fetch_detail_for_target(const ui_partdb_target_t *target,
                                         ui_partdb_detail_t *detail)
{
    if (!target || !detail || target->id <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char *body = heap_caps_malloc(UI_PARTDB_BODY_MAX, MALLOC_CAP_8BIT);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    const size_t path_len = 1536;
    char *path = heap_caps_malloc(path_len, MALLOC_CAP_8BIT);
    if (!path) {
        heap_caps_free(body);
        return ESP_ERR_NO_MEM;
    }
    int written = 0;
    if (target->prefix == 'L') {
        written = snprintf(path, path_len,
                           "/api/part_lots/%d.jsonld?properties[]=id&"
                           "properties[]=description&properties[]=comment&properties[]=user_barcode&"
                           "properties[]=amount&properties[]=instock_unknown&"
                           "properties[part][]=id&properties[part][]=name&properties[part][]=ipn&"
                           "properties[part][]=description&properties[part][]=comment&"
                           "properties[part][]=manufacturer_product_number&properties[part][category][]=name&"
                           "properties[part][manufacturer][]=name&properties[part][footprint][]=name&"
                           "properties[part][orderdetails][]=id&properties[part][orderdetails][]=supplierpartnr&"
                           "properties[part][orderdetails][]=obsolete&properties[part][orderdetails][supplier][]=name&"
                           "properties[part][attachments][]=id&properties[part][attachments][]=name&"
                           "properties[part][attachments][]=original_filename&properties[part][attachments][attachment_type][]=name&"
                           "properties[part][associated_parts_as_owner][]=id&"
                           "properties[part][associated_parts_as_owner][]=type&"
                           "properties[part][associated_parts_as_owner][]=other_type&"
                           "properties[part][associated_parts_as_owner][other][]=id&"
                           "properties[part][associated_parts_as_owner][other][]=name&"
                           "properties[part][associated_parts_as_other][]=id&"
                           "properties[part][associated_parts_as_other][]=type&"
                           "properties[part][associated_parts_as_other][]=other_type&"
                           "properties[part][associated_parts_as_other][owner][]=id&"
                           "properties[part][associated_parts_as_other][owner][]=name&"
                           "properties[storage_location][]=id&properties[storage_location][]=name",
                           target->id);
    } else {
        written = snprintf(path, path_len,
                           "/api/parts/%d.jsonld?properties[]=id&properties[]=name&"
                           "properties[]=ipn&properties[]=description&properties[]=comment&"
                           "properties[]=manufacturer_product_number&properties[]=total_instock&"
                           "properties[category][]=name&properties[manufacturer][]=name&"
                           "properties[footprint][]=name&properties[partLots][]=id&"
                           "properties[partLots][]=amount&properties[partLots][]=instock_unknown&"
                           "properties[partLots][]=user_barcode&"
                           "properties[partLots][storage_location][]=name&"
                           "properties[orderdetails][]=id&properties[orderdetails][]=supplierpartnr&"
                           "properties[orderdetails][]=obsolete&properties[orderdetails][supplier][]=name&"
                           "properties[attachments][]=id&properties[attachments][]=name&"
                           "properties[attachments][]=original_filename&properties[attachments][attachment_type][]=name&"
                           "properties[associated_parts_as_owner][]=id&"
                           "properties[associated_parts_as_owner][]=type&"
                           "properties[associated_parts_as_owner][]=other_type&"
                           "properties[associated_parts_as_owner][other][]=id&"
                           "properties[associated_parts_as_owner][other][]=name&"
                           "properties[associated_parts_as_other][]=id&"
                           "properties[associated_parts_as_other][]=type&"
                           "properties[associated_parts_as_other][]=other_type&"
                           "properties[associated_parts_as_other][owner][]=id&"
                           "properties[associated_parts_as_other][owner][]=name",
                           target->id);
    }
    if (written < 0 || written >= (int)path_len) {
        heap_caps_free(path);
        heap_caps_free(body);
        return ESP_ERR_INVALID_SIZE;
    }

    int status = 0;
    esp_err_t err = partdb_client_get_json(path, body, UI_PARTDB_BODY_MAX, &status);
    detail->http_status = status;
    detail->cache_hit = partdb_client_get_status().last_cache_hit;
    snprintf(detail->cache_source, sizeof(detail->cache_source), "%s", partdb_client_last_source());
    bool ok = err == ESP_OK && status >= 200 && status < 300;
    cJSON *parsed = ok ? cJSON_Parse(body) : NULL;
    if (parsed) {
        if (target->prefix == 'L') {
            parse_lot_detail(parsed, detail);
        } else {
            parse_part_detail(parsed, detail);
        }
        cJSON_Delete(parsed);
        if (target->prefix != 'L') {
            esp_err_t param_err = fetch_part_parameters_for_detail(target->id, detail, body,
                                                                   UI_PARTDB_BODY_MAX, true);
            if (param_err != ESP_OK) {
                (void)fetch_part_parameters_for_detail(target->id, detail, body,
                                                       UI_PARTDB_BODY_MAX, false);
            }
        }
        detail->found = true;
        detail->valid = true;
        err = ESP_OK;
    } else if (ok) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    heap_caps_free(path);
    heap_caps_free(body);
    return err;
}

static void start_detail_lookup_task(void)
{
    s_dirty = true;
    BaseType_t ok = xTaskCreate(detail_lookup_task, "ui_partdb", 10240, NULL, 2, &s_detail_task);
    if (ok != pdPASS) {
        s_detail_task = NULL;
        s_detail_target_direct = false;
        s_detail.loading = false;
        s_detail.last_err = ESP_ERR_NO_MEM;
    }
}

static void detail_lookup_task(void *arg)
{
    (void)arg;
    ui_partdb_detail_t *next = heap_caps_malloc(sizeof(*next), MALLOC_CAP_8BIT);
    if (!next) {
        s_detail_task = NULL;
        s_detail.loading = false;
        s_detail.valid = false;
        s_detail.found = false;
        s_detail.last_err = ESP_ERR_NO_MEM;
        s_dirty = true;
        vTaskDelete(NULL);
        return;
    }
    *next = s_detail;
    next->loading = true;
    next->valid = false;
    next->found = false;
    next->last_err = ESP_ERR_INVALID_STATE;

    int lookup_http = 0;
    ui_partdb_target_t target = {0};
    bool direct = s_detail_target_direct;
    if (direct) {
        target = s_detail_direct_target;
        lookup_http = 0;
        s_detail_target_direct = false;
    }
    esp_err_t err = direct ? ESP_OK : lookup_target_by_query(next->query, &target, &lookup_http);
    next->http_status = lookup_http;
    if (err == ESP_OK) {
        err = fetch_detail_for_target(&target, next);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "detail lookup failed target=%c%d direct=%d http=%d err=%s",
                 target.prefix ? target.prefix : '-',
                 target.id,
                 direct,
                 next->http_status,
                 esp_err_to_name(err));
    }
    next->loading = false;
    next->last_err = err;
    next->fetch_count++;
    next->updated_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_detail_pending) {
        bool pending_direct = s_detail_pending_direct;
        ui_partdb_target_t pending_target = s_detail_pending_target;
        char pending_query[UI_DETAIL_TEXT_MAX];
        snprintf(pending_query, sizeof(pending_query), "%s", s_detail_pending_query);
        s_detail_pending = false;
        s_detail_pending_direct = false;
        memset(&s_detail_pending_target, 0, sizeof(s_detail_pending_target));
        s_detail_pending_query[0] = '\0';

        memset(&s_detail, 0, sizeof(s_detail));
        s_detail.loading = true;
        s_detail.last_err = ESP_ERR_INVALID_STATE;
        snprintf(s_detail.query, sizeof(s_detail.query), "%s", pending_query);
        s_detail_target_direct = pending_direct;
        if (pending_direct) {
            s_detail_direct_target = pending_target;
        }
        s_detail_info_page = 0;
        s_detail_task = NULL;
        start_detail_lookup_task();
        if (s_status.page == DEVICE_UI_PAGE_DETAIL) {
            s_dirty = true;
        }
        heap_caps_free(next);
        vTaskDelete(NULL);
        return;
    }
    s_detail = *next;
    heap_caps_free(next);
    s_detail_task = NULL;
    if (s_status.page == DEVICE_UI_PAGE_DETAIL) {
        s_dirty = true;
    }
    vTaskDelete(NULL);
}

static void schedule_detail_lookup(const char *query)
{
    if (!query || !query[0]) {
        return;
    }
    if (s_detail_task) {
        s_detail_pending = true;
        s_detail_pending_direct = false;
        memset(&s_detail_pending_target, 0, sizeof(s_detail_pending_target));
        snprintf(s_detail_pending_query, sizeof(s_detail_pending_query), "%s", query);
        memset(&s_detail, 0, sizeof(s_detail));
        s_detail.loading = true;
        s_detail.last_err = ESP_ERR_INVALID_STATE;
        snprintf(s_detail.query, sizeof(s_detail.query), "%s", query);
        s_detail_info_page = 0;
        clear_stock_result();
        clear_nfc_write_result();
        s_dirty = true;
        return;
    }
    memset(&s_detail, 0, sizeof(s_detail));
    s_detail.loading = true;
    s_detail.last_err = ESP_ERR_INVALID_STATE;
    snprintf(s_detail.query, sizeof(s_detail.query), "%s", query);
    s_detail_target_direct = false;
    s_detail_pending = false;
    s_detail_pending_direct = false;
    s_detail_info_page = 0;
    clear_stock_result();
    clear_nfc_write_result();
    start_detail_lookup_task();
}

static void schedule_detail_target(const char *query, const ui_partdb_target_t *target)
{
    if (!query || !query[0] || !target || target->id <= 0) {
        return;
    }
    if (s_detail_task) {
        s_detail_pending = true;
        s_detail_pending_direct = true;
        s_detail_pending_target = *target;
        snprintf(s_detail_pending_query, sizeof(s_detail_pending_query), "%s", query);
        memset(&s_detail, 0, sizeof(s_detail));
        s_detail.loading = true;
        s_detail.last_err = ESP_ERR_INVALID_STATE;
        snprintf(s_detail.query, sizeof(s_detail.query), "%s", query);
        s_detail_info_page = 0;
        clear_stock_result();
        clear_nfc_write_result();
        s_dirty = true;
        return;
    }
    memset(&s_detail, 0, sizeof(s_detail));
    s_detail.loading = true;
    s_detail.last_err = ESP_ERR_INVALID_STATE;
    snprintf(s_detail.query, sizeof(s_detail.query), "%s", query);
    s_detail_direct_target = *target;
    s_detail_target_direct = true;
    s_detail_pending = false;
    s_detail_pending_direct = false;
    s_detail_info_page = 0;
    clear_stock_result();
    clear_nfc_write_result();
    start_detail_lookup_task();
}

static const char *stock_op_label(ui_stock_op_t op)
{
    switch (op) {
    case UI_STOCK_OP_IN:
        return "入库";
    case UI_STOCK_OP_OUT:
        return "出库";
    case UI_STOCK_OP_NONE:
    default:
        return "未选择";
    }
}

static void clear_stock_result(void)
{
    if (s_stock.busy) {
        return;
    }
    s_stock.done = false;
    s_stock.ok = false;
    s_stock.last_err = ESP_ERR_INVALID_STATE;
    s_stock.http_status = 0;
    s_stock.message[0] = '\0';
}

static bool parse_qty_value(const char *text, float *out)
{
    if (!text || !out) {
        return false;
    }
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    int value = 0;
    bool any = false;
    while (*text >= '0' && *text <= '9') {
        any = true;
        value = value * 10 + (*text - '0');
        if (value > 1000000) {
            return false;
        }
        text++;
    }
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    if (!any || *text != '\0' || value <= 0) {
        return false;
    }
    *out = (float)value;
    return true;
}

static bool stock_amount_matches(float actual, float expected)
{
    float diff = actual - expected;
    if (diff < 0.0f) {
        diff = -diff;
    }
    return diff < 0.01f;
}

static bool parse_lot_stock_json(const char *body, float *amount, bool *unknown)
{
    if (!body || !amount || !unknown) {
        return false;
    }
    cJSON *parsed = cJSON_Parse(body);
    if (!parsed) {
        return false;
    }
    cJSON *amount_json = cJSON_GetObjectItem(parsed, "amount");
    cJSON *unknown_json = cJSON_GetObjectItem(parsed, "instock_unknown");
    bool ok = cJSON_IsNumber(amount_json);
    if (ok) {
        *amount = (float)amount_json->valuedouble;
        *unknown = cJSON_IsBool(unknown_json) && cJSON_IsTrue(unknown_json);
    }
    cJSON_Delete(parsed);
    return ok;
}

static bool verify_lot_stock_response(const char *body, float expected, float *actual)
{
    float amount = 0.0f;
    bool unknown = false;
    if (!parse_lot_stock_json(body, &amount, &unknown) || unknown) {
        return false;
    }
    if (actual) {
        *actual = amount;
    }
    return stock_amount_matches(amount, expected);
}

static int active_detail_lot_id(const ui_partdb_detail_t *detail)
{
    if (!detail) {
        return 0;
    }
    if (detail->lot_id > 0) {
        return detail->lot_id;
    }
    return detail->is_lot && detail->id > 0 ? detail->id : 0;
}

static void stock_apply_task(void *arg)
{
    (void)arg;
    ui_partdb_detail_t detail = s_detail;
    ui_stock_op_t op = s_stock_op;
    ui_stock_state_t next = s_stock;
    next.busy = true;
    next.done = false;
    next.ok = false;
    next.http_status = 0;
    next.last_err = ESP_ERR_INVALID_STATE;
    next.message[0] = '\0';

    float qty = 0;
    int lot_id = active_detail_lot_id(&detail);
    esp_err_t err = ESP_OK;
    if (op == UI_STOCK_OP_NONE) {
        err = ESP_ERR_INVALID_STATE;
        snprintf(next.message, sizeof(next.message), "请先选择入库或出库");
    } else if (!detail.found) {
        err = ESP_ERR_NOT_FOUND;
        snprintf(next.message, sizeof(next.message), "没有 Part-DB 对象");
    } else if (lot_id <= 0) {
        err = ESP_ERR_INVALID_STATE;
        snprintf(next.message, sizeof(next.message), "请先扫码/NFC具体批次");
    } else if (!parse_qty_value(s_detail_qty, &qty)) {
        err = ESP_ERR_INVALID_ARG;
        snprintf(next.message, sizeof(next.message), "数量必须大于 0");
    } else if (!detail.amount_valid || detail.amount_unknown) {
        err = ESP_ERR_INVALID_STATE;
        snprintf(next.message, sizeof(next.message), "未知库存不能直接修改");
    }

    float old_amount = detail.amount_valid ? detail.amount : 0.0f;
    float new_amount = old_amount;
    if (err == ESP_OK) {
        new_amount = op == UI_STOCK_OP_IN ? old_amount + qty : old_amount - qty;
        if (new_amount < 0.0f) {
            err = ESP_ERR_INVALID_SIZE;
            snprintf(next.message, sizeof(next.message), "出库数量超过库存");
        }
    }

    char *body = NULL;
    if (err == ESP_OK) {
        body = heap_caps_malloc(UI_PARTDB_BODY_MAX, MALLOC_CAP_8BIT);
        if (!body) {
            err = ESP_ERR_NO_MEM;
            snprintf(next.message, sizeof(next.message), "库存写回内存不足");
        }
    }

    if (err == ESP_OK) {
        char path[96];
        char verify_path[160];
        char patch_body[96];
        snprintf(path, sizeof(path), "/api/part_lots/%d.jsonld", lot_id);
        snprintf(verify_path, sizeof(verify_path),
                 "/api/part_lots/%d.jsonld?properties[]=id&properties[]=amount&properties[]=instock_unknown",
                 lot_id);
        int written = snprintf(patch_body, sizeof(patch_body),
                               "{\"amount\":%.3f,\"instock_unknown\":false}",
                               (double)new_amount);
        if (written < 0 || written >= (int)sizeof(patch_body)) {
            err = ESP_ERR_INVALID_SIZE;
            snprintf(next.message, sizeof(next.message), "库存写回内容过长");
        } else {
            err = partdb_client_patch_json(path, patch_body, body,
                                           UI_PARTDB_BODY_MAX, &next.http_status);
            bool ok = err == ESP_OK && next.http_status >= 200 && next.http_status < 300;
            if (!ok) {
                err = err == ESP_OK ? ESP_FAIL : err;
                snprintf(next.message, sizeof(next.message), "Part-DB HTTP %d",
                         next.http_status);
            } else {
                float verified_amount = 0.0f;
                bool verified = verify_lot_stock_response(body, new_amount,
                                                          &verified_amount);
                if (!verified) {
                    int verify_status = 0;
                    esp_err_t verify_err = partdb_client_request_json(PARTDB_HTTP_GET,
                                                                      verify_path,
                                                                      NULL, NULL,
                                                                      body,
                                                                      UI_PARTDB_BODY_MAX,
                                                                      &verify_status);
                    if (verify_err == ESP_OK && verify_status >= 200 && verify_status < 300) {
                        verified = verify_lot_stock_response(body, new_amount,
                                                             &verified_amount);
                    }
                    if (!verified) {
                        err = verify_err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : verify_err;
                        next.http_status = verify_status > 0 ? verify_status : next.http_status;
                        snprintf(next.message, sizeof(next.message),
                                 "库存未确认 %.0f->%.0f",
                                 (double)old_amount, (double)new_amount);
                    }
                }
            }
        }
    }

    if (body) {
        heap_caps_free(body);
    }

    next.busy = false;
    next.done = true;
    next.ok = err == ESP_OK;
    next.last_err = err;
    next.lot_id = lot_id;
    next.qty = qty;
    next.old_amount = old_amount;
    next.new_amount = err == ESP_OK ? new_amount : old_amount;
    next.apply_count++;
    next.updated_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (err == ESP_OK) {
        snprintf(next.message, sizeof(next.message), "%s成功 %.0f -> %.0f",
                 stock_op_label(op), (double)old_amount, (double)new_amount);
        if (s_detail.found && active_detail_lot_id(&s_detail) == lot_id) {
            s_detail.amount = new_amount;
            s_detail.amount_valid = true;
            s_detail.amount_unknown = false;
        }
        s_inventory.valid = false;
    } else if (next.message[0] == '\0') {
        snprintf(next.message, sizeof(next.message), "%s", esp_err_to_name(err));
    }

    s_stock = next;
    s_stock_task = NULL;
    s_dirty = true;
    vTaskDelete(NULL);
}

static void schedule_stock_apply(void)
{
    if (s_stock_task || s_stock.busy) {
        s_dirty = true;
        return;
    }
    s_stock.busy = true;
    s_stock.done = false;
    s_stock.ok = false;
    s_stock.last_err = ESP_ERR_INVALID_STATE;
    snprintf(s_stock.message, sizeof(s_stock.message), "正在写回 Part-DB");
    s_dirty = true;
    BaseType_t ok = xTaskCreate(stock_apply_task, "ui_stock", 6144,
                                NULL, 2, &s_stock_task);
    if (ok != pdPASS) {
        s_stock_task = NULL;
        s_stock.busy = false;
        s_stock.done = true;
        s_stock.ok = false;
        s_stock.last_err = ESP_ERR_NO_MEM;
        snprintf(s_stock.message, sizeof(s_stock.message), "库存任务创建失败");
    }
}

static void clear_nfc_write_result(void)
{
    release_nfc_result_hold(false);
    s_nfc_confirm_open = false;
    s_nfc_confirm_action = UI_NFC_ACTION_NONE;
    if (s_nfc_write.busy) {
        return;
    }
    s_nfc_write.done = false;
    s_nfc_write.ok = false;
    s_nfc_write.action = UI_NFC_ACTION_NONE;
    s_nfc_write.last_err = ESP_ERR_INVALID_STATE;
    s_nfc_write.payload[0] = '\0';
    s_nfc_write.message[0] = '\0';
    s_have_view_hash = false;
    s_dirty = true;
}

static const char *nfc_action_label(ui_nfc_action_t action)
{
    switch (action) {
    case UI_NFC_ACTION_WRITE_DETAIL:
        return "写入";
    case UI_NFC_ACTION_ERASE:
        return "擦除";
    case UI_NFC_ACTION_NONE:
    default:
        return "操作";
    }
}

static void restart_nfc_after_manual_io(void)
{
    (void)nfc_service_restart(2500);
}

static void release_nfc_result_hold(bool restart)
{
    if (s_nfc_result_holds_service) {
        nfc_service_resume_after_request();
        s_nfc_result_holds_service = false;
    }
    s_nfc_confirm_tag_valid = false;
    memset(&s_nfc_confirm_tag, 0, sizeof(s_nfc_confirm_tag));
    (void)restart;
}

static void close_nfc_read_prompt(void)
{
    memset(&s_nfc_read_prompt, 0, sizeof(s_nfc_read_prompt));
    s_have_view_hash = false;
    s_dirty = true;
}

static void open_nfc_route_prompt(const nfc_service_status_t *nfc,
                                  const ui_partdb_target_t *target,
                                  const char *query)
{
    if (!nfc || !target || !query || !query[0]) {
        return;
    }
    memset(&s_nfc_read_prompt, 0, sizeof(s_nfc_read_prompt));
    s_nfc_read_prompt.open = true;
    s_nfc_read_prompt.matched = true;
    s_nfc_read_prompt.read_count = nfc->read_count;
    s_nfc_read_prompt.target = *target;
    snprintf(s_nfc_read_prompt.uid, sizeof(s_nfc_read_prompt.uid), "%s", nfc->uid);
    snprintf(s_nfc_read_prompt.raw, sizeof(s_nfc_read_prompt.raw), "%s", nfc->text);
    snprintf(s_nfc_read_prompt.query, sizeof(s_nfc_read_prompt.query), "%s", query);
    snprintf(s_nfc_read_prompt.title, sizeof(s_nfc_read_prompt.title), "%s #%d",
             target->prefix == 'L' ? "批次" : "元器件", target->id);
    snprintf(s_nfc_read_prompt.message, sizeof(s_nfc_read_prompt.message),
             "打开 %s 详情", s_nfc_read_prompt.title);
    open_keyboard(UI_INPUT_NONE);
    s_have_view_hash = false;
    s_dirty = true;
}

static void confirm_nfc_read_prompt(void)
{
    if (!s_nfc_read_prompt.open || !s_nfc_read_prompt.query[0]) {
        close_nfc_read_prompt();
        return;
    }
    char query[sizeof(s_nfc_read_prompt.query)] = {0};
    snprintf(query, sizeof(query), "%s", s_nfc_read_prompt.query);
    close_nfc_read_prompt();
    enter_detail_with_object(query);
}

static esp_err_t settle_nfc_erase_result(const nfc_tag_t *tag, esp_err_t err)
{
    if (err == ESP_OK) {
        nfc_service_note_tag_payload(tag, "");
        return ESP_OK;
    }
    return err;
}

static esp_err_t claim_nfc_tag_for_action(nfc_tag_t *tag, uint32_t timeout_ms)
{
    if (!tag) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(s_nfc_write.message, sizeof(s_nfc_write.message), "请贴近 NFC 卡");
    s_dirty = true;

    if (s_nfc_confirm_tag_valid && s_nfc_confirm_tag.uid_len > 0 &&
        s_nfc_confirm_tag.target_number > 0) {
        *tag = s_nfc_confirm_tag;
        nfc_service_suspend_for_request();
        snprintf(s_nfc_write.message, sizeof(s_nfc_write.message), "已锁定 NFC 卡");
        s_dirty = true;
        return ESP_OK;
    }

    esp_err_t err = nfc_service_claim_tag(tag, timeout_ms);
    if (err == ESP_OK) {
        snprintf(s_nfc_write.message, sizeof(s_nfc_write.message), "已检测到 NFC 卡");
        s_dirty = true;
    }
    return err;
}

static bool detail_nfc_payload(const ui_partdb_detail_t *detail, char *out, size_t out_len)
{
    if (!detail || !out || out_len == 0 || !detail->found) {
        return false;
    }
    out[0] = '\0';
    int lot_id = active_detail_lot_id(detail);
    if (detail->is_lot && lot_id > 0) {
        snprintf(out, out_len, "l/%d", lot_id);
        return true;
    }
    int part_id = detail->part_id > 0 ? detail->part_id : detail->id;
    if (part_id > 0) {
        snprintf(out, out_len, "p/%d", part_id);
        return true;
    }
    if (detail->ipn[0]) {
        snprintf(out, out_len, "%s", detail->ipn);
        return true;
    }
    return false;
}

static void nfc_write_task(void *arg)
{
    (void)arg;
    ui_nfc_write_state_t next = s_nfc_write;
    ui_nfc_action_t action = next.action;
    next.busy = true;
    next.done = false;
    next.ok = false;
    next.last_err = ESP_ERR_INVALID_STATE;
    next.message[0] = '\0';

    char payload[sizeof(next.payload)] = {0};
    esp_err_t err = ESP_OK;
    if (action == UI_NFC_ACTION_NONE) {
        action = UI_NFC_ACTION_WRITE_DETAIL;
    }

    if (action == UI_NFC_ACTION_WRITE_DETAIL) {
        if (next.payload[0]) {
            snprintf(payload, sizeof(payload), "%s", next.payload);
        } else if (!detail_nfc_payload(&s_detail, payload, sizeof(payload))) {
            err = ESP_ERR_INVALID_STATE;
        }
        if (err != ESP_OK) {
            snprintf(next.message, sizeof(next.message), "没有可写入对象");
        } else {
            snprintf(next.payload, sizeof(next.payload), "%s", payload);
            snprintf(next.message, sizeof(next.message), "请贴近 NFC 卡");
            nfc_tag_t tag = {0};
            err = claim_nfc_tag_for_action(&tag, UI_NFC_TAG_WAIT_MS);
            if (err == ESP_OK) {
                err = nfc_pn532_write_ndef_text_to_tag(&tag, payload, 3000);
                if (err == ESP_OK) {
                    nfc_service_note_tag_payload(&tag, payload);
                }
                nfc_service_release_tag();
                restart_nfc_after_manual_io();
            }
        }
    } else if (action == UI_NFC_ACTION_ERASE) {
        next.payload[0] = '\0';
        snprintf(next.message, sizeof(next.message), "请贴近 NFC 卡");
        nfc_tag_t tag = {0};
        err = claim_nfc_tag_for_action(&tag, UI_NFC_TAG_WAIT_MS);
        if (err == ESP_OK) {
            err = nfc_pn532_erase_ndef_from_tag(&tag, 1200);
            nfc_service_release_tag();
            err = settle_nfc_erase_result(&tag, err);
            restart_nfc_after_manual_io();
        }
    } else {
        err = ESP_ERR_INVALID_ARG;
    }

    next.busy = false;
    next.done = true;
    next.ok = err == ESP_OK;
    next.action = action;
    next.last_err = err;
    next.write_count++;
    next.updated_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (err == ESP_OK) {
        if (action == UI_NFC_ACTION_ERASE) {
            snprintf(next.message, sizeof(next.message), "已擦除 NFC 卡");
        } else {
            snprintf(next.message, sizeof(next.message), "已写入 %.64s", next.payload);
        }
    } else if (next.message[0] == '\0' || strcmp(next.message, "请贴近 NFC 卡") == 0) {
        if (err == ESP_ERR_NOT_FOUND) {
            snprintf(next.message, sizeof(next.message), "未检测到 NFC 卡");
        } else {
            snprintf(next.message, sizeof(next.message), "NFC%s %s",
                     nfc_action_label(action), esp_err_to_name(err));
        }
    }
    s_nfc_write = next;
    s_nfc_write_task = NULL;
    s_dirty = true;
    vTaskDelete(NULL);
}

static void schedule_nfc_action(ui_nfc_action_t action, const char *payload_hint)
{
    if (s_nfc_write_task || s_nfc_write.busy) {
        s_dirty = true;
        return;
    }
    char payload[sizeof(s_nfc_write.payload)] = {0};
    if (action == UI_NFC_ACTION_WRITE_DETAIL) {
        if (payload_hint && payload_hint[0]) {
            snprintf(payload, sizeof(payload), "%s", payload_hint);
        } else if (s_nfc_write.action == UI_NFC_ACTION_WRITE_DETAIL && s_nfc_write.payload[0]) {
            snprintf(payload, sizeof(payload), "%s", s_nfc_write.payload);
        } else if (!detail_nfc_payload(&s_detail, payload, sizeof(payload))) {
            s_nfc_write.busy = false;
            s_nfc_write.done = true;
            s_nfc_write.ok = false;
            s_nfc_write.action = action;
            s_nfc_write.last_err = ESP_ERR_INVALID_STATE;
            snprintf(s_nfc_write.message, sizeof(s_nfc_write.message), "没有可写入对象");
            s_dirty = true;
            return;
        }
    } else if (action != UI_NFC_ACTION_ERASE) {
        s_nfc_write.busy = false;
        s_nfc_write.done = true;
        s_nfc_write.ok = false;
        s_nfc_write.action = action;
        s_nfc_write.last_err = ESP_ERR_INVALID_ARG;
        snprintf(s_nfc_write.message, sizeof(s_nfc_write.message), "NFC操作无效");
        s_dirty = true;
        return;
    }
    s_nfc_write.busy = true;
    s_nfc_write.done = false;
    s_nfc_write.ok = false;
    s_nfc_write.action = action;
    s_nfc_write.last_err = ESP_ERR_INVALID_STATE;
    snprintf(s_nfc_write.payload, sizeof(s_nfc_write.payload), "%s", payload);
    snprintf(s_nfc_write.message, sizeof(s_nfc_write.message), "请贴近 NFC 卡");
    s_dirty = true;
    BaseType_t ok = xTaskCreate(nfc_write_task, "ui_nfc_wr", 4096,
                                NULL, 2, &s_nfc_write_task);
    if (ok != pdPASS) {
        s_nfc_write_task = NULL;
        s_nfc_write.busy = false;
        s_nfc_write.done = true;
        s_nfc_write.ok = false;
        s_nfc_write.last_err = ESP_ERR_NO_MEM;
        snprintf(s_nfc_write.message, sizeof(s_nfc_write.message), "NFC任务创建失败");
    }
}

static void schedule_nfc_write(void)
{
    schedule_nfc_action(UI_NFC_ACTION_WRITE_DETAIL, NULL);
}

static void schedule_nfc_erase(void)
{
    schedule_nfc_action(UI_NFC_ACTION_ERASE, NULL);
}

static void prepare_nfc_confirm(ui_nfc_action_t action, const char *payload)
{
    release_nfc_result_hold(false);
    s_nfc_write.busy = false;
    s_nfc_write.done = false;
    s_nfc_write.ok = false;
    s_nfc_write.action = action;
    s_nfc_write.last_err = ESP_ERR_INVALID_STATE;
    if (payload && payload[0]) {
        snprintf(s_nfc_write.payload, sizeof(s_nfc_write.payload), "%s", payload);
    } else {
        s_nfc_write.payload[0] = '\0';
    }
    s_nfc_write.message[0] = '\0';
    s_nfc_confirm_action = action;
    s_nfc_confirm_open = true;
    s_dirty = true;
}

static void begin_nfc_write_from_detail(void)
{
    char payload[sizeof(s_nfc_write.payload)] = {0};
    if (!detail_nfc_payload(&s_detail, payload, sizeof(payload))) {
        schedule_nfc_write();
        return;
    }
    prepare_nfc_confirm(UI_NFC_ACTION_WRITE_DETAIL, payload);
}

static void begin_nfc_erase_from_settings(void)
{
    if (s_nfc_write_task || s_nfc_write.busy) {
        s_dirty = true;
        return;
    }
    prepare_nfc_confirm(UI_NFC_ACTION_ERASE, NULL);
}

static void inventory_task(void *arg)
{
    (void)arg;
    const TickType_t short_delay = pdMS_TO_TICKS(10000);
    const TickType_t normal_delay = pdMS_TO_TICKS(60000);
    vTaskDelay(pdMS_TO_TICKS(3500));
    while (true) {
        s_inventory_refresh_requested = false;
        partdb_client_status_t pdb = partdb_client_get_status();
        if (!pdb.configured) {
            s_inventory.valid = false;
            s_inventory.refreshing = false;
            s_inventory.last_err = ESP_ERR_INVALID_STATE;
            (void)ulTaskNotifyTake(pdTRUE, short_delay);
            continue;
        }

        ui_inventory_stats_t old = s_inventory;
        s_inventory.refreshing = true;
        ui_inventory_stats_t next = s_inventory;
        int status = 0;
        esp_err_t err = fetch_total_items("/parts.jsonld?itemsPerPage=1", &next.parts, &status);
        next.last_http_status = status;
        if (err == ESP_OK) {
            err = fetch_total_items("/part_lots.jsonld?itemsPerPage=1", &next.lots, &status);
            next.last_http_status = status;
        }
        if (err == ESP_OK) {
            err = fetch_total_items("/storage_locations.jsonld?itemsPerPage=1", &next.locations, &status);
            next.last_http_status = status;
        }
        if (err == ESP_OK) {
            err = fetch_total_items("/categories.jsonld?itemsPerPage=1", &next.categories, &status);
            next.last_http_status = status;
        }
        if (err == ESP_OK) {
            uint32_t optional = 0;
            if (fetch_total_items("/suppliers.jsonld?itemsPerPage=1", &optional, &status) == ESP_OK) {
                next.suppliers = optional;
            }
            next.last_http_status = status;
            optional = 0;
            if (fetch_total_items("/manufacturers.jsonld?itemsPerPage=1", &optional, &status) == ESP_OK) {
                next.manufacturers = optional;
            }
            next.last_http_status = status;
            optional = 0;
            if (fetch_total_items("/footprints.jsonld?itemsPerPage=1", &optional, &status) == ESP_OK) {
                next.footprints = optional;
            }
            next.last_http_status = status;
            optional = 0;
            if (fetch_total_items("/attachments.jsonld?itemsPerPage=1", &optional, &status) == ESP_OK) {
                next.attachments = optional;
            }
            next.last_http_status = status;
            optional = 0;
            if (fetch_total_items("/projects.jsonld?itemsPerPage=1", &optional, &status) == ESP_OK) {
                next.projects = optional;
            }
            next.last_http_status = status;
        }

        next.refreshing = false;
        next.valid = err == ESP_OK;
        next.last_err = err;
        next.refresh_count++;
        next.updated_ms = (uint32_t)(esp_timer_get_time() / 1000);
        bool changed = inventory_stats_changed(&old, &next);
        s_inventory = next;
        if (changed && s_status.page == DEVICE_UI_PAGE_HOME) {
            s_dirty = true;
        }
        (void)ulTaskNotifyTake(pdTRUE, err == ESP_OK ? normal_delay : short_delay);
    }
}

static int camera_button_y(void)
{
    return display_ili9488_get_height() - UI_NAV_H - UI_CAMERA_BUTTON_H - 8;
}

static uint16_t *alloc_camera_frame_buffer(uint16_t w, uint16_t h)
{
    size_t bytes = (size_t)w * h * sizeof(uint16_t);
    uint16_t *buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }
    return buf;
}

static void release_camera_frame_buffer_internal(bool reset_preview_mode)
{
    if (!s_camera_frame) {
        return;
    }
    size_t bytes = (size_t)s_camera_frame_buf_w * s_camera_frame_buf_h * sizeof(uint16_t);
    heap_caps_free(s_camera_frame);
    s_camera_frame = NULL;
    s_camera_frame_buf_w = 0;
    s_camera_frame_buf_h = 0;
    if (reset_preview_mode) {
        s_camera_preview_low_mem = false;
    }
    s_camera_ui.frame_w = 0;
    s_camera_ui.frame_h = 0;
    s_camera_ui.frame_seq++;
    ESP_LOGI(TAG, "camera preview buffer released bytes=%u", (unsigned)bytes);
}

static void release_camera_frame_buffer(void)
{
    release_camera_frame_buffer_internal(true);
}

static void release_camera_frame_buffer_keep_mode(void)
{
    release_camera_frame_buffer_internal(false);
}

static bool ensure_camera_frame_buffer(void)
{
    if (s_camera_frame) {
        return true;
    }

    const struct {
        uint16_t w;
        uint16_t h;
    } attempts[] = {
        {UI_CAMERA_FRAME_W, UI_CAMERA_FRAME_H},
        {UI_CAMERA_FRAME_LOW_W, UI_CAMERA_FRAME_LOW_H},
    };
    size_t first_attempt = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0 ? 0 : 1;

    for (size_t i = first_attempt; i < sizeof(attempts) / sizeof(attempts[0]); i++) {
        s_camera_frame = alloc_camera_frame_buffer(attempts[i].w, attempts[i].h);
        if (s_camera_frame) {
            s_camera_frame_buf_w = attempts[i].w;
            s_camera_frame_buf_h = attempts[i].h;
            s_camera_preview_low_mem = attempts[i].w != UI_CAMERA_FRAME_W ||
                                       attempts[i].h != UI_CAMERA_FRAME_H;
            size_t bytes = (size_t)s_camera_frame_buf_w * s_camera_frame_buf_h * sizeof(uint16_t);
            memset(s_camera_frame, 0, bytes);
            s_camera_ui.frame_w = s_camera_frame_buf_w;
            s_camera_ui.frame_h = s_camera_frame_buf_h;
            if (s_camera_preview_low_mem) {
                ESP_LOGW(TAG, "camera preview using low memory buffer %ux%u bytes=%u",
                         s_camera_frame_buf_w, s_camera_frame_buf_h, (unsigned)bytes);
            }
            return true;
        }
    }

    if (!s_camera_frame) {
        s_camera_ui.mode = UI_CAMERA_ERROR;
        s_camera_ui.last_err = ESP_ERR_NO_MEM;
        snprintf(s_camera_ui.message, sizeof(s_camera_ui.message), "预览缓冲不足");
        size_t bytes = (size_t)UI_CAMERA_FRAME_LOW_W * UI_CAMERA_FRAME_LOW_H * sizeof(uint16_t);
        ESP_LOGW(TAG, "camera preview buffer alloc failed bytes=%u free_8bit=%u largest_8bit=%u free_spiram=%u largest_spiram=%u",
                 (unsigned)bytes,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        return false;
    }
    return false;
}

static inline uint16_t camera_preview_pixel_to_rgb565(uint16_t pixel)
{
#if BOARD_CAMERA_PREVIEW_SWAP_BYTES
    pixel = (uint16_t)((pixel >> 8) | (pixel << 8));
#endif
#if BOARD_CAMERA_PREVIEW_SWAP_RB
    pixel = (uint16_t)(((pixel & 0xF800) >> 11) |
                       (pixel & 0x07E0) |
                       ((pixel & 0x001F) << 11));
#endif
    return pixel;
}

static inline uint16_t camera_preview_gray_to_rgb565(uint8_t gray)
{
    return (uint16_t)(((uint16_t)(gray & 0xF8) << 8) |
                      ((uint16_t)(gray & 0xFC) << 3) |
                      ((uint16_t)gray >> 3));
}

static void copy_camera_preview_frame(const camera_fb_t *fb)
{
    if (!fb || !s_camera_frame || !fb->buf || fb->width == 0 || fb->height == 0) {
        return;
    }
    size_t src_pixels = (size_t)fb->width * fb->height;
    if ((fb->format == PIXFORMAT_RGB565 && fb->len < src_pixels * sizeof(uint16_t)) ||
        (fb->format == PIXFORMAT_GRAYSCALE && fb->len < src_pixels)) {
        return;
    }
    if (fb->format != PIXFORMAT_RGB565 && fb->format != PIXFORMAT_GRAYSCALE) {
        return;
    }

    uint16_t dst_w = s_camera_frame_buf_w ? s_camera_frame_buf_w : UI_CAMERA_FRAME_W;
    uint16_t dst_h = s_camera_frame_buf_h ? s_camera_frame_buf_h : UI_CAMERA_FRAME_H;

    const uint16_t *rgb = (const uint16_t *)fb->buf;
    const uint8_t *gray = fb->buf;
    for (uint16_t row = 0; row < dst_h; row++) {
        uint16_t src_y = (uint16_t)(((uint32_t)row * fb->height) / dst_h);
        if (src_y >= fb->height) {
            src_y = fb->height - 1;
        }
        uint16_t *dst = &s_camera_frame[(size_t)row * dst_w];
        for (uint16_t col = 0; col < dst_w; col++) {
            uint16_t src_x = (uint16_t)(((uint32_t)col * fb->width) / dst_w);
            if (src_x >= fb->width) {
                src_x = fb->width - 1;
            }
            size_t src_index = (size_t)src_y * fb->width + src_x;
            dst[col] = fb->format == PIXFORMAT_GRAYSCALE ?
                       camera_preview_gray_to_rgb565(gray[src_index]) :
                       camera_preview_pixel_to_rgb565(rgb[src_index]);
        }
    }
    s_camera_ui.frame_w = dst_w;
    s_camera_ui.frame_h = dst_h;
    s_camera_ui.frame_seq++;
}

static void camera_preview_task(void *arg)
{
    (void)arg;
    s_camera_ui.mode = UI_CAMERA_STARTING;
    s_camera_ui.last_err = ESP_OK;
    snprintf(s_camera_ui.message, sizeof(s_camera_ui.message), "摄像头启动中");
    s_dirty = true;

    camera_mgr_set_keep_online(true);
    nfc_service_suspend_for_camera();
    vTaskDelay(pdMS_TO_TICKS(BOARD_CAMERA_NFC_QUIET_MS));

    while (!s_camera_ui.stop_requested) {
        if (s_camera_ui.capture_requested) {
            s_camera_ui.capture_requested = false;
            s_camera_ui.mode = UI_CAMERA_CAPTURING;
            snprintf(s_camera_ui.message, sizeof(s_camera_ui.message), "正在识别二维码");
            s_dirty = true;

            memset(&s_camera_ui.result, 0, sizeof(s_camera_ui.result));
            s_camera_ui.result_ready = false;
            if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) {
                release_camera_frame_buffer_keep_mode();
            }
            esp_err_t err = ESP_ERR_NOT_FOUND;
            if (s_camera_frame && s_camera_ui.frame_w > 0 && s_camera_ui.frame_h > 0) {
                snprintf(s_camera_ui.message, sizeof(s_camera_ui.message), "识别实时画面");
                s_dirty = true;
                err = qr_scanner_decode_rgb565(s_camera_frame, s_camera_ui.frame_w,
                                               s_camera_ui.frame_h, &s_camera_ui.result);
                if (!(err == ESP_OK && s_camera_ui.result.found && s_camera_ui.result.text[0])) {
                    ESP_LOGI(TAG, "live preview QR decode missed: err=%s detail=%s",
                             esp_err_to_name(err),
                             s_camera_ui.result.decode_error[0] ?
                             s_camera_ui.result.decode_error : "-");
                }
            }
            if (!(err == ESP_OK && s_camera_ui.result.found && s_camera_ui.result.text[0])) {
                snprintf(s_camera_ui.message, sizeof(s_camera_ui.message), "识别高清画面");
                s_dirty = true;
                err = qr_scanner_scan(&s_camera_ui.result);
            }
            bool found = err == ESP_OK && s_camera_ui.result.found &&
                         s_camera_ui.result.text[0];
            s_camera_ui.result_ready = found;
            s_camera_ui.last_err = found ? ESP_OK :
                                   (err == ESP_OK ? ESP_ERR_NOT_FOUND : err);
            s_camera_ui.capture_count++;
            if (found) {
                ui_partdb_target_t target = {0};
                char target_code[16] = {0};
                if (target_from_text(s_camera_ui.result.text, &target)) {
                    format_target_code(&target, target_code, sizeof(target_code));
                }
                snprintf(s_camera_ui.message, sizeof(s_camera_ui.message),
                         "识别 %.54s", target_code[0] ? target_code : s_camera_ui.result.text);
            } else if (s_camera_ui.result.decode_error[0]) {
                snprintf(s_camera_ui.message, sizeof(s_camera_ui.message),
                         "未识别 %s", s_camera_ui.result.decode_error);
            } else {
                snprintf(s_camera_ui.message, sizeof(s_camera_ui.message),
                         "未识别二维码");
            }
            s_camera_ui.mode = UI_CAMERA_RESULT;
            s_dirty = true;
            vTaskDelay(pdMS_TO_TICKS(UI_CAMERA_PREVIEW_DELAY_MS));
            continue;
        }

        if (s_camera_ui.mode == UI_CAMERA_RESULT) {
            vTaskDelay(pdMS_TO_TICKS(UI_CAMERA_PREVIEW_DELAY_MS));
            continue;
        }

        camera_fb_t *fb = NULL;
        esp_err_t err = s_camera_preview_low_mem ?
                        camera_mgr_capture_preview_lowmem(&fb) :
                        camera_mgr_capture_preview_rgb565(&fb);
        if (err != ESP_OK || !fb) {
            s_camera_ui.mode = UI_CAMERA_ERROR;
            s_camera_ui.last_err = err;
            snprintf(s_camera_ui.message, sizeof(s_camera_ui.message), "摄像头错误 %s",
                     esp_err_to_name(err));
            release_camera_frame_buffer();
            s_full_repaint = true;
            s_have_view_hash = false;
            s_dirty = true;
            break;
        }

        copy_camera_preview_frame(fb);
        camera_mgr_release_frame(fb);
        fb = NULL;
        s_camera_ui.mode = UI_CAMERA_LIVE;
        snprintf(s_camera_ui.message, sizeof(s_camera_ui.message), "%s",
                 s_camera_preview_low_mem ? "灰度扫码预览，点击扫码" : "点击扫码");
        s_dirty = true;

        vTaskDelay(pdMS_TO_TICKS(UI_CAMERA_PREVIEW_DELAY_MS));
    }

    bool stopped_by_request = s_camera_ui.stop_requested;
    camera_mgr_set_keep_online(false);
#if !BOARD_CAMERA_KEEP_ONLINE
    camera_mgr_deinit();
#endif
    nfc_service_resume_after_camera();
    if (stopped_by_request) {
        s_camera_ui.mode = UI_CAMERA_STOPPED;
        snprintf(s_camera_ui.message, sizeof(s_camera_ui.message), "摄像头已停止");
        release_camera_frame_buffer();
        s_full_repaint = true;
        s_have_view_hash = false;
    }
    s_camera_task = NULL;
    s_dirty = true;
    vTaskDelete(NULL);
}

static bool wait_camera_preview_stopped(uint32_t timeout_ms)
{
    int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (s_camera_task) {
        if (esp_timer_get_time() >= deadline_us) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

static void begin_camera_preview(void)
{
    if (s_camera_task && s_camera_ui.stop_requested) {
        s_camera_ui.mode = UI_CAMERA_STARTING;
        s_camera_ui.last_err = ESP_OK;
        snprintf(s_camera_ui.message, sizeof(s_camera_ui.message), "摄像头释放中");
        set_page_internal(DEVICE_UI_PAGE_CAMERA);
        s_dirty = true;
        if (!wait_camera_preview_stopped(UI_CAMERA_STOP_WAIT_MS)) {
            return;
        }
    }
    if (!ensure_camera_frame_buffer()) {
        set_page_internal(DEVICE_UI_PAGE_CAMERA);
        s_dirty = true;
        return;
    }
    if (!s_camera_task) {
        memset(&s_camera_ui.result, 0, sizeof(s_camera_ui.result));
        s_camera_ui.stop_requested = false;
        s_camera_ui.capture_requested = false;
        s_camera_ui.result_ready = false;
        s_camera_ui.mode = UI_CAMERA_STARTING;
        s_camera_ui.last_err = ESP_OK;
        snprintf(s_camera_ui.message, sizeof(s_camera_ui.message), "摄像头启动中");
        BaseType_t ok = xTaskCreate(camera_preview_task, "ui_cam_prev",
                                    UI_CAMERA_TASK_STACK_BYTES, NULL, 3, &s_camera_task);
        if (ok != pdPASS) {
            s_camera_task = NULL;
            s_camera_ui.mode = UI_CAMERA_ERROR;
            s_camera_ui.last_err = ESP_ERR_NO_MEM;
            snprintf(s_camera_ui.message, sizeof(s_camera_ui.message), "摄像头任务创建失败");
        }
    }
    set_page_internal(DEVICE_UI_PAGE_CAMERA);
    s_dirty = true;
}

static void stop_camera_preview(void)
{
    if (s_camera_task) {
        s_camera_ui.stop_requested = true;
    } else if (s_camera_ui.mode != UI_CAMERA_STOPPED) {
        camera_mgr_set_keep_online(false);
#if !BOARD_CAMERA_KEEP_ONLINE
        camera_mgr_deinit();
#endif
        nfc_service_resume_after_camera();
        s_camera_ui.mode = UI_CAMERA_STOPPED;
        release_camera_frame_buffer();
        s_full_repaint = true;
        s_have_view_hash = false;
    }
    s_dirty = true;
}

static void request_camera_capture(void)
{
    if (s_camera_ui.mode == UI_CAMERA_ERROR && !s_camera_task) {
        begin_camera_preview();
        return;
    }
    if (!s_camera_task) {
        return;
    }
    if (s_camera_ui.mode == UI_CAMERA_LIVE || s_camera_ui.mode == UI_CAMERA_RESULT) {
        s_camera_ui.capture_requested = true;
        s_camera_ui.mode = UI_CAMERA_CAPTURING;
        snprintf(s_camera_ui.message, sizeof(s_camera_ui.message), "正在识别二维码");
        s_dirty = true;
    }
}

static void handle_camera_result_entry(void)
{
    if (s_status.page != DEVICE_UI_PAGE_CAMERA || !s_camera_ui.result_ready) {
        return;
    }
    char text[QR_SCANNER_TEXT_MAX] = {0};
    snprintf(text, sizeof(text), "%s", s_camera_ui.result.text);
    s_camera_ui.result_ready = false;
    stop_camera_preview();
    if (text[0]) {
        enter_detail_with_object(text);
    }
}

static bool nfc_ui_ready(void)
{
    nfc_service_status_t nfc = nfc_service_get_status();
    return nfc.ready || nfc_pn532_is_ready();
}

static void hash_u32(uint32_t *hash, uint32_t value)
{
    for (int i = 0; i < 4; i++) {
        *hash ^= (value >> (i * 8)) & 0xff;
        *hash *= 16777619u;
    }
}

static void hash_u64(uint32_t *hash, uint64_t value)
{
    hash_u32(hash, (uint32_t)(value & 0xffffffffu));
    hash_u32(hash, (uint32_t)(value >> 32));
}

static void hash_bool(uint32_t *hash, bool value)
{
    hash_u32(hash, value ? 1u : 0u);
}

static void hash_string(uint32_t *hash, const char *value)
{
    if (!value) {
        hash_u32(hash, 0);
        return;
    }
    while (*value) {
        *hash ^= (uint8_t)*value++;
        *hash *= 16777619u;
    }
    *hash ^= 0xff;
    *hash *= 16777619u;
}

static uint32_t current_view_hash(void)
{
    uint32_t hash = 2166136261u;
    hash_u32(&hash, s_status.page);
    hash_bool(&hash, display_ili9488_is_ready());
    hash_bool(&hash, display_ili9488_is_awake());
    hash_bool(&hash, s_screen_sleeping);
    hash_u32(&hash, display_ili9488_get_width());
    hash_u32(&hash, display_ili9488_get_height());
    hash_string(&hash, display_ili9488_get_orientation());
    hash_string(&hash, ui_font_get_active_path());
    if (s_cfg) {
        hash_string(&hash, s_cfg->device_name);
    }
    hash_string(&hash, s_search);
    hash_string(&hash, s_detail_ipn);
    hash_string(&hash, s_detail_qty);
    hash_bool(&hash, s_detail.loading);
    hash_bool(&hash, s_detail.valid);
    hash_bool(&hash, s_detail.found);
    hash_u32(&hash, (uint32_t)s_detail.last_err);
    hash_u32(&hash, (uint32_t)s_detail.id);
    hash_u32(&hash, (uint32_t)s_detail.part_id);
    hash_u32(&hash, (uint32_t)s_detail.lot_id);
    hash_u32(&hash, (uint32_t)s_detail.lot_count);
    hash_bool(&hash, s_detail.amount_valid);
    hash_bool(&hash, s_detail.amount_unknown);
    hash_u32(&hash, (uint32_t)(s_detail.amount * 1000.0f));
    hash_string(&hash, s_detail.query);
    hash_string(&hash, s_detail.name);
    hash_string(&hash, s_detail.ipn);
    hash_string(&hash, s_detail.description);
    hash_string(&hash, s_detail.comment);
    hash_string(&hash, s_detail.category);
    hash_string(&hash, s_detail.manufacturer);
    hash_string(&hash, s_detail.mpn);
    hash_string(&hash, s_detail.footprint);
    hash_string(&hash, s_detail.location);
    hash_string(&hash, s_detail.barcode);
    hash_string(&hash, s_detail.parameters);
    hash_u32(&hash, (uint32_t)s_detail.order_count);
    hash_u32(&hash, (uint32_t)s_detail.attachment_count);
    hash_u32(&hash, (uint32_t)s_detail.association_count);
    hash_string(&hash, s_detail.supplier);
    hash_string(&hash, s_detail.supplier_partnr);
    hash_string(&hash, s_detail.attachment);
    hash_string(&hash, s_detail.modules);
    hash_u32(&hash, (uint32_t)s_detail_info_page);
    hash_u32(&hash, (uint32_t)s_stock_op);
    hash_bool(&hash, s_stock.busy);
    hash_bool(&hash, s_stock.done);
    hash_bool(&hash, s_stock.ok);
    hash_u32(&hash, (uint32_t)s_stock.last_err);
    hash_u32(&hash, (uint32_t)s_stock.http_status);
    hash_string(&hash, s_stock.message);
    hash_bool(&hash, s_nfc_write.busy);
    hash_bool(&hash, s_nfc_write.done);
    hash_bool(&hash, s_nfc_write.ok);
    hash_u32(&hash, (uint32_t)s_nfc_write.action);
    hash_u32(&hash, (uint32_t)s_nfc_write.last_err);
    hash_string(&hash, s_nfc_write.payload);
    hash_string(&hash, s_nfc_write.message);
    hash_bool(&hash, s_nfc_confirm_open);
    hash_u32(&hash, (uint32_t)s_nfc_confirm_action);
    hash_bool(&hash, s_nfc_result_holds_service);
    hash_bool(&hash, s_nfc_confirm_tag_valid);
    hash_bool(&hash, s_nfc_read_prompt.open);
    hash_bool(&hash, s_nfc_read_prompt.matched);
    hash_u32(&hash, s_nfc_read_prompt.read_count);
    hash_string(&hash, s_nfc_read_prompt.uid);
    hash_string(&hash, s_nfc_read_prompt.query);
    hash_string(&hash, s_nfc_read_prompt.raw);
    hash_string(&hash, s_nfc_read_prompt.message);
    hash_bool(&hash, s_keyboard_open);
    hash_u32(&hash, s_keyboard_page);
    hash_u32(&hash, (uint32_t)s_input_target);

    switch ((device_ui_page_t)s_status.page) {
    case DEVICE_UI_PAGE_RESULTS:
        hash_bool(&hash, s_results.loading);
        hash_bool(&hash, s_results.valid);
        hash_u32(&hash, (uint32_t)s_results.result_count);
        hash_u32(&hash, (uint32_t)s_results.http_status);
        hash_u32(&hash, (uint32_t)s_results.last_err);
        hash_u32(&hash, (uint32_t)s_results_scroll);
        hash_u32(&hash, (uint32_t)s_results_selected);
        hash_string(&hash, s_results.query);
        for (int i = 0; i < s_results.result_count && i < UI_SEARCH_RESULT_MAX; i++) {
            hash_u32(&hash, (uint32_t)s_results.items[i].id);
            hash_bool(&hash, s_results.items[i].amount_valid);
            hash_u32(&hash, (uint32_t)(s_results.items[i].amount * 1000.0f));
            hash_string(&hash, s_results.items[i].name);
            hash_string(&hash, s_results.items[i].ipn);
            hash_string(&hash, s_results.items[i].category);
        }
        break;
    case DEVICE_UI_PAGE_DETAIL: {
        partdb_client_status_t pdb = partdb_client_get_status();
        hash_bool(&hash, pdb.configured);
        hash_bool(&hash, nfc_ui_ready());
        break;
    }
    case DEVICE_UI_PAGE_SHORTCUTS: {
        nfc_service_status_t nfc = nfc_service_get_status();
        qr_scanner_status_t qr = qr_scanner_get_status();
        hash_bool(&hash, nfc.ready);
        hash_bool(&hash, nfc_ui_ready());
        hash_bool(&hash, nfc.tag_present);
        hash_string(&hash, nfc.uid);
        hash_string(&hash, nfc.text);
        hash_u32(&hash, (uint32_t)nfc.last_err);
        hash_u32(&hash, nfc.read_count);
        hash_u32(&hash, nfc.text_read_count);
        hash_u32(&hash, nfc.error_count);
        hash_bool(&hash, camera_mgr_is_active());
        hash_bool(&hash, qr.last_found);
        hash_string(&hash, qr.last_text);
        hash_string(&hash, qr.last_decode_error);
        hash_u32(&hash, qr.last_code_count);
        hash_u32(&hash, qr.last_elapsed_ms);
        hash_u32(&hash, qr.scan_count);
        hash_u32(&hash, qr.found_count);
        hash_u32(&hash, (uint32_t)qr.last_err);
        break;
    }
    case DEVICE_UI_PAGE_INFO:
        hash_bool(&hash, s_parts_list.loading);
        hash_bool(&hash, s_parts_list.valid);
        hash_u32(&hash, (uint32_t)s_parts_list.page);
        hash_u32(&hash, (uint32_t)s_parts_list.item_count);
        hash_u32(&hash, s_parts_list.total_items);
        hash_u32(&hash, (uint32_t)s_parts_list.http_status);
        hash_u32(&hash, (uint32_t)s_parts_list.last_err);
        hash_u32(&hash, s_parts_list.fetch_count);
        for (int i = 0; i < s_parts_list.item_count && i < UI_PARTS_LIST_MAX; i++) {
            hash_u32(&hash, (uint32_t)s_parts_list.items[i].id);
            hash_bool(&hash, s_parts_list.items[i].amount_valid);
            hash_u32(&hash, (uint32_t)(s_parts_list.items[i].amount * 1000.0f));
            hash_string(&hash, s_parts_list.items[i].name);
            hash_string(&hash, s_parts_list.items[i].ipn);
            hash_string(&hash, s_parts_list.items[i].category);
            hash_string(&hash, s_parts_list.items[i].footprint);
            hash_string(&hash, s_parts_list.items[i].description);
        }
        break;
    case DEVICE_UI_PAGE_SYSTEM: {
        hardware_diag_status_t diag = hardware_diag_get_status();
        storage_sd_status_t sd = storage_sd_get_status();
        wifi_portal_status_t wifi = wifi_portal_get_status();
        nfc_service_status_t nfc = nfc_service_get_status();
        qr_scanner_status_t qr = qr_scanner_get_status();
        hash_bool(&hash, touch_ft6336_is_ready());
        hash_bool(&hash, nfc_pn532_is_ready());
        hash_bool(&hash, sd.mounted);
        hash_u64(&hash, sd.free_bytes);
        hash_u64(&hash, sd.total_bytes);
        hash_bool(&hash, wifi.sta_connected);
        hash_string(&hash, wifi.ip);
        hash_bool(&hash, nfc.ready);
        hash_bool(&hash, nfc_ui_ready());
        hash_bool(&hash, nfc.tag_present);
        hash_string(&hash, nfc.text);
        hash_string(&hash, nfc.uid);
        hash_bool(&hash, qr.last_found);
        hash_string(&hash, qr.last_text);
        hash_u32(&hash, qr.scan_count);
        hash_bool(&hash, diag.running);
        hash_bool(&hash, diag.finished);
        hash_u32(&hash, diag.last_run_ms);
        break;
    }
    case DEVICE_UI_PAGE_SETTINGS: {
        wifi_portal_status_t wifi = wifi_portal_get_status();
        hash_bool(&hash, s_settings_busy);
        hash_bool(&hash, wifi.ap_enabled);
        hash_bool(&hash, wifi.ap_started);
        hash_bool(&hash, nfc_pn532_is_ready());
        hash_bool(&hash, nfc_ui_ready());
        hash_u32(&hash, display_ili9488_get_brightness());
        if (s_cfg) {
            hash_u32(&hash, s_cfg->display_brightness);
            hash_bool(&hash, s_cfg->ap_enabled);
            hash_string(&hash, s_cfg->ui_language);
            hash_string(&hash, s_cfg->screen_bg_path);
            hash_string(&hash, s_cfg->boot_anim_path);
            hash_string(&hash, s_cfg->lock_bg_path);
            hash_bool(&hash, s_cfg->nfc_read_confirm);
            hash_u32(&hash, s_cfg->screen_sleep_minutes);
        }
        break;
    }
    case DEVICE_UI_PAGE_NFC_SETTINGS: {
        nfc_service_status_t nfc = nfc_service_get_status();
        hash_bool(&hash, s_settings_busy);
        hash_bool(&hash, nfc_pn532_is_ready());
        hash_bool(&hash, nfc_ui_ready());
        hash_bool(&hash, nfc.tag_present);
        hash_string(&hash, nfc.uid);
        hash_string(&hash, nfc.text);
        hash_u32(&hash, nfc.read_count);
        hash_bool(&hash, s_nfc_write.busy);
        hash_bool(&hash, s_nfc_write.done);
        hash_bool(&hash, s_nfc_write.ok);
        hash_u32(&hash, (uint32_t)s_nfc_write.last_err);
        hash_string(&hash, s_nfc_write.message);
        if (s_cfg) {
            hash_bool(&hash, s_cfg->nfc_read_confirm);
        }
        break;
    }
    case DEVICE_UI_PAGE_CAMERA:
        hash_u32(&hash, (uint32_t)s_camera_ui.mode);
        hash_bool(&hash, s_camera_task != NULL);
        hash_bool(&hash, s_camera_ui.capture_requested);
        hash_bool(&hash, s_camera_ui.result_ready);
        hash_u32(&hash, (uint32_t)s_camera_ui.last_err);
        hash_u32(&hash, s_camera_ui.frame_seq);
        hash_u32(&hash, s_camera_ui.frame_w);
        hash_u32(&hash, s_camera_ui.frame_h);
        hash_bool(&hash, s_camera_preview_low_mem);
        hash_u32(&hash, s_camera_ui.capture_count);
        hash_string(&hash, s_camera_ui.message);
        hash_bool(&hash, s_camera_ui.result.found);
        hash_string(&hash, s_camera_ui.result.text);
        hash_string(&hash, s_camera_ui.result.decode_error);
        break;
    case DEVICE_UI_PAGE_HOME:
    default: {
        partdb_client_status_t pdb = partdb_client_get_status();
        hash_bool(&hash, pdb.configured);
        hash_bool(&hash, s_inventory.valid);
        hash_u32(&hash, s_inventory.parts);
        hash_u32(&hash, s_inventory.lots);
        hash_u32(&hash, s_inventory.locations);
        hash_u32(&hash, s_inventory.categories);
        hash_u32(&hash, s_inventory.suppliers);
        hash_u32(&hash, s_inventory.manufacturers);
        hash_u32(&hash, s_inventory.footprints);
        hash_u32(&hash, s_inventory.attachments);
        hash_u32(&hash, s_inventory.projects);
        hash_u32(&hash, (uint32_t)s_inventory.last_err);
        break;
    }
    }
    return hash;
}

static void format_count_digits(uint32_t value, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    if (value > 999999) {
        value = 999999;
    }
    snprintf(out, out_len, "%lu", (unsigned long)value);
}

static const char *inventory_sync_state_text(const partdb_client_status_t *pdb)
{
    if (!pdb || !pdb->configured) {
        return "未配置";
    }
    if (s_inventory.refreshing) {
        return "同步中";
    }
    if (s_inventory.valid) {
        return "同步成功";
    }
    if (s_inventory.last_err != ESP_OK) {
        return "同步失败";
    }
    return "同步异常";
}

static uint16_t inventory_sync_state_color(const partdb_client_status_t *pdb)
{
    if (!pdb || !pdb->configured) {
        return UI_RED;
    }
    if (s_inventory.valid || s_inventory.refreshing) {
        return UI_HOME_ACCENT;
    }
    return UI_RED;
}

static int segment_digit_width(int t, int l)
{
    return t + l + t;
}

static int segment_digit_height(int t, int l)
{
    return t + l + t + l + t;
}

static int segment_number_width(const char *digits, int t, int l, int gap)
{
    int count = digits && digits[0] ? (int)strlen(digits) : 1;
    return count * segment_digit_width(t, l) + (count - 1) * gap;
}

static void buffer_segment_digit_sized(uint16_t *buf, int bw, int bh, int x, int y,
                                       char digit, uint16_t color, uint16_t dim,
                                       int t, int l)
{
    static const uint8_t map[10] = {
        0x3f, 0x06, 0x5b, 0x4f, 0x66,
        0x6d, 0x7d, 0x07, 0x7f, 0x6f,
    };
    if (t < 1) {
        t = 1;
    }
    if (l < 4) {
        l = 4;
    }
    uint8_t seg = 0;
    if (digit >= '0' && digit <= '9') {
        seg = map[digit - '0'];
    } else if (digit == '-') {
        seg = 0x40;
    }
    const struct {
        int x;
        int y;
        int w;
        int h;
        uint8_t bit;
    } parts[] = {
        {t, 0, l, t, 0x01},
        {t + l, t, t, l, 0x02},
        {t + l, t + l + t, t, l, 0x04},
        {t, t + l + t + l, l, t, 0x08},
        {0, t + l + t, t, l, 0x10},
        {0, t, t, l, 0x20},
        {t, t + l, l, t, 0x40},
    };
    for (int i = 0; i < (int)(sizeof(parts) / sizeof(parts[0])); i++) {
        buffer_fill_rect(buf, bw, bh, x + parts[i].x, y + parts[i].y,
                         parts[i].w, parts[i].h,
                         (seg & parts[i].bit) ? color : dim);
    }
}

static void buffer_segment_number_sized(uint16_t *buf, int bw, int bh, int x, int y,
                                        const char *digits, uint16_t color,
                                        int t, int l, int gap)
{
    if (!digits || !digits[0]) {
        digits = "-";
    }
    int digit_w = segment_digit_width(t, l);
    for (const char *p = digits; *p; p++) {
        buffer_segment_digit_sized(buf, bw, bh, x, y, *p, color, UI_SEG_DIM, t, l);
        x += digit_w + gap;
    }
}

static void draw_stat_cell(uint16_t *buf, int bw, int bh, int x, int y,
                           int w, int h, const char *label, uint32_t value,
                           uint16_t accent)
{
    buffer_fill_rect(buf, bw, bh, x, y, w, h, UI_BG);
    buffer_fill_rect(buf, bw, bh, x, y, w, 1, UI_METAL_HI);
    buffer_fill_rect(buf, bw, bh, x, y + h - 1, w, 1, UI_PANEL_DARK);
    buffer_fill_rect(buf, bw, bh, x, y, 3, h, accent);
    buffer_text_small(buf, bw, bh, x + 8, y + 6, label, UI_MUTED);

    char digits[8];
    format_count_digits(value, digits, sizeof(digits));
    int digit_w = segment_number_width(digits, 2, 7, 2);
    int dx = x + w - digit_w - 9;
    if (dx < x + 48) {
        dx = x + 48;
    }
    buffer_segment_number_sized(buf, bw, bh, dx, y + 12, digits, accent, 2, 7, 2);
}

static void draw_stat_tile(int x, int y, int w, int h, const char *label,
                           uint32_t value, uint16_t accent)
{
    uint16_t *buf = ui_alloc_pixels((size_t)w * h);
    if (!buf) {
        return;
    }
    draw_stat_cell(buf, w, h, 0, 0, w, h, label, value, accent);
    (void)display_ili9488_draw_bitmap565(x, y, w, h, buf);
    ui_free_pixels(buf);
}

static void draw_inventory_title(int x, int y, int w, int h, const char *right,
                                 bool configured)
{
    uint16_t *buf = ui_alloc_pixels((size_t)w * h);
    if (!buf) {
        return;
    }
    buffer_fill(buf, w, h, UI_PANEL_DARK);
    buffer_fill_rect(buf, w, h, 0, 0, 5, h, configured ? UI_CYAN : UI_RED);
    buffer_fill_rect(buf, w, h, 0, 0, w, 1, UI_HOME_ACCENT_DIM);
    buffer_fill_rect(buf, w, h, 0, h - 1, w, 1, UI_BG);
    buffer_text(buf, w, h, 15, 8, "Part-DB 数据核心", 1, UI_TEXT);
    if (right && right[0]) {
        buffer_text_small(buf, w, h, 156, 11, right, UI_MUTED);
    }
    (void)display_ili9488_draw_bitmap565(x, y, w, h, buf);
    ui_free_pixels(buf);
}

static void draw_inventory_panel(int y)
{
    int w = display_ili9488_get_width();
    int x = 12;
    int cw = w - 24;
    int ch = 122;
    partdb_client_status_t pdb = partdb_client_get_status();
    (void)display_ili9488_fill_rect(x, y, cw, ch, UI_METAL);
    for (int gx = 20; gx < cw; gx += 32) {
        (void)display_ili9488_fill_rect(x + gx, y + 30, 1, ch - 32, UI_BG);
    }
    for (int gy = 40; gy < ch; gy += 32) {
        (void)display_ili9488_fill_rect(x + 8, y + gy, cw - 16, 1, UI_BG);
    }
    draw_inventory_title(x, y, cw, 30, inventory_sync_state_text(&pdb), pdb.configured);

    int gap = 8;
    int cell_w = (cw - 30 - gap) / 2;
    int cell_h = 36;
    int x1 = x + 15;
    int x2 = x1 + cell_w + gap;
    int y1 = y + 38;
    int y2 = y + 78;
    draw_stat_tile(x1, y1, cell_w, cell_h, "元件",
                   s_inventory.valid ? s_inventory.parts : 0, UI_HOME_ACCENT);
    draw_stat_tile(x2, y1, cell_w, cell_h, "批次",
                   s_inventory.valid ? s_inventory.lots : 0, UI_HOME_ACCENT);
    draw_stat_tile(x1, y2, cell_w, cell_h, "库位",
                   s_inventory.valid ? s_inventory.locations : 0, UI_HOME_ACCENT);
    draw_stat_tile(x2, y2, cell_w, cell_h, "分类",
                   s_inventory.valid ? s_inventory.categories : 0, UI_HOME_ACCENT);
}

static void draw_resource_strip(int x, int y, int w, int h, const char *left,
                                const char *right, uint16_t accent)
{
    uint16_t *buf = ui_alloc_pixels((size_t)w * h);
    if (!buf) {
        return;
    }
    buffer_fill(buf, w, h, UI_PANEL_DARK);
    buffer_fill_rect(buf, w, h, 0, 0, w, 1, UI_HOME_ACCENT_DIM);
    buffer_fill_rect(buf, w, h, 0, h - 1, w, 1, UI_BG);
    buffer_fill_rect(buf, w, h, 0, 0, 5, h, accent);
    if (left && left[0]) {
        buffer_text(buf, w, h, 15, 6, left, 1, UI_TEXT);
    }
    if (right && right[0]) {
        buffer_text_small(buf, w, h, 150, 9, right, UI_MUTED);
    }
    (void)display_ili9488_draw_bitmap565(x, y, w, h, buf);
    ui_free_pixels(buf);
}

static void draw_resource_node(int x, int y, int w, int h, const char *label,
                               const char *value, bool active, uint16_t accent)
{
    uint16_t *buf = ui_alloc_pixels((size_t)w * h);
    if (!buf) {
        return;
    }
    buffer_fill(buf, w, h, active ? UI_PANEL : UI_BG);
    buffer_fill_rect(buf, w, h, 0, 0, w, 1, active ? UI_HOME_ACCENT_DIM : UI_LINE);
    buffer_fill_rect(buf, w, h, 0, h - 1, w, 1, UI_BG);
    buffer_fill_rect(buf, w, h, 0, 0, 3, h, active ? accent : UI_LINE);
    buffer_text_small(buf, w, h, 8, 8, label, active ? UI_TEXT : UI_MUTED);
    const char *digits = value && value[0] ? value : "0";
    int digit_w = segment_number_width(digits, 2, 6, 2);
    int dx = (w - digit_w) / 2;
    if (dx < 8) {
        dx = 8;
    }
    int digit_h = segment_digit_height(2, 6);
    int dy = h - digit_h - 5;
    if (dy < 24) {
        dy = 24;
    }
    buffer_segment_number_sized(buf, w, h, dx, dy, digits,
                                active ? accent : UI_MUTED, 2, 6, 2);
    (void)display_ili9488_draw_bitmap565(x, y, w, h, buf);
    ui_free_pixels(buf);
}

static void draw_resource_text_node(int x, int y, int w, int h, const char *label,
                                    const char *value, bool active, uint16_t accent)
{
    uint16_t *buf = ui_alloc_pixels((size_t)w * h);
    if (!buf) {
        return;
    }
    buffer_fill(buf, w, h, active ? UI_PANEL : UI_BG);
    buffer_fill_rect(buf, w, h, 0, 0, w, 1, active ? UI_HOME_ACCENT_DIM : UI_LINE);
    buffer_fill_rect(buf, w, h, 0, h - 1, w, 1, UI_BG);
    buffer_fill_rect(buf, w, h, 0, 0, 3, h, active ? accent : UI_LINE);
    buffer_text_small(buf, w, h, 8, 8, label, active ? UI_TEXT : UI_MUTED);
    buffer_text(buf, w, h, 8, 28, value && value[0] ? value : "-", 1,
                active ? accent : UI_MUTED);
    (void)display_ili9488_draw_bitmap565(x, y, w, h, buf);
    ui_free_pixels(buf);
}

static void draw_cache_layer(int y)
{
    int screen_h = display_ili9488_get_height();
    int w = display_ili9488_get_width();
    int x = 12;
    int cw = w - 24;
    int ch = screen_h - UI_NAV_H - y - 4;
    if (ch < 176) {
        ch = 176;
    }

    partdb_client_status_t pdb = partdb_client_get_status();
    const char *sync_state = inventory_sync_state_text(&pdb);
    uint16_t sync_color = inventory_sync_state_color(&pdb);

    (void)display_ili9488_fill_rect(x, y, cw, ch, UI_PANEL_DARK);
    for (int gx = 16; gx < cw; gx += 32) {
        (void)display_ili9488_fill_rect(x + gx, y, 1, ch, UI_BG);
    }
    for (int gy = 24; gy < ch; gy += 32) {
        (void)display_ili9488_fill_rect(x, y + gy, cw, 1, UI_BG);
    }
    (void)display_ili9488_fill_rect(x, y, cw, 1, UI_HOME_ACCENT_DIM);
    (void)display_ili9488_fill_rect(x, y + ch - 1, cw, 1, UI_BG);
    (void)display_ili9488_fill_rect(x, y, 5, ch, sync_color);
    draw_resource_strip(x, y, cw, 28, "Part-DB 信息资源", sync_state, sync_color);

    int gap = 8;
    int node_w = (cw - 24 - gap * 2) / 3;
    int node_h = 56;
    int nx = x + 12;
    int ny = y + 39;
    char value[32];
    format_count_digits(s_inventory.valid ? s_inventory.suppliers : 0, value, sizeof(value));
    draw_resource_node(nx, ny, node_w, node_h, "供应商", value, s_inventory.valid, UI_HOME_ACCENT);
    format_count_digits(s_inventory.valid ? s_inventory.manufacturers : 0, value, sizeof(value));
    draw_resource_node(nx + node_w + gap, ny, node_w, node_h, "厂家", value,
                       s_inventory.valid, UI_HOME_ACCENT);
    format_count_digits(s_inventory.valid ? s_inventory.footprints : 0, value, sizeof(value));
    draw_resource_node(nx + (node_w + gap) * 2, ny, node_w, node_h, "封装", value,
                       s_inventory.valid, UI_HOME_ACCENT);

    ny += node_h + 10;
    format_count_digits(s_inventory.valid ? s_inventory.attachments : 0, value, sizeof(value));
    draw_resource_node(nx, ny, node_w, node_h, "附件", value, s_inventory.valid, UI_HOME_ACCENT);
    format_count_digits(s_inventory.valid ? s_inventory.projects : 0, value, sizeof(value));
    draw_resource_node(nx + node_w + gap, ny, node_w, node_h, "项目", value,
                       s_inventory.valid, UI_HOME_ACCENT);
    draw_resource_text_node(nx + (node_w + gap) * 2, ny, node_w, node_h, "同步", sync_state,
                            pdb.configured, sync_color);

}

static void draw_home(void)
{
    draw_header("主页", UI_CYAN);
    draw_inventory_panel(112);
    draw_cache_layer(240);
}

static void draw_shortcuts(void)
{
    draw_header("快捷", UI_ORANGE);
    int w = display_ili9488_get_width();
    int gap = 8;
    int x1 = 12;
    int tile_w = (w - 24 - gap) / 2;
    int x2 = x1 + tile_w + gap;
    draw_tile(x1, 116, tile_w, 74, "入库",
              s_stock_op == UI_STOCK_OP_IN ? "已选择" : "选择模式",
              "再搜索/NFC/扫码", s_stock_op == UI_STOCK_OP_IN ? UI_CYAN : UI_GREEN);
    draw_tile(x2, 116, tile_w, 74, "出库",
              s_stock_op == UI_STOCK_OP_OUT ? "已选择" : "选择模式",
              "再搜索/NFC/扫码", s_stock_op == UI_STOCK_OP_OUT ? UI_CYAN : UI_ORANGE);
    draw_tile(x1, 200, tile_w, 74, "扫码", "相机二维码", "当前页返回", UI_CYAN);
    nfc_service_status_t nfc = nfc_service_get_status();
    bool nfc_ready = nfc_ui_ready();
    char nfc_line1[48];
    char nfc_line2[96];
    snprintf(nfc_line1, sizeof(nfc_line1), "%s",
             nfc_ready ? (nfc.tag_present ? "已读卡" : "在线") : "离线");
    const char *nfc_hint = nfc.tag_present ?
                           (nfc.text[0] ? nfc.text : (nfc.uid[0] ? nfc.uid : "已触发")) :
                           (nfc_ready ? "等待贴卡" : esp_err_to_name(nfc.last_err));
    snprintf(nfc_line2, sizeof(nfc_line2), "%.95s", nfc_hint);
    draw_tile(x2, 200, tile_w, 74, "NFC", nfc_line1, nfc_line2,
              nfc_ready ? (nfc.tag_present ? UI_GREEN : UI_BLUE) : UI_RED);
    draw_tile(x1, 284, tile_w, 74, "搜索", s_search[0] ? s_search : "模糊关键字", "打开键盘", UI_YELLOW);
    draw_tile(x2, 284, tile_w, 74, "盘点", "预留", "后续功能", UI_PANEL_DARK);
}

static void clamp_results_cursor(void)
{
    if (s_results.result_count <= 0) {
        s_results_scroll = 0;
        s_results_selected = 0;
        return;
    }
    if (s_results_selected < 0) {
        s_results_selected = 0;
    }
    if (s_results_selected >= s_results.result_count) {
        s_results_selected = s_results.result_count - 1;
    }
    int max_scroll = s_results.result_count - UI_SEARCH_RESULT_VISIBLE;
    if (max_scroll < 0) {
        max_scroll = 0;
    }
    if (s_results_scroll > max_scroll) {
        s_results_scroll = max_scroll;
    }
    if (s_results_scroll < 0) {
        s_results_scroll = 0;
    }
    if (s_results_selected < s_results_scroll) {
        s_results_scroll = s_results_selected;
    }
    if (s_results_selected >= s_results_scroll + UI_SEARCH_RESULT_VISIBLE) {
        s_results_scroll = s_results_selected - UI_SEARCH_RESULT_VISIBLE + 1;
    }
}

static bool move_results_cursor(int delta)
{
    if (!s_results.valid || s_results.loading || s_results.result_count <= 0 || delta == 0) {
        return false;
    }
    int old_selected = s_results_selected;
    s_results_selected += delta;
    clamp_results_cursor();
    if (s_results_selected == old_selected) {
        return false;
    }
    s_nfc_confirm_open = false;
    s_dirty = true;
    return true;
}

static bool move_detail_info_page(int delta)
{
    if (!s_detail.found || delta == 0) {
        return false;
    }
    const int page_count = 3;
    s_detail_info_page = (s_detail_info_page + delta) % page_count;
    if (s_detail_info_page < 0) {
        s_detail_info_page += page_count;
    }
    s_dirty = true;
    return true;
}

static void draw_result_item(int y, int idx, int total, const ui_search_result_item_t *item, bool selected)
{
    int w = display_ili9488_get_width();
    int x = 12;
    int cw = w - 24;
    int ch = UI_RESULTS_ITEM_H;
    uint16_t *buf = ui_alloc_pixels((size_t)cw * ch);
    if (!buf || !item) {
        if (buf) {
            ui_free_pixels(buf);
        }
        return;
    }
    buffer_fill(buf, cw, ch, selected ? UI_PANEL_DARK : UI_PANEL);
    buffer_fill_rect(buf, cw, ch, 0, 0, 5, ch, selected ? UI_YELLOW : UI_CYAN);
    buffer_fill_rect(buf, cw, ch, 0, 0, cw, 1, selected ? UI_YELLOW : UI_LINE);
    buffer_fill_rect(buf, cw, ch, 0, ch - 1, cw, 1, UI_BG);

    char title[64];
    snprintf(title, sizeof(title), "%d/%d P%04d", idx + 1, total, item->id);
    buffer_text_small(buf, cw, ch, 15, 10, title, UI_MUTED);
    buffer_text(buf, cw, ch, 86, 8,
                item->name[0] ? item->name : "未命名元件", 1, UI_TEXT);

    char line[96];
    if (item->amount_valid) {
        snprintf(line, sizeof(line), "IPN %s  库存 %.0f",
                 item->ipn[0] ? item->ipn : "-", (double)item->amount);
    } else {
        snprintf(line, sizeof(line), "IPN %s  库存 --",
                 item->ipn[0] ? item->ipn : "-");
    }
    buffer_text_small(buf, cw, ch, 15, 36, line, UI_TEXT);
    snprintf(line, sizeof(line), "%s", item->category[0] ? item->category : "点击打开详情");
    buffer_text_small(buf, cw, ch, 15, 57, line, UI_MUTED);

    (void)display_ili9488_draw_bitmap565(x, y, cw, ch, buf);
    ui_free_pixels(buf);
}

static void draw_results(void)
{
    draw_header("结果", UI_CYAN);
    char line1[96];
    char line2[96];
    if (s_results.loading) {
        snprintf(line1, sizeof(line1), "%s", s_results.query[0] ? s_results.query : s_search);
        draw_card(114, "搜索中", line1, "正在查询 Part-DB", UI_YELLOW);
        return;
    }
    if (!s_results.query[0]) {
        draw_card(114, "搜索结果", "输入关键字开始搜索", "点击顶部搜索框", UI_BLUE);
        return;
    }
    if (!s_results.valid || s_results.result_count <= 0) {
        snprintf(line1, sizeof(line1), "%s", s_results.query);
        snprintf(line2, sizeof(line2), "HTTP %d  %s",
                 s_results.http_status, esp_err_to_name(s_results.last_err));
        draw_card(114, "未找到结果", line1, line2, UI_RED);
        return;
    }

    clamp_results_cursor();
    int visible = s_results.result_count;
    if (visible > UI_SEARCH_RESULT_VISIBLE) {
        visible = UI_SEARCH_RESULT_VISIBLE;
    }
    for (int i = 0; i < visible; i++) {
        int idx = s_results_scroll + i;
        if (idx >= s_results.result_count) {
            break;
        }
        draw_result_item(UI_RESULTS_Y + i * UI_RESULTS_ROW_H, idx,
                         s_results.result_count, &s_results.items[idx],
                         idx == s_results_selected);
    }
}

static int detail_action_y(void)
{
    int y = display_ili9488_get_height() - UI_DETAIL_BOTTOM_MARGIN - UI_DETAIL_ACTION_H;
    int min_y = UI_DETAIL_INFO_Y + 76 + UI_DETAIL_GAP;
    return y < min_y ? min_y : y;
}

static int detail_info_h(void)
{
    int h = detail_action_y() - UI_DETAIL_INFO_Y - UI_DETAIL_GAP;
    return h < 76 ? 76 : h;
}

static int estimate_ui_text_width(const char *text, uint8_t scale)
{
    if (!text || scale == 0) {
        return 0;
    }
    int width = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        if (*p < 0x80) {
            width += 8 * scale;
            p++;
            continue;
        }
        if (p[0] == 0xC2 && p[1] == 0xB0) {
            width += 8 * scale;
        } else {
            width += 16 * scale;
        }
        if ((*p & 0xE0) == 0xC0 && p[1]) {
            p += 2;
        } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
            p += 3;
        } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
            p += 4;
        } else {
            p++;
        }
    }
    return width;
}

static void detail_input_layout(int *left_x, int *right_x, int *field_y,
                                int *field_w, int *field_h)
{
    int w = display_ili9488_get_width();
    int gap = 8;
    int fw = (w - UI_DETAIL_FIELD_X * 2 - gap) / 2;
    if (fw < 96) {
        fw = 96;
    }
    if (left_x) {
        *left_x = UI_DETAIL_FIELD_X;
    }
    if (right_x) {
        *right_x = UI_DETAIL_FIELD_X + fw + gap;
    }
    if (field_y) {
        *field_y = UI_DETAIL_FIELD_ROW_Y;
    }
    if (field_w) {
        *field_w = fw;
    }
    if (field_h) {
        *field_h = UI_DETAIL_FIELD_H;
    }
}

static void draw_detail_field_box(int x, int y, int cw, const char *title,
                                  const char *value, bool active)
{
    int ch = UI_DETAIL_FIELD_H;
    uint16_t bg = active ? UI_PANEL_DARK : UI_BG;
    (void)display_ili9488_fill_rect(x, y, cw, ch, bg);
    (void)display_ili9488_fill_rect(x, y, 4, ch, active ? UI_CYAN : UI_BLUE);
    (void)display_ili9488_fill_rect(x, y, cw, 1, UI_LINE);
    (void)display_ili9488_fill_rect(x, y + ch - 1, cw, 1, UI_LINE);
    draw_text_patch_small(x + 10, y + 3, cw - 18, 12, title, UI_MUTED, bg);
    draw_text_patch(x + 10, y + 16, cw - 18, 17,
                    value && value[0] ? value : "-", UI_TEXT, bg);
}

static void draw_detail_inputs(void)
{
    int x1 = 0;
    int x2 = 0;
    int y = 0;
    int fw = 0;
    int fh = 0;
    detail_input_layout(&x1, &x2, &y, &fw, &fh);
    (void)fh;
    draw_detail_field_box(x1, y, fw, "对象/编号", s_detail_ipn,
                          s_input_target == UI_INPUT_IPN);
    draw_detail_field_box(x2, y, fw, "出入库数量", s_detail_qty,
                          s_input_target == UI_INPUT_QTY);
}

static void draw_detail_name_strip(void)
{
    int w = display_ili9488_get_width();
    int x = UI_DETAIL_FIELD_X;
    int y = UI_DETAIL_NAME_Y;
    int cw = w - UI_DETAIL_FIELD_X * 2;
    int ch = UI_DETAIL_NAME_H;
    char name[96];
    if (s_detail.found && s_detail.name[0]) {
        snprintf(name, sizeof(name), "%.95s", s_detail.name);
    } else if (s_detail.query[0]) {
        snprintf(name, sizeof(name), "%.95s", s_detail.query);
    } else if (s_detail_ipn[0]) {
        snprintf(name, sizeof(name), "%.95s", s_detail_ipn);
    } else {
        snprintf(name, sizeof(name), "等待选择元件");
    }
    char location[72];
    snprintf(location, sizeof(location), "库位  %s",
             s_detail.found && s_detail.location[0] ? s_detail.location : "未设置");
    (void)display_ili9488_fill_rect(x, y, cw, ch, UI_PANEL);
    (void)display_ili9488_fill_rect(x, y, cw, 1, UI_LINE);
    (void)display_ili9488_fill_rect(x, y + ch - 1, cw, 1, UI_BG);
    (void)display_ili9488_fill_rect(x, y, 5, ch, UI_GREEN);
    draw_text_patch_small(x + 15, y + 3, cw - 30, 12, "元器件名称", UI_MUTED, UI_PANEL);
    draw_text_patch(x + 15, y + 16, cw - 30, 17, name, UI_TEXT, UI_PANEL);
    draw_text_patch(x + 15, y + 35, cw - 30, 17, location,
                    s_detail.found && s_detail.location[0] ? UI_YELLOW : UI_MUTED,
                    UI_PANEL);
}

static void draw_detail_info_card(int y)
{
    int w = display_ili9488_get_width();
    int x = UI_DETAIL_FIELD_X;
    int cw = w - UI_DETAIL_FIELD_X * 2;
    int ch = detail_info_h();

    char desc[96];
    if (s_detail.description[0]) {
        copy_compact_text(s_detail.description, desc, sizeof(desc));
    } else {
        snprintf(desc, sizeof(desc), "简介 --");
    }
    char comment[96];
    if (s_detail.comment[0]) {
        copy_compact_text(s_detail.comment, comment, sizeof(comment));
    } else {
        snprintf(comment, sizeof(comment), "备注 --");
    }
    char params[96];
    if (s_detail.parameters[0]) {
        copy_compact_text(s_detail.parameters, params, sizeof(params));
    } else {
        snprintf(params, sizeof(params), "参数 --");
    }

    int page = s_detail_info_page % 3;
    if (page < 0) {
        page = 0;
    }
    char title[32];
    char line1[96] = {0};
    char line2[96] = {0};
    char line3[96] = {0};
    char line4[96] = {0};
    char line5[96] = {0};
    char line6[96] = {0};
    char line7[96] = {0};
    char line8[96] = {0};
    char line9[96] = {0};
    char supply[96] = {0};
    char attachment[96] = {0};
    char modules[96] = {0};
    if (s_detail.order_count > 0) {
        snprintf(supply, sizeof(supply), "供应 %d  %.24s %.28s",
                 s_detail.order_count,
                 s_detail.supplier[0] ? s_detail.supplier : "-",
                 s_detail.supplier_partnr[0] ? s_detail.supplier_partnr : "-");
    } else {
        snprintf(supply, sizeof(supply), "供应 --");
    }
    if (s_detail.attachment_count > 0) {
        snprintf(attachment, sizeof(attachment), "附件 %d  %.48s",
                 s_detail.attachment_count,
                 s_detail.attachment[0] ? s_detail.attachment : "-");
    } else {
        snprintf(attachment, sizeof(attachment), "附件 --");
    }
    if (s_detail.association_count > 0) {
        snprintf(modules, sizeof(modules), "模块 %d  %.52s",
                 s_detail.association_count,
                 s_detail.modules[0] ? s_detail.modules : "-");
    } else {
        snprintf(modules, sizeof(modules), "模块 --");
    }
    if (page == 0) {
        snprintf(title, sizeof(title), "Part-DB 基础 1/3");
        int part_id = s_detail.part_id > 0 ? s_detail.part_id : s_detail.id;
        int lot_id = active_detail_lot_id(&s_detail);
        if (lot_id > 0) {
            snprintf(line1, sizeof(line1), "IPN %.24s  P%d L%d",
                     s_detail.ipn[0] ? s_detail.ipn : "-", part_id, lot_id);
        } else {
            snprintf(line1, sizeof(line1), "IPN %.24s  P%d",
                     s_detail.ipn[0] ? s_detail.ipn : "-", part_id);
        }
        if (s_detail.amount_unknown) {
            snprintf(line2, sizeof(line2), "库存未知  批次 %d", s_detail.lot_count);
        } else if (s_detail.amount_valid) {
            snprintf(line2, sizeof(line2), "库存 %.0f  批次 %d",
                     (double)s_detail.amount, s_detail.lot_count);
        } else {
            snprintf(line2, sizeof(line2), "库存 --  批次 %d", s_detail.lot_count);
        }
        snprintf(line3, sizeof(line3), "分类 %.56s",
                 s_detail.category[0] ? s_detail.category : "-");
        snprintf(line4, sizeof(line4), "厂家 %.56s",
                 s_detail.manufacturer[0] ? s_detail.manufacturer : "-");
        snprintf(line5, sizeof(line5), "MPN %.60s",
                 s_detail.mpn[0] ? s_detail.mpn : "-");
        snprintf(line6, sizeof(line6), "封装 %.56s",
                 s_detail.footprint[0] ? s_detail.footprint : "-");
        snprintf(line7, sizeof(line7), "条码 %.56s",
                 s_detail.barcode[0] ? s_detail.barcode : "-");
        snprintf(line8, sizeof(line8), "简介 %.80s", desc);
        snprintf(line9, sizeof(line9), "参数 %.80s", params);
    } else if (page == 1) {
        snprintf(title, sizeof(title), "Part-DB 简介 2/3");
        snprintf(line1, sizeof(line1), "简介 %.80s", desc);
        snprintf(line2, sizeof(line2), "备注 %.80s", comment);
        snprintf(line3, sizeof(line3), "参数 %.80s", params);
        snprintf(line4, sizeof(line4), "MPN %.60s",
                 s_detail.mpn[0] ? s_detail.mpn : "-");
        snprintf(line5, sizeof(line5), "封装 %.56s",
                 s_detail.footprint[0] ? s_detail.footprint : "-");
        snprintf(line6, sizeof(line6), "IPN %.40s",
                 s_detail.ipn[0] ? s_detail.ipn : "-");
        snprintf(line7, sizeof(line7), "条码 %.56s",
                 s_detail.barcode[0] ? s_detail.barcode : "-");
        snprintf(line8, sizeof(line8), "%.95s", supply);
        snprintf(line9, sizeof(line9), "%.95s", attachment);
    } else {
        snprintf(title, sizeof(title), "Part-DB 模块 3/3");
        snprintf(line1, sizeof(line1), "参数 %.80s", params);
        snprintf(line2, sizeof(line2), "%.95s", supply);
        snprintf(line3, sizeof(line3), "%.95s", attachment);
        snprintf(line4, sizeof(line4), "%.95s", modules);
        snprintf(line5, sizeof(line5), "条码 %.56s",
                 s_detail.barcode[0] ? s_detail.barcode : "-");
        snprintf(line6, sizeof(line6), "MPN %.60s",
                 s_detail.mpn[0] ? s_detail.mpn : "-");
        snprintf(line7, sizeof(line7), "封装 %.56s",
                 s_detail.footprint[0] ? s_detail.footprint : "-");
        snprintf(line8, sizeof(line8), "分类 %.56s",
                 s_detail.category[0] ? s_detail.category : "-");
        snprintf(line9, sizeof(line9), "备注 %.80s", comment);
    }

    (void)display_ili9488_fill_rect(x, y, cw, ch, UI_PANEL);
    (void)display_ili9488_fill_rect(x, y, cw, 1, UI_LINE);
    (void)display_ili9488_fill_rect(x, y + ch - 1, cw, 1, UI_BG);
    (void)display_ili9488_fill_rect(x, y, 5, ch, UI_GREEN);
    draw_text_patch(x + 15, y + 8, cw - 30, 18, title, UI_MUTED, UI_PANEL);
    const char *lines[] = {
        line1, line2, line3, line4, line5, line6, line7, line8, line9
    };
    int ly = 34;
    for (int i = 0; i < (int)(sizeof(lines) / sizeof(lines[0])); i++) {
        if (!lines[i][0]) {
            break;
        }
        if (ly + 18 > ch - 4) {
            break;
        }
        draw_text_patch(x + 15, y + ly, cw - 30, 18, lines[i],
                        i == 0 ? UI_TEXT : UI_MUTED, UI_PANEL);
        ly += 22;
    }
}

static void draw_detail_action_card(int y)
{
    int w = display_ili9488_get_width();
    int x = UI_DETAIL_FIELD_X;
    int cw = w - UI_DETAIL_FIELD_X * 2;
    int ch = UI_DETAIL_ACTION_H;
    uint16_t *buf = ui_alloc_pixels((size_t)cw * ch);
    if (!buf) {
        return;
    }

    char line[96];
    bool transient_status = false;
    if (s_stock.busy) {
        snprintf(line, sizeof(line), "正在写回 Part-DB");
        transient_status = true;
    } else if (s_nfc_write.busy) {
        snprintf(line, sizeof(line), "NFC%s中  %.76s",
                 nfc_action_label(s_nfc_write.action),
                 s_nfc_write.payload[0] ? s_nfc_write.payload : "等待贴卡");
        transient_status = true;
    } else if (s_stock.done && s_stock.message[0]) {
        snprintf(line, sizeof(line), "%.95s", s_stock.message);
        transient_status = true;
    } else if (s_nfc_write.done && s_nfc_write.message[0]) {
        snprintf(line, sizeof(line), "%.95s", s_nfc_write.message);
        transient_status = true;
    } else if (s_detail.amount_unknown) {
        snprintf(line, sizeof(line), "库存未知  数量 %s", s_detail_qty);
    } else if (s_detail.amount_valid) {
        snprintf(line, sizeof(line), "库存 %.0f  数量 %s", (double)s_detail.amount, s_detail_qty);
    } else {
        snprintf(line, sizeof(line), "库存 --  数量 %s", s_detail_qty);
    }
    if (!transient_status && active_detail_lot_id(&s_detail) <= 0 && s_detail.lot_count > 1) {
        snprintf(line, sizeof(line), "多批次需扫码/NFC具体批次");
    }

    buffer_fill(buf, cw, ch, UI_PANEL);
    buffer_fill_rect(buf, cw, ch, 0, 0, cw, 1, UI_LINE);
    buffer_fill_rect(buf, cw, ch, 0, ch - 1, cw, 1, UI_BG);
    buffer_fill_rect(buf, cw, ch, 0, 0, 5, ch, UI_ORANGE);
    buffer_text_small(buf, cw, ch, 15, 7, "库存", UI_MUTED);
    buffer_text_small(buf, cw, ch, 48, 7, line, UI_TEXT);

    int gap = 6;
    int by = UI_DETAIL_ACTION_BUTTON_Y;
    int bh = UI_DETAIL_ACTION_BUTTON_H;
    int bw = (cw - 30 - gap * 2) / 3;
    int bx = 15;
    const char *labels[] = {"入库", "出库", "写NFC"};
    bool nfc_ready = nfc_ui_ready();
    uint16_t colors[] = {
        UI_BG,
        UI_BG,
        nfc_ready ? UI_BLUE : UI_PANEL_DARK,
    };
    uint16_t accents[] = {
        s_stock_op == UI_STOCK_OP_IN ? UI_CYAN : UI_LINE,
        s_stock_op == UI_STOCK_OP_OUT ? UI_CYAN : UI_LINE,
        nfc_ready ? UI_CYAN : UI_LINE,
    };
    for (int i = 0; i < 3; i++) {
        int px = bx + i * (bw + gap);
        buffer_fill_rect(buf, cw, ch, px, by, bw, bh, colors[i]);
        buffer_fill_rect(buf, cw, ch, px, by, bw, 2, accents[i]);
        int label_w = estimate_ui_text_width(labels[i], 1);
        buffer_text(buf, cw, ch, px + (bw - label_w) / 2, by + 8, labels[i], 1, UI_TEXT);
    }

    (void)display_ili9488_draw_bitmap565(x, y, cw, ch, buf);
    ui_free_pixels(buf);
}

static void draw_detail_panel(int y, int ch, const char *title, const char *line1,
                              const char *line2, uint16_t accent)
{
    int w = display_ili9488_get_width();
    int x = UI_DETAIL_FIELD_X;
    int cw = w - UI_DETAIL_FIELD_X * 2;
    if (ch < 48) {
        ch = 48;
    }
    (void)display_ili9488_fill_rect(x, y, cw, ch, UI_PANEL);
    (void)display_ili9488_fill_rect(x, y, cw, 1, UI_LINE);
    (void)display_ili9488_fill_rect(x, y + ch - 1, cw, 1, UI_BG);
    (void)display_ili9488_fill_rect(x, y, 5, ch, accent);
    int title_y = ch <= UI_DETAIL_ACTION_H ? 6 : 9;
    int line1_y = ch <= UI_DETAIL_ACTION_H ? 24 : 31;
    int line2_y = ch <= UI_DETAIL_ACTION_H ? 43 : 54;
    draw_text_patch(x + 15, y + title_y, cw - 30, 18, title, UI_MUTED, UI_PANEL);
    draw_text_patch(x + 15, y + line1_y, cw - 30, 18, line1, UI_TEXT, UI_PANEL);
    if (line2 && line2[0]) {
        draw_text_patch(x + 15, y + line2_y, cw - 30, 18, line2, UI_MUTED, UI_PANEL);
    }
}

static void draw_detail(void)
{
    draw_header("三级详情", UI_GREEN);
    draw_detail_name_strip();
    draw_detail_inputs();
    int action_y = detail_action_y();
    int info_h = detail_info_h();
    char line1[96];
    char line2[96];
    if (s_detail.loading) {
        snprintf(line1, sizeof(line1), "%s", s_detail.query[0] ? s_detail.query : "等待对象");
        draw_detail_panel(UI_DETAIL_INFO_Y, info_h, "Part-DB 查询", line1, "正在读取详情", UI_YELLOW);
        snprintf(line1, sizeof(line1), "%s  数量 %s", stock_op_label(s_stock_op), s_detail_qty);
        draw_detail_panel(action_y, UI_DETAIL_ACTION_H, "库存", line1, "等待详情返回", UI_PANEL_DARK);
        return;
    }
    if (s_detail.found) {
        draw_detail_info_card(UI_DETAIL_INFO_Y);
        draw_detail_action_card(action_y);
        return;
    }
    if (s_detail.query[0]) {
        snprintf(line1, sizeof(line1), "%s", s_detail.query);
        snprintf(line2, sizeof(line2), "HTTP %d  %s", s_detail.http_status,
                 esp_err_to_name(s_detail.last_err));
        bool not_found = s_detail.last_err == ESP_ERR_NOT_FOUND ||
                         s_detail.http_status == 404;
        draw_detail_panel(UI_DETAIL_INFO_Y, info_h,
                          not_found ? "未找到元件" : "详情读取失败",
                          line1, line2, UI_RED);
        draw_detail_panel(action_y, UI_DETAIL_ACTION_H, "库存", "不能写回",
                          not_found ? "请检查对象内容" : "请检查网络/缓存",
                          UI_PANEL_DARK);
    } else {
        draw_detail_panel(UI_DETAIL_INFO_Y, info_h, "当前对象", s_search[0] ? s_search : "等待搜索/NFC/扫码选择", "输入后自动查询 Part-DB", UI_BLUE);
        draw_detail_panel(action_y, UI_DETAIL_ACTION_H, "库存", "先选择对象和数量", "确认后写回 Part-DB", UI_ORANGE);
    }
}

static void draw_nfc_confirm_modal(void)
{
    int screen_w = display_ili9488_get_width();
    int screen_h = display_ili9488_get_height();
    int mw = screen_w - 48;
    if (mw > 360) {
        mw = 360;
    }
    int mh = 178;
    int mx = (screen_w - mw) / 2;
    int my = (screen_h - mh) / 2;
    uint16_t *buf = ui_alloc_pixels((size_t)mw * mh);
    if (!buf) {
        return;
    }

    ui_nfc_action_t action = s_nfc_confirm_action != UI_NFC_ACTION_NONE ?
                             s_nfc_confirm_action : s_nfc_write.action;
    char payload[sizeof(s_nfc_write.payload)] = {0};
    if (action == UI_NFC_ACTION_WRITE_DETAIL) {
        if (s_nfc_write.payload[0]) {
            snprintf(payload, sizeof(payload), "%s", s_nfc_write.payload);
        } else {
            (void)detail_nfc_payload(&s_detail, payload, sizeof(payload));
        }
    }
    char line[96];
    const char *title = action == UI_NFC_ACTION_ERASE ? "确认擦除 NFC" : "确认写入 NFC";
    const char *line2 = "先放卡到感应区，再点确认";
    const char *cancel_label = "取消";
    const char *confirm_label = "确认";
    uint16_t state_color = UI_MUTED;
    if (s_nfc_write.busy) {
        title = action == UI_NFC_ACTION_ERASE ? "NFC 擦除中" : "NFC 写入中";
        snprintf(line, sizeof(line), "%s", s_nfc_write.message[0] ?
                 s_nfc_write.message : "正在处理 NFC 卡");
        line2 = "请保持卡片贴近";
        cancel_label = "等待";
        confirm_label = "执行中";
        state_color = UI_YELLOW;
    } else if (s_nfc_write.done) {
        title = s_nfc_write.ok ? "NFC 操作完成" : "NFC 操作失败";
        snprintf(line, sizeof(line), "%s", s_nfc_write.message[0] ?
                 s_nfc_write.message : esp_err_to_name(s_nfc_write.last_err));
        line2 = "确认后关闭并恢复轮询";
        cancel_label = "关闭";
        confirm_label = "确认";
        state_color = s_nfc_write.ok ? UI_GREEN : UI_RED;
    } else if (action == UI_NFC_ACTION_ERASE) {
        snprintf(line, sizeof(line), "清空卡片 NDEF 内容");
    } else {
        snprintf(line, sizeof(line), "将写入 %.80s", payload[0] ? payload : "--");
    }
    nfc_tag_t active_tag = {0};
    bool active = s_nfc_confirm_tag_valid ||
                  nfc_service_get_active_tag(&active_tag, 3000);
    const char *state = nfc_ui_ready() ? (active ? "已检测到卡" : "等待卡片") : "NFC 离线";

    buffer_fill(buf, mw, mh, UI_PANEL_DARK);
    buffer_fill_rect(buf, mw, mh, 0, 0, mw, 2, UI_YELLOW);
    buffer_fill_rect(buf, mw, mh, 0, 0, 1, mh, UI_LINE);
    buffer_fill_rect(buf, mw, mh, mw - 1, 0, 1, mh, UI_LINE);
    buffer_fill_rect(buf, mw, mh, 0, mh - 1, mw, 1, UI_LINE);
    buffer_text(buf, mw, mh, 18, 18, title, 1, UI_TEXT);
    buffer_text(buf, mw, mh, 18, 48, line, 1, state_color);
    buffer_text(buf, mw, mh, 18, 74, line2, 1, UI_MUTED);
    if (s_nfc_write.busy || s_nfc_write.done) {
        snprintf(line, sizeof(line), "NFC %s", nfc_ui_ready() ? "在线" : "离线");
    } else {
        snprintf(line, sizeof(line), "%s  确认后不要移开", state);
    }
    buffer_text(buf, mw, mh, 18, 98, line, 1, active ? UI_GREEN : UI_MUTED);

    int by = mh - 54;
    int bw = (mw - 48) / 2;
    int bx1 = 18;
    int bx2 = bx1 + bw + 12;
    buffer_fill_rect(buf, mw, mh, bx1, by, bw, 36, UI_PANEL);
    buffer_fill_rect(buf, mw, mh, bx1, by, bw, 1, UI_LINE);
    buffer_text(buf, mw, mh, bx1 + 30, by + 10, cancel_label, 1, UI_MUTED);
    buffer_fill_rect(buf, mw, mh, bx2, by, bw, 36, UI_BLUE);
    buffer_fill_rect(buf, mw, mh, bx2, by, bw, 1, UI_CYAN);
    buffer_text(buf, mw, mh, bx2 + 30, by + 10, confirm_label, 1, UI_TEXT);

    (void)display_ili9488_draw_bitmap565(mx, my, mw, mh, buf);
    ui_free_pixels(buf);
}

static void draw_nfc_read_modal(void)
{
    if (!s_nfc_read_prompt.open) {
        return;
    }
    int screen_w = display_ili9488_get_width();
    int screen_h = display_ili9488_get_height();
    int mw = screen_w - 48;
    if (mw > 360) {
        mw = 360;
    }
    int mh = 178;
    int mx = (screen_w - mw) / 2;
    int my = (screen_h - mh) / 2;
    uint16_t *buf = ui_alloc_pixels((size_t)mw * mh);
    if (!buf) {
        return;
    }

    char line[96];
    snprintf(line, sizeof(line), "%s",
             s_nfc_read_prompt.message[0] ? s_nfc_read_prompt.message : "打开元器件详情");
    char raw[96];
    snprintf(raw, sizeof(raw), "卡片 %.88s",
             s_nfc_read_prompt.raw[0] ? s_nfc_read_prompt.raw : s_nfc_read_prompt.query);

    buffer_fill(buf, mw, mh, UI_PANEL_DARK);
    buffer_fill_rect(buf, mw, mh, 0, 0, mw, 2, UI_GREEN);
    buffer_fill_rect(buf, mw, mh, 0, 0, 1, mh, UI_LINE);
    buffer_fill_rect(buf, mw, mh, mw - 1, 0, 1, mh, UI_LINE);
    buffer_fill_rect(buf, mw, mh, 0, mh - 1, mw, 1, UI_LINE);
    buffer_text(buf, mw, mh, 18, 18,
                s_nfc_read_prompt.matched ? "NFC 读取到对象" : "NFC 读取到卡片",
                1, UI_TEXT);
    buffer_text(buf, mw, mh, 18, 48, line, 1,
                s_nfc_read_prompt.matched ? UI_GREEN : UI_YELLOW);
    buffer_text(buf, mw, mh, 18, 74, raw, 1, UI_MUTED);
    snprintf(line, sizeof(line), "%s",
             s_nfc_read_prompt.matched ? s_nfc_read_prompt.query : "等待 Part-DB 路由");
    buffer_text(buf, mw, mh, 18, 98, line, 1,
                s_nfc_read_prompt.matched ? UI_CYAN : UI_MUTED);

    int by = mh - 54;
    int bw = (mw - 48) / 2;
    int bx1 = 18;
    int bx2 = bx1 + bw + 12;
    buffer_fill_rect(buf, mw, mh, bx1, by, bw, 36, UI_PANEL);
    buffer_fill_rect(buf, mw, mh, bx1, by, bw, 1, UI_LINE);
    buffer_text(buf, mw, mh, bx1 + 30, by + 10, "忽略", 1, UI_MUTED);
    buffer_fill_rect(buf, mw, mh, bx2, by, bw, 36, UI_BLUE);
    buffer_fill_rect(buf, mw, mh, bx2, by, bw, 1, UI_CYAN);
    buffer_text(buf, mw, mh, bx2 + (s_nfc_read_prompt.matched ? 22 : 30),
                by + 10, s_nfc_read_prompt.matched ? "打开详情" : "关闭", 1, UI_TEXT);

    (void)display_ili9488_draw_bitmap565(mx, my, mw, mh, buf);
    ui_free_pixels(buf);
}

typedef struct {
    int gap;
    int x1;
    int x2;
    int tile_w;
    int y1;
    int h1;
    int y2;
    int h2;
    int y3;
    int h3;
    int y4;
    int h4;
} ui_settings_layout_t;

static ui_settings_layout_t settings_layout(void)
{
    int w = display_ili9488_get_width();
    ui_settings_layout_t layout = {
        .gap = 8,
        .x1 = 12,
        .y1 = 112,
        .h1 = 66,
        .y2 = 186,
        .h2 = 82,
        .y3 = 276,
        .h3 = 74,
        .y4 = 358,
        .h4 = 58,
    };
    layout.tile_w = (w - 24 - layout.gap) / 2;
    layout.x2 = layout.x1 + layout.tile_w + layout.gap;
    return layout;
}

static void draw_settings(void)
{
    draw_header("设置", UI_BLUE);
    wifi_portal_status_t wifi = wifi_portal_get_status();
    char line1[80];
    const char *busy_hint = s_settings_busy ? "处理中" : "点按切换";
    ui_settings_layout_t layout = settings_layout();
    snprintf(line1, sizeof(line1), "%u%%", s_cfg ? s_cfg->display_brightness : display_ili9488_get_brightness());
    draw_tile(layout.x1, layout.y1, layout.tile_w, layout.h1,
              "屏幕亮度", line1, "点按循环", UI_CYAN);
    snprintf(line1, sizeof(line1), "%s", wifi.ap_enabled ? "启用" : "关闭");
    draw_tile(layout.x2, layout.y1, layout.tile_w, layout.h1,
              "AP 模式", line1,
              s_settings_busy ? "处理中" : (wifi.ap_started ? "AP 已启动" : "点按切换"),
              wifi.ap_enabled ? UI_GREEN : UI_YELLOW);
    snprintf(line1, sizeof(line1), "%s", BOARD_DEVELOPER_NAME);
    draw_tile(layout.x1, layout.y2, layout.tile_w, layout.h2, "项目作者", line1,
              "开源 V1.1", UI_BLUE);
    snprintf(line1, sizeof(line1), "%s", s_cfg && s_cfg->screen_bg_path[0] ? path_basename(s_cfg->screen_bg_path) : "未选择");
    draw_tile(layout.x2, layout.y2, layout.tile_w, layout.h2,
              "屏幕背景", line1, busy_hint, UI_GREEN);
    snprintf(line1, sizeof(line1), "%s", s_cfg && s_cfg->lock_bg_path[0] ? path_basename(s_cfg->lock_bg_path) : "未选择");
    draw_tile(layout.x1, layout.y3, layout.tile_w, layout.h3,
              "锁屏壁纸", line1, busy_hint, UI_ORANGE);
    draw_tile(layout.x2, layout.y3, layout.tile_w, layout.h3,
              "系统状态", "硬件/网络/NFC", "点按查看", UI_YELLOW);
    snprintf(line1, sizeof(line1), "%u 分钟",
             s_cfg ? normalize_sleep_minutes(s_cfg->screen_sleep_minutes) : 5);
    draw_tile(layout.x1, layout.y4, layout.tile_w, layout.h4, "息屏时间", line1,
              "点按循环", UI_CYAN);
    snprintf(line1, sizeof(line1), "%s",
             s_cfg && s_cfg->nfc_read_confirm ? "弹窗确认" : "直接跳转");
    draw_tile(layout.x2, layout.y4, layout.tile_w, layout.h4, "NFC 分类", line1, "",
              nfc_ui_ready() ? UI_MAGENTA : UI_PANEL_DARK);
}

static void draw_nfc_settings(void)
{
    draw_header("NFC 设置", UI_MAGENTA);
    nfc_service_status_t nfc = nfc_service_get_status();
    char line1[96];
    char line2[96];
    int w = display_ili9488_get_width();
    int gap = 8;
    int tile_w = (w - 24 - gap) / 2;
    int x1 = 12;
    int x2 = x1 + tile_w + gap;

    snprintf(line1, sizeof(line1), "%s",
             s_cfg && s_cfg->nfc_read_confirm ? "弹窗确认" : "直接跳转");
    draw_tile(x1, 116, tile_w, 74, "读取模式", line1, "仅 p/id l/id",
              s_cfg && s_cfg->nfc_read_confirm ? UI_YELLOW : UI_GREEN);

    snprintf(line1, sizeof(line1), "%s", nfc_ui_ready() ? "在线" : "离线");
    snprintf(line2, sizeof(line2), "%.79s",
             nfc.tag_present ? (nfc.text[0] ? nfc.text : nfc.uid) : esp_err_to_name(nfc.last_err));
    draw_tile(x2, 116, tile_w, 74, "读取状态", line1, line2,
              nfc_ui_ready() ? (nfc.tag_present ? UI_GREEN : UI_BLUE) : UI_RED);

    bool nfc_busy = s_nfc_write.busy && s_nfc_write.action == UI_NFC_ACTION_ERASE;
    bool nfc_done = s_nfc_write.done && s_nfc_write.action == UI_NFC_ACTION_ERASE;
    if (nfc_busy) {
        snprintf(line1, sizeof(line1), "处理中");
    } else if (nfc_done && s_nfc_write.message[0]) {
        snprintf(line1, sizeof(line1), "%.79s", s_nfc_write.message);
    } else {
        snprintf(line1, sizeof(line1), "%s", nfc_ui_ready() ? "等待卡片" : "离线");
    }
    draw_tile(x1, 200, tile_w, 74, "擦除 NFC", line1, "先放卡再确认",
              nfc_ui_ready() ? UI_MAGENTA : UI_PANEL_DARK);

    snprintf(line1, sizeof(line1), "%s", s_settings_busy ? "处理中" :
             (nfc_ui_ready() ? "在线" : "离线"));
    draw_tile(x2, 200, tile_w, 74, "重启 NFC", line1, "恢复读卡模块",
              s_settings_busy ? UI_PANEL_DARK : UI_YELLOW);

    draw_tile(x1, 284, tile_w, 74, "写卡格式", "p/id 或 l/id", "详情页写入",
              UI_PANEL_DARK);
    draw_tile(x2, 284, tile_w, 74, "过滤规则", "短路由命中", "其他内容忽略",
              UI_PANEL_DARK);
}

static void draw_part_list_item(int y, const ui_parts_list_item_t *item)
{
    int w = display_ili9488_get_width();
    int x = 12;
    int cw = w - 24;
    int ch = 54;
    uint16_t *buf = ui_alloc_pixels((size_t)cw * ch);
    if (!buf || !item) {
        if (buf) {
            ui_free_pixels(buf);
        }
        return;
    }
    buffer_fill(buf, cw, ch, UI_PANEL);
    buffer_fill_rect(buf, cw, ch, 0, 0, cw, 1, UI_LINE);
    buffer_fill_rect(buf, cw, ch, 0, ch - 1, cw, 1, UI_BG);
    buffer_fill_rect(buf, cw, ch, 0, 0, 4, ch, UI_CYAN);

    char line[112];
    snprintf(line, sizeof(line), "P%04d  %.62s", item->id,
             item->name[0] ? item->name : "未命名元件");
    buffer_text(buf, cw, ch, 12, 4, line, 1, UI_TEXT);
    snprintf(line, sizeof(line), "IPN %.24s  封装 %.28s",
             item->ipn[0] ? item->ipn : "-",
             item->footprint[0] ? item->footprint : "-");
    buffer_text_small(buf, cw, ch, 12, 23, line, UI_MUTED);
    if (item->amount_valid) {
        snprintf(line, sizeof(line), "库存 %.0f  %.30s  %.34s",
                 (double)item->amount,
                 item->category[0] ? item->category : "-",
                 item->description[0] ? item->description : "");
    } else {
        snprintf(line, sizeof(line), "库存 --  %.30s  %.34s",
                 item->category[0] ? item->category : "-",
                 item->description[0] ? item->description : "");
    }
    buffer_text_small(buf, cw, ch, 12, 40, line, UI_MUTED);
    (void)display_ili9488_draw_bitmap565(x, y, cw, ch, buf);
    ui_free_pixels(buf);
}

static void draw_parts(void)
{
    draw_header("元件", UI_CYAN);
    char line1[96];
    char line2[96];
    if (s_parts_list.loading) {
        snprintf(line1, sizeof(line1), "第 %d 页", s_parts_list.page <= 0 ? 1 : s_parts_list.page);
        draw_card(114, "Part-DB 元件", line1, "正在读取列表", UI_YELLOW);
        return;
    }
    if (!partdb_client_get_status().configured) {
        draw_card(114, "Part-DB 元件", "接口未配置", "请到 Web 后台保存地址和 Token", UI_RED);
        return;
    }
    if (!s_parts_list.valid || s_parts_list.item_count <= 0) {
        snprintf(line1, sizeof(line1), "HTTP %d  %s",
                 s_parts_list.http_status, esp_err_to_name(s_parts_list.last_err));
        draw_card(114, "元件列表不可用", line1, "上下滑动可重试翻页", UI_RED);
        return;
    }

    int total_pages = 0;
    if (s_parts_list.total_items > 0) {
        total_pages = (int)((s_parts_list.total_items + UI_PARTS_LIST_MAX - 1) /
                            UI_PARTS_LIST_MAX);
    }
    snprintf(line1, sizeof(line1), "第 %d/%d 页  共 %lu",
             s_parts_list.page <= 0 ? 1 : s_parts_list.page,
             total_pages > 0 ? total_pages : 1,
             (unsigned long)s_parts_list.total_items);
    draw_resource_strip(12, 108, display_ili9488_get_width() - 24, 28,
                        "Part-DB 元器件", line1, UI_CYAN);

    int y = 138;
    for (int i = 0; i < s_parts_list.item_count && i < UI_PARTS_LIST_MAX; i++) {
        draw_part_list_item(y + i * 56, &s_parts_list.items[i]);
    }
    if (s_parts_list.item_count < UI_PARTS_LIST_MAX) {
        snprintf(line2, sizeof(line2), "上滑下一页  下滑上一页");
        draw_text_patch(18, display_ili9488_get_height() - UI_NAV_H - 24,
                        display_ili9488_get_width() - 36, 18, line2,
                        UI_MUTED, UI_BG);
    }
}

static void draw_system(void)
{
    hardware_diag_status_t diag = hardware_diag_get_status();
    storage_sd_status_t sd = storage_sd_get_status();
    wifi_portal_status_t wifi = wifi_portal_get_status();
    nfc_service_status_t nfc = nfc_service_get_status();
    qr_scanner_status_t qr = qr_scanner_get_status();
    bool nfc_ready = nfc_ui_ready();
    draw_header("系统状态", UI_YELLOW);
    char line1[80];
    char line2[96];
    int w = display_ili9488_get_width();
    int gap = 8;
    int tile_w = (w - 24 - gap) / 2;
    int x1 = 12;
    int x2 = x1 + tile_w + gap;
    snprintf(line1, sizeof(line1), "%s", wifi.sta_connected ? "在线" : "离线");
    snprintf(line2, sizeof(line2), "%s", wifi.sta_connected ? wifi.ip : "AP/配网");
    draw_tile(x1, 116, tile_w, 74, "网络", line1, line2, ok_color(wifi.sta_connected));
    snprintf(line1, sizeof(line1), "%s", sd.mounted ? "TF 正常" : "无 TF");
    snprintf(line2, sizeof(line2), "%lluMB 可用",
             (unsigned long long)(sd.free_bytes / 1024 / 1024));
    draw_tile(x2, 116, tile_w, 74, "缓存", line1, line2, ok_color(sd.mounted));
    snprintf(line1, sizeof(line1), "%s", nfc_ready ? "在线" : "离线");
    snprintf(line2, sizeof(line2), "%.95s",
             nfc.tag_present ? (nfc.text[0] ? nfc.text : nfc.uid) : esp_err_to_name(nfc.last_err));
    draw_tile(x1, 200, tile_w, 74, "NFC", line1, line2, ok_color(nfc_ready));
    snprintf(line1, sizeof(line1), qr.last_found ? "已识别" : (qr.scan_count ? "未识别" : "待扫码"));
    snprintf(line2, sizeof(line2), "扫码 %lu", (unsigned long)qr.scan_count);
    draw_tile(x2, 200, tile_w, 74, "扫码", line1, line2, qr.last_found ? UI_GREEN : UI_YELLOW);
    snprintf(line1, sizeof(line1), "触摸 %s", touch_ft6336_is_ready() ? "正常" : "缺失");
    snprintf(line2, sizeof(line2), "%ux%u %s", display_ili9488_get_width(), display_ili9488_get_height(),
             display_ili9488_get_orientation());
    draw_tile(x1, 284, tile_w, 74, "屏幕/触摸", line1, line2, ok_color(display_ili9488_is_ready() && touch_ft6336_is_ready()));
    snprintf(line1, sizeof(line1), "%s", diag.running ? "诊断中" : (diag.finished ? "诊断完成" : "等待诊断"));
    snprintf(line2, sizeof(line2), "耗时 %lums", (unsigned long)diag.last_run_ms);
    draw_tile(x2, 284, tile_w, 74, "硬件诊断", line1, line2, diag.finished ? UI_GREEN : UI_YELLOW);
}

static void draw_camera_action_button(int y, const char *value, uint16_t accent)
{
    int w = display_ili9488_get_width();
    int x = 12;
    int cw = w - 24;
    int ch = UI_CAMERA_BUTTON_H;
    (void)display_ili9488_fill_rect(x, y, cw, ch, UI_PANEL);
    (void)display_ili9488_fill_rect(x, y, cw, 1, accent);
    (void)display_ili9488_fill_rect(x, y + ch - 1, cw, 1, UI_BG);
    (void)display_ili9488_fill_rect(x, y, 5, ch, accent);
    draw_text_patch(x + 15, y + 7, 48, 24, "相机", UI_MUTED, UI_PANEL);
    draw_text_patch(x + 72, y + 7, cw - 84, 24, value, UI_TEXT, UI_PANEL);
}

static void draw_camera_status_line(int y, const char *text, uint16_t color)
{
    int w = display_ili9488_get_width();
    int x = 12;
    int cw = w - 24;
    int ch = 18;
    (void)display_ili9488_fill_rect(x, y, cw, ch, UI_BG);
    draw_text_patch(x, y, cw, ch, text, color, UI_BG);
}

static void draw_camera_center_guide(int px, int py)
{
    int side = (UI_CAMERA_FRAME_H * 3) / 4;
    int gx = px + (UI_CAMERA_FRAME_W - side) / 2;
    int gy = py + (UI_CAMERA_FRAME_H - side) / 2;
    int tick = side / 5;
    uint16_t c = UI_CYAN;

    (void)display_ili9488_fill_rect(gx, gy, tick, 2, c);
    (void)display_ili9488_fill_rect(gx, gy, 2, tick, c);
    (void)display_ili9488_fill_rect(gx + side - tick, gy, tick, 2, c);
    (void)display_ili9488_fill_rect(gx + side - 2, gy, 2, tick, c);
    (void)display_ili9488_fill_rect(gx, gy + side - 2, tick, 2, c);
    (void)display_ili9488_fill_rect(gx, gy + side - tick, 2, tick, c);
    (void)display_ili9488_fill_rect(gx + side - tick, gy + side - 2, tick, 2, c);
    (void)display_ili9488_fill_rect(gx + side - 2, gy + side - tick, 2, tick, c);
}

static void draw_camera(void)
{
    draw_header("相机预览", UI_CYAN);
    int w = display_ili9488_get_width();
    int px = (w - UI_CAMERA_FRAME_W) / 2;
    int py = UI_CAMERA_FRAME_Y;
    (void)display_ili9488_fill_rect(px - 2, py - 2, UI_CAMERA_FRAME_W + 4, 2, UI_LINE);
    (void)display_ili9488_fill_rect(px - 2, py + UI_CAMERA_FRAME_H,
                                    UI_CAMERA_FRAME_W + 4, 2, UI_LINE);
    (void)display_ili9488_fill_rect(px - 2, py, 2, UI_CAMERA_FRAME_H, UI_LINE);
    (void)display_ili9488_fill_rect(px + UI_CAMERA_FRAME_W, py, 2,
                                    UI_CAMERA_FRAME_H, UI_LINE);
    if (s_camera_frame && s_camera_ui.frame_seq > 0) {
        uint16_t frame_w = s_camera_ui.frame_w ? s_camera_ui.frame_w : s_camera_frame_buf_w;
        uint16_t frame_h = s_camera_ui.frame_h ? s_camera_ui.frame_h : s_camera_frame_buf_h;
        if (frame_w == 0 || frame_w > UI_CAMERA_FRAME_W) {
            frame_w = UI_CAMERA_FRAME_W;
        }
        if (frame_h == 0 || frame_h > UI_CAMERA_FRAME_H) {
            frame_h = UI_CAMERA_FRAME_H;
        }
        int frame_x = px + (UI_CAMERA_FRAME_W - frame_w) / 2;
        int frame_y = py + (UI_CAMERA_FRAME_H - frame_h) / 2;
        if (frame_y > py) {
            (void)display_ili9488_fill_rect(px, py, UI_CAMERA_FRAME_W,
                                            frame_y - py, 0x0000);
            (void)display_ili9488_fill_rect(px, frame_y + frame_h,
                                            UI_CAMERA_FRAME_W,
                                            py + UI_CAMERA_FRAME_H - frame_y - frame_h,
                                            0x0000);
        }
        if (frame_x > px) {
            (void)display_ili9488_fill_rect(px, frame_y, frame_x - px, frame_h,
                                            0x0000);
            (void)display_ili9488_fill_rect(frame_x + frame_w, frame_y,
                                            px + UI_CAMERA_FRAME_W - frame_x - frame_w,
                                            frame_h, 0x0000);
        }
        (void)display_ili9488_draw_bitmap565(frame_x, frame_y, frame_w,
                                             frame_h, s_camera_frame);
        draw_camera_center_guide(px, py);
    } else {
        (void)display_ili9488_fill_rect(px, py, UI_CAMERA_FRAME_W,
                                        UI_CAMERA_FRAME_H, 0x0000);
        draw_camera_status_line(py + UI_CAMERA_FRAME_H / 2 - 9,
                                s_camera_ui.message[0] ? s_camera_ui.message : "等待摄像头",
                                UI_MUTED);
    }

    const char *button = "扫码";
    uint16_t accent = UI_CYAN;
    uint16_t status_color = UI_MUTED;
    if (s_camera_ui.mode == UI_CAMERA_STARTING) {
        button = "启动中";
        accent = UI_YELLOW;
    } else if (s_camera_ui.mode == UI_CAMERA_CAPTURING) {
        button = "识别中";
        accent = UI_YELLOW;
    } else if (s_camera_ui.mode == UI_CAMERA_ERROR) {
        button = "重试";
        accent = UI_RED;
        status_color = UI_RED;
    } else if (s_camera_ui.mode == UI_CAMERA_RESULT) {
        button = "再次扫码";
        accent = s_camera_ui.result.found ? UI_GREEN : UI_YELLOW;
        status_color = s_camera_ui.result.found ? UI_GREEN : UI_YELLOW;
    }
    int by = camera_button_y();
    draw_camera_status_line(by - 22, s_camera_ui.message[0] ? s_camera_ui.message : "点击扫码",
                            status_color);
    draw_camera_action_button(by, button, accent);
}

static void draw_key(int x, int y, int w, int h, const char *label, bool accent)
{
    uint16_t *buf = ui_alloc_pixels((size_t)w * h);
    if (!buf) {
        return;
    }
    buffer_fill(buf, w, h, accent ? UI_BLUE : UI_PANEL);
    buffer_fill_rect(buf, w, h, 0, 0, w, 1, accent ? UI_CYAN : UI_LINE);
    buffer_fill_rect(buf, w, h, 0, h - 1, w, 1, UI_BG);
    buffer_fill_rect(buf, w, h, 0, 0, 1, h, UI_BG);
    buffer_fill_rect(buf, w, h, w - 1, 0, 1, h, UI_BG);
    int text_w = estimate_ui_text_width(label, 1);
    int tx = (w - text_w) / 2;
    if (tx < 4) {
        tx = 4;
    }
    int ty = (h - 16) / 2;
    if (ty < 4) {
        ty = 4;
    }
    buffer_text(buf, w, h, tx, ty, label, 1, accent ? UI_TEXT : UI_MUTED);
    (void)display_ili9488_draw_bitmap565(x, y, w, h, buf);
    ui_free_pixels(buf);
}

typedef struct {
    const char *label;
    const char *value;
} ui_keyboard_key_t;

typedef struct {
    const ui_keyboard_key_t *keys;
    uint8_t count;
} ui_keyboard_row_t;

static const ui_keyboard_key_t KEYBOARD_ROW_1[] = {
    {"1", "1"}, {"2", "2"}, {"3", "3"}, {"4", "4"},
    {"5", "5"}, {"6", "6"}, {"7", "7"},
};
static const ui_keyboard_key_t KEYBOARD_ROW_2[] = {
    {"8", "8"}, {"9", "9"}, {"0", "0"}, {".", "."},
    {"-", "-"}, {"/", "/"},
};
static const ui_keyboard_key_t KEYBOARD_ROW_3[] = {
    {"Q", "Q"}, {"W", "W"}, {"E", "E"}, {"R", "R"},
    {"T", "T"}, {"Y", "Y"},
};
static const ui_keyboard_key_t KEYBOARD_ROW_4[] = {
    {"U", "U"}, {"I", "I"}, {"O", "O"}, {"P", "P"},
    {"A", "A"}, {"S", "S"}, {"D", "D"},
};
static const ui_keyboard_key_t KEYBOARD_ROW_5[] = {
    {"F", "F"}, {"G", "G"}, {"H", "H"}, {"J", "J"},
    {"K", "K"}, {"L", "L"},
};
static const ui_keyboard_key_t KEYBOARD_ROW_6[] = {
    {"Z", "Z"}, {"X", "X"}, {"C", "C"}, {"V", "V"},
    {"B", "B"}, {"N", "N"}, {"M", "M"},
};
static const ui_keyboard_key_t SYMBOL_ROW_1[] = {
    {"Ω", "Ω"}, {"µ", "µ"}, {"π", "π"}, {"Δ", "Δ"},
    {"Σ", "Σ"}, {"√", "√"}, {"∞", "∞"},
};
static const ui_keyboard_key_t SYMBOL_ROW_2[] = {
    {"±", "±"}, {"×", "×"}, {"÷", "÷"}, {"≈", "≈"},
    {"≠", "≠"}, {"≤", "≤"}, {"≥", "≥"},
};
static const ui_keyboard_key_t SYMBOL_ROW_3[] = {
    {"°", "°"}, {"℃", "℃"}, {"∠", "∠"}, {"∫", "∫"},
    {"∴", "∴"}, {"∂", "∂"}, {"⚡", "⚡"},
};
static const ui_keyboard_key_t SYMBOL_ROW_4[] = {
    {"(", "("}, {")", ")"}, {"[", "["}, {"]", "]"},
    {"{", "{"}, {"}", "}"}, {"_", "_"},
};
static const ui_keyboard_key_t SYMBOL_ROW_5[] = {
    {"+", "+"}, {"-", "-"}, {"=", "="}, {"/", "/"},
    {":", ":"}, {";", ";"}, {".", "."},
};
static const ui_keyboard_key_t SYMBOL_ROW_6[] = {
    {"P", "P"}, {"L", "L"}, {"R", "R"}, {"C", "C"},
    {"V", "V"}, {"A", "A"}, {"W", "W"},
};

static const ui_keyboard_row_t KEYBOARD_MAIN_ROWS[] = {
    {.keys = KEYBOARD_ROW_1, .count = sizeof(KEYBOARD_ROW_1) / sizeof(KEYBOARD_ROW_1[0])},
    {.keys = KEYBOARD_ROW_2, .count = sizeof(KEYBOARD_ROW_2) / sizeof(KEYBOARD_ROW_2[0])},
    {.keys = KEYBOARD_ROW_3, .count = sizeof(KEYBOARD_ROW_3) / sizeof(KEYBOARD_ROW_3[0])},
    {.keys = KEYBOARD_ROW_4, .count = sizeof(KEYBOARD_ROW_4) / sizeof(KEYBOARD_ROW_4[0])},
    {.keys = KEYBOARD_ROW_5, .count = sizeof(KEYBOARD_ROW_5) / sizeof(KEYBOARD_ROW_5[0])},
    {.keys = KEYBOARD_ROW_6, .count = sizeof(KEYBOARD_ROW_6) / sizeof(KEYBOARD_ROW_6[0])},
};

static const ui_keyboard_row_t KEYBOARD_SYMBOL_ROWS[] = {
    {.keys = SYMBOL_ROW_1, .count = sizeof(SYMBOL_ROW_1) / sizeof(SYMBOL_ROW_1[0])},
    {.keys = SYMBOL_ROW_2, .count = sizeof(SYMBOL_ROW_2) / sizeof(SYMBOL_ROW_2[0])},
    {.keys = SYMBOL_ROW_3, .count = sizeof(SYMBOL_ROW_3) / sizeof(SYMBOL_ROW_3[0])},
    {.keys = SYMBOL_ROW_4, .count = sizeof(SYMBOL_ROW_4) / sizeof(SYMBOL_ROW_4[0])},
    {.keys = SYMBOL_ROW_5, .count = sizeof(SYMBOL_ROW_5) / sizeof(SYMBOL_ROW_5[0])},
    {.keys = SYMBOL_ROW_6, .count = sizeof(SYMBOL_ROW_6) / sizeof(SYMBOL_ROW_6[0])},
};

static const ui_keyboard_row_t *keyboard_rows(uint8_t *row_count)
{
    if (s_keyboard_page == 1) {
        if (row_count) {
            *row_count = sizeof(KEYBOARD_SYMBOL_ROWS) / sizeof(KEYBOARD_SYMBOL_ROWS[0]);
        }
        return KEYBOARD_SYMBOL_ROWS;
    }
    if (row_count) {
        *row_count = sizeof(KEYBOARD_MAIN_ROWS) / sizeof(KEYBOARD_MAIN_ROWS[0]);
    }
    return KEYBOARD_MAIN_ROWS;
}

static int keyboard_base_key_w(int screen_w)
{
    int key_w = (screen_w - UI_KEYBOARD_MARGIN * 2 - UI_KEYBOARD_GAP * 6) / 7;
    return key_w < 34 ? 34 : key_w;
}

static int keyboard_row_y(int y0, int row)
{
    return y0 + UI_KEYBOARD_TOP + row * (UI_KEYBOARD_ROW_H + UI_KEYBOARD_GAP);
}

static void draw_keyboard(void)
{
    int screen_w = display_ili9488_get_width();
    int screen_h = display_ili9488_get_height();
    int y0 = screen_h - UI_KEYBOARD_H;
    (void)display_ili9488_fill_rect(0, y0, screen_w, UI_KEYBOARD_H, UI_PANEL_DARK);
    (void)display_ili9488_fill_rect(0, y0, screen_w, 1, UI_LINE);

    int key_w = keyboard_base_key_w(screen_w);
    uint8_t row_count = 0;
    const ui_keyboard_row_t *rows = keyboard_rows(&row_count);
    for (int r = 0; r < row_count; r++) {
        const ui_keyboard_row_t *row = &rows[r];
        int row_w = row->count * key_w + (row->count - 1) * UI_KEYBOARD_GAP;
        int x = (screen_w - row_w) / 2;
        if (x < UI_KEYBOARD_MARGIN) {
            x = UI_KEYBOARD_MARGIN;
        }
        int y = keyboard_row_y(y0, r);
        for (int i = 0; i < row->count; i++) {
            draw_key(x, y, key_w, UI_KEYBOARD_ROW_H, row->keys[i].label, false);
            x += key_w + UI_KEYBOARD_GAP;
        }
    }

    int special_y = keyboard_row_y(y0, row_count);
    int special_h = screen_h - special_y - 8;
    if (special_h < UI_KEYBOARD_ROW_H) {
        special_h = UI_KEYBOARD_ROW_H;
    }
    int special_w = (screen_w - UI_KEYBOARD_MARGIN * 2 - UI_KEYBOARD_GAP * 2) / 3;
    int x = UI_KEYBOARD_MARGIN;
    draw_key(x, special_y, special_w, special_h,
             s_keyboard_page == 1 ? "ABC" : "符号", true);
    x += special_w + UI_KEYBOARD_GAP;
    draw_key(x, special_y, special_w, special_h, "DEL", true);
    x += special_w + UI_KEYBOARD_GAP;
    draw_key(x, special_y, screen_w - UI_KEYBOARD_MARGIN - x,
             special_h, "OK", true);
}

static void redraw(void)
{
    if (!display_ili9488_is_ready() || s_screen_sleeping || !display_ili9488_is_awake()) {
        return;
    }
    if (s_full_repaint) {
        (void)display_ili9488_clear(UI_BG);
        s_full_repaint = false;
    }
    switch ((device_ui_page_t)s_status.page) {
    case DEVICE_UI_PAGE_RESULTS:
        draw_results();
        break;
    case DEVICE_UI_PAGE_SHORTCUTS:
        draw_shortcuts();
        break;
    case DEVICE_UI_PAGE_DETAIL:
        draw_detail();
        break;
    case DEVICE_UI_PAGE_INFO:
        draw_parts();
        break;
    case DEVICE_UI_PAGE_SETTINGS:
        draw_settings();
        break;
    case DEVICE_UI_PAGE_SYSTEM:
        draw_system();
        break;
    case DEVICE_UI_PAGE_NFC_SETTINGS:
        draw_nfc_settings();
        break;
    case DEVICE_UI_PAGE_CAMERA:
        draw_camera();
        break;
    case DEVICE_UI_PAGE_HOME:
    default:
        draw_home();
        break;
    }
    if (s_nfc_confirm_open) {
        draw_nfc_confirm_modal();
    }
    if (s_nfc_read_prompt.open) {
        draw_nfc_read_modal();
    }
    if (s_keyboard_open) {
        draw_keyboard();
    } else if (main_page_index((device_ui_page_t)s_status.page) >= 0) {
        draw_footer();
    }
    s_status.redraw_count++;
}

static void draw_search_input_only(void)
{
    int w = display_ili9488_get_width();
    int sx = 12;
    int sy = 58;
    int sw = w - 24;
    int sh = 34;
    draw_search_field_direct(sx, sy, sw, sh,
                             page_accent((device_ui_page_t)s_status.page));
}

static void redraw_active_input(void)
{
    if (!display_ili9488_is_ready() || s_screen_sleeping || !display_ili9488_is_awake()) {
        return;
    }
    switch (s_input_target) {
    case UI_INPUT_SEARCH:
        draw_search_input_only();
        break;
    case UI_INPUT_IPN:
        if (s_status.page == DEVICE_UI_PAGE_DETAIL) {
            draw_detail_inputs();
        }
        break;
    case UI_INPUT_QTY:
        if (s_status.page == DEVICE_UI_PAGE_DETAIL) {
            draw_detail_inputs();
        }
        break;
    case UI_INPUT_NONE:
    default:
        return;
    }
    s_status.redraw_count++;
}

static void set_page_internal(device_ui_page_t page)
{
    if (page >= DEVICE_UI_PAGE_COUNT) {
        page = DEVICE_UI_PAGE_HOME;
    }
    if (s_status.page == DEVICE_UI_PAGE_CAMERA && page != DEVICE_UI_PAGE_CAMERA) {
        stop_camera_preview();
    }
    if (s_status.page != page) {
        s_full_repaint = true;
        s_have_view_hash = false;
    }
    s_status.page = page;
    s_status.page_name = device_ui_page_name(page);
    s_dirty = true;
    if (page == DEVICE_UI_PAGE_INFO && !s_parts_task &&
        !s_parts_list.loading && !s_parts_list.valid) {
        schedule_parts_list(1);
    }
}

esp_err_t device_ui_set_page(device_ui_page_t page)
{
    if (page >= DEVICE_UI_PAGE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (page == DEVICE_UI_PAGE_CAMERA) {
        begin_camera_preview();
        return ESP_OK;
    }
    set_page_internal(page);
    return ESP_OK;
}

esp_err_t device_ui_next_page(void)
{
    int idx = main_page_index((device_ui_page_t)s_status.page);
    if (idx < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    idx = (idx + 1) % UI_MAIN_PAGE_COUNT;
    set_page_internal(s_main_pages[idx]);
    return ESP_OK;
}

esp_err_t device_ui_prev_page(void)
{
    int idx = main_page_index((device_ui_page_t)s_status.page);
    if (idx < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    idx = (idx + UI_MAIN_PAGE_COUNT - 1) % UI_MAIN_PAGE_COUNT;
    set_page_internal(s_main_pages[idx]);
    return ESP_OK;
}

esp_err_t device_ui_request_redraw(void)
{
    s_dirty = true;
    s_full_repaint = true;
    s_have_view_hash = false;
    return ESP_OK;
}

static void go_back_page(void)
{
    s_keyboard_open = false;
    s_input_target = UI_INPUT_NONE;
    s_input_dirty = false;
    release_nfc_result_hold(false);
    s_nfc_confirm_open = false;
    s_nfc_confirm_action = UI_NFC_ACTION_NONE;
    close_nfc_read_prompt();
    if (s_camera_task || s_camera_ui.mode != UI_CAMERA_STOPPED) {
        stop_camera_preview();
        if (!s_camera_task) {
            release_camera_frame_buffer();
        }
    }
    s_full_repaint = true;
    s_have_view_hash = false;
    switch ((device_ui_page_t)s_status.page) {
    case DEVICE_UI_PAGE_RESULTS:
        set_page_internal(DEVICE_UI_PAGE_HOME);
        break;
    case DEVICE_UI_PAGE_DETAIL:
        set_page_internal(s_results.valid ? DEVICE_UI_PAGE_RESULTS : DEVICE_UI_PAGE_HOME);
        break;
    case DEVICE_UI_PAGE_CAMERA:
        set_page_internal(DEVICE_UI_PAGE_SHORTCUTS);
        break;
    case DEVICE_UI_PAGE_SYSTEM:
        set_page_internal(DEVICE_UI_PAGE_SETTINGS);
        break;
    case DEVICE_UI_PAGE_NFC_SETTINGS:
        set_page_internal(DEVICE_UI_PAGE_SETTINGS);
        break;
    default:
        set_page_internal(DEVICE_UI_PAGE_HOME);
        break;
    }
}

static void go_home_page(void)
{
    s_keyboard_open = false;
    s_input_target = UI_INPUT_NONE;
    s_input_dirty = false;
    release_nfc_result_hold(false);
    s_nfc_confirm_open = false;
    s_nfc_confirm_action = UI_NFC_ACTION_NONE;
    close_nfc_read_prompt();
    if (s_camera_task || s_camera_ui.mode != UI_CAMERA_STOPPED) {
        stop_camera_preview();
        if (!s_camera_task) {
            release_camera_frame_buffer();
        }
    }
    s_full_repaint = true;
    s_have_view_hash = false;
    set_page_internal(DEVICE_UI_PAGE_HOME);
}

static bool handle_context_button_event(const char *event)
{
    if (!event) {
        return false;
    }
    if (s_nfc_read_prompt.open) {
        if (strcmp(event, "confirm") == 0) {
            confirm_nfc_read_prompt();
        } else if (strcmp(event, "back") == 0 || strcmp(event, "up") == 0 ||
                   strcmp(event, "down") == 0) {
            close_nfc_read_prompt();
        } else {
            return false;
        }
        return true;
    }
    if (s_nfc_confirm_open) {
        if (strcmp(event, "confirm") == 0) {
            if (s_nfc_write.busy) {
                s_dirty = true;
                return true;
            }
            if (s_nfc_write.done) {
                clear_nfc_write_result();
                return true;
            }
            ui_nfc_action_t action = s_nfc_confirm_action;
            s_nfc_confirm_action = UI_NFC_ACTION_NONE;
            if (action == UI_NFC_ACTION_ERASE) {
                schedule_nfc_erase();
            } else {
                schedule_nfc_write();
            }
        } else if (strcmp(event, "back") == 0 || strcmp(event, "up") == 0 ||
                   strcmp(event, "down") == 0) {
            if (s_nfc_write.busy) {
                s_dirty = true;
                return true;
            }
            clear_nfc_write_result();
        } else {
            return false;
        }
        return true;
    }

    if (s_status.page == DEVICE_UI_PAGE_RESULTS) {
        if (strcmp(event, "up") == 0) {
            (void)move_results_cursor(-1);
        } else if (strcmp(event, "down") == 0) {
            (void)move_results_cursor(1);
        } else if (strcmp(event, "confirm") == 0) {
            (void)enter_selected_result();
        } else if (strcmp(event, "back") == 0) {
            go_back_page();
        } else {
            return false;
        }
        return true;
    }

    if (s_status.page == DEVICE_UI_PAGE_DETAIL) {
        if (strcmp(event, "back") == 0) {
            go_back_page();
        } else if (strcmp(event, "up") == 0) {
            (void)move_detail_info_page(-1);
        } else if (strcmp(event, "down") == 0) {
            (void)move_detail_info_page(1);
        } else if (strcmp(event, "confirm") == 0) {
            s_dirty = true;
        } else {
            return false;
        }
        return true;
    }
    if (s_status.page == DEVICE_UI_PAGE_CAMERA) {
        if (strcmp(event, "confirm") == 0 || strcmp(event, "down") == 0) {
            request_camera_capture();
        } else if (strcmp(event, "back") == 0 || strcmp(event, "up") == 0) {
            go_back_page();
        } else {
            return false;
        }
        return true;
    }
    if (s_status.page == DEVICE_UI_PAGE_SYSTEM) {
        if (strcmp(event, "back") == 0 || strcmp(event, "up") == 0) {
            go_back_page();
        } else if (strcmp(event, "home") == 0) {
            go_home_page();
        } else {
            return false;
        }
        return true;
    }
    if (s_status.page == DEVICE_UI_PAGE_NFC_SETTINGS) {
        if (strcmp(event, "confirm") == 0 || strcmp(event, "down") == 0) {
            schedule_settings_action(UI_SETTINGS_ACTION_NFC_READ_MODE);
        } else if (strcmp(event, "back") == 0 || strcmp(event, "up") == 0) {
            go_back_page();
        } else if (strcmp(event, "home") == 0) {
            go_home_page();
        } else {
            return false;
        }
        return true;
    }
    if (s_status.page == DEVICE_UI_PAGE_INFO) {
        if (strcmp(event, "up") == 0) {
            (void)move_parts_page(-1);
        } else if (strcmp(event, "down") == 0) {
            (void)move_parts_page(1);
        } else if (strcmp(event, "confirm") == 0) {
            (void)enter_parts_list_item(0);
        } else {
            return false;
        }
        return true;
    }
    return false;
}

static void handle_button_event(void)
{
    button_input_status_t buttons = button_input_get_status();
    if (buttons.event_count == s_status.handled_button_count) {
        return;
    }
    s_status.handled_button_count = buttons.event_count;
    s_status.last_button_event = buttons.last_event ? buttons.last_event : "none";

    if (strcmp(s_status.last_button_event, "sleep_wake") == 0) {
        if (!s_screen_sleeping && display_ili9488_is_awake()) {
            sleep_display_now("button");
        } else {
            wake_display_for_activity("button");
        }
        return;
    }
    if (s_screen_sleeping || !display_ili9488_is_awake()) {
        wake_display_for_activity("button");
        return;
    }
    mark_ui_activity();
    if (handle_context_button_event(s_status.last_button_event)) {
        return;
    }
    if (strcmp(s_status.last_button_event, "up") == 0 ||
        strcmp(s_status.last_button_event, "back") == 0) {
        (void)device_ui_prev_page();
    } else if (strcmp(s_status.last_button_event, "down") == 0 ||
               strcmp(s_status.last_button_event, "confirm") == 0) {
        (void)device_ui_next_page();
    } else if (strcmp(s_status.last_button_event, "home") == 0) {
        go_home_page();
    } else if (strcmp(s_status.last_button_event, "up_long") == 0) {
        set_page_internal(DEVICE_UI_PAGE_SHORTCUTS);
    } else if (strcmp(s_status.last_button_event, "down_long") == 0) {
        set_page_internal(DEVICE_UI_PAGE_INFO);
    } else {
        s_dirty = true;
    }
}

static void record_touch_event(const char *event, uint16_t x, uint16_t y)
{
    s_status.touch_event_count++;
    s_status.last_touch_event = event ? event : "touch";
    s_status.last_touch_x = x;
    s_status.last_touch_y = y;
    if (!s_screen_sleeping && display_ili9488_is_awake()) {
        mark_ui_activity();
    }
}

static void update_touch_point(const char *event, uint16_t x, uint16_t y)
{
    s_status.last_touch_event = event ? event : "touch";
    s_status.last_touch_x = x;
    s_status.last_touch_y = y;
}

static void update_touch_range(uint16_t raw_x, uint16_t raw_y, uint16_t x, uint16_t y)
{
    if (!s_status.touch_range_valid) {
        s_status.touch_range_valid = true;
        s_status.touch_raw_min_x = raw_x;
        s_status.touch_raw_max_x = raw_x;
        s_status.touch_raw_min_y = raw_y;
        s_status.touch_raw_max_y = raw_y;
        s_status.touch_min_x = x;
        s_status.touch_max_x = x;
        s_status.touch_min_y = y;
        s_status.touch_max_y = y;
        return;
    }
    if (raw_x < s_status.touch_raw_min_x) s_status.touch_raw_min_x = raw_x;
    if (raw_x > s_status.touch_raw_max_x) s_status.touch_raw_max_x = raw_x;
    if (raw_y < s_status.touch_raw_min_y) s_status.touch_raw_min_y = raw_y;
    if (raw_y > s_status.touch_raw_max_y) s_status.touch_raw_max_y = raw_y;
    if (x < s_status.touch_min_x) s_status.touch_min_x = x;
    if (x > s_status.touch_max_x) s_status.touch_max_x = x;
    if (y < s_status.touch_min_y) s_status.touch_min_y = y;
    if (y > s_status.touch_max_y) s_status.touch_max_y = y;
}

static void transform_touch_point(touch_point_t *point)
{
    uint8_t rotation = s_cfg ? s_cfg->touch_rotation : 2;
    bool swap_xy = s_cfg ? s_cfg->touch_swap_xy : false;
    bool flip_x = s_cfg ? s_cfg->touch_flip_x : false;
    bool flip_y = s_cfg ? s_cfg->touch_flip_y : false;
    uint16_t raw_width = s_cfg ? s_cfg->touch_raw_width : BOARD_TOUCH_RAW_WIDTH;
    uint16_t raw_height = s_cfg ? s_cfg->touch_raw_height : BOARD_TOUCH_RAW_HEIGHT;
    touch_ft6336_transform_to_display(point, swap_xy, raw_width, raw_height,
                                      rotation, flip_x, flip_y,
                                      display_ili9488_get_width(),
                                      display_ili9488_get_height());
}

static void apply_touch_page_action(const char *event, bool next, uint16_t x, uint16_t y)
{
    record_touch_event(event, x, y);
    if (s_screen_sleeping || !display_ili9488_is_awake()) {
        wake_display_for_activity("touch");
        return;
    }
    if (next) {
        (void)device_ui_next_page();
    } else {
        (void)device_ui_prev_page();
    }
    ESP_LOGI(TAG, "touch %s x=%u y=%u page=%s",
             s_status.last_touch_event, x, y, s_status.page_name);
}

static void handle_nfc_confirm_tag_lock(void)
{
    if (!s_nfc_confirm_open || s_nfc_write.busy || s_nfc_write.done ||
        s_nfc_result_holds_service || nfc_pn532_passive_scan_pending()) {
        return;
    }
    nfc_tag_t tag = {0};
    if (!nfc_service_get_active_tag(&tag, UI_NFC_TAG_WAIT_MS)) {
        return;
    }
    nfc_service_suspend_for_request();
    s_nfc_result_holds_service = true;
    s_nfc_confirm_tag = tag;
    s_nfc_confirm_tag_valid = true;
    snprintf(s_nfc_write.message, sizeof(s_nfc_write.message), "已检测到 NFC 卡");
    s_have_view_hash = false;
    s_dirty = true;
}

static void handle_nfc_detail_entry(void)
{
    if (s_nfc_confirm_open || s_nfc_write.busy ||
        (s_status.page == DEVICE_UI_PAGE_CAMERA && !s_screen_sleeping &&
         display_ili9488_is_awake())) {
        return;
    }
    nfc_service_status_t nfc = nfc_service_get_status();
    if (!nfc.tag_present) {
        s_last_nfc_prompt_key[0] = '\0';
        s_seen_nfc_read_count = nfc.read_count;
        return;
    }
    if (!nfc.text[0]) {
        return;
    }

    ui_partdb_target_t target = {0};
    char query[32] = {0};
    if (!nfc_fast_route_from_text(nfc.text, &target, query, sizeof(query))) {
        return;
    }

    char key[sizeof(s_last_nfc_prompt_key)] = {0};
    snprintf(key, sizeof(key), "%s|%s", nfc.uid, query);
    if (strcmp(key, s_last_nfc_prompt_key) == 0 ||
        current_detail_matches_nfc_query(query)) {
        return;
    }

    snprintf(s_last_nfc_prompt_key, sizeof(s_last_nfc_prompt_key), "%s", key);
    s_seen_nfc_read_count = nfc.read_count;
    if (s_nfc_read_prompt.open) {
        close_nfc_read_prompt();
    }
    if (s_screen_sleeping || !display_ili9488_is_awake()) {
        wake_display_for_activity("nfc");
    } else {
        mark_ui_activity();
    }
    if (s_cfg && s_cfg->nfc_read_confirm) {
        open_nfc_route_prompt(&nfc, &target, query);
    } else {
        enter_detail_with_object(query);
    }
}

static void handle_auto_sleep(void)
{
    if (s_screen_sleeping || !display_ili9488_is_awake() || s_nfc_write.busy) {
        return;
    }
    int64_t timeout_ms = screen_sleep_timeout_ms();
    if (timeout_ms == 0 || s_last_activity_ms == 0) {
        return;
    }
    int64_t now_ms = ui_time_ms();
    if (now_ms < s_last_activity_ms || now_ms < s_next_sleep_allowed_ms) {
        return;
    }
    int64_t idle_ms = now_ms - s_last_activity_ms;
    if (!s_auto_sleep_applied && idle_ms >= timeout_ms) {
        sleep_display_now("idle");
    }
}

static char *input_buffer(size_t *max_len)
{
    if (max_len) {
        *max_len = 0;
    }
    switch (s_input_target) {
    case UI_INPUT_SEARCH:
        if (max_len) {
            *max_len = UI_SEARCH_MAX;
        }
        return s_search;
    case UI_INPUT_IPN:
        if (max_len) {
            *max_len = UI_PART_FIELD_MAX;
        }
        return s_detail_ipn;
    case UI_INPUT_QTY:
        if (max_len) {
            *max_len = UI_PART_FIELD_MAX;
        }
        return s_detail_qty;
    case UI_INPUT_NONE:
    default:
        return NULL;
    }
}

static void open_keyboard(ui_input_target_t target)
{
    s_input_target = target;
    s_keyboard_open = target != UI_INPUT_NONE;
    if (s_keyboard_open) {
        s_keyboard_page = 0;
    }
    s_input_dirty = false;
    s_full_repaint = true;
    s_have_view_hash = false;
    s_dirty = true;
}

static void request_input_redraw(void)
{
    if (s_keyboard_open) {
        s_input_dirty = true;
    } else {
        s_dirty = true;
    }
}

static void input_backspace(void)
{
    size_t max_len = 0;
    char *buf = input_buffer(&max_len);
    if (!buf || max_len == 0) {
        return;
    }
    size_t len = strlen(buf);
    if (len > 0) {
        size_t cut = len - 1;
        while (cut > 0 && (((unsigned char)buf[cut] & 0xC0) == 0x80)) {
            cut--;
        }
        buf[cut] = '\0';
        request_input_redraw();
    }
}

static void input_append_text(const char *text)
{
    size_t max_len = 0;
    char *buf = input_buffer(&max_len);
    if (!buf || max_len == 0 || !text || !text[0]) {
        return;
    }
    if (s_input_target == UI_INPUT_QTY &&
        (text[1] != '\0' || text[0] < '0' || text[0] > '9')) {
        return;
    }
    size_t len = strlen(buf);
    size_t add_len = strlen(text);
    if (len + add_len <= max_len) {
        memcpy(buf + len, text, add_len + 1);
        request_input_redraw();
    }
}

static void enter_detail_with_object(const char *value)
{
    if (value && value[0]) {
        ui_partdb_target_t target = {0};
        if (target_from_text(value, &target)) {
            char target_code[16] = {0};
            format_target_code(&target, target_code, sizeof(target_code));
            if (target_code[0]) {
                snprintf(s_detail_ipn, sizeof(s_detail_ipn), "%s", target_code);
            }
            open_keyboard(UI_INPUT_NONE);
            set_page_internal(DEVICE_UI_PAGE_DETAIL);
            schedule_detail_target(target_code[0] ? target_code : value, &target);
            return;
        }
        if (value != s_search) {
            snprintf(s_search, sizeof(s_search), "%s", value);
        }
        if (strlen(value) <= UI_PART_FIELD_MAX) {
            snprintf(s_detail_ipn, sizeof(s_detail_ipn), "%s", value);
        }
    }
    open_keyboard(UI_INPUT_NONE);
    set_page_internal(DEVICE_UI_PAGE_DETAIL);
    if (value && value[0]) {
        schedule_detail_lookup(value);
    }
}

static bool enter_selected_result(void)
{
    if (!s_results.valid || s_results.loading || s_results.result_count <= 0) {
        return false;
    }
    clamp_results_cursor();
    if (s_results_selected < 0 || s_results_selected >= s_results.result_count) {
        return false;
    }
    const ui_search_result_item_t *item = &s_results.items[s_results_selected];
    char part_code[16];
    snprintf(part_code, sizeof(part_code), "P%04d", item->id);
    char target_label[UI_DETAIL_TEXT_MAX];
    snprintf(target_label, sizeof(target_label), "%.79s",
             item->name[0] ? item->name : part_code);
    snprintf(s_detail_ipn, sizeof(s_detail_ipn), "%.32s",
             item->ipn[0] ? item->ipn : part_code);
    open_keyboard(UI_INPUT_NONE);
    set_page_internal(DEVICE_UI_PAGE_DETAIL);
    ui_partdb_target_t direct = {
        .prefix = 'P',
        .id = item->id,
    };
    schedule_detail_target(target_label, &direct);
    return true;
}

static bool enter_parts_list_item(int index)
{
    if (!s_parts_list.valid || s_parts_list.loading || index < 0 ||
        index >= s_parts_list.item_count || index >= UI_PARTS_LIST_MAX) {
        return false;
    }
    const ui_parts_list_item_t *item = &s_parts_list.items[index];
    if (item->id <= 0) {
        return false;
    }
    char part_code[16];
    snprintf(part_code, sizeof(part_code), "P%04d", item->id);
    char target_label[UI_DETAIL_TEXT_MAX];
    snprintf(target_label, sizeof(target_label), "%.79s",
             item->name[0] ? item->name : part_code);
    snprintf(s_detail_ipn, sizeof(s_detail_ipn), "%.32s",
             item->ipn[0] ? item->ipn : part_code);
    open_keyboard(UI_INPUT_NONE);
    set_page_internal(DEVICE_UI_PAGE_DETAIL);
    ui_partdb_target_t direct = {
        .prefix = 'P',
        .id = item->id,
    };
    schedule_detail_target(target_label, &direct);
    return true;
}

static void enter_results_with_query(const char *query)
{
    if (!query || !query[0]) {
        return;
    }
    if (query != s_search) {
        snprintf(s_search, sizeof(s_search), "%s", query);
    }
    open_keyboard(UI_INPUT_NONE);
    set_page_internal(DEVICE_UI_PAGE_RESULTS);
    schedule_search_results(s_search);
}

static bool point_in(uint16_t x, uint16_t y, int rx, int ry, int rw, int rh);

static bool handle_keyboard_tap(uint16_t x, uint16_t y)
{
    if (!s_keyboard_open) {
        return false;
    }
    int screen_w = display_ili9488_get_width();
    int screen_h = display_ili9488_get_height();
    int y0 = screen_h - UI_KEYBOARD_H;
    if (y < y0) {
        open_keyboard(UI_INPUT_NONE);
        return true;
    }

    int key_w = keyboard_base_key_w(screen_w);
    uint8_t row_count = 0;
    const ui_keyboard_row_t *rows = keyboard_rows(&row_count);
    for (int r = 0; r < row_count; r++) {
        const ui_keyboard_row_t *row = &rows[r];
        int row_y = keyboard_row_y(y0, r);
        if (y < row_y || y >= row_y + UI_KEYBOARD_ROW_H) {
            continue;
        }
        int row_w = row->count * key_w + (row->count - 1) * UI_KEYBOARD_GAP;
        int row_x = (screen_w - row_w) / 2;
        if (row_x < UI_KEYBOARD_MARGIN) {
            row_x = UI_KEYBOARD_MARGIN;
        }
        for (int i = 0; i < row->count; i++) {
            int key_x = row_x + i * (key_w + UI_KEYBOARD_GAP);
            if (point_in(x, y, key_x, row_y, key_w, UI_KEYBOARD_ROW_H)) {
                input_append_text(row->keys[i].value);
                return true;
            }
        }
        return true;
    }

    int special_y = keyboard_row_y(y0, row_count);
    if (y >= special_y) {
        int special_h = screen_h - special_y - 8;
        if (special_h < UI_KEYBOARD_ROW_H) {
            special_h = UI_KEYBOARD_ROW_H;
        }
        int special_w = (screen_w - UI_KEYBOARD_MARGIN * 2 - UI_KEYBOARD_GAP * 2) / 3;
        int switch_x = UI_KEYBOARD_MARGIN;
        int del_x = switch_x + special_w + UI_KEYBOARD_GAP;
        int ok_x = del_x + special_w + UI_KEYBOARD_GAP;
        int ok_w = screen_w - UI_KEYBOARD_MARGIN - ok_x;
        if (point_in(x, y, switch_x, special_y, special_w, special_h)) {
            s_keyboard_page = s_keyboard_page == 1 ? 0 : 1;
            s_full_repaint = true;
            s_have_view_hash = false;
            s_dirty = true;
        } else if (point_in(x, y, del_x, special_y, special_w, special_h)) {
            input_backspace();
        } else if (point_in(x, y, ok_x, special_y, ok_w, special_h)) {
            ui_input_target_t target = s_input_target;
            open_keyboard(UI_INPUT_NONE);
            if (target == UI_INPUT_SEARCH && s_search[0]) {
                enter_results_with_query(s_search);
            } else if (target == UI_INPUT_IPN && s_detail_ipn[0]) {
                schedule_detail_lookup(s_detail_ipn);
            } else if (target == UI_INPUT_QTY) {
                clear_stock_result();
                s_dirty = true;
            }
        }
        return true;
    }
    return true;
}

static bool point_in(uint16_t x, uint16_t y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void set_page_from_nav(uint16_t x)
{
    int w = display_ili9488_get_width();
    int idx = (int)(((uint32_t)x * UI_MAIN_PAGE_COUNT) / w);
    if (idx < 0) {
        idx = 0;
    }
    if (idx >= UI_MAIN_PAGE_COUNT) {
        idx = UI_MAIN_PAGE_COUNT - 1;
    }
    open_keyboard(UI_INPUT_NONE);
    release_nfc_result_hold(false);
    s_nfc_confirm_open = false;
    s_nfc_confirm_action = UI_NFC_ACTION_NONE;
    close_nfc_read_prompt();
    s_search[0] = '\0';
    set_page_internal(s_main_pages[idx]);
}

static bool handle_force_nav_tap(uint16_t x, uint16_t y)
{
    int h = display_ili9488_get_height();
    bool main_page = main_page_index((device_ui_page_t)s_status.page) >= 0;
    if (!main_page && y < 51) {
        if (point_in(x, y, 10, 9, 58, 28)) {
            go_back_page();
            return true;
        }
        if (point_in(x, y, 78, 9, 58, 28)) {
            go_home_page();
            return true;
        }
    }
    if (main_page && y >= h - UI_NAV_H) {
        set_page_from_nav(x);
        return true;
    }
    return false;
}

static bool handle_nfc_confirm_tap(uint16_t x, uint16_t y)
{
    if (!s_nfc_confirm_open) {
        return false;
    }
    int screen_w = display_ili9488_get_width();
    int screen_h = display_ili9488_get_height();
    int mw = screen_w - 48;
    if (mw > 360) {
        mw = 360;
    }
    int mh = 178;
    int mx = (screen_w - mw) / 2;
    int my = (screen_h - mh) / 2;
    int by = my + mh - 54;
    int bw = (mw - 48) / 2;
    int bx1 = mx + 18;
    int bx2 = bx1 + bw + 12;
    int hit_y = by - 10;
    int hit_h = 56;

    if (point_in(x, y, bx1 - 6, hit_y, bw + 12, hit_h)) {
        if (s_nfc_write.busy) {
            s_dirty = true;
            return true;
        }
        clear_nfc_write_result();
        return true;
    }
    if (point_in(x, y, bx2 - 6, hit_y, bw + 12, hit_h)) {
        if (s_nfc_write.busy) {
            s_dirty = true;
            return true;
        }
        if (s_nfc_write.done) {
            clear_nfc_write_result();
            return true;
        }
        ui_nfc_action_t action = s_nfc_confirm_action;
        s_nfc_confirm_action = UI_NFC_ACTION_NONE;
        if (action == UI_NFC_ACTION_ERASE) {
            schedule_nfc_erase();
        } else {
            schedule_nfc_write();
        }
        return true;
    }
    s_dirty = true;
    return true;
}

static bool handle_nfc_read_prompt_tap(uint16_t x, uint16_t y)
{
    if (!s_nfc_read_prompt.open) {
        return false;
    }
    int screen_w = display_ili9488_get_width();
    int screen_h = display_ili9488_get_height();
    int mw = screen_w - 48;
    if (mw > 360) {
        mw = 360;
    }
    int mh = 178;
    int mx = (screen_w - mw) / 2;
    int my = (screen_h - mh) / 2;
    int by = my + mh - 54;
    int bw = (mw - 48) / 2;
    int bx1 = mx + 18;
    int bx2 = bx1 + bw + 12;
    int hit_y = by - 10;
    int hit_h = 56;

    if (point_in(x, y, bx1 - 6, hit_y, bw + 12, hit_h)) {
        close_nfc_read_prompt();
        return true;
    }
    if (point_in(x, y, bx2 - 6, hit_y, bw + 12, hit_h)) {
        confirm_nfc_read_prompt();
        return true;
    }
    s_dirty = true;
    return true;
}

static bool handle_ui_tap(uint16_t x, uint16_t y)
{
    int w = display_ili9488_get_width();
    if (handle_force_nav_tap(x, y)) {
        return true;
    }
    if (handle_keyboard_tap(x, y)) {
        return true;
    }
    if (handle_nfc_confirm_tap(x, y)) {
        return true;
    }
    if (handle_nfc_read_prompt_tap(x, y)) {
        return true;
    }
    if (point_in(x, y, 12, 58, w - 24, 34)) {
        s_stock_op = UI_STOCK_OP_NONE;
        clear_stock_result();
        open_keyboard(UI_INPUT_SEARCH);
        return true;
    }

    if (s_status.page == DEVICE_UI_PAGE_CAMERA) {
        int by = camera_button_y();
        if (point_in(x, y, 12, by, w - 24, UI_CAMERA_BUTTON_H)) {
            request_camera_capture();
            return true;
        }
        s_dirty = true;
        return true;
    }

    if (s_status.page == DEVICE_UI_PAGE_HOME) {
        int screen_h = display_ili9488_get_height();
        int layer_y = 240;
        int layer_x = 12;
        int layer_w = w - 24;
        int layer_h = screen_h - UI_NAV_H - layer_y - 4;
        if (layer_h < 176) {
            layer_h = 176;
        }
        int gap = 8;
        int node_w = (layer_w - 24 - gap * 2) / 3;
        int node_h = 56;
        int node_x = layer_x + 12 + (node_w + gap) * 2;
        int node_y = layer_y + 39 + node_h + 10;
        if (point_in(x, y, node_x, node_y, node_w, node_h)) {
            request_inventory_refresh();
            return true;
        }
    }

    if (s_status.page == DEVICE_UI_PAGE_DETAIL) {
        int action_y = detail_action_y();
        int info_h = detail_info_h();
        int input_x1 = 0;
        int input_x2 = 0;
        int input_y = 0;
        int input_w = 0;
        int input_h = 0;
        detail_input_layout(&input_x1, &input_x2, &input_y, &input_w, &input_h);
        if (point_in(x, y, input_x1, input_y, input_w, input_h)) {
            open_keyboard(UI_INPUT_IPN);
            return true;
        }
        if (point_in(x, y, input_x2, input_y, input_w, input_h)) {
            open_keyboard(UI_INPUT_QTY);
            return true;
        }
        if (point_in(x, y, UI_DETAIL_FIELD_X, UI_DETAIL_INFO_Y,
                     w - UI_DETAIL_FIELD_X * 2, info_h)) {
            (void)move_detail_info_page(1);
            return true;
        }
        if (point_in(x, y, UI_DETAIL_FIELD_X, action_y,
                     w - UI_DETAIL_FIELD_X * 2, UI_DETAIL_ACTION_H)) {
            int gap = 6;
            int card_w = w - UI_DETAIL_FIELD_X * 2;
            int button_w = (card_w - 30 - gap * 2) / 3;
            int button_y = action_y + UI_DETAIL_ACTION_BUTTON_Y;
            int button_h = UI_DETAIL_ACTION_BUTTON_H;
            int button_x = UI_DETAIL_FIELD_X + 15;
            if (point_in(x, y, button_x, button_y, button_w, button_h)) {
                if (!s_stock.busy) {
                    s_stock_op = UI_STOCK_OP_IN;
                    clear_stock_result();
                    schedule_stock_apply();
                } else {
                    s_dirty = true;
                }
                return true;
            }
            button_x += button_w + gap;
            if (point_in(x, y, button_x, button_y, button_w, button_h)) {
                if (!s_stock.busy) {
                    s_stock_op = UI_STOCK_OP_OUT;
                    clear_stock_result();
                    schedule_stock_apply();
                } else {
                    s_dirty = true;
                }
                return true;
            }
            button_x += button_w + gap;
            if (point_in(x, y, button_x, button_y, button_w, button_h)) {
                begin_nfc_write_from_detail();
                return true;
            }
            s_dirty = true;
            return true;
        }
    }

    if (s_status.page == DEVICE_UI_PAGE_RESULTS &&
        s_results.valid && !s_results.loading && s_results.result_count > 0) {
        int visible = s_results.result_count;
        if (visible > UI_SEARCH_RESULT_VISIBLE) {
            visible = UI_SEARCH_RESULT_VISIBLE;
        }
        for (int i = 0; i < visible; i++) {
            if (point_in(x, y, 12, UI_RESULTS_Y + i * UI_RESULTS_ROW_H,
                         w - 24, UI_RESULTS_ITEM_H)) {
                s_results_selected = s_results_scroll + i;
                (void)enter_selected_result();
                return true;
            }
        }
    }

    if (s_status.page == DEVICE_UI_PAGE_INFO &&
        s_parts_list.valid && !s_parts_list.loading && s_parts_list.item_count > 0) {
        for (int i = 0; i < s_parts_list.item_count && i < UI_PARTS_LIST_MAX; i++) {
            if (point_in(x, y, 12, 138 + i * 56, w - 24, 54)) {
                (void)enter_parts_list_item(i);
                return true;
            }
        }
    }

    if (s_status.page == DEVICE_UI_PAGE_SETTINGS) {
        ui_settings_layout_t layout = settings_layout();
        if (point_in(x, y, layout.x1, layout.y1, layout.tile_w, layout.h1)) {
            cycle_brightness();
            return true;
        }
        if (point_in(x, y, layout.x2, layout.y1, layout.tile_w, layout.h1)) {
            schedule_settings_action(UI_SETTINGS_ACTION_AP_TOGGLE);
            return true;
        }
        if (point_in(x, y, layout.x1, layout.y2, layout.tile_w, layout.h2)) {
            return true;
        }
        if (point_in(x, y, layout.x2, layout.y2, layout.tile_w, layout.h2)) {
            schedule_settings_action(UI_SETTINGS_ACTION_SCREEN_BG_NEXT);
            return true;
        }
        if (point_in(x, y, layout.x1, layout.y3, layout.tile_w, layout.h3)) {
            schedule_settings_action(UI_SETTINGS_ACTION_LOCK_BG_NEXT);
            return true;
        }
        if (point_in(x, y, layout.x2, layout.y3, layout.tile_w, layout.h3)) {
            set_page_internal(DEVICE_UI_PAGE_SYSTEM);
            return true;
        }
        if (point_in(x, y, layout.x1, layout.y4, layout.tile_w, layout.h4)) {
            schedule_settings_action(UI_SETTINGS_ACTION_SLEEP_TIMEOUT_NEXT);
            return true;
        }
        if (point_in(x, y, layout.x2, layout.y4, layout.tile_w, layout.h4)) {
            set_page_internal(DEVICE_UI_PAGE_NFC_SETTINGS);
            return true;
        }
    }

    if (s_status.page == DEVICE_UI_PAGE_NFC_SETTINGS) {
        int gap = 8;
        int tile_w = (w - 24 - gap) / 2;
        int x1 = 12;
        int x2 = x1 + tile_w + gap;
        if (point_in(x, y, x1, 116, tile_w, 74)) {
            schedule_settings_action(UI_SETTINGS_ACTION_NFC_READ_MODE);
            return true;
        }
        if (point_in(x, y, x1, 200, tile_w, 74)) {
            begin_nfc_erase_from_settings();
            return true;
        }
        if (point_in(x, y, x2, 200, tile_w, 74)) {
            schedule_settings_action(UI_SETTINGS_ACTION_NFC_RESTART);
            return true;
        }
    }

    if (s_status.page == DEVICE_UI_PAGE_SHORTCUTS) {
        int gap = 8;
        int tile_w = (w - 24 - gap) / 2;
        int x1 = 12;
        int x2 = x1 + tile_w + gap;
        if (point_in(x, y, x1, 116, tile_w, 74)) {
            s_stock_op = UI_STOCK_OP_IN;
            clear_stock_result();
            s_dirty = true;
            return true;
        }
        if (point_in(x, y, x2, 116, tile_w, 74)) {
            s_stock_op = UI_STOCK_OP_OUT;
            clear_stock_result();
            s_dirty = true;
            return true;
        }
        if (point_in(x, y, x1, 200, tile_w, 74)) {
            begin_camera_preview();
            return true;
        }
        if (point_in(x, y, x2, 200, tile_w, 74)) {
            nfc_service_status_t nfc = nfc_service_get_status();
            ui_partdb_target_t target = {0};
            char query[32] = {0};
            if (nfc.tag_present && nfc.text[0] &&
                nfc_fast_route_from_text(nfc.text, &target, query, sizeof(query))) {
                if (current_detail_matches_nfc_query(query)) {
                    s_dirty = true;
                } else if (s_cfg && s_cfg->nfc_read_confirm) {
                    open_nfc_route_prompt(&nfc, &target, query);
                } else {
                    enter_detail_with_object(query);
                }
            } else {
                s_dirty = true;
            }
            return true;
        }
        if (point_in(x, y, x1, 284, tile_w, 74)) {
            open_keyboard(UI_INPUT_SEARCH);
            return true;
        }
    }
    return false;
}

static void handle_sleep_touch_tap(uint16_t x, uint16_t y)
{
    int64_t now = ui_time_ms();
    int dx = (int)x - (int)s_sleep_touch_tap_x;
    int dy = (int)y - (int)s_sleep_touch_tap_y;
    bool near_last = s_sleep_touch_tap_ms != 0 &&
                     abs(dx) <= UI_TOUCH_SWIPE_PX &&
                     abs(dy) <= UI_TOUCH_SWIPE_PX;
    if (s_sleep_touch_tap_ms != 0 &&
        now - s_sleep_touch_tap_ms <= UI_SLEEP_DOUBLE_TAP_MS &&
        near_last) {
        s_sleep_touch_tap_ms = 0;
        wake_display_for_activity("touch_double");
        record_touch_event("sleep_double_tap_wake", x, y);
        return;
    }
    s_sleep_touch_tap_ms = now;
    s_sleep_touch_tap_x = x;
    s_sleep_touch_tap_y = y;
    update_touch_point("sleep_tap", x, y);
}

static void finish_touch_gesture(void)
{
    if (!s_touch.active || s_touch.action_sent) {
        return;
    }
    if (s_touch.samples < UI_TOUCH_MIN_SAMPLES) {
        update_touch_point("touch_ignored", s_touch.last_x, s_touch.last_y);
        return;
    }

    int dx = (int)s_touch.last_x - (int)s_touch.start_x;
    int dy = (int)s_touch.last_y - (int)s_touch.start_y;
    int ax = abs(dx);
    int ay = abs(dy);

    if (s_screen_sleeping || !display_ili9488_is_awake()) {
        if (ax <= UI_TOUCH_TAP_PX && ay <= UI_TOUCH_TAP_PX) {
            handle_sleep_touch_tap(s_touch.last_x, s_touch.last_y);
        } else {
            update_touch_point("sleep_touch_ignored", s_touch.last_x, s_touch.last_y);
        }
        s_touch.action_sent = true;
        return;
    }

    if (s_keyboard_open) {
        (void)handle_keyboard_tap(s_touch.last_x, s_touch.last_y);
        record_touch_event("keyboard_tap", s_touch.last_x, s_touch.last_y);
        s_touch.action_sent = true;
        return;
    }

    if (ax >= UI_TOUCH_SWIPE_PX || ay >= UI_TOUCH_SWIPE_PX) {
        if (s_nfc_confirm_open) {
            update_touch_point("modal_swipe_ignored", s_touch.last_x, s_touch.last_y);
            s_touch.action_sent = true;
            return;
        }
        if (s_status.page == DEVICE_UI_PAGE_RESULTS) {
            if (ay >= ax) {
                (void)move_results_cursor(dy < 0 ? 1 : -1);
                record_touch_event(dy < 0 ? "results_next" : "results_prev",
                                   s_touch.last_x, s_touch.last_y);
            } else {
                update_touch_point("results_swipe_ignored", s_touch.last_x, s_touch.last_y);
            }
            s_touch.action_sent = true;
            return;
        }
        if (s_status.page == DEVICE_UI_PAGE_DETAIL) {
            if (ay >= ax) {
                (void)move_detail_info_page(dy < 0 ? 1 : -1);
                record_touch_event(dy < 0 ? "detail_info_next" : "detail_info_prev",
                                   s_touch.last_x, s_touch.last_y);
            } else {
                update_touch_point("detail_swipe_ignored", s_touch.last_x, s_touch.last_y);
            }
            s_touch.action_sent = true;
            return;
        }
        if (s_status.page == DEVICE_UI_PAGE_INFO && ay >= ax) {
            (void)move_parts_page(dy < 0 ? 1 : -1);
            record_touch_event(dy < 0 ? "parts_next" : "parts_prev",
                               s_touch.last_x, s_touch.last_y);
            s_touch.action_sent = true;
            return;
        }
        if (ax >= ay) {
            apply_touch_page_action(dx < 0 ? "swipe_left" : "swipe_right",
                                    dx > 0, s_touch.last_x, s_touch.last_y);
        } else {
            apply_touch_page_action(dy < 0 ? "swipe_up" : "swipe_down",
                                    dy > 0, s_touch.last_x, s_touch.last_y);
        }
        s_touch.action_sent = true;
        return;
    }

    if (ax <= UI_TOUCH_TAP_PX && ay <= UI_TOUCH_TAP_PX) {
        if (handle_ui_tap(s_touch.last_x, s_touch.last_y)) {
            record_touch_event("tap_control", s_touch.last_x, s_touch.last_y);
            s_touch.action_sent = true;
            return;
        }
        update_touch_point("tap_empty", s_touch.last_x, s_touch.last_y);
        return;
    }
    update_touch_point("touch_ignored", s_touch.last_x, s_touch.last_y);
}

static void handle_touch_event(TickType_t now)
{
    if (now - s_last_touch_poll < pdMS_TO_TICKS(UI_TOUCH_POLL_MS)) {
        return;
    }
    s_last_touch_poll = now;

    if (!touch_ft6336_is_ready()) {
        if (now >= s_next_touch_retry) {
            esp_err_t err = touch_ft6336_init();
            s_next_touch_retry = now + pdMS_TO_TICKS(UI_TOUCH_RETRY_MS);
            if (err != ESP_OK) {
                return;
            }
        } else {
            return;
        }
    }

    touch_point_t point = {0};
    esp_err_t err = touch_ft6336_read(&point);
    if (err != ESP_OK) {
        s_touch.active = false;
        s_touch.action_sent = false;
        return;
    }
    uint16_t raw_x = point.x;
    uint16_t raw_y = point.y;
    transform_touch_point(&point);

    if (point.touched) {
        s_status.last_touch_raw_x = raw_x;
        s_status.last_touch_raw_y = raw_y;
        update_touch_range(raw_x, raw_y, point.x, point.y);
        if (!s_touch.active) {
            update_touch_point("touch_down", point.x, point.y);
            s_touch.active = true;
            s_touch.action_sent = false;
            s_touch.start_x = point.x;
            s_touch.start_y = point.y;
            s_touch.samples = 1;
            s_touch.started_at = now;
            if (s_screen_sleeping || !display_ili9488_is_awake()) {
                handle_sleep_touch_tap(point.x, point.y);
                s_touch.action_sent = true;
                return;
            }
            if (s_keyboard_open && point.y >= display_ili9488_get_height() - UI_KEYBOARD_H) {
                if (handle_keyboard_tap(point.x, point.y)) {
                    record_touch_event("keyboard_down", point.x, point.y);
                    s_touch.action_sent = true;
                }
            }
        } else {
            update_touch_point("touch_move", point.x, point.y);
            if (s_touch.samples < UINT8_MAX) {
                s_touch.samples++;
            }
        }
        s_touch.last_x = point.x;
        s_touch.last_y = point.y;
        return;
    }

    if (s_touch.active) {
        finish_touch_gesture();
        s_touch.active = false;
        s_touch.action_sent = false;
    }
}

static void ui_task(void *arg)
{
    (void)arg;
#if UI_STATUS_POLL_MS > 0
    TickType_t last_status_poll = 0;
#endif
    while (true) {
        if (!display_ili9488_is_ready()) {
            (void)display_ili9488_init();
        }
        s_status.awake = !s_screen_sleeping && display_ili9488_is_awake();
        handle_button_event();
        TickType_t now = xTaskGetTickCount();
        handle_touch_event(now);
        handle_nfc_confirm_tag_lock();
        handle_nfc_detail_entry();
        handle_camera_result_entry();
        handle_auto_sleep();
        uint32_t time_bucket = header_time_bucket();
        if (time_bucket != s_header_time_bucket) {
            s_header_time_bucket = time_bucket;
            redraw_header_time_only();
        }
        bool should_check_hash = !s_have_view_hash;
#if UI_STATUS_POLL_MS > 0
        should_check_hash = should_check_hash ||
                            (!s_keyboard_open && !s_input_dirty &&
                             now - last_status_poll >= pdMS_TO_TICKS(UI_STATUS_POLL_MS));
#endif
        if (should_check_hash) {
            uint32_t hash = current_view_hash();
            if (!s_have_view_hash || hash != s_last_view_hash) {
                s_last_view_hash = hash;
                s_have_view_hash = true;
                s_dirty = true;
            }
#if UI_STATUS_POLL_MS > 0
            last_status_poll = now;
#endif
        }
        if (s_dirty) {
            s_dirty = false;
            s_input_dirty = false;
            redraw();
            s_last_view_hash = current_view_hash();
            s_have_view_hash = true;
        } else if (s_input_dirty) {
            s_input_dirty = false;
            redraw_active_input();
            s_last_view_hash = current_view_hash();
            s_have_view_hash = true;
        }
        vTaskDelay(pdMS_TO_TICKS(UI_POLL_MS));
    }
}

esp_err_t device_ui_start(app_config_t *cfg)
{
    if (s_status.started) {
        return ESP_OK;
    }
    s_cfg = cfg;
    if (s_cfg) {
        ui_font_set_active_path(s_cfg->font_path);
        (void)display_ili9488_configure(s_cfg->display_driver, s_cfg->display_width,
                                        s_cfg->display_height, s_cfg->display_orientation,
                                        s_cfg->display_flip);
    }
    s_status.started = true;
    s_status.page_name = device_ui_page_name((device_ui_page_t)s_status.page);
    s_seen_nfc_read_count = nfc_service_get_status().read_count;
    mark_ui_activity();
    BaseType_t ok = xTaskCreate(ui_task, "device_ui", 6144, NULL, 3, &s_task);
    if (ok != pdPASS) {
        s_task = NULL;
        s_status.started = false;
        return ESP_ERR_NO_MEM;
    }
    if (!s_inventory_task) {
        (void)xTaskCreate(inventory_task, "pdb_stats", 6144, NULL, 2, &s_inventory_task);
    }
    ESP_LOGI(TAG, "device UI started");
    return ESP_OK;
}

device_ui_status_t device_ui_get_status(void)
{
    s_status.awake = !s_screen_sleeping && display_ili9488_is_awake();
    s_status.page_name = device_ui_page_name((device_ui_page_t)s_status.page);
    s_status.nfc_busy = s_nfc_write.busy;
    s_status.nfc_done = s_nfc_write.done;
    s_status.nfc_ok = s_nfc_write.ok;
    s_status.nfc_action = (uint8_t)s_nfc_write.action;
    s_status.nfc_last_err = s_nfc_write.last_err;
    s_status.nfc_write_count = s_nfc_write.write_count;
    s_status.nfc_payload = s_nfc_write.payload;
    s_status.nfc_message = s_nfc_write.message;
    s_status.screen_sleep_minutes = s_cfg ? normalize_sleep_minutes(s_cfg->screen_sleep_minutes) : 5;
    int64_t idle_ms = ui_time_ms() - s_last_activity_ms;
    if (idle_ms < 0) {
        idle_ms = 0;
    }
    if (idle_ms > UINT32_MAX) {
        idle_ms = UINT32_MAX;
    }
    s_status.idle_ms = (uint32_t)idle_ms;
    return s_status;
}
