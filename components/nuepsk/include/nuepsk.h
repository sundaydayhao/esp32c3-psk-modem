#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum {
    NUEPSK_MODE_BPSK31 = 0,
    NUEPSK_MODE_QPSK31_USB,
    NUEPSK_MODE_QPSK31_LSB,
    NUEPSK_MODE_RTTY,
    NUEPSK_MODE_PSK63,
    NUEPSK_MODE_COUNT
} nuepsk_mode_t;

typedef void (*nuepsk_char_cb_t)(char c, void *userdata);

typedef struct {
    nuepsk_mode_t mode;
    int sample_rate_hz;
    int rx_tone_hz;
    nuepsk_char_cb_t char_cb;
    void *userdata;
} nuepsk_config_t;

typedef struct {
    nuepsk_config_t config;
    float nco_phase;
    float prev_symbol_phase;
    float symbol_clock;
    float symbol_period_samples;
    float i_acc;
    float q_acc;
    int samples_in_symbol;
    uint16_t bit_acc;
    int last_bit_zero;
    int rtty_figures;
    int rtty_bit_index;
    uint8_t rtty_shift_reg;
} nuepsk_rx_t;

void nuepsk_rx_init(nuepsk_rx_t *rx, const nuepsk_config_t *config);
void nuepsk_rx_set_mode(nuepsk_rx_t *rx, nuepsk_mode_t mode);
void nuepsk_rx_set_tone(nuepsk_rx_t *rx, int tone_hz);
void nuepsk_rx_feed(nuepsk_rx_t *rx, const int16_t *samples, size_t count);
