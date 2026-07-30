#include "audio_input.h"

#include <string.h>

#include "board_config.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "es8311.h"

static const char *TAG = "audio";
static i2s_chan_handle_t s_rx_handle;
static es8311_handle_t s_codec;

static esp_err_t codec_i2c_init(void)
{
    s_codec = es8311_create(BOARD_AUDIO_I2C_PORT, ES8311_ADDRRES_0);
    ESP_RETURN_ON_FALSE(s_codec, ESP_FAIL, TAG, "es8311 create failed");

    const es8311_clock_config_t es_clk = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = BOARD_SAMPLE_RATE_HZ * BOARD_AUDIO_MCLK_MULTIPLE,
        .sample_frequency = BOARD_SAMPLE_RATE_HZ,
    };

    ESP_RETURN_ON_ERROR(es8311_init(s_codec, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16), TAG, "es8311 init");
    ESP_RETURN_ON_ERROR(es8311_sample_frequency_config(s_codec, BOARD_SAMPLE_RATE_HZ * BOARD_AUDIO_MCLK_MULTIPLE, BOARD_SAMPLE_RATE_HZ),
                        TAG, "es8311 sample rate");
    ESP_RETURN_ON_ERROR(es8311_microphone_config(s_codec, false), TAG, "es8311 mic config");
    ESP_RETURN_ON_ERROR(es8311_microphone_gain_set(s_codec, ES8311_MIC_GAIN_6DB), TAG, "es8311 mic gain");
    return ESP_OK;
}

static esp_err_t i2s_driver_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_AUDIO_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx_handle), TAG, "i2s channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_AUDIO_I2S_MCLK,
            .bclk = BOARD_AUDIO_I2S_BCLK,
            .ws = BOARD_AUDIO_I2S_WS,
            .dout = BOARD_AUDIO_I2S_DOUT,
            .din = BOARD_AUDIO_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = BOARD_AUDIO_MCLK_MULTIPLE;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_handle, &std_cfg), TAG, "i2s init");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_handle), TAG, "i2s enable");
    return ESP_OK;
}

esp_err_t audio_input_init(void)
{
    ESP_RETURN_ON_ERROR(i2s_driver_init(), TAG, "i2s driver init");
    ESP_RETURN_ON_ERROR(codec_i2c_init(), TAG, "codec init");
    ESP_LOGI(TAG, "ES8311/I2S audio backend ready");
    return ESP_OK;
}

size_t audio_input_read(int16_t *samples, size_t max_samples, uint32_t timeout_ms)
{
    if (!samples || max_samples == 0) {
        return 0;
    }

    if (max_samples > 256) {
        max_samples = 256;
    }

    int16_t raw[512];
    size_t bytes_read = 0;
    const size_t bytes_requested = max_samples * sizeof(raw[0]) * 2;
    const esp_err_t err = i2s_channel_read(s_rx_handle, raw, bytes_requested, &bytes_read, timeout_ms);
    if (err != ESP_OK || bytes_read < sizeof(int16_t) * 2) {
        return 0;
    }

    const size_t frames = bytes_read / (sizeof(int16_t) * 2);
    const size_t out_count = frames < max_samples ? frames : max_samples;
    for (size_t i = 0; i < out_count; ++i) {
        const int32_t left = raw[i * 2];
        const int32_t right = raw[i * 2 + 1];
        samples[i] = (int16_t)((left + right) / 2);
    }
    return out_count;
}
