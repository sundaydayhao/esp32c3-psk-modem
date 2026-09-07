#include "audio_decimator.h"
#include "rx_history.h"
#include "modem.h"
#include "spectrum.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    rx_history_t h = {0};
    uint8_t out[4096];
    uint64_t next, dropped;
    assert(rx_history_read(&h, 0, out, sizeof(out), &next, &dropped) == 0);
    char data[5000];
    for (unsigned i = 0; i < sizeof(data); ++i) data[i] = (char)(i % 251);
    rx_history_append(&h, data, 5000);
    assert(rx_history_read(&h, 0, out, 1000, &next, &dropped) == 1000);
    assert(next == 1904 && dropped == 904 && memcmp(out, data + 904, 1000) == 0);
    assert(rx_history_read(&h, 1904, out, sizeof(out), &next, &dropped) == 3096);
    assert(next == 5000 && dropped == 0 && memcmp(out, data + 1904, 3096) == 0);
    assert(rx_history_read(&h, 5000, out, sizeof(out), &next, &dropped) == 0);
    assert(rx_history_read(&h, 9999, out, sizeof(out), &next, &dropped) == 4096);
    assert(next == 5000 && memcmp(out, data + 904, 4096) == 0);
    /* A second client can reread without consuming the first client's data. */
    assert(rx_history_read(&h, 4500, out, sizeof(out), &next, &dropped) == 500);
    assert(memcmp(out, data + 4500, 500) == 0);

    audio_decimator_t a = {0}, b = {0};
    int16_t pcm[1025], whole[513], chunks[513];
    for (unsigned i = 0; i < 1025; ++i) pcm[i] = (int16_t)(i * 113);
    size_t total = audio_decimator_feed(&a, pcm, 1025, whole), produced = 0;
    for (unsigned i = 0; i < 1025; ++i) produced += audio_decimator_feed(&b, pcm + i, 1, chunks + produced);
    assert(produced == total && memcmp(whole, chunks, total * sizeof(int16_t)) == 0);

    bpsk_rx_t rx;
    assert(!bpsk_rx_init(&rx, 7, 1500, NULL, NULL));
    assert(!bpsk_rx_init(&rx, 16, -1, NULL, NULL));
    assert(!modem_mode_supported((modem_mode_t)-1));
    assert(!modem_mode_supported(MODEM_MODE_QPSK31_USB));
    modem_t modem;
    modem_config_t config = {.mode=MODEM_MODE_BPSK1000,.rx_tone_hz=100,.sample_rate_hz=8000};
    modem_init(&modem, &config);
    assert(modem_get_rx_tone(&modem) == 1100);
    modem_set_mode(&modem, (modem_mode_t)-1);
    assert(modem_get_mode(&modem) == MODEM_MODE_BPSK1000);
    memset(pcm, 0, sizeof(pcm));
    char text[96];
    for (unsigned i = 0; i < 100; ++i) assert(modem_process(&modem, pcm, 256, text, sizeof(text)) == 0);
    assert(!modem.rx.signal);

    for (unsigned i = 0; i < 256; ++i) pcm[i] = (int16_t)(16000 * sin(6.28318530718 * 1500 * i / 8000));
    spectrum_t spectrum;
    spectrum_analyze(pcm, 256, 8000, &spectrum);
    assert(spectrum.peak_hz >= 1450 && spectrum.peak_hz <= 1550);
    puts("PASS: history wrap, independent cursors, stale cursors, decimator chunking, mode validation, silence, FFT");
    return 0;
}
