#include "spectrum.h"
#include "bpsk_tables.h"
#include <stdlib.h>
#include <string.h>

void spectrum_analyze(const int16_t *samples, size_t count, int sample_rate_hz, spectrum_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!samples || count < 2 || sample_rate_hz <= 0) return;
    if (count > 256) count = 256;
    int32_t real[256] = {0}, imag[256] = {0};
    for (unsigned n = 0; n < count; ++n) {
        unsigned reverse = 0, value = n;
        for (unsigned b = 0; b < 8; ++b) { reverse = (reverse << 1) | (value & 1); value >>= 1; }
        const unsigned phase = n * 1024 / (count - 1);
        const int32_t window = (32767 - bpsk_sine_q15[(phase + 256) & 1023]) / 2;
        real[reverse] = ((samples[n] / 4) * window) >> 15;
    }
    for (unsigned width = 2; width <= 256; width *= 2) {
        for (unsigned base = 0; base < 256; base += width) {
            for (unsigned j = 0; j < width / 2; ++j) {
                unsigned phase = j * 1024 / width;
                int32_t c = bpsk_sine_q15[(phase + 256) & 1023], s = -bpsk_sine_q15[phase];
                unsigned a = base + j, b = a + width / 2;
                int32_t tr = (real[b] * c - imag[b] * s) >> 15;
                int32_t ti = (real[b] * s + imag[b] * c) >> 15;
                real[b] = (real[a] - tr) / 2; imag[b] = (imag[a] - ti) / 2;
                real[a] = (real[a] + tr) / 2; imag[a] = (imag[a] + ti) / 2;
            }
        }
    }
    uint32_t magnitudes[64], maximum = 4;
    for (unsigned b = 0; b < 64; ++b) {
        /* Combine adjacent FFT bins for the existing 64-column display. */
        uint32_t a = abs(real[2*b]) + abs(imag[2*b]);
        uint32_t c = abs(real[2*b+1]) + abs(imag[2*b+1]);
        magnitudes[b] = a > c ? a : c;
        if (magnitudes[b] > maximum) { maximum = magnitudes[b]; out->peak_bin = b; }
    }
    for (unsigned b = 0; b < 64; ++b) out->bins[b] = magnitudes[b] * 255 / maximum;
    out->peak_hz = (2 * out->peak_bin + 1) * sample_rate_hz / 256;
}
