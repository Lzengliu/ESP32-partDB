#include "device_ui.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

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

#define UI_STATUS_POLL_MS  0
#define UI_POLL_MS         25
#define UI_TOUCH_POLL_MS   25
#define UI_TOUCH_RETRY_MS  3000
#define UI_TOUCH_SWIPE_PX  60
#define UI_TOUCH_TAP_PX    40
#define UI_TOUCH_MIN_SAMPLES 1
#define UI_SEARCH_MAX      32
#define UI_PART_FIELD_MAX  32
#define UI_DETAIL_TEXT_MAX 80
#define UI_PARTDB_BODY_MAX 12288
#define UI_SEARCH_RESULT_MAX 8
#define UI_SEARCH_RESULT_VISIBLE 4
#define UI_RESULTS_Y       112
#define UI_RESULTS_ROW_H   77
#define UI_RESULTS_ITEM_H  74
#define UI_KEYBOARD_H      240
#define UI_KEYBOARD_MARGIN 6
#define UI_KEYBOARD_GAP    2
#define UI_KEYBOARD_ROW_H  24
#define UI_KEYBOARD_TOP    6
#define UI_NAV_H           58
#define UI_TOP_H           102
#define UI_DETAIL_FIELD_X  12
#define UI_DETAIL_FIELD1_Y 112
#define UI_DETAIL_FIELD2_Y 150
#define UI_DETAIL_FIELD_H  34
#define UI_DETAIL_INFO_Y   190
#define UI_DETAIL_GAP      8
#define UI_DETAIL_ACTION_H 64
#define UI_DETAIL_ACTION_BUTTON_Y 30
#define UI_DETAIL_ACTION_BUTTON_H 29
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

static app_config_t *s_cfg;
static TaskHandle_t s_task;
static TaskHandle_t s_inventory_task;
static bool s_dirty = true;
static bool s_input_dirty;
static bool s_full_repaint = true;
static bool s_have_view_hash;
static uint32_t s_last_view_hash;
static device_ui_status_t s_status = {
    .page = DEVICE_UI_PAGE_HOME,
    .page_name = "HOME",
    .last_button_event = "none",
    .last_touch_event = "none",
};

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

static char s_search[UI_SEARCH_MAX + 1];
static char s_detail_ipn[UI_PART_FIELD_MAX + 1] = "P0001";
static char s_detail_qty[UI_PART_FIELD_MAX + 1] = "1";
static bool s_keyboard_open;
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
static int s_results_scroll;
static int s_results_selected;
static int s_detail_info_page;

typedef enum {
    UI_STOCK_OP_NONE = 0,
    UI_STOCK_OP_IN,
    UI_STOCK_OP_OUT,
} ui_stock_op_t;

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
    esp_err_t last_err;
    uint32_t write_count;
    uint32_t updated_ms;
    char payload[24];
    char message[UI_DETAIL_TEXT_MAX];
} ui_nfc_write_state_t;

static ui_nfc_write_state_t s_nfc_write = {
    .last_err = ESP_ERR_INVALID_STATE,
};
static TaskHandle_t s_nfc_write_task;
static bool s_nfc_confirm_open;

typedef enum {
    UI_SETTINGS_ACTION_NONE = 0,
    UI_SETTINGS_ACTION_AP_TOGGLE,
    UI_SETTINGS_ACTION_FONT_NEXT,
    UI_SETTINGS_ACTION_SCREEN_BG_NEXT,
    UI_SETTINGS_ACTION_LOCK_BG_NEXT,
    UI_SETTINGS_ACTION_BOOT_ANIM_NEXT,
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

static void enter_detail_with_object(const char *value);
static void enter_results_with_query(const char *query);
static bool enter_selected_result(void);
static void clear_stock_result(void);
static void clear_nfc_write_result(void);
static void begin_nfc_write_from_detail(void);
static void detail_lookup_task(void *arg);

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
        return "INFO";
    case DEVICE_UI_PAGE_SETTINGS:
        return "SETTINGS";
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
    case DEVICE_UI_PAGE_HOME:
    default:
        return UI_CYAN;
    }
}

static void buffer_search_field(uint16_t *buf, int bw, int bh, int sx, int sy,
                                int sw, int sh, uint16_t accent)
{
    buffer_fill_rect(buf, bw, bh, sx, sy, sw, sh, UI_BG);
    buffer_fill_rect(buf, bw, bh, sx, sy, sw, 1, accent);
    buffer_fill_rect(buf, bw, bh, sx, sy + sh - 1, sw, 1, UI_LINE);
    buffer_fill_rect(buf, bw, bh, sx, sy, 1, sh, UI_LINE);
    buffer_fill_rect(buf, bw, bh, sx + sw - 1, sy, 1, sh, UI_LINE);
    char search_line[64];
    if (s_search[0]) {
        snprintf(search_line, sizeof(search_line), "搜索 %s", s_search);
        buffer_text(buf, bw, bh, sx + 10, sy + 9, search_line, 1, UI_TEXT);
    } else {
        buffer_text(buf, bw, bh, sx + 10, sy + 9, "模糊搜索元件/IPN/条码", 1, UI_MUTED);
    }
}

