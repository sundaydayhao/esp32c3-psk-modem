#include "audio_input.h"

#include <string.h>

#include "board_config.h"
#include "esp_adc/adc_continuous.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "audio";
static adc_continuous_handle_t s_adc;

esp_err_t audio_input_init(void)
{
    adc_continuous_handle_cfg_t handle_cfg = {
        .max_store_buf_size = 4096,
        .conv_frame_size = 512,
    };
    ESP_RETURN_ON_ERROR(adc_continuous_new_handle(&handle_cfg, &s_adc), TAG, "adc handle");

    adc_continuous_config_t dig_cfg = {
        .sample_freq_hz = BOARD_SAMPLE_RATE_HZ,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
    };

    adc_digi_pattern_config_t pattern = {
        .atten = BOARD_AUDIO_ADC_ATTEN,
        .channel = BOARD_AUDIO_ADC_CHANNEL,
        .unit = BOARD_AUDIO_ADC_UNIT,
        .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,
    };
    dig_cfg.pattern_num = 1;
    dig_cfg.adc_pattern = &pattern;

    ESP_RETURN_ON_ERROR(adc_continuous_config(s_adc, &dig_cfg), TAG, "adc config");
    ESP_RETURN_ON_ERROR(adc_continuous_start(s_adc), TAG, "adc start");
    return ESP_OK;
}

size_t audio_input_read(int16_t *samples, size_t max_samples, uint32_t timeout_ms)
{
    uint8_t raw[512];
    uint32_t got = 0;
    const esp_err_t err = adc_continuous_read(s_adc, raw, sizeof(raw), &got, timeout_ms);
    if (err != ESP_OK || got == 0) {
        return 0;
    }

    size_t out = 0;
    for (uint32_t offset = 0; offset + sizeof(adc_digi_output_data_t) <= got && out < max_samples;
         offset += sizeof(adc_digi_output_data_t)) {
        adc_digi_output_data_t data;
        memcpy(&data, raw + offset, sizeof(data));
        const int raw_sample = data.type2.data;
        samples[out++] = (int16_t)((raw_sample - 2048) << 4);
    }
    return out;
}
