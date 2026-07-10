#include "button_input.h"

#include "board_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "buttons";

#define BUTTON_POLL_MS          20
#define BUTTON_DEBOUNCE_MS      40
#define BUTTON_DOUBLE_MS        350
#define BUTTON_LONG_MS          900

typedef enum {
    BTN_UP = 0,
    BTN_DOWN,
    BTN_OK,
    BTN_WAKE,
    BTN_COUNT,
} button_id_t;

typedef struct {
    gpio_num_t gpio;
    const char *name;
    bool pressed;
    bool raw_pressed;
    uint16_t debounce_ms;
    uint32_t press_start_ms;
} button_state_t;

static button_state_t s_buttons[BTN_COUNT] = {
    [BTN_UP] = {.gpio = BOARD_BUTTON_UP_GPIO, .name = "up"},
    [BTN_DOWN] = {.gpio = BOARD_BUTTON_DOWN_GPIO, .name = "down"},
    [BTN_OK] = {.gpio = BOARD_BUTTON_OK_GPIO, .name = "ok"},
    [BTN_WAKE] = {.gpio = BOARD_BUTTON_WAKE_GPIO, .name = "wake"},
};

static button_input_status_t s_status = {
    .last_event = "none",
};
static TaskHandle_t s_task;
static bool s_ok_click_waiting;
static uint32_t s_ok_click_due_ms;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static bool button_configured(const button_state_t *button)
{
    return button && button->gpio != GPIO_NUM_NC;
}

static void emit_event(const char *event)
{
    s_status.last_event = event ? event : "none";
    s_status.event_count++;
    ESP_LOGI(TAG, "button event=%s count=%lu",
             s_status.last_event, (unsigned long)s_status.event_count);
}

static bool gpio_pressed(gpio_num_t gpio)
{
    int level = gpio_get_level(gpio);
    return BOARD_BUTTON_ACTIVE_LEVEL ? level != 0 : level == 0;
}

static void classify_release(button_id_t id, uint32_t held_ms, uint32_t t_ms)
{
    if (id == BTN_UP) {
        emit_event(held_ms >= BUTTON_LONG_MS ? "up_long" : "up");
        return;
    }
    if (id == BTN_DOWN) {
        emit_event(held_ms >= BUTTON_LONG_MS ? "down_long" : "down");
        return;
    }
    if (id == BTN_WAKE) {
        emit_event("sleep_wake");
        return;
    }
    if (id != BTN_OK) {
        return;
    }

    if (held_ms >= BUTTON_LONG_MS) {
        s_ok_click_waiting = false;
        emit_event("home");
        return;
    }
    if (s_ok_click_waiting && t_ms <= s_ok_click_due_ms) {
        s_ok_click_waiting = false;
        emit_event("back");
        return;
    }
    s_ok_click_waiting = true;
    s_ok_click_due_ms = t_ms + BUTTON_DOUBLE_MS;
}

static void update_status_pressed(void)
{
    s_status.up_pressed = s_buttons[BTN_UP].pressed;
    s_status.down_pressed = s_buttons[BTN_DOWN].pressed;
    s_status.ok_pressed = s_buttons[BTN_OK].pressed;
    s_status.wake_pressed = s_buttons[BTN_WAKE].pressed;
}

static void poll_button(button_id_t id, uint32_t t_ms)
{
    button_state_t *button = &s_buttons[id];
    if (!button_configured(button)) {
        return;
    }
    bool raw = gpio_pressed(button->gpio);
    if (raw != button->raw_pressed) {
        button->raw_pressed = raw;
        button->debounce_ms = 0;
        return;
    }
    if (button->debounce_ms < BUTTON_DEBOUNCE_MS) {
        button->debounce_ms += BUTTON_POLL_MS;
        return;
    }
    if (button->pressed == raw) {
        return;
    }
    button->pressed = raw;
    if (raw) {
        button->press_start_ms = t_ms;
    } else {
        classify_release(id, t_ms - button->press_start_ms, t_ms);
    }
}

static void button_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t t_ms = now_ms();
        for (button_id_t i = 0; i < BTN_COUNT; i++) {
            poll_button(i, t_ms);
        }
        if (s_ok_click_waiting && t_ms > s_ok_click_due_ms) {
            s_ok_click_waiting = false;
            emit_event("confirm");
        }
        update_status_pressed();
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

esp_err_t button_input_init(void)
{
    if (s_status.started) {
        return ESP_OK;
    }

    uint64_t pin_mask = 0;
    for (button_id_t i = 0; i < BTN_COUNT; i++) {
        if (button_configured(&s_buttons[i])) {
            pin_mask |= 1ULL << s_buttons[i].gpio;
            s_status.configured_count++;
        }
    }
    s_status.started = true;
    if (pin_mask == 0) {
        ESP_LOGI(TAG, "button service started without configured GPIOs");
        return ESP_OK;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = BOARD_BUTTON_ACTIVE_LEVEL ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
        .pull_down_en = BOARD_BUTTON_ACTIVE_LEVEL ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    BaseType_t ok = xTaskCreate(button_task, "buttons", 3072, NULL, 5, &s_task);
    if (ok != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "button service started configured=%u", s_status.configured_count);
    return ESP_OK;
}

button_input_status_t button_input_get_status(void)
{
    return s_status;
}
