#include "bpsk_rx.h"
#include "audio_decimator.h"
#include "psk_tx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char received[8192];
static size_t used;
static void output(char c, void *user) {
    (void)user;
    if (used + 1 < sizeof(received)) received[used++] = c;
    received[used] = 0;
}
int main(void) {
    const unsigned periods[] = {256, 128, 64, 32, 16, 8};
    const psk_mode_t modes[] = {PSK_MODE_BPSK31, PSK_MODE_BPSK63, PSK_MODE_BPSK125,
                                PSK_MODE_BPSK250, PSK_MODE_BPSK500, PSK_MODE_BPSK1000};
    const char *payload = "\nBEGIN C3 TEST\nThe quick brown fox jumps over the lazy dog. 0123456789\n"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz\n!@#$%^&*()_+-=[]{};:'\",.<>/? \\ | ~ `\n"
        "UTF8: \xe4\xb8\xad\xe6\x96\x87\xe6\x8e\xa5\xe6\x94\xb6\nEND C3 TEST\n";
    unsigned failures = 0;
    for (unsigned mode = 0; mode < 6; ++mode) {
        for (unsigned scenario = 0; scenario < 12; ++scenario) {
            psk_tx_t tx;
            bpsk_rx_t rx;
            int16_t pcm[257];
            int16_t decimated[129];
            audio_decimator_t decimator = {0};
            const unsigned rate = scenario >= 6 ? 16000 : 8000;
            const unsigned variant = scenario % 6;
            psk_tx_init(&tx, rate);
            psk_tx_start_text(&tx, modes[mode], 1500, payload);
            /* Long acquisition preamble; also test non-integral clock drift. */
            tx.preamble_left = 256;
            if (variant == 2) tx.symbol_period_samples *= 1.0001f;
            if (variant == 3) tx.symbol_period_samples *= 0.9999f;
            if (variant == 4) tx.carrier_step[0] += 6.28318530718f * (8000.0f / periods[mode] / 32) / rate;
            if (variant == 5) tx.carrier_step[0] -= 6.28318530718f * (8000.0f / periods[mode] / 32) / rate;
            if (!bpsk_rx_init(&rx, periods[mode], 1500, output, NULL)) return 2;
            used = 0; received[0] = 0;
            size_t total = 0;
            while (psk_tx_active(&tx)) {
                const size_t chunk = scenario ? (total % 257) + 1 : 256;
                size_t n = psk_tx_generate(&tx, pcm, chunk);
                if (!n) break;
                if (variant == 1) for (size_t k = 0; k < n; ++k) pcm[k] /= 8;
                if (rate == 16000) {
                    size_t produced = audio_decimator_feed(&decimator, pcm, n, decimated);
                    bpsk_rx_feed(&rx, decimated, produced);
                } else {
                    bpsk_rx_feed(&rx, pcm, n);
                }
                total += n;
            }
            memset(pcm, 0, sizeof(pcm));
            for (int n = 0; n < 16; ++n) bpsk_rx_feed(&rx, pcm, 257);
            const char *start = strstr(received, "\nBEGIN C3 TEST\n");
            const int ok = start && strncmp(start, payload, strlen(payload)) == 0;
            printf("%s scenario=%u %s decoded=%zu invalid=%u state_bytes=%zu\n",
                psk_mode_name(modes[mode]), scenario, ok ? "PASS" : "FAIL", used, rx.invalid_codes, sizeof(rx));
            if (!ok) { ++failures; printf("RX: %s\n", received); }
        }
    }
    return failures ? 1 : 0;
}
