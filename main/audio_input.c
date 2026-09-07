#include "audio_input.h"
#include "board_config.h"
#include "board_i2c.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "es8311.h"
#include "audio_decimator.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "audio";
static i2s_chan_handle_t s_rx_handle;
static es8311_handle_t s_codec;
static portMUX_TYPE stats_lock = portMUX_INITIALIZER_UNLOCKED;
static audio_input_stats_t stats;
static audio_decimator_t decimator;

static bool overflow(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user)
{
    (void)handle; (void)event; (void)user;
    portENTER_CRITICAL_ISR(&stats_lock);
    ++stats.dma_overflows;
    portEXIT_CRITICAL_ISR(&stats_lock);
    return false;
}
void audio_input_get_stats(audio_input_stats_t *out)
{
    portENTER_CRITICAL(&stats_lock);
    *out = stats;
    portEXIT_CRITICAL(&stats_lock);
}
static esp_err_t codec_init(void)
{
    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(board_i2c_get_bus(&bus), TAG, "I2C bus");
    s_codec = es8311_create(bus, ES8311_ADDRRES_0);
    ESP_RETURN_ON_FALSE(s_codec, ESP_ERR_NO_MEM, TAG, "codec allocation");
    const es8311_clock_config_t clock = {
        .mclk_from_mclk_pin = true,
        .mclk_frequency = BOARD_CODEC_SAMPLE_RATE_HZ * BOARD_AUDIO_MCLK_MULTIPLE,
        .sample_frequency = BOARD_CODEC_SAMPLE_RATE_HZ,
    };
    ESP_RETURN_ON_ERROR(es8311_init(s_codec, &clock, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16), TAG, "codec init");
    ESP_RETURN_ON_ERROR(es8311_sample_frequency_config(s_codec, clock.mclk_frequency, clock.sample_frequency), TAG, "codec rate");
    ESP_RETURN_ON_ERROR(es8311_voice_volume_set(s_codec, 70, NULL), TAG, "volume");
    ESP_RETURN_ON_ERROR(es8311_microphone_config(s_codec, false), TAG, "analog input");
    return es8311_microphone_gain_set(s_codec, ES8311_MIC_GAIN_24DB);
}
esp_err_t audio_input_init(void)
{
    _Static_assert(BOARD_CODEC_SAMPLE_RATE_HZ == 16000 && BOARD_MODEM_SAMPLE_RATE_HZ == 8000,
                   "The anti-alias filter is designed for 16 kHz to 8 kHz");
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_AUDIO_I2S_NUM, I2S_ROLE_MASTER);
    chan.dma_desc_num = 8;
    chan.dma_frame_num = 256; /* 128 ms DMA reserve, stereo 16-bit. */
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, NULL, &s_rx_handle), TAG, "I2S channel");
    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_CODEC_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_AUDIO_I2S_MCLK, .bclk = BOARD_AUDIO_I2S_BCLK,
            .ws = BOARD_AUDIO_I2S_WS, .dout = BOARD_AUDIO_I2S_DOUT, .din = BOARD_AUDIO_I2S_DIN,
        },
    };
    config.clk_cfg.mclk_multiple = BOARD_AUDIO_MCLK_MULTIPLE;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_handle, &config), TAG, "I2S init");
    const i2s_event_callbacks_t callbacks = {.on_recv_q_ovf = overflow};
    ESP_RETURN_ON_ERROR(i2s_channel_register_event_callback(s_rx_handle, &callbacks, NULL), TAG, "I2S callbacks");
    /* Codec configuration needs the master clock running. */
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_handle), TAG, "I2S enable");
    esp_err_t result = codec_init();
    if (result != ESP_OK) { i2s_channel_disable(s_rx_handle); return result; }
    portENTER_CRITICAL(&stats_lock);
    stats = (audio_input_stats_t){0};
    portEXIT_CRITICAL(&stats_lock);
    ESP_LOGI(TAG, "ES8311 16000 Hz -> FIR -> 8000 Hz, slot=%s", BOARD_AUDIO_SLOT_LEFT ? "left" : "right");
    return ESP_OK;
}
size_t audio_input_read(int16_t *samples, size_t max_samples, uint32_t timeout_ms)
{
    if (!samples || !max_samples || !s_rx_handle) return 0;
    if (max_samples > 256) max_samples = 256;
    int16_t raw[1024];
    size_t bytes = 0;
    esp_err_t error = i2s_channel_read(s_rx_handle, raw, max_samples * 8, &bytes, timeout_ms);
    const size_t frames = bytes / 4;
    const int64_t started = esp_timer_get_time();
    uint32_t clips = 0;
    /* Keep partial timeout data; never switch slots at block boundaries. */
    for (size_t n = 0; n < frames; ++n) {
        int32_t sample = raw[2 * n + (BOARD_AUDIO_SLOT_LEFT ? 0 : 1)];
        if (sample >= 32760 || sample <= -32760) ++clips;
        raw[n] = (int16_t)sample; /* Safe in-place stereo -> mono. */
    }
    const size_t produced = audio_decimator_feed(&decimator, raw, frames, samples);
    for (size_t n = 0; n < produced; ++n) {
        int32_t value = samples[n] * BOARD_AUDIO_SOFTWARE_GAIN;
        if (value > INT16_MAX) value = INT16_MAX;
        if (value < INT16_MIN) value = INT16_MIN;
        samples[n] = (int16_t)value;
    }
    portENTER_CRITICAL(&stats_lock);
    stats.read_errors += error != ESP_OK;
    stats.clipped_samples += clips;
    stats.processing_us = (uint32_t)(esp_timer_get_time() - started);
    portEXIT_CRITICAL(&stats_lock);
    return produced;
}
