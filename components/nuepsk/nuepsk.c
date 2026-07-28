#include "nuepsk.h"

#include <math.h>
#include <string.h>

#include "PSKTables.h"

#define TWO_PI 6.28318530717958647692f

static unsigned char s_varicode_decode[2048];
static int s_tables_ready;

static const unsigned char s_rtty_table[0x20][2] = {
    {0, 0}, {'E', '3'}, {'\n', '\n'}, {'A', '-'}, {' ', ' '}, {'S', '\''}, {'I', '8'}, {'U', '7'},
    {'\r', '\r'}, {'D', '$'}, {'R', '4'}, {'J', 7}, {'N', ','}, {'F', '!'}, {'C', ':'}, {'K', '('},
    {'T', '5'}, {'Z', '"'}, {'L', ')'}, {'W', '2'}, {'H', '#'}, {'Y', '6'}, {'P', '0'}, {'Q', '1'},
    {'O', '9'}, {'B', '?'}, {'G', '&'}, {0, 0}, {'M', '.'}, {'X', '/'}, {0, 0}, {0, 0},
};

static void init_tables(void)
{
    if (s_tables_ready) {
        return;
    }
    memset(s_varicode_decode, 0, sizeof(s_varicode_decode));
    for (int code = 0; code < 2048; ++code) {
        for (int ch = 0; ch < 256; ++ch) {
            unsigned int word = VARICODE_TABLE[ch];
            word >>= 4;
            while ((word & 1U) == 0U && word != 0U) {
                word >>= 1;
            }
            word >>= 1;
            if (word == (unsigned int)code) {
                s_varicode_decode[code] = (unsigned char)ch;
            }
        }
    }
    s_tables_ready = 1;
}

static float symbol_rate_for_mode(nuepsk_mode_t mode)
{
    switch (mode) {
    case NUEPSK_MODE_PSK63:
        return 62.5f;
    case NUEPSK_MODE_RTTY:
        return 45.45f;
    case NUEPSK_MODE_BPSK31:
    case NUEPSK_MODE_QPSK31_USB:
    case NUEPSK_MODE_QPSK31_LSB:
    default:
        return 31.25f;
    }
}

static void reset_runtime(nuepsk_rx_t *rx)
{
    rx->nco_phase = 0.0f;
    rx->prev_symbol_phase = 0.0f;
    rx->symbol_clock = 0.0f;
    rx->symbol_period_samples = (float)rx->config.sample_rate_hz / symbol_rate_for_mode(rx->config.mode);
    rx->i_acc = 0.0f;
    rx->q_acc = 0.0f;
    rx->samples_in_symbol = 0;
    rx->bit_acc = 0;
    rx->last_bit_zero = 0;
    rx->rtty_figures = 0;
    rx->rtty_bit_index = 0;
    rx->rtty_shift_reg = 0;
}

void nuepsk_rx_init(nuepsk_rx_t *rx, const nuepsk_config_t *config)
{
    init_tables();
    memset(rx, 0, sizeof(*rx));
    rx->config = *config;
    reset_runtime(rx);
}

void nuepsk_rx_set_mode(nuepsk_rx_t *rx, nuepsk_mode_t mode)
{
    if (mode >= NUEPSK_MODE_COUNT) {
        return;
    }
    rx->config.mode = mode;
    reset_runtime(rx);
}

void nuepsk_rx_set_tone(nuepsk_rx_t *rx, int tone_hz)
{
    if (tone_hz < 100) {
        tone_hz = 100;
    } else if (tone_hz > 3500) {
        tone_hz = 3500;
    }
    rx->config.rx_tone_hz = tone_hz;
    reset_runtime(rx);
}

static void emit(nuepsk_rx_t *rx, char c)
{
    if (c && rx->config.char_cb) {
        rx->config.char_cb(c, rx->config.userdata);
    }
}

static void push_varicode_bit(nuepsk_rx_t *rx, int bit)
{
    if (bit == 0) {
        if (rx->last_bit_zero) {
            if (rx->bit_acc < 2048) {
                emit(rx, (char)s_varicode_decode[rx->bit_acc]);
            }
            rx->bit_acc = 0;
            rx->last_bit_zero = 0;
        } else {
            rx->last_bit_zero = 1;
        }
        return;
    }

    if (rx->last_bit_zero) {
        rx->bit_acc <<= 1;
        rx->last_bit_zero = 0;
    }
    rx->bit_acc = (uint16_t)(((rx->bit_acc << 1) | 1U) & 0x07ffU);
}

static void push_rtty_bit(nuepsk_rx_t *rx, int bit)
{
    if (rx->rtty_bit_index == 0) {
        if (bit == 0) {
            rx->rtty_shift_reg = 0;
            rx->rtty_bit_index = 1;
        }
        return;
    }
    if (rx->rtty_bit_index <= 5) {
        rx->rtty_shift_reg >>= 1;
        if (bit) {
            rx->rtty_shift_reg |= 0x10;
        }
        rx->rtty_bit_index++;
        return;
    }

    if (bit) {
        const uint8_t code = rx->rtty_shift_reg & 0x1f;
        if (code == 0x1b) {
            rx->rtty_figures = 1;
        } else if (code == 0x1f) {
            rx->rtty_figures = 0;
        } else {
            emit(rx, (char)s_rtty_table[code][rx->rtty_figures]);
        }
    }
    rx->rtty_bit_index = 0;
}

static void finish_symbol(nuepsk_rx_t *rx)
{
    if (rx->samples_in_symbol == 0) {
        return;
    }

    const float phase = atan2f(rx->q_acc, rx->i_acc);
    float diff = phase - rx->prev_symbol_phase;
    while (diff > (float)M_PI) {
        diff -= TWO_PI;
    }
    while (diff < -(float)M_PI) {
        diff += TWO_PI;
    }
    rx->prev_symbol_phase = phase;

    if (rx->config.mode == NUEPSK_MODE_RTTY) {
        push_rtty_bit(rx, rx->i_acc > 0.0f ? 1 : 0);
    } else {
        const int bit = fabsf(diff) > ((float)M_PI * 0.5f) ? 0 : 1;
        push_varicode_bit(rx, bit);
    }

    rx->i_acc = 0.0f;
    rx->q_acc = 0.0f;
    rx->samples_in_symbol = 0;
}

void nuepsk_rx_feed(nuepsk_rx_t *rx, const int16_t *samples, size_t count)
{
    const float phase_inc = TWO_PI * (float)rx->config.rx_tone_hz / (float)rx->config.sample_rate_hz;
    for (size_t n = 0; n < count; ++n) {
        const float s = (float)samples[n] / 32768.0f;
        rx->i_acc += s * cosf(rx->nco_phase);
        rx->q_acc += s * sinf(rx->nco_phase);
        rx->samples_in_symbol++;
        rx->nco_phase += phase_inc;
        if (rx->nco_phase >= TWO_PI) {
            rx->nco_phase -= TWO_PI;
        }

        rx->symbol_clock += 1.0f;
        if (rx->symbol_clock >= rx->symbol_period_samples) {
            rx->symbol_clock -= rx->symbol_period_samples;
            finish_symbol(rx);
        }
    }
}
