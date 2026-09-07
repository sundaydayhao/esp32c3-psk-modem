#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "modem.h"

typedef struct {
    int mode; /* -1 leaves mode unchanged */
    int tone_hz; /* 0 leaves frequency unchanged */
} modem_command_t;

typedef struct {
    modem_mode_t mode;
    int tone_hz;
    unsigned quality;
    bool signal, audio_ready;
    uint32_t samples, blocks, dsp_us, max_dsp_us, deadline_misses;
    uint32_t dma_overflows, read_errors, clipped_samples, output_drops, lcd_drops;
    uint32_t rms, invalid_codes, control_revision;
} receiver_status_t;

void receiver_append(const char *data, size_t count);
void receiver_update_status(const receiver_status_t *status);
void receiver_get_status(receiver_status_t *status);
size_t receiver_read(uint64_t since, uint8_t *out, size_t capacity,
                     uint64_t *next, uint64_t *dropped);
