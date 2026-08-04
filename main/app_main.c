#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "audio_input.h"
#include "board_config.h"
#include "display.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "modem.h"
#include "spectrum.h"
#include "ui.h"

static const char *TAG = "psk-modem";

typedef struct {
    char text[96];
} decoded_msg_t;

typedef struct {
    int16_t samples[SPECTRUM_MAX_SAMPLES];
    size_t count;
} spectrum_msg_t;

static QueueHandle_t s_decoded_queue;
static QueueHandle_t s_control_queue;
static QueueHandle_t s_spectrum_queue;

static int32_t calc_block_rms(const int16_t *samples, size_t count)
{
    if (count == 0) {
        return 0;
    }
    int64_t sum_sq = 0;
    for (size_t i = 0; i < count; ++i) {
        sum_sq += (int32_t)samples[i] * (int32_t)samples[i];
    }
    return (int32_t)sqrt((double)sum_sq / (double)count);
}

static void apply_control_event(modem_t *modem, const ui_event_t *event)
{
    switch (event->type) {
    case UI_EVENT_MODE_CHANGED:
        modem_set_mode(modem, event->mode);
        break;
    case UI_EVENT_TONE_CHANGED:
        modem_set_rx_tone(modem, event->tone_hz);
        break;
    case UI_EVENT_MENU_TOGGLED:
    case UI_EVENT_NONE:
    default:
        break;
    }
}

static void audio_task(void *arg)
{
    modem_t modem;
    const modem_config_t modem_config = {
        .mode = MODEM_MODE_PSK63,
        .sample_rate_hz = BOARD_MODEM_SAMPLE_RATE_HZ,
        .rx_tone_hz = 1000,
        .squelch = 70,
    };
    modem_init(&modem, &modem_config);

    int16_t samples[256];
    char decoded[sizeof(((decoded_msg_t *)0)->text)];
    int64_t last_status_log_us = 0;
    uint32_t reads_this_second = 0;
    uint32_t samples_this_second = 0;
    int32_t last_rms = 0;
    bool last_active = false;
    bool decoder_active = false;
    int active_hold_blocks = 0;

    ESP_LOGI(TAG, "audio decode task started");

    while (true) {
        ui_event_t event;
        while (xQueueReceive(s_control_queue, &event, 0) == pdTRUE) {
            apply_control_event(&modem, &event);
        }

        const size_t count = audio_input_read(samples, 256, 50);
        const int64_t now_us = esp_timer_get_time();
        if (count > 0) {
            reads_this_second++;
            samples_this_second += count;

            spectrum_msg_t spectrum_msg = {
                .count = count > SPECTRUM_MAX_SAMPLES ? SPECTRUM_MAX_SAMPLES : count,
            };
            memcpy(spectrum_msg.samples, samples, spectrum_msg.count * sizeof(spectrum_msg.samples[0]));
            (void)xQueueOverwrite(s_spectrum_queue, &spectrum_msg);

            last_rms = calc_block_rms(samples, count);
            last_active = last_rms >= BOARD_AUDIO_ACTIVE_RMS_MIN;
            if (last_active) {
                active_hold_blocks = 1;
                decoder_active = true;
            } else if (active_hold_blocks > 0) {
                active_hold_blocks--;
            } else if (decoder_active) {
                modem_set_mode(&modem, modem_get_mode(&modem));
                decoder_active = false;
            }

            if ((last_active || active_hold_blocks > 0) &&
                modem_process(&modem, samples, count, decoded, sizeof(decoded)) > 0) {
                printf("%s", decoded);
                fflush(stdout);

                decoded_msg_t msg = {0};
                strlcpy(msg.text, decoded, sizeof(msg.text));
                (void)xQueueSend(s_decoded_queue, &msg, 0);
            }
        }

#if BOARD_MODEM_DEBUG_LOGS
        if (now_us - last_status_log_us >= 5000000) {
            last_status_log_us = now_us;
            const nuepsk_rx_t *rx = &modem.nue_rx;
            ESP_LOGI(TAG,
                     "mode=%s rx=%d Hz rms=%d active=%d reads=%u samples=%u sq=%d lvl=%d agc=%d dev=%d pk=%d/%d/%d warm=%d sym=%u bits=%u z=%u o=%u dd=%u ch=%u unk=%u",
                     modem_mode_name(modem_get_mode(&modem)), modem_get_rx_tone(&modem),
                     (int)last_rms, last_active, (unsigned)reads_this_second, (unsigned)samples_this_second,
                     rx->sq_open, rx->sq_level, (int)rx->agc_ave, (int)rx->dev_ave,
                     rx->pk_pos, rx->new_pk_pos, rx->raw_pk_pos, rx->warmup_symbols,
                     (unsigned)rx->debug_symbols, (unsigned)rx->debug_bits,
                     (unsigned)rx->debug_zeros, (unsigned)rx->debug_ones,
                     (unsigned)rx->debug_delims, (unsigned)rx->debug_chars, (unsigned)rx->debug_unknown);
            reads_this_second = 0;
            samples_this_second = 0;
        }
#else
        (void)now_us;
        if (last_status_log_us == 0) {
            last_status_log_us = 1;
        }
        reads_this_second = 0;
        samples_this_second = 0;
#endif

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void ui_task(void *arg)
{
    display_state_t display_state = {
        .selected_mode = MODEM_MODE_PSK63,
        .rx_tone_hz = 1000,
    };
    spectrum_t spectrum = {0};
    int64_t last_render_us = 0;

    ESP_LOGI(TAG, "ui task started");

    while (true) {
        decoded_msg_t msg;
        while (xQueueReceive(s_decoded_queue, &msg, 0) == pdTRUE) {
            display_append_text(&display_state, msg.text);
        }

        const ui_event_t event = ui_poll(&display_state);
        switch (event.type) {
        case UI_EVENT_MENU_TOGGLED:
            display_state.menu_open = !display_state.menu_open;
            break;
        case UI_EVENT_MODE_CHANGED:
            display_state.selected_mode = event.mode;
            (void)xQueueSend(s_control_queue, &event, 0);
            break;
        case UI_EVENT_TONE_CHANGED:
            display_state.rx_tone_hz = event.tone_hz;
            (void)xQueueSend(s_control_queue, &event, 0);
            break;
        case UI_EVENT_NONE:
        default:
            break;
        }

        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_render_us >= 100000) {
            last_render_us = now_us;
            spectrum_msg_t spectrum_msg;
            if (xQueueReceive(s_spectrum_queue, &spectrum_msg, 0) == pdTRUE) {
                spectrum_analyze(spectrum_msg.samples, spectrum_msg.count, BOARD_MODEM_SAMPLE_RATE_HZ, &spectrum);
            }
            display_render(&display_state, &spectrum);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(ui_init());
    ESP_ERROR_CHECK(audio_input_init());

    s_decoded_queue = xQueueCreate(8, sizeof(decoded_msg_t));
    s_control_queue = xQueueCreate(4, sizeof(ui_event_t));
    s_spectrum_queue = xQueueCreate(1, sizeof(spectrum_msg_t));
    ESP_ERROR_CHECK(s_decoded_queue && s_control_queue && s_spectrum_queue ? ESP_OK : ESP_ERR_NO_MEM);

    xTaskCreate(audio_task, "psk_audio", 8192, NULL, 8, NULL);
    xTaskCreate(ui_task, "psk_ui", 6144, NULL, 2, NULL);

    ESP_LOGI(TAG, "ESP32-C3 PSK modem started");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
