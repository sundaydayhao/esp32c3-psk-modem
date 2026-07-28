#pragma once

#include <stddef.h>
#include <stdint.h>

#include "nuepsk.h"

typedef enum {
    MODEM_MODE_BPSK31 = 0,
    MODEM_MODE_QPSK31_USB,
    MODEM_MODE_QPSK31_LSB,
    MODEM_MODE_RTTY,
    MODEM_MODE_PSK63,
    MODEM_MODE_COUNT
} modem_mode_t;

typedef struct {
    modem_mode_t mode;
    int sample_rate_hz;
    int rx_tone_hz;
    int squelch;
} modem_config_t;

typedef struct modem modem_t;

void modem_init(modem_t *m, const modem_config_t *config);
void modem_set_mode(modem_t *m, modem_mode_t mode);
void modem_set_rx_tone(modem_t *m, int tone_hz);
modem_mode_t modem_get_mode(const modem_t *m);
const char *modem_mode_name(modem_mode_t mode);
int modem_get_rx_tone(const modem_t *m);
size_t modem_process(modem_t *m, const int16_t *samples, size_t count, char *out, size_t out_size);

struct modem {
    modem_config_t config;
    float phase;
    float bit_clock;
    uint32_t sample_counter;
    uint32_t demo_counter;
    nuepsk_rx_t nue_rx;
    char *out;
    size_t out_size;
    size_t out_len;
};
