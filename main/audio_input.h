#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t audio_input_init(void);
size_t audio_input_read(int16_t *samples, size_t max_samples, uint32_t timeout_ms);

typedef struct {
    uint32_t dma_overflows, read_errors, clipped_samples, processing_us;
} audio_input_stats_t;
void audio_input_get_stats(audio_input_stats_t *stats);
