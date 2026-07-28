#include <string.h>

#include "audio_input.h"
#include "board_config.h"
#include "display.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "modem.h"
#include "spectrum.h"
#include "ui.h"

static const char *TAG = "psk-modem";

void app_main(void)
{
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(ui_init());
    ESP_ERROR_CHECK(audio_input_init());

    modem_t modem;
    modem_config_t modem_config = {
        .mode = MODEM_MODE_BPSK31,
        .sample_rate_hz = BOARD_SAMPLE_RATE_HZ,
        .rx_tone_hz = 1000,
        .squelch = 20,
    };
    modem_init(&modem, &modem_config);

    display_state_t display_state = {
        .selected_mode = modem_get_mode(&modem),
        .rx_tone_hz = modem_get_rx_tone(&modem),
    };
    spectrum_t spectrum = {0};

    int16_t samples[256];
    char decoded[96];

    ESP_LOGI(TAG, "ESP32-C3 PSK modem started");

    while (true) {
        const size_t count = audio_input_read(samples, 256, 50);
        if (count > 0) {
            spectrum_analyze(samples, count, BOARD_SAMPLE_RATE_HZ, &spectrum);
            if (modem_process(&modem, samples, count, decoded, sizeof(decoded)) > 0) {
                display_append_text(&display_state, decoded);
            }
        }

        const ui_event_t event = ui_poll(&display_state);
        switch (event.type) {
        case UI_EVENT_MENU_TOGGLED:
            display_state.menu_open = !display_state.menu_open;
            break;
        case UI_EVENT_MODE_CHANGED:
            modem_set_mode(&modem, event.mode);
            display_state.selected_mode = modem_get_mode(&modem);
            break;
        case UI_EVENT_TONE_CHANGED:
            modem_set_rx_tone(&modem, event.tone_hz);
            display_state.rx_tone_hz = modem_get_rx_tone(&modem);
            break;
        case UI_EVENT_NONE:
        default:
            break;
        }

        display_state.selected_mode = modem_get_mode(&modem);
        display_state.rx_tone_hz = modem_get_rx_tone(&modem);
        display_render(&display_state, &spectrum);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
