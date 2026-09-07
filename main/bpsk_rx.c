#include "bpsk_rx.h"

#include <stdlib.h>
#include <string.h>

#include "PSKTables.h"
#include "bpsk_tables.h"

bool bpsk_rx_init(bpsk_rx_t *rx, unsigned sps, int tone_hz,
                  bpsk_char_cb_t callback, void *user)
{
    if (!rx || (sps != 256 && sps != 128 && sps != 64 &&
                sps != 32 && sps != 16 && sps != 8) ||
        tone_hz < 100 || tone_hz > 3500) {
        return false;
    }
    memset(rx, 0, sizeof(*rx));
    rx->phase_step = (uint32_t)(((uint64_t)(unsigned)tone_hz << 32) / 8000);
    rx->decimation = sps > 16 ? sps / 16 : 1;
    rx->bins = sps / rx->decimation;
    rx->warmup = 64;
    rx->callback = callback;
    rx->user = user;
    return true;
}

static void reset_code(bpsk_rx_t *rx)
{
    rx->code = 0;
    rx->code_bits = 0;
    rx->zeros = 0;
}

static void push_bit(bpsk_rx_t *rx, bool bit)
{
    if (!bit) {
        if (++rx->zeros == 2) {
            if (rx->code_bits > 0 && rx->code_bits <= 12) {
                bool found = false;
                for (unsigned c = 1; c < 256; ++c) {
                    if (VARICODE_TABLE[c] == rx->code) {
                        if (rx->callback) rx->callback((char)c, rx->user);
                        ++rx->characters;
                        found = true;
                        break;
                    }
                }
                if (!found) ++rx->invalid_codes;
            } else if (rx->code_bits > 12) {
                ++rx->invalid_codes;
            }
            rx->code = 0;
            rx->code_bits = 0;
        }
        if (rx->zeros > 2) rx->zeros = 2;
        return;
    }
    if (rx->zeros == 1 && rx->code_bits) {
        rx->code <<= 1;
        if (rx->code_bits < 13) ++rx->code_bits;
    }
    rx->zeros = 0;
    rx->code = (uint16_t)((rx->code << 1) | 1);
    if (rx->code_bits < 13) ++rx->code_bits;
}

static void symbol(bpsk_rx_t *rx, int32_t i, int32_t q)
{
    /* Input headroom bounds the products well inside int32_t. */
    const int32_t dot = i * rx->prev_i + q * rx->prev_q;
    const int32_t cross = i * rx->prev_q - q * rx->prev_i;
    rx->prev_i = i;
    rx->prev_q = q;
    ++rx->symbols;
    const uint32_t d = (uint32_t)abs(dot) >> 8;
    const uint32_t c = (uint32_t)abs(cross) >> 8;
    const unsigned quality = d + c ? (100 * d) / (d + c) : 0;
    rx->quality = (7 * rx->quality + quality) / 8;
    rx->signal = abs(i) + abs(q) >= 20 && rx->quality >= 55;
    if (rx->warmup) {
        --rx->warmup;
        reset_code(rx);
    } else if (rx->signal) {
        push_bit(rx, dot >= 0);
    } else {
        reset_code(rx);
    }
}

static void filtered_sample(bpsk_rx_t *rx, int16_t i, int16_t q)
{
    rx->history_i[rx->head] = i;
    rx->history_q[rx->head] = q;
    int32_t fi = 0, fq = 0;
    unsigned pos = rx->head;
    /* The same 65-tap matched filter and timing concept as the HPM
     * single-carrier path, quantized to Q15. No float FIR or libm calls. */
    const unsigned taps = rx->bins == 8 ? 33 : 65;
    const int16_t *coefficients = rx->bins == 8 ? bpsk_fir8_q15 : bpsk_fir_q15;
    for (unsigned tap = 0; tap < taps; ++tap) {
        fi += rx->history_i[pos] * coefficients[tap];
        fq += rx->history_q[pos] * coefficients[tap];
        if (++pos == taps) pos = 0;
    }
    rx->head = rx->head ? rx->head - 1 : taps - 1;
    fi >>= 15;
    fq >>= 15;

    const unsigned index = (unsigned)rx->clock_q16 >> 16;
    const int32_t energy = (fi * fi + fq * fq) >> 8;
    rx->timing_energy[index] += (energy - rx->timing_energy[index]) / 5;
    int32_t diff = 0, sum = 0;
    for (unsigned p = 0; p < rx->bins / 2; ++p) {
        diff += rx->timing_energy[p] - rx->timing_energy[p + rx->bins / 2];
        sum += rx->timing_energy[p] + rx->timing_energy[p + rx->bins / 2];
    }
    const int32_t error_q8 = sum ? (diff * 256) / sum : 0;
    rx->clock_q16 += 65536 - (error_q8 * (int32_t)rx->bins * 256) / 80;
    if (rx->clock_q16 >= (int32_t)(rx->bins << 16)) {
        rx->clock_q16 -= (int32_t)(rx->bins << 16);
        symbol(rx, fi, fq);
    }
}

void bpsk_rx_feed(bpsk_rx_t *rx, const int16_t *samples, size_t count)
{
    for (size_t n = 0; n < count; ++n) {
        const unsigned phase = rx->phase >> 22;
        /* Divide mixer amplitude by eight: FIR L1 gain < 1.6, so
         * FIR, energy, dot and cross accumulators cannot overflow. */
        rx->mix_i += (samples[n] * bpsk_sine_q15[(phase + 256) & 1023]) >> 18;
        rx->mix_q += (samples[n] * bpsk_sine_q15[phase]) >> 18;
        rx->phase += rx->phase_step;
        if (++rx->decim_count == rx->decimation) {
            filtered_sample(rx, (int16_t)(rx->mix_i / (int32_t)rx->decimation),
                            (int16_t)(rx->mix_q / (int32_t)rx->decimation));
            rx->mix_i = rx->mix_q = 0;
            rx->decim_count = 0;
        }
    }
}