static void draw_header(const char *title, uint16_t accent)
{
    int w = display_ili9488_get_width();
    int h = UI_TOP_H;
    uint16_t *buf = heap_caps_malloc((size_t)w * h * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (!buf) {
        return;
    }
    buffer_fill(buf, w, h, UI_PANEL_DARK);
    buffer_fill_rect(buf, w, h, 0, 0, w, 3, accent);
    buffer_fill_rect(buf, w, h, 0, 50, w, 1, UI_LINE);
    buffer_fill_rect(buf, w, h, 0, h - 1, w, 1, UI_LINE);

    const char *name = (s_cfg && s_cfg->device_name[0]) ? s_cfg->device_name : "Part-DB Terminal";
    buffer_text(buf, w, h, 12, 13, name, 1, UI_TEXT);
    if (title && title[0] && w > 180) {
        buffer_text(buf, w, h, w - 112, 13, title, 1, UI_MUTED);
    }

    int sx = 12;
    int sy = 58;
    int sw = w - 24;
    int sh = 34;
    buffer_search_field(buf, w, h, sx, sy, sw, sh, accent);
    (void)display_ili9488_draw_bitmap565(0, 0, w, h, buf);
    heap_caps_free(buf);
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
    uint16_t *buf = heap_caps_malloc((size_t)w * h * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (!buf) {
        return;
    }
    buffer_fill(buf, w, h, UI_PANEL_DARK);
    buffer_fill_rect(buf, w, h, 0, 0, w, 1, UI_LINE);
    const char *labels[] = {"主页", "快捷", "信息", "设置"};
    int active_idx = main_page_index((device_ui_page_t)s_status.page);
    int bw = w / UI_MAIN_PAGE_COUNT;
    for (int i = 0; i < UI_MAIN_PAGE_COUNT; i++) {
        int x = i * bw;
        int ww = (i == UI_MAIN_PAGE_COUNT - 1) ? (w - x) : bw;
        bool active = active_idx == i;
        buffer_fill_rect(buf, w, h, x + 5, 8, ww - 10, h - 16, active ? UI_BLUE : UI_PANEL);
        buffer_fill_rect(buf, w, h, x + 5, 8, ww - 10, 1, active ? UI_CYAN : UI_LINE);
        buffer_text(buf, w, h, x + 23, 22, labels[i], 1, active ? UI_TEXT : UI_MUTED);
    }
    (void)display_ili9488_draw_bitmap565(0, screen_h - h, w, h, buf);
    heap_caps_free(buf);
}

static void draw_card(int y, const char *title, const char *line1, const char *line2, uint16_t accent)
{
    int w = display_ili9488_get_width();
    int x = 12;
    int cw = w - 24;
    int ch = 76;
    uint16_t *buf = heap_caps_malloc((size_t)cw * ch * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (!buf) {
        return;
    }
    buffer_fill(buf, cw, ch, UI_PANEL);
    buffer_fill_rect(buf, cw, ch, 0, 0, cw, 1, UI_LINE);
    buffer_fill_rect(buf, cw, ch, 0, ch - 1, cw, 1, UI_BG);
    buffer_fill_rect(buf, cw, ch, 0, 0, 5, ch, accent);
    buffer_text(buf, cw, ch, 15, 9, title, 1, UI_MUTED);
    buffer_text(buf, cw, ch, 15, 31, line1, 1, UI_TEXT);
    if (line2 && line2[0]) {
        buffer_text(buf, cw, ch, 15, 54, line2, 1, UI_MUTED);
    }
    (void)display_ili9488_draw_bitmap565(x, y, cw, ch, buf);
    heap_caps_free(buf);
}

static void draw_tile(int x, int y, int cw, int ch, const char *title,
                      const char *value, const char *sub, uint16_t accent)
{
    if (cw <= 0 || ch <= 0) {
        return;
    }
    uint16_t *buf = heap_caps_malloc((size_t)cw * ch * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (!buf) {
        return;
    }
    buffer_fill(buf, cw, ch, UI_PANEL);
    buffer_fill_rect(buf, cw, ch, 0, 0, cw, 1, UI_LINE);
    buffer_fill_rect(buf, cw, ch, 0, 0, 4, ch, accent);
    buffer_fill_rect(buf, cw, ch, 4, ch - 1, cw - 4, 1, UI_BG);
    buffer_text(buf, cw, ch, 12, 10, title, 1, UI_MUTED);
    buffer_text(buf, cw, ch, 12, 34, value, 1, UI_TEXT);
    if (sub && sub[0]) {
        buffer_text(buf, cw, ch, 12, 59, sub, 1, UI_MUTED);
    }
    (void)display_ili9488_draw_bitmap565(x, y, cw, ch, buf);
    heap_caps_free(buf);
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

typedef struct {
    char prefix;
    int id;
} ui_partdb_target_t;

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
        if (value <= 0 || value > 999999) {
            return false;
        }
        s++;
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
        parse_id_after_marker(text, "/parts/", &id)) {
        target->prefix = 'P';
        target->id = id;
        return true;
    }
    if (parse_id_after_marker(text, "/scan/lot/", &id) ||
        parse_id_after_marker(text, "/api/part_lots/", &id) ||
        parse_id_after_marker(text, "/part_lots/", &id)) {
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
    return false;
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
    char path[640];
    int written = 0;
    if (target->prefix == 'L') {
        written = snprintf(path, sizeof(path),
                           "/api/part_lots/%d.jsonld?properties[]=id&"
                           "properties[]=description&properties[]=comment&properties[]=user_barcode&"
                           "properties[]=amount&properties[]=instock_unknown&"
                           "properties[part][]=id&properties[part][]=name&properties[part][]=ipn&"
                           "properties[part][]=description&properties[part][]=comment&"
                           "properties[part][]=manufacturer_product_number&properties[part][category][]=name&"
                           "properties[part][manufacturer][]=name&properties[part][footprint][]=name&"
                           "properties[storage_location][]=id&properties[storage_location][]=name",
                           target->id);
    } else {
        written = snprintf(path, sizeof(path),
                           "/api/parts/%d.jsonld?properties[]=id&properties[]=name&"
                           "properties[]=ipn&properties[]=description&properties[]=comment&"
                           "properties[]=manufacturer_product_number&properties[]=total_instock&"
                           "properties[category][]=name&properties[manufacturer][]=name&"
                           "properties[footprint][]=name&properties[partLots][]=id&"
                           "properties[partLots][]=amount&properties[partLots][]=instock_unknown&"
                           "properties[partLots][]=user_barcode&"
                           "properties[partLots][storage_location][]=name",
                           target->id);
    }
    if (written < 0 || written >= (int)sizeof(path)) {
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
    heap_caps_free(body);
    return err;
}

static void start_detail_lookup_task(void)
{
    s_dirty = true;
    BaseType_t ok = xTaskCreate(detail_lookup_task, "ui_partdb", 6144, NULL, 2, &s_detail_task);
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
    ui_partdb_detail_t next = s_detail;
    next.loading = true;
    next.valid = false;
    next.found = false;
    next.last_err = ESP_ERR_INVALID_STATE;

    int lookup_http = 0;
    ui_partdb_target_t target = {0};
    bool direct = s_detail_target_direct;
    if (direct) {
        target = s_detail_direct_target;
        lookup_http = 0;
        s_detail_target_direct = false;
    }
    esp_err_t err = direct ? ESP_OK : lookup_target_by_query(next.query, &target, &lookup_http);
    next.http_status = lookup_http;
    if (err == ESP_OK) {
        err = fetch_detail_for_target(&target, &next);
    }
    next.loading = false;
    next.last_err = err;
    next.fetch_count++;
    next.updated_ms = (uint32_t)(esp_timer_get_time() / 1000);
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
        vTaskDelete(NULL);
        return;
    }
    s_detail = next;
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
        char patch_body[96];
        snprintf(path, sizeof(path), "/api/part_lots/%d.jsonld", lot_id);
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
    s_nfc_confirm_open = false;
    if (s_nfc_write.busy) {
        return;
    }
    s_nfc_write.done = false;
    s_nfc_write.ok = false;
    s_nfc_write.last_err = ESP_ERR_INVALID_STATE;
    s_nfc_write.payload[0] = '\0';
    s_nfc_write.message[0] = '\0';
}

static bool detail_nfc_payload(const ui_partdb_detail_t *detail, char *out, size_t out_len)
{
    if (!detail || !out || out_len == 0 || !detail->found) {
        return false;
    }
    out[0] = '\0';
    int lot_id = active_detail_lot_id(detail);
    if (detail->is_lot && lot_id > 0) {
        snprintf(out, out_len, "L%04d", lot_id);
        return true;
    }
    int part_id = detail->part_id > 0 ? detail->part_id : detail->id;
    if (part_id > 0) {
        snprintf(out, out_len, "P%04d", part_id);
        return true;
    }
    return false;
}

static void nfc_write_task(void *arg)
{
    (void)arg;
    ui_nfc_write_state_t next = s_nfc_write;
    next.busy = true;
    next.done = false;
    next.ok = false;
    next.last_err = ESP_ERR_INVALID_STATE;
    next.message[0] = '\0';

    char payload[sizeof(next.payload)] = {0};
    esp_err_t err = detail_nfc_payload(&s_detail, payload, sizeof(payload)) ?
                    ESP_OK : ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) {
        snprintf(next.message, sizeof(next.message), "没有可写入对象");
    } else {
        snprintf(next.payload, sizeof(next.payload), "%s", payload);
        snprintf(next.message, sizeof(next.message), "请贴近 NFC 卡");
        nfc_service_suspend_for_request();
        err = nfc_pn532_write_ndef_text(payload, 1500);
        nfc_service_resume_after_request();
    }

    next.busy = false;
    next.done = true;
    next.ok = err == ESP_OK;
    next.last_err = err;
    next.write_count++;
    next.updated_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (err == ESP_OK) {
        snprintf(next.message, sizeof(next.message), "已写入 %s", next.payload);
    } else if (next.message[0] == '\0' || strcmp(next.message, "请贴近 NFC 卡") == 0) {
        snprintf(next.message, sizeof(next.message), "NFC写入 %s", esp_err_to_name(err));
    }
    s_nfc_write = next;
    s_nfc_write_task = NULL;
    s_dirty = true;
    vTaskDelete(NULL);
}

static void schedule_nfc_write(void)
{
    if (s_nfc_write_task || s_nfc_write.busy) {
        s_dirty = true;
        return;
    }
    char payload[sizeof(s_nfc_write.payload)] = {0};
    if (!detail_nfc_payload(&s_detail, payload, sizeof(payload))) {
        s_nfc_write.busy = false;
        s_nfc_write.done = true;
        s_nfc_write.ok = false;
        s_nfc_write.last_err = ESP_ERR_INVALID_STATE;
        snprintf(s_nfc_write.message, sizeof(s_nfc_write.message), "没有可写入对象");
        s_dirty = true;
        return;
    }
    if (!nfc_pn532_is_ready()) {
        s_nfc_confirm_open = false;
        s_nfc_write.busy = false;
        s_nfc_write.done = true;
        s_nfc_write.ok = false;
        s_nfc_write.last_err = ESP_ERR_INVALID_STATE;
        snprintf(s_nfc_write.payload, sizeof(s_nfc_write.payload), "%s", payload);
        snprintf(s_nfc_write.message, sizeof(s_nfc_write.message), "NFC离线，不能写入");
        s_dirty = true;
        return;
    }
    s_nfc_write.busy = true;
    s_nfc_write.done = false;
    s_nfc_write.ok = false;
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

static void begin_nfc_write_from_detail(void)
{
    char payload[sizeof(s_nfc_write.payload)] = {0};
    if (!detail_nfc_payload(&s_detail, payload, sizeof(payload))) {
        schedule_nfc_write();
        return;
    }
    if (!nfc_pn532_is_ready()) {
        s_nfc_confirm_open = false;
        s_nfc_write.busy = false;
        s_nfc_write.done = true;
        s_nfc_write.ok = false;
        s_nfc_write.last_err = ESP_ERR_INVALID_STATE;
        snprintf(s_nfc_write.payload, sizeof(s_nfc_write.payload), "%s", payload);
        snprintf(s_nfc_write.message, sizeof(s_nfc_write.message), "NFC离线，不能写入");
        s_dirty = true;
        return;
    }
    snprintf(s_nfc_write.payload, sizeof(s_nfc_write.payload), "%s", payload);
    s_nfc_confirm_open = true;
    s_dirty = true;
}

static void inventory_task(void *arg)
{
    (void)arg;
    const TickType_t short_delay = pdMS_TO_TICKS(10000);
    const TickType_t normal_delay = pdMS_TO_TICKS(60000);
    vTaskDelay(pdMS_TO_TICKS(3500));
    while (true) {
        partdb_client_status_t pdb = partdb_client_get_status();
        if (!pdb.configured) {
            s_inventory.valid = false;
            s_inventory.refreshing = false;
            s_inventory.last_err = ESP_ERR_INVALID_STATE;
            vTaskDelay(short_delay);
            continue;
        }

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

        next.refreshing = false;
        next.valid = err == ESP_OK;
        next.last_err = err;
        next.refresh_count++;
        next.updated_ms = (uint32_t)(esp_timer_get_time() / 1000);
        s_inventory = next;
        if (s_status.page == DEVICE_UI_PAGE_HOME) {
            s_dirty = true;
        }
        vTaskDelay(err == ESP_OK ? normal_delay : short_delay);
    }
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
    hash_u32(&hash, (uint32_t)s_nfc_write.last_err);
    hash_string(&hash, s_nfc_write.payload);
    hash_string(&hash, s_nfc_write.message);
    hash_bool(&hash, s_nfc_confirm_open);
    hash_bool(&hash, s_keyboard_open);
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
        break;
    }
    case DEVICE_UI_PAGE_SHORTCUTS: {
        nfc_service_status_t nfc = nfc_service_get_status();
        qr_scanner_status_t qr = qr_scanner_get_status();
        hash_bool(&hash, nfc.ready);
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
    case DEVICE_UI_PAGE_INFO: {
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
        hash_u32(&hash, display_ili9488_get_brightness());
        if (s_cfg) {
            hash_u32(&hash, s_cfg->display_brightness);
            hash_bool(&hash, s_cfg->ap_enabled);
            hash_string(&hash, s_cfg->font_path);
            hash_string(&hash, s_cfg->screen_bg_path);
            hash_string(&hash, s_cfg->boot_anim_path);
            hash_string(&hash, s_cfg->lock_bg_path);
        }
        break;
    }
    case DEVICE_UI_PAGE_HOME:
    default: {
        partdb_client_status_t pdb = partdb_client_get_status();
        hash_bool(&hash, pdb.configured);
        hash_bool(&hash, s_inventory.valid);
        hash_u32(&hash, s_inventory.parts);
        hash_u32(&hash, s_inventory.lots);
        hash_u32(&hash, s_inventory.locations);
        hash_u32(&hash, s_inventory.categories);
        hash_u32(&hash, (uint32_t)s_inventory.last_err);
        break;
    }
    }
    return hash;
}

static void draw_bar(uint16_t *buf, int bw, int bh, int x, int y, int w, const char *label,
                     uint32_t value, uint32_t max_value, uint16_t color)
{
    if (!buf || w <= 0) {
        return;
    }
    char text[32];
    snprintf(text, sizeof(text), "%s %lu", label, (unsigned long)value);
    buffer_text(buf, bw, bh, x, y - 1, text, 1, UI_MUTED);
    int bar_x = x + 78;
    int bar_w = w - 86;
    int fill = max_value ? (int)(((uint64_t)bar_w * value) / max_value) : 0;
    if (fill > bar_w) {
        fill = bar_w;
    }
    buffer_fill_rect(buf, bw, bh, bar_x, y + 3, bar_w, 10, UI_BG);
    buffer_fill_rect(buf, bw, bh, bar_x, y + 3, fill, 10, color);
}

static void draw_inventory_panel(int y)
{
    int w = display_ili9488_get_width();
    int x = 12;
    int cw = w - 24;
    int ch = 132;
    uint16_t *buf = heap_caps_malloc((size_t)cw * ch * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (!buf) {
        return;
    }
    partdb_client_status_t pdb = partdb_client_get_status();
    uint32_t max_value = s_inventory.parts;
    if (s_inventory.lots > max_value) {
        max_value = s_inventory.lots;
    }
    if (s_inventory.locations > max_value) {
        max_value = s_inventory.locations;
    }
    if (max_value == 0) {
        max_value = 1;
    }

    buffer_fill(buf, cw, ch, UI_PANEL);
    buffer_fill_rect(buf, cw, ch, 0, 0, 5, ch, pdb.configured ? UI_BLUE : UI_RED);
    buffer_fill_rect(buf, cw, ch, 0, 0, cw, 1, UI_LINE);
    buffer_fill_rect(buf, cw, ch, 0, ch - 1, cw, 1, UI_BG);
    buffer_text(buf, cw, ch, 15, 10, "Part-DB 库存", 1, UI_TEXT);
    char line[64];
    if (s_inventory.valid) {
        snprintf(line, sizeof(line), "HTTP %d  刷新 %lu", s_inventory.last_http_status,
                 (unsigned long)s_inventory.refresh_count);
    } else {
        snprintf(line, sizeof(line), "%s  %s", s_inventory.refreshing ? "同步中" : "待同步",
                 esp_err_to_name(s_inventory.last_err));
    }
    buffer_text(buf, cw, ch, 128, 10, line, 1, UI_MUTED);

    if (s_inventory.valid) {
        draw_bar(buf, cw, ch, 16, 40, cw - 30, "元件", s_inventory.parts, max_value, UI_CYAN);
        draw_bar(buf, cw, ch, 16, 65, cw - 30, "批次", s_inventory.lots, max_value, UI_GREEN);
        draw_bar(buf, cw, ch, 16, 90, cw - 30, "库位", s_inventory.locations, max_value, UI_ORANGE);
    } else {
        buffer_text(buf, cw, ch, 16, 44, "元件 --", 1, UI_MUTED);
        buffer_text(buf, cw, ch, 16, 69, "批次 --", 1, UI_MUTED);
        buffer_text(buf, cw, ch, 16, 94, "库位 --", 1, UI_MUTED);
    }
    (void)display_ili9488_draw_bitmap565(x, y, cw, ch, buf);
    heap_caps_free(buf);
}

static void draw_home(void)
{
    draw_header("主页", UI_CYAN);
    draw_inventory_panel(112);
    draw_card(254, "快速入口", "点击搜索框输入关键字", "扫码/NFC 成功后进入详情", UI_BLUE);
    draw_card(340, "库存操作", "快捷页保留入库和出库", "详情页用于三级编辑", UI_GREEN);
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
    char nfc_line1[48];
    char nfc_line2[96];
    snprintf(nfc_line1, sizeof(nfc_line1), "%s",
             nfc.ready ? (nfc.tag_present ? "已读卡" : "在线") : "离线");
    const char *nfc_hint = nfc.tag_present ?
                           (nfc.text[0] ? nfc.text : (nfc.uid[0] ? nfc.uid : "已触发")) :
                           (nfc.ready ? "等待贴卡" : esp_err_to_name(nfc.last_err));
    snprintf(nfc_line2, sizeof(nfc_line2), "%.95s", nfc_hint);
    draw_tile(x2, 200, tile_w, 74, "NFC", nfc_line1, nfc_line2,
              nfc.ready ? (nfc.tag_present ? UI_GREEN : UI_BLUE) : UI_RED);
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
    uint16_t *buf = heap_caps_malloc((size_t)cw * ch * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (!buf || !item) {
        if (buf) {
            heap_caps_free(buf);
        }
        return;
    }
    buffer_fill(buf, cw, ch, selected ? UI_PANEL_DARK : UI_PANEL);
    buffer_fill_rect(buf, cw, ch, 0, 0, 5, ch, selected ? UI_YELLOW : UI_CYAN);
    buffer_fill_rect(buf, cw, ch, 0, 0, cw, 1, selected ? UI_YELLOW : UI_LINE);
    buffer_fill_rect(buf, cw, ch, 0, ch - 1, cw, 1, UI_BG);

    char title[64];
    snprintf(title, sizeof(title), "%d/%d P%04d", idx + 1, total, item->id);
    buffer_text(buf, cw, ch, 15, 8, title, 1, UI_MUTED);
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
    buffer_text(buf, cw, ch, 15, 32, line, 1, UI_TEXT);
    snprintf(line, sizeof(line), "%s", item->category[0] ? item->category : "点击打开详情");
    buffer_text(buf, cw, ch, 15, 55, line, 1, UI_MUTED);

    (void)display_ili9488_draw_bitmap565(x, y, cw, ch, buf);
    heap_caps_free(buf);
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
    int y = display_ili9488_get_height() - UI_NAV_H - UI_DETAIL_ACTION_H;
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

static void draw_detail_field(int y, const char *title, const char *value, bool active)
{
    int w = display_ili9488_get_width();
    int x = UI_DETAIL_FIELD_X;
    int max_w = w - UI_DETAIL_FIELD_X * 2;
    int title_w = estimate_ui_text_width(title, 1);
    int value_w = estimate_ui_text_width(value, 1);
    int cw = 18 + title_w + 12 + value_w + 18;
    if (cw < 128) {
        cw = 128;
    }
    if (cw > max_w) {
        cw = max_w;
    }
    int ch = UI_DETAIL_FIELD_H;
    uint16_t *buf = heap_caps_malloc((size_t)cw * ch * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (!buf) {
        return;
    }
    buffer_fill(buf, cw, ch, active ? UI_PANEL_DARK : UI_BG);
    buffer_fill_rect(buf, cw, ch, 0, 0, 4, ch, active ? UI_CYAN : UI_BLUE);
    buffer_fill_rect(buf, cw, ch, 0, 0, cw, 1, UI_LINE);
    buffer_fill_rect(buf, cw, ch, 0, ch - 1, cw, 1, UI_LINE);
    buffer_text(buf, cw, ch, 12, 9, title, 1, UI_MUTED);
    buffer_text(buf, cw, ch, 12 + title_w + 12, 9, value, 1, UI_TEXT);
    (void)display_ili9488_draw_bitmap565(x, y, cw, ch, buf);
    heap_caps_free(buf);
}

static void draw_detail_info_card(int y)
{
    int w = display_ili9488_get_width();
    int x = UI_DETAIL_FIELD_X;
    int cw = w - UI_DETAIL_FIELD_X * 2;
    int ch = detail_info_h();
    uint16_t *buf = heap_caps_malloc((size_t)cw * ch * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (!buf) {
        return;
    }

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
    if (page == 0) {
        snprintf(title, sizeof(title), "Part-DB 基础 1/3");
        snprintf(line1, sizeof(line1), "%.95s",
                 s_detail.name[0] ? s_detail.name : "未命名元件");
        int part_id = s_detail.part_id > 0 ? s_detail.part_id : s_detail.id;
        int lot_id = active_detail_lot_id(&s_detail);
        if (lot_id > 0) {
            snprintf(line2, sizeof(line2), "IPN %.24s  P%d L%d",
                     s_detail.ipn[0] ? s_detail.ipn : "-", part_id, lot_id);
        } else {
            snprintf(line2, sizeof(line2), "IPN %.24s  P%d",
                     s_detail.ipn[0] ? s_detail.ipn : "-", part_id);
        }
        snprintf(line3, sizeof(line3), "分类 %.24s  厂家 %.24s",
                 s_detail.category[0] ? s_detail.category : "-",
                 s_detail.manufacturer[0] ? s_detail.manufacturer : "-");
        snprintf(line4, sizeof(line4), "MPN %.28s  封装 %.28s",
                 s_detail.mpn[0] ? s_detail.mpn : "-",
                 s_detail.footprint[0] ? s_detail.footprint : "-");
        snprintf(line5, sizeof(line5), "库位 %.28s  条码 %.28s",
                 s_detail.location[0] ? s_detail.location : "-",
                 s_detail.barcode[0] ? s_detail.barcode : "-");
        if (s_detail.amount_unknown) {
            snprintf(line6, sizeof(line6), "库存未知  批次 %d", s_detail.lot_count);
        } else if (s_detail.amount_valid) {
            snprintf(line6, sizeof(line6), "库存 %.0f  批次 %d",
                     (double)s_detail.amount, s_detail.lot_count);
        } else {
            snprintf(line6, sizeof(line6), "库存 --  批次 %d", s_detail.lot_count);
        }
        snprintf(line7, sizeof(line7), "参数 %.80s", params);
    } else if (page == 1) {
        snprintf(title, sizeof(title), "Part-DB 简介 2/3");
        snprintf(line1, sizeof(line1), "简介 %.80s", desc);
        snprintf(line2, sizeof(line2), "备注 %.80s", comment);
        snprintf(line3, sizeof(line3), "MPN %.28s  封装 %.28s",
                 s_detail.mpn[0] ? s_detail.mpn : "-",
                 s_detail.footprint[0] ? s_detail.footprint : "-");
        snprintf(line4, sizeof(line4), "分类 %.24s  厂家 %.24s",
                 s_detail.category[0] ? s_detail.category : "-",
                 s_detail.manufacturer[0] ? s_detail.manufacturer : "-");
        snprintf(line5, sizeof(line5), "库位 %.28s  条码 %.28s",
                 s_detail.location[0] ? s_detail.location : "-",
                 s_detail.barcode[0] ? s_detail.barcode : "-");
        snprintf(line6, sizeof(line6), "IPN %.32s",
                 s_detail.ipn[0] ? s_detail.ipn : "-");
        snprintf(line7, sizeof(line7), "参数 %.80s", params);
    } else {
        snprintf(title, sizeof(title), "Part-DB 参数 3/3");
        snprintf(line1, sizeof(line1), "参数 %.80s", params);
        snprintf(line2, sizeof(line2), "库位 %.28s  条码 %.28s",
                 s_detail.location[0] ? s_detail.location : "-",
                 s_detail.barcode[0] ? s_detail.barcode : "-");
        snprintf(line3, sizeof(line3), "MPN %.28s  封装 %.28s",
                 s_detail.mpn[0] ? s_detail.mpn : "-",
                 s_detail.footprint[0] ? s_detail.footprint : "-");
        snprintf(line4, sizeof(line4), "分类 %.24s",
                 s_detail.category[0] ? s_detail.category : "-");
        snprintf(line5, sizeof(line5), "厂家 %.24s",
                 s_detail.manufacturer[0] ? s_detail.manufacturer : "-");
        snprintf(line6, sizeof(line6), "简介 %.80s", desc);
        snprintf(line7, sizeof(line7), "备注 %.80s", comment);
    }

    buffer_fill(buf, cw, ch, UI_PANEL);
    buffer_fill_rect(buf, cw, ch, 0, 0, cw, 1, UI_LINE);
    buffer_fill_rect(buf, cw, ch, 0, ch - 1, cw, 1, UI_BG);
    buffer_fill_rect(buf, cw, ch, 0, 0, 5, ch, UI_GREEN);
    buffer_text(buf, cw, ch, 15, 7, title, 1, UI_MUTED);
    const char *lines[] = {line1, line2, line3, line4, line5, line6, line7};
    for (int i = 0; i < (int)(sizeof(lines) / sizeof(lines[0])); i++) {
        int ly = 25 + i * 18;
        if (ly + 16 > ch || !lines[i][0]) {
            break;
        }
        buffer_text(buf, cw, ch, 15, ly, lines[i], 1, i == 0 ? UI_TEXT : UI_MUTED);
    }
    (void)display_ili9488_draw_bitmap565(x, y, cw, ch, buf);
    heap_caps_free(buf);
}

static void draw_detail_action_card(int y)
{
    int w = display_ili9488_get_width();
    int x = UI_DETAIL_FIELD_X;
    int cw = w - UI_DETAIL_FIELD_X * 2;
    int ch = UI_DETAIL_ACTION_H;
    uint16_t *buf = heap_caps_malloc((size_t)cw * ch * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (!buf) {
        return;
    }

    char line[96];
    bool transient_status = false;
    if (s_stock.busy) {
        snprintf(line, sizeof(line), "正在写回 Part-DB");
        transient_status = true;
    } else if (s_nfc_write.busy) {
        snprintf(line, sizeof(line), "NFC写入中  %.76s",
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
    buffer_text(buf, cw, ch, 15, 6, "库存", 1, UI_MUTED);
    buffer_text(buf, cw, ch, 62, 6, line, 1, UI_TEXT);

    int gap = 6;
    int by = UI_DETAIL_ACTION_BUTTON_Y;
    int bh = UI_DETAIL_ACTION_BUTTON_H;
    int bw = (cw - 30 - gap * 2) / 3;
    int bx = 15;
    const char *labels[] = {"入库", "出库", "写NFC"};
    uint16_t colors[] = {
        UI_BG,
        UI_BG,
        nfc_pn532_is_ready() ? UI_BLUE : UI_PANEL_DARK,
    };
    uint16_t accents[] = {
        s_stock_op == UI_STOCK_OP_IN ? UI_CYAN : UI_LINE,
        s_stock_op == UI_STOCK_OP_OUT ? UI_CYAN : UI_LINE,
        nfc_pn532_is_ready() ? UI_CYAN : UI_LINE,
    };
    for (int i = 0; i < 3; i++) {
        int px = bx + i * (bw + gap);
        buffer_fill_rect(buf, cw, ch, px, by, bw, bh, colors[i]);
        buffer_fill_rect(buf, cw, ch, px, by, bw, 2, accents[i]);
        int label_w = estimate_ui_text_width(labels[i], 1);
        buffer_text(buf, cw, ch, px + (bw - label_w) / 2, by + 8, labels[i], 1, UI_TEXT);
    }

    (void)display_ili9488_draw_bitmap565(x, y, cw, ch, buf);
    heap_caps_free(buf);
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
    uint16_t *buf = heap_caps_malloc((size_t)cw * ch * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (!buf) {
        return;
    }
    buffer_fill(buf, cw, ch, UI_PANEL);
    buffer_fill_rect(buf, cw, ch, 0, 0, cw, 1, UI_LINE);
    buffer_fill_rect(buf, cw, ch, 0, ch - 1, cw, 1, UI_BG);
    buffer_fill_rect(buf, cw, ch, 0, 0, 5, ch, accent);
    int title_y = ch <= UI_DETAIL_ACTION_H ? 6 : 9;
    int line1_y = ch <= UI_DETAIL_ACTION_H ? 24 : 31;
    int line2_y = ch <= UI_DETAIL_ACTION_H ? 43 : 54;
    buffer_text(buf, cw, ch, 15, title_y, title, 1, UI_MUTED);
    buffer_text(buf, cw, ch, 15, line1_y, line1, 1, UI_TEXT);
    if (line2 && line2[0]) {
        buffer_text(buf, cw, ch, 15, line2_y, line2, 1, UI_MUTED);
    }
    (void)display_ili9488_draw_bitmap565(x, y, cw, ch, buf);
    heap_caps_free(buf);
}

static void draw_detail(void)
{
    draw_header("三级详情", UI_GREEN);
    draw_detail_field(UI_DETAIL_FIELD1_Y, "对象/编号", s_detail_ipn, s_input_target == UI_INPUT_IPN);
    draw_detail_field(UI_DETAIL_FIELD2_Y, "出入库数量", s_detail_qty, s_input_target == UI_INPUT_QTY);
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
        draw_detail_panel(UI_DETAIL_INFO_Y, info_h, "未找到元件", line1, line2, UI_RED);
        draw_detail_panel(action_y, UI_DETAIL_ACTION_H, "库存", "不能写回", "请检查对象内容", UI_PANEL_DARK);
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
    uint16_t *buf = heap_caps_malloc((size_t)mw * mh * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (!buf) {
        return;
    }

    char payload[sizeof(s_nfc_write.payload)] = {0};
    (void)detail_nfc_payload(&s_detail, payload, sizeof(payload));
    char line[96];
    snprintf(line, sizeof(line), "将写入 %s", payload[0] ? payload : "--");

    buffer_fill(buf, mw, mh, UI_PANEL_DARK);
    buffer_fill_rect(buf, mw, mh, 0, 0, mw, 2, UI_YELLOW);
    buffer_fill_rect(buf, mw, mh, 0, 0, 1, mh, UI_LINE);
    buffer_fill_rect(buf, mw, mh, mw - 1, 0, 1, mh, UI_LINE);
    buffer_fill_rect(buf, mw, mh, 0, mh - 1, mw, 1, UI_LINE);
    buffer_text(buf, mw, mh, 18, 18, "确认写入 NFC", 1, UI_TEXT);
    buffer_text(buf, mw, mh, 18, 48, line, 1, UI_MUTED);
    buffer_text(buf, mw, mh, 18, 74, "请确认已贴近目标卡片", 1, UI_MUTED);

    int by = mh - 54;
    int bw = (mw - 48) / 2;
    int bx1 = 18;
    int bx2 = bx1 + bw + 12;
    buffer_fill_rect(buf, mw, mh, bx1, by, bw, 36, UI_PANEL);
    buffer_fill_rect(buf, mw, mh, bx1, by, bw, 1, UI_LINE);
    buffer_text(buf, mw, mh, bx1 + 30, by + 10, "取消", 1, UI_MUTED);
    buffer_fill_rect(buf, mw, mh, bx2, by, bw, 36, UI_BLUE);
    buffer_fill_rect(buf, mw, mh, bx2, by, bw, 1, UI_CYAN);
    buffer_text(buf, mw, mh, bx2 + 30, by + 10, "确认", 1, UI_TEXT);

    (void)display_ili9488_draw_bitmap565(mx, my, mw, mh, buf);
    heap_caps_free(buf);
}

static void draw_settings(void)
{
    draw_header("设置", UI_BLUE);
    wifi_portal_status_t wifi = wifi_portal_get_status();
    char line1[80];
    const char *busy_hint = s_settings_busy ? "处理中" : "点按切换";
    int w = display_ili9488_get_width();
    int gap = 8;
    int tile_w = (w - 24 - gap) / 2;
    int x1 = 12;
    int x2 = x1 + tile_w + gap;
    snprintf(line1, sizeof(line1), "%u%%", s_cfg ? s_cfg->display_brightness : display_ili9488_get_brightness());
    draw_tile(x1, 116, tile_w, 74, "屏幕亮度", line1, "点按循环", UI_CYAN);
    snprintf(line1, sizeof(line1), "%s", wifi.ap_enabled ? "启用" : "关闭");
    draw_tile(x2, 116, tile_w, 74, "AP 模式", line1, s_settings_busy ? "处理中" : (wifi.ap_started ? "AP 已启动" : "点按切换"), wifi.ap_enabled ? UI_GREEN : UI_YELLOW);
    snprintf(line1, sizeof(line1), "%s", s_cfg && s_cfg->font_path[0] ? path_basename(s_cfg->font_path) : "默认内置");
    draw_tile(x1, 200, tile_w, 74, "字体", line1, s_settings_busy ? "处理中" : "TF 字体目录", UI_BLUE);
    snprintf(line1, sizeof(line1), "%s", s_cfg && s_cfg->screen_bg_path[0] ? path_basename(s_cfg->screen_bg_path) : "未选择");
    draw_tile(x2, 200, tile_w, 74, "屏幕背景", line1, busy_hint, UI_GREEN);
    snprintf(line1, sizeof(line1), "%s", s_cfg && s_cfg->lock_bg_path[0] ? path_basename(s_cfg->lock_bg_path) : "未选择");
    draw_tile(x1, 284, tile_w, 74, "锁屏壁纸", line1, busy_hint, UI_ORANGE);
    snprintf(line1, sizeof(line1), "%s", s_cfg && s_cfg->boot_anim_path[0] ? path_basename(s_cfg->boot_anim_path) : "未选择");
    draw_tile(x2, 284, tile_w, 74, "开机动画", line1, s_settings_busy ? "处理中" : "GIF/WebP", UI_YELLOW);
}

static void draw_info(void)
{
    hardware_diag_status_t diag = hardware_diag_get_status();
    storage_sd_status_t sd = storage_sd_get_status();
    wifi_portal_status_t wifi = wifi_portal_get_status();
    nfc_service_status_t nfc = nfc_service_get_status();
    qr_scanner_status_t qr = qr_scanner_get_status();
    draw_header("信息", UI_YELLOW);
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
    snprintf(line1, sizeof(line1), "%s", nfc.ready ? "在线" : "离线");
    snprintf(line2, sizeof(line2), "%.95s",
             nfc.tag_present ? (nfc.text[0] ? nfc.text : nfc.uid) : esp_err_to_name(nfc.last_err));
    draw_tile(x1, 200, tile_w, 74, "NFC", line1, line2, ok_color(nfc.ready));
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

static void draw_key(int x, int y, int w, int h, const char *label, bool accent)
{
    uint16_t *buf = heap_caps_malloc((size_t)w * h * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (!buf) {
        return;
    }
    buffer_fill(buf, w, h, accent ? UI_BLUE : UI_PANEL);
    buffer_fill_rect(buf, w, h, 0, 0, w, 1, accent ? UI_CYAN : UI_LINE);
    buffer_fill_rect(buf, w, h, 0, h - 1, w, 1, UI_BG);
    buffer_fill_rect(buf, w, h, 0, 0, 1, h, UI_BG);
    buffer_fill_rect(buf, w, h, w - 1, 0, 1, h, UI_BG);
    int text_w = label ? (int)strlen(label) * 8 : 0;
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
    heap_caps_free(buf);
}

typedef struct {
    const char *keys;
    uint8_t count;
} ui_keyboard_row_t;

static const ui_keyboard_row_t KEYBOARD_ROWS[] = {
    {.keys = "123456", .count = 6},
    {.keys = "7890", .count = 4},
    {.keys = "QWERTY", .count = 6},
    {.keys = "UIOP", .count = 4},
    {.keys = "ASDFGH", .count = 6},
    {.keys = "JKL", .count = 3},
    {.keys = "ZXCVBNM", .count = 7},
};

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
    for (int r = 0; r < (int)(sizeof(KEYBOARD_ROWS) / sizeof(KEYBOARD_ROWS[0])); r++) {
        const ui_keyboard_row_t *row = &KEYBOARD_ROWS[r];
        int row_w = row->count * key_w + (row->count - 1) * UI_KEYBOARD_GAP;
        int x = (screen_w - row_w) / 2;
        if (x < UI_KEYBOARD_MARGIN) {
            x = UI_KEYBOARD_MARGIN;
        }
        int y = keyboard_row_y(y0, r);
        char label[2] = {0};
        for (int i = 0; i < row->count; i++) {
            label[0] = row->keys[i];
            draw_key(x, y, key_w, UI_KEYBOARD_ROW_H, label, false);
            x += key_w + UI_KEYBOARD_GAP;
        }
    }

    int row_count = (int)(sizeof(KEYBOARD_ROWS) / sizeof(KEYBOARD_ROWS[0]));
    int special_y = keyboard_row_y(y0, row_count);
    int special_h = screen_h - special_y - 8;
    if (special_h < UI_KEYBOARD_ROW_H) {
        special_h = UI_KEYBOARD_ROW_H;
    }
    int special_w = (screen_w - UI_KEYBOARD_MARGIN * 2 - UI_KEYBOARD_GAP) / 2;
    draw_key(UI_KEYBOARD_MARGIN, special_y, special_w, special_h, "DEL", true);
    draw_key(UI_KEYBOARD_MARGIN + special_w + UI_KEYBOARD_GAP, special_y,
             screen_w - UI_KEYBOARD_MARGIN * 2 - special_w - UI_KEYBOARD_GAP,
             special_h, "OK", true);
}

static void redraw(void)
{
    if (!display_ili9488_is_ready() || !display_ili9488_is_awake()) {
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
        draw_info();
        break;
    case DEVICE_UI_PAGE_SETTINGS:
        draw_settings();
        break;
    case DEVICE_UI_PAGE_HOME:
    default:
        draw_home();
        break;
    }
    if (s_nfc_confirm_open) {
        draw_nfc_confirm_modal();
    }
    if (s_keyboard_open) {
        draw_keyboard();
    } else {
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
    uint16_t *buf = heap_caps_malloc((size_t)sw * sh * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (!buf) {
        return;
    }
    buffer_fill(buf, sw, sh, UI_BG);
    buffer_search_field(buf, sw, sh, 0, 0, sw, sh,
                        page_accent((device_ui_page_t)s_status.page));
    (void)display_ili9488_draw_bitmap565(sx, sy, sw, sh, buf);
    heap_caps_free(buf);
}

static void redraw_active_input(void)
{
    if (!display_ili9488_is_ready() || !display_ili9488_is_awake()) {
        return;
    }
    switch (s_input_target) {
    case UI_INPUT_SEARCH:
        draw_search_input_only();
        break;
    case UI_INPUT_IPN:
        if (s_status.page == DEVICE_UI_PAGE_DETAIL) {
            draw_detail_field(UI_DETAIL_FIELD1_Y, "对象/编号", s_detail_ipn, true);
        }
        break;
    case UI_INPUT_QTY:
        if (s_status.page == DEVICE_UI_PAGE_DETAIL) {
            draw_detail_field(UI_DETAIL_FIELD2_Y, "出入库数量", s_detail_qty, true);
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
    if (s_status.page != page) {
        s_full_repaint = true;
        s_have_view_hash = false;
    }
    s_status.page = page;
    s_status.page_name = device_ui_page_name(page);
    s_dirty = true;
}

esp_err_t device_ui_set_page(device_ui_page_t page)
{
    if (page >= DEVICE_UI_PAGE_COUNT) {
        return ESP_ERR_INVALID_ARG;
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

static bool handle_context_button_event(const char *event)
{
    if (!event) {
        return false;
    }
    if (s_nfc_confirm_open) {
        if (strcmp(event, "confirm") == 0) {
            s_nfc_confirm_open = false;
            schedule_nfc_write();
        } else if (strcmp(event, "back") == 0 || strcmp(event, "up") == 0 ||
                   strcmp(event, "down") == 0) {
            s_nfc_confirm_open = false;
            s_dirty = true;
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
            set_page_internal(DEVICE_UI_PAGE_HOME);
        } else {
            return false;
        }
        return true;
    }

    if (s_status.page == DEVICE_UI_PAGE_DETAIL) {
        if (strcmp(event, "back") == 0) {
            set_page_internal(s_results.valid ? DEVICE_UI_PAGE_RESULTS : DEVICE_UI_PAGE_HOME);
        } else if (strcmp(event, "up") == 0 || strcmp(event, "down") == 0 ||
                   strcmp(event, "confirm") == 0) {
            s_dirty = true;
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
        (void)display_ili9488_set_awake(!display_ili9488_is_awake());
        s_dirty = display_ili9488_is_awake();
        return;
    }
    if (!display_ili9488_is_awake()) {
        (void)display_ili9488_set_awake(true);
        s_dirty = true;
        return;
    }
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
        set_page_internal(DEVICE_UI_PAGE_HOME);
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
    if (!display_ili9488_is_awake()) {
        (void)display_ili9488_set_awake(true);
        s_dirty = true;
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

static void handle_nfc_detail_entry(void)
{
    if (s_keyboard_open || s_nfc_confirm_open || s_status.page == DEVICE_UI_PAGE_DETAIL) {
        return;
    }
    nfc_service_status_t nfc = nfc_service_get_status();
    if (nfc.read_count == s_seen_nfc_read_count) {
        return;
    }
    s_seen_nfc_read_count = nfc.read_count;
    if (nfc.tag_present && nfc.text[0]) {
        enter_detail_with_object(nfc.text);
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
        buf[len - 1] = '\0';
        request_input_redraw();
    }
}

static void input_append(char ch)
{
    size_t max_len = 0;
    char *buf = input_buffer(&max_len);
    if (!buf || max_len == 0) {
        return;
    }
    if (s_input_target == UI_INPUT_QTY && (ch < '0' || ch > '9')) {
        return;
    }
    size_t len = strlen(buf);
    if (len < max_len) {
        buf[len] = ch;
        buf[len + 1] = '\0';
        request_input_redraw();
    }
}

static void enter_detail_with_object(const char *value)
{
    if (value && value[0]) {
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
    int row_count = (int)(sizeof(KEYBOARD_ROWS) / sizeof(KEYBOARD_ROWS[0]));
    for (int r = 0; r < row_count; r++) {
        const ui_keyboard_row_t *row = &KEYBOARD_ROWS[r];
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
                input_append(row->keys[i]);
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
        int special_w = (screen_w - UI_KEYBOARD_MARGIN * 2 - UI_KEYBOARD_GAP) / 2;
        if (point_in(x, y, UI_KEYBOARD_MARGIN, special_y, special_w, special_h)) {
            input_backspace();
        } else if (point_in(x, y, UI_KEYBOARD_MARGIN + special_w + UI_KEYBOARD_GAP,
                            special_y,
                            screen_w - UI_KEYBOARD_MARGIN * 2 - special_w - UI_KEYBOARD_GAP,
                            special_h)) {
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
    s_search[0] = '\0';
    set_page_internal(s_main_pages[idx]);
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

    if (point_in(x, y, bx1, by, bw, 36)) {
        s_nfc_confirm_open = false;
        s_dirty = true;
        return true;
    }
    if (point_in(x, y, bx2, by, bw, 36)) {
        s_nfc_confirm_open = false;
        schedule_nfc_write();
        return true;
    }
    s_nfc_confirm_open = false;
    s_dirty = true;
    return true;
}

static bool handle_ui_tap(uint16_t x, uint16_t y)
{
    if (handle_keyboard_tap(x, y)) {
        return true;
    }
    if (handle_nfc_confirm_tap(x, y)) {
        return true;
    }
    int w = display_ili9488_get_width();
    int h = display_ili9488_get_height();
    if (point_in(x, y, 12, 58, w - 24, 34)) {
        s_stock_op = UI_STOCK_OP_NONE;
        clear_stock_result();
        open_keyboard(UI_INPUT_SEARCH);
        return true;
    }
    if (y >= h - UI_NAV_H) {
        set_page_from_nav(x);
        return true;
    }

    if (s_status.page == DEVICE_UI_PAGE_DETAIL) {
        int action_y = detail_action_y();
        int info_h = detail_info_h();
        if (point_in(x, y, UI_DETAIL_FIELD_X, UI_DETAIL_FIELD1_Y,
                     w - UI_DETAIL_FIELD_X * 2, UI_DETAIL_FIELD_H)) {
            open_keyboard(UI_INPUT_IPN);
            return true;
        }
        if (point_in(x, y, UI_DETAIL_FIELD_X, UI_DETAIL_FIELD2_Y,
                     w - UI_DETAIL_FIELD_X * 2, UI_DETAIL_FIELD_H)) {
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

    if (s_status.page == DEVICE_UI_PAGE_SETTINGS) {
        int gap = 8;
        int tile_w = (w - 24 - gap) / 2;
        int x1 = 12;
        int x2 = x1 + tile_w + gap;
        if (point_in(x, y, x1, 116, tile_w, 74)) {
            cycle_brightness();
            return true;
        }
        if (point_in(x, y, x2, 116, tile_w, 74)) {
            schedule_settings_action(UI_SETTINGS_ACTION_AP_TOGGLE);
            return true;
        }
        if (point_in(x, y, x1, 200, tile_w, 74)) {
            schedule_settings_action(UI_SETTINGS_ACTION_FONT_NEXT);
            return true;
        }
        if (point_in(x, y, x2, 200, tile_w, 74)) {
            schedule_settings_action(UI_SETTINGS_ACTION_SCREEN_BG_NEXT);
            return true;
        }
        if (point_in(x, y, x1, 284, tile_w, 74)) {
            schedule_settings_action(UI_SETTINGS_ACTION_LOCK_BG_NEXT);
            return true;
        }
        if (point_in(x, y, x2, 284, tile_w, 74)) {
            schedule_settings_action(UI_SETTINGS_ACTION_BOOT_ANIM_NEXT);
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
            qr_scanner_result_t result = {0};
            (void)qr_scanner_scan(&result);
            if (result.found && result.text[0]) {
                enter_detail_with_object(result.text);
            }
            s_dirty = true;
            return true;
        }
        if (point_in(x, y, x2, 200, tile_w, 74)) {
            nfc_service_status_t nfc = nfc_service_get_status();
            if (nfc.tag_present && nfc.text[0]) {
                enter_detail_with_object(nfc.text);
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

    if (s_keyboard_open) {
        (void)handle_keyboard_tap(s_touch.last_x, s_touch.last_y);
        record_touch_event("keyboard_tap", s_touch.last_x, s_touch.last_y);
        s_touch.action_sent = true;
        return;
    }

    if (s_nfc_confirm_open) {
        update_touch_point("modal_swipe_ignored", s_touch.last_x, s_touch.last_y);
        s_touch.action_sent = true;
        return;
    }

    if (ax >= UI_TOUCH_SWIPE_PX || ay >= UI_TOUCH_SWIPE_PX) {
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
        s_status.awake = display_ili9488_is_awake();
        handle_button_event();
        TickType_t now = xTaskGetTickCount();
        handle_touch_event(now);
        handle_nfc_detail_entry();
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
    s_status.awake = display_ili9488_is_awake();
    s_status.page_name = device_ui_page_name((device_ui_page_t)s_status.page);
    return s_status;
}
