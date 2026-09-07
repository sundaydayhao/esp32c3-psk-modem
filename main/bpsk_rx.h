#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Single carrier, 8 kHz PCM. Runtime DSP uses integer arithmetic only. */
typedef void (*bpsk_char_cb_t)(char value, void *user);
typedef struct {
    uint32_t phase, phase_step;
    int32_t mix_i, mix_q;
    int16_t history_i[65], history_q[65];
    int32_t timing_energy[16];
    int32_t clock_q16;
    int32_t prev_i, prev_q;
    uint16_t code;
    unsigned code_bits, zeros;
    unsigned decimation, decim_count, bins, head, warmup;
    unsigned quality, symbols, characters, invalid_codes;
    bool signal;
    bpsk_char_cb_t callback;
    void *user;
} bpsk_rx_t;

bool bpsk_rx_init(bpsk_rx_t *rx, unsigned samples_per_symbol, int tone_hz,
                  bpsk_char_cb_t callback, void *user);
void bpsk_rx_feed(bpsk_rx_t *rx, const int16_t *samples, size_t count);
