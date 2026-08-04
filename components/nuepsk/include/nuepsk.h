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
    float x;
    float y;
} nuepsk_complex_t;

typedef struct {
    nuepsk_config_t config;
    float nco_phase;
    float nco_phase_inc;
    float nco_cos;
    float nco_sin;
    float freq_error;
    float afc_limit;
    float afc_min;
    float afc_max;
    float fferr_ave;
    float fperr_ave;
    float q_freq_error;
    float nlp_k;
    float agc_ave;
    float dev_ave;
    float bit_phase_inc;
    float bit_phase_pos;
    float prev_symbol_phase;
    float symbol_clock;
    float symbol_period_samples;
    float i_acc;
    float q_acc;
    float sync_ave[21];
    float i0;
    float i1;
    float q0;
    float q1;
    nuepsk_complex_t dec1_queue[35];
    nuepsk_complex_t dec2_queue[35];
    nuepsk_complex_t bit_queue[65];
    nuepsk_complex_t freq_signal;
    nuepsk_complex_t bit_signal;
    nuepsk_complex_t z1;
    nuepsk_complex_t z2;
    int samples_in_symbol;
    int sample_count;
    int afc_timer;
    int dec1_state;
    int dec2_state;
    int bit_fir_state;
    int pk_pos;
    int new_pk_pos;
    int raw_pk_pos;
    int bit_pos;
    int warmup_symbols;
    int last_pk_pos;
    int sq_open;
    int sq_was_open;
    int sq_stable_symbols;
    int sq_level;
    int sq_thresh;
    int squelch_speed;
    int on_count;
    int off_count;
    int p_count;
    int n_count;
    uint16_t bit_acc;
    int last_bit_zero;
    int rtty_figures;
    int rtty_bit_index;
    uint8_t rtty_shift_reg;
    uint32_t debug_bits;
    uint32_t debug_symbols;
    uint32_t debug_ones;
    uint32_t debug_zeros;
    uint32_t debug_delims;
    uint32_t debug_chars;
    uint32_t debug_unknown;
} nuepsk_rx_t;

void nuepsk_rx_init(nuepsk_rx_t *rx, const nuepsk_config_t *config);
void nuepsk_rx_set_mode(nuepsk_rx_t *rx, nuepsk_mode_t mode);
void nuepsk_rx_set_tone(nuepsk_rx_t *rx, int tone_hz);
void nuepsk_rx_feed(nuepsk_rx_t *rx, const int16_t *samples, size_t count);
