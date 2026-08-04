#include "audio_input.h"

#include <math.h>
#include <string.h>

#include "board_config.h"
#include "board_i2c.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "es8311.h"

static const char *TAG = "audio";
static i2s_chan_handle_t s_rx_handle;
static es8311_handle_t s_codec;

static esp_err_t codec_i2c_init(void)
{
    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_RETURN_ON_ERROR(board_i2c_get_bus(&i2c_bus), TAG, "i2c bus");

    s_codec = es8311_create(i2c_bus, ES8311_ADDRRES_0);
    ESP_RETURN_ON_FALSE(s_codec, ESP_FAIL, TAG, "es8311 create failed");

    const es8311_clock_config_t es_clk = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = BOARD_CODEC_SAMPLE_RATE_HZ * BOARD_AUDIO_MCLK_MULTIPLE,
        .sample_frequency = BOARD_CODEC_SAMPLE_RATE_HZ,
    };

    ESP_RETURN_ON_ERROR(es8311_init(s_codec, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16), TAG, "es8311 init");
    ESP_RETURN_ON_ERROR(es8311_sample_frequency_config(s_codec, BOARD_CODEC_SAMPLE_RATE_HZ * BOARD_AUDIO_MCLK_MULTIPLE,
                                                       BOARD_CODEC_SAMPLE_RATE_HZ),
                        TAG, "es8311 sample rate");
    ESP_RETURN_ON_ERROR(es8311_voice_volume_set(s_codec, 70, NULL), TAG, "es8311 volume");
    ESP_RETURN_ON_ERROR(es8311_microphone_config(s_codec, false), TAG, "es8311 mic config");
    ESP_RETURN_ON_ERROR(es8311_microphone_gain_set(s_codec, ES8311_MIC_GAIN_24DB), TAG, "es8311 mic gain");
    return ESP_OK;
}

static esp_err_t i2s_driver_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_AUDIO_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx_handle), TAG, "i2s channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_CODEC_SAMPLE_RATE_HZ),
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
    ESP_LOGI(TAG, "ES8311/I2S audio backend ready: codec %d Hz, modem %d Hz",
             BOARD_CODEC_SAMPLE_RATE_HZ, BOARD_MODEM_SAMPLE_RATE_HZ);
    return ESP_OK;
}

size_t audio_input_read(int16_t *samples, size_t max_samples, uint32_t timeout_ms)
{
    if (!samples || max_samples == 0) {
        return 0;
    }

    const size_t decim = BOARD_CODEC_SAMPLE_RATE_HZ / BOARD_MODEM_SAMPLE_RATE_HZ;
    if (decim == 0 || BOARD_CODEC_SAMPLE_RATE_HZ % BOARD_MODEM_SAMPLE_RATE_HZ != 0) {
        ESP_LOGE(TAG, "unsupported codec/modem sample rates");
        return 0;
    }

    if (max_samples > 256) {
        max_samples = 256;
    }

    int16_t raw[1024];
    size_t bytes_read = 0;
    const size_t bytes_requested = max_samples * decim * sizeof(raw[0]) * 2;
    const esp_err_t err = i2s_channel_read(s_rx_handle, raw, bytes_requested, &bytes_read, timeout_ms);
    if (err != ESP_OK || bytes_read < sizeof(int16_t) * 2) {
        return 0;
    }

    const size_t frames = bytes_read / (sizeof(int16_t) * 2);
    const size_t out_count = (frames / decim) < max_samples ? (frames / decim) : max_samples;
    int16_t l_min = INT16_MAX;
    int16_t l_max = INT16_MIN;
    int16_t r_min = INT16_MAX;
    int16_t r_max = INT16_MIN;
    int64_t l_abs_sum = 0;
    int64_t r_abs_sum = 0;
    int64_t l_sum_sq = 0;
    int64_t r_sum_sq = 0;
    size_t l_clip_count = 0;
    size_t r_clip_count = 0;

    for (size_t i = 0; i < frames; ++i) {
        const int16_t left = raw[i * 2];
        const int16_t right = raw[i * 2 + 1];
        if (left < l_min) {
            l_min = left;
        }
        if (left > l_max) {
            l_max = left;
        }
        if (right < r_min) {
            r_min = right;
        }
        if (right > r_max) {
            r_max = right;
        }
        l_abs_sum += left < 0 ? -(int32_t)left : left;
        r_abs_sum += right < 0 ? -(int32_t)right : right;
        l_sum_sq += (int32_t)left * (int32_t)left;
        r_sum_sq += (int32_t)right * (int32_t)right;
        if (left <= INT16_MIN + 1 || left >= INT16_MAX - 1) {
            l_clip_count++;
        }
        if (right <= INT16_MIN + 1 || right >= INT16_MAX - 1) {
            r_clip_count++;
        }
    }

    const bool use_left = l_abs_sum >= r_abs_sum;
    int16_t s_min = INT16_MAX;
    int16_t s_max = INT16_MIN;
    int64_t s_abs_sum = 0;
    int64_t s_sum_sq = 0;
    size_t s_clip_count = 0;
    for (size_t i = 0; i < out_count; ++i) {
        int32_t acc = 0;
        for (size_t d = 0; d < decim; ++d) {
            const size_t frame = i * decim + d;
            acc += use_left ? raw[frame * 2] : raw[frame * 2 + 1];
        }
        int32_t sample32 = (acc / (int32_t)decim) * BOARD_AUDIO_SOFTWARE_GAIN;
        if (sample32 > INT16_MAX) {
            sample32 = INT16_MAX;
        } else if (sample32 < INT16_MIN) {
            sample32 = INT16_MIN;
        }
        const int16_t sample = (int16_t)sample32;
        samples[i] = sample;
        if (sample < s_min) {
            s_min = sample;
        }
        if (sample > s_max) {
            s_max = sample;
        }
        s_abs_sum += sample < 0 ? -(int32_t)sample : sample;
        s_sum_sq += (int32_t)sample * (int32_t)sample;
        if (sample <= INT16_MIN + 1 || sample >= INT16_MAX - 1) {
            s_clip_count++;
        }
    }

#if BOARD_AUDIO_DEBUG_LOGS
    static int64_t last_log_us;
    const int64_t now_us = esp_timer_get_time();
    if (now_us - last_log_us >= 1000000) {
        last_log_us = now_us;
        const int32_t l_rms = frames ? (int32_t)sqrt((double)l_sum_sq / (double)frames) : 0;
        const int32_t r_rms = frames ? (int32_t)sqrt((double)r_sum_sq / (double)frames) : 0;
        const int32_t s_rms = out_count ? (int32_t)sqrt((double)s_sum_sq / (double)out_count) : 0;
        ESP_LOGI(TAG, "rx bytes=%u frames=%u ch=%c L[%d,%d] abs=%d rms=%d clip=%u R[%d,%d] abs=%d rms=%d clip=%u S[%d,%d] abs=%d rms=%d clip=%u",
                 (unsigned)bytes_read, (unsigned)frames, use_left ? 'L' : 'R',
                 l_min, l_max, frames ? (int)(l_abs_sum / (int64_t)frames) : 0, (int)l_rms, (unsigned)l_clip_count,
                 r_min, r_max, frames ? (int)(r_abs_sum / (int64_t)frames) : 0, (int)r_rms, (unsigned)r_clip_count,
                 s_min, s_max, out_count ? (int)(s_abs_sum / (int64_t)out_count) : 0, (int)s_rms, (unsigned)s_clip_count);
    }
#endif
    return out_count;
}
