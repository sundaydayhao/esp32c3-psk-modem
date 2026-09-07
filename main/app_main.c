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
#include "web_rx.h"

static const char *TAG = "psk-modem";
typedef struct { char text[96]; } decoded_msg_t;
typedef struct { int16_t samples[256]; size_t count; } spectrum_msg_t;
static QueueHandle_t decoded_queue, control_queue, spectrum_queue;

static void audio_task(void *arg)
{
    (void)arg;
    modem_t modem;
    const modem_config_t config = {
        .mode = BOARD_DEFAULT_MODE, .sample_rate_hz = BOARD_MODEM_SAMPLE_RATE_HZ,
        .rx_tone_hz = BOARD_DEFAULT_TONE_HZ,
    };
    modem_init(&modem, &config);
    receiver_status_t state = {.mode = config.mode, .tone_hz = config.rx_tone_hz};
    /* AP remains usable if the codec is missing. Initialize audio in its owner
     * task, after all potentially slow Wi-Fi/LCD startup operations. */
    esp_err_t result = audio_input_init();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Audio init failed: %s; AP diagnostics remain available", esp_err_to_name(result));
        receiver_update_status(&state);
        vTaskDelete(NULL);
        return;
    }
    state.audio_ready = true;
    int16_t samples[256];
    char decoded[96];
    int64_t last_status = 0, last_spectrum = 0;
    uint32_t previous_overflows = 0;
    while (true) {
        /* A bounded, single owner control path. HTTP never edits DSP state. */
        modem_command_t command;
        if (xQueueReceive(control_queue, &command, 0) == pdTRUE) {
            if (command.mode >= 0) modem_set_mode(&modem, (modem_mode_t)command.mode);
            if (command.tone_hz) modem_set_rx_tone(&modem, command.tone_hz);
            ++state.control_revision;
            state.mode = modem_get_mode(&modem);
            state.tone_hz = modem_get_rx_tone(&modem);
            receiver_update_status(&state);
        }
        size_t count = audio_input_read(samples, 256, 60);
        int64_t started = esp_timer_get_time();
        audio_input_stats_t audio;
        audio_input_get_stats(&audio);
        if (audio.dma_overflows != previous_overflows) {
            modem_set_mode(&modem, modem_get_mode(&modem));
            previous_overflows = audio.dma_overflows;
        }
        if (count) {
            int64_t sum = 0;
            for (size_t n = 0; n < count; ++n) sum += (int32_t)samples[n] * samples[n];
            state.rms = (uint32_t)sqrtf((float)(sum / (int64_t)count));
            /* Feed all samples: an RMS gate would interrupt symbol timing. */
            const size_t length = modem_process(&modem, samples, count, decoded, sizeof(decoded));
            if (length) {
                receiver_append(decoded, length);
                if (decoded_queue) {
                    decoded_msg_t msg = {0};
                    memcpy(msg.text, decoded, length);
                    if (xQueueSend(decoded_queue, &msg, 0) != pdTRUE) ++state.lcd_drops;
                }
            }
            state.dsp_us = audio.processing_us + (uint32_t)(esp_timer_get_time() - started);
            if (state.dsp_us > state.max_dsp_us) state.max_dsp_us = state.dsp_us;
            if (state.dsp_us >= count * 1000000 / BOARD_MODEM_SAMPLE_RATE_HZ) ++state.deadline_misses;
            state.samples += count;
            ++state.blocks;
            if (spectrum_queue && started - last_spectrum >= 250000) {
                spectrum_msg_t msg = {.count = count};
                memcpy(msg.samples, samples, count * sizeof(samples[0]));
                xQueueOverwrite(spectrum_queue, &msg);
                last_spectrum = started;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (started - last_status >= 250000) {
            state.quality = modem.rx.quality;
            state.signal = count && modem.rx.signal;
            state.dma_overflows = audio.dma_overflows;
            state.read_errors = audio.read_errors;
            state.clipped_samples = audio.clipped_samples;
            state.output_drops = modem.output_drops;
            state.invalid_codes = modem.rx.invalid_codes;
            receiver_update_status(&state);
            last_status = started;
        }
        /* i2s_channel_read blocks on DMA and yields to HTTP/UI/idle naturally. */
    }
}
#if BOARD_LOCAL_UI_ENABLED
static void ui_task(void *arg)
{
    (void)arg;
    display_state_t display_state = {.selected_mode = BOARD_DEFAULT_MODE, .rx_tone_hz = BOARD_DEFAULT_TONE_HZ};
    display_append_text(&display_state, "Wi-Fi: ESP32C3-PSK-xxxx\nKey: " BOARD_AP_PASSWORD "\nhttp://192.168.4.1\n");
    spectrum_t spectrum = {0};
    int64_t last_render = 0;
    while (true) {
        decoded_msg_t msg;
        while (xQueueReceive(decoded_queue, &msg, 0) == pdTRUE) display_append_text(&display_state, msg.text);
        receiver_status_t status;
        receiver_get_status(&status);
        display_state.selected_mode = status.mode;
        display_state.rx_tone_hz = status.tone_hz;
        const ui_event_t event = ui_poll(&display_state);
        modem_command_t command = {.mode = -1};
        if (event.type == UI_EVENT_MENU_TOGGLED) display_state.menu_open = !display_state.menu_open;
        if (event.type == UI_EVENT_MODE_CHANGED) { command.mode = event.mode; display_state.menu_open = false; }
        if (event.type == UI_EVENT_TONE_CHANGED) command.tone_hz = event.tone_hz;
        if (command.mode >= 0 || command.tone_hz) {
            if (xQueueSend(control_queue, &command, 0) != pdTRUE) ESP_LOGW(TAG, "Control queue full");
        }
        int64_t now = esp_timer_get_time();
        if (now - last_render >= 250000) {
            spectrum_msg_t pcm;
            if (xQueueReceive(spectrum_queue, &pcm, 0) == pdTRUE)
                spectrum_analyze(pcm.samples, pcm.count, BOARD_MODEM_SAMPLE_RATE_HZ, &spectrum);
            display_render(&display_state, &spectrum);
            last_render = now;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
#endif
void app_main(void)
{
    control_queue = xQueueCreate(4, sizeof(modem_command_t));
    ESP_ERROR_CHECK(control_queue ? ESP_OK : ESP_ERR_NO_MEM);
    const receiver_status_t status = {.mode = BOARD_DEFAULT_MODE, .tone_hz = BOARD_DEFAULT_TONE_HZ};
    receiver_update_status(&status);
    ESP_ERROR_CHECK(web_rx_start(control_queue));
#if BOARD_LOCAL_UI_ENABLED
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(ui_init());
    decoded_queue = xQueueCreate(8, sizeof(decoded_msg_t));
    spectrum_queue = xQueueCreate(1, sizeof(spectrum_msg_t));
    ESP_ERROR_CHECK(decoded_queue && spectrum_queue ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(ui_task, "psk_ui", 6144, NULL, 2, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
#endif
    ESP_ERROR_CHECK(xTaskCreate(audio_task, "psk_audio", 8192, NULL, 10, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_LOGI(TAG, "Receiver started: %s, %d Hz", modem_mode_name(BOARD_DEFAULT_MODE), BOARD_DEFAULT_TONE_HZ);
}
