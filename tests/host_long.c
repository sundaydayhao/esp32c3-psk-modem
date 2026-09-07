#include "bpsk_rx.h"
#include "audio_decimator.h"
#include "psk_tx.h"
#include <stdio.h>
#include <string.h>

static char expected[4096], received[8192];
static size_t cursor, used;
static int fetch(void *user) { (void)user; return expected[cursor] ? (unsigned char)expected[cursor++] : -1; }
static void output(char c, void *user) { (void)user; if (used + 1 < sizeof(received)) received[used++] = c; received[used] = 0; }
int main(void)
{
    const unsigned periods[] = {256,128,64,32,16,8};
    const psk_mode_t modes[] = {PSK_MODE_BPSK31,PSK_MODE_BPSK63,PSK_MODE_BPSK125,PSK_MODE_BPSK250,PSK_MODE_BPSK500,PSK_MODE_BPSK1000};
    strcpy(expected, "BEGIN-LONG-STREAM\n");
    unsigned random = 0x31415926;
    size_t prefix = strlen(expected);
    for (size_t n = prefix; n < sizeof(expected)-32; ++n) {
        random = random * 1664525U + 1013904223U;
        expected[n] = n % 73 ? (char)(32 + (random >> 16) % 95) : '\n';
    }
    strcpy(expected + sizeof(expected)-32, "\nEND-LONG-STREAM\n");
    unsigned failures = 0;
    for (unsigned mode = 0; mode < 6; ++mode) {
        for (unsigned variant = 0; variant < 3; ++variant) {
            psk_tx_t tx; bpsk_rx_t rx; audio_decimator_t decimator = {0};
            int16_t pcm[511], audio[256];
            cursor = used = 0; received[0] = 0;
            psk_tx_init(&tx, 16000);
            psk_tx_start_stream(&tx, modes[mode], 1500, fetch, NULL);
            tx.preamble_left = 256;
            tx.symbol_period_samples *= variant == 1 ? 1.0001f : variant == 2 ? .9999f : 1.0f;
            bpsk_rx_init(&rx, periods[mode], 1500, output, NULL);
            size_t samples = 0;
            while (psk_tx_active(&tx) && samples < 16000U * 2000U) {
                size_t n = psk_tx_generate(&tx, pcm, 511);
                if (!n) break;
                size_t count = audio_decimator_feed(&decimator, pcm, n, audio);
                bpsk_rx_feed(&rx, audio, count);
                samples += n;
            }
            memset(audio, 0, sizeof(audio));
            for (unsigned i = 0; i < 16; ++i) bpsk_rx_feed(&rx, audio, 256);
            const char *start = strstr(received, "BEGIN-LONG-STREAM\n");
            const int ok = start && strncmp(start, expected, strlen(expected)) == 0;
            printf("LONG %s ppm=%d chars=%zu %s\n", psk_mode_name(modes[mode]), variant == 1 ? 100 : variant == 2 ? -100 : 0, strlen(expected), ok ? "PASS" : "FAIL");
            if (!ok) ++failures;
        }
    }
    return failures ? 1 : 0;
}
