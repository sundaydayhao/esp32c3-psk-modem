#include "ui.h"

#include "board_config.h"
#include "display.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "button_gesture.h"

static const char *TAG = "ui";
static button_gesture_t button;

esp_err_t ui_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_SW1_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    ESP_LOGI(TAG, "SW1 on GPIO%d; touch backend placeholder", BOARD_SW1_GPIO);
    return ESP_OK;
}

ui_event_t ui_poll(const display_state_t *display_state)
{
    ui_event_t event = { .type = UI_EVENT_NONE };
    display_task_handler();

    modem_mode_t mode;
    if (display_take_mode_selection(&mode)) {
        event.type = UI_EVENT_MODE_CHANGED;
        event.mode = mode;
        return event;
    }

    int tone_hz;
    if (display_take_tone_selection(&tone_hz)) {
        event.type = UI_EVENT_TONE_CHANGED;
        event.tone_hz = tone_hz;
        return event;
    }

    if (button_gesture_update(&button, gpio_get_level(BOARD_SW1_GPIO) == 0,
                              (uint32_t)(esp_timer_get_time() / 1000)) == BUTTON_SHORT) {
        event.type = UI_EVENT_MENU_TOGGLED;
    }

    (void)display_state;
    return event;
}
