#include "modem.h"
#include <string.h>

static const char *const names[MODEM_MODE_COUNT] = {
    "BPSK31", "QPSK31-U", "QPSK31-L", "RTTY", "BPSK63",
    "BPSK125", "BPSK250", "BPSK500", "BPSK1000"
};
unsigned modem_samples_per_symbol(modem_mode_t mode)
{
    static const unsigned periods[MODEM_MODE_COUNT] = {256, 0, 0, 0, 128, 64, 32, 16, 8};
    return (unsigned)mode < MODEM_MODE_COUNT ? periods[mode] : 0;
}
bool modem_mode_supported(modem_mode_t mode) { return modem_samples_per_symbol(mode) != 0; }
static void on_char(char c, void *user)
{
    modem_t *m = user;
    if (m->out_len + 1 >= m->out_size) { ++m->output_drops; return; }
    m->out[m->out_len++] = c;
    m->out[m->out_len] = 0;
}
static void reset(modem_t *m)
{
    bpsk_rx_init(&m->rx, modem_samples_per_symbol(m->config.mode), m->config.rx_tone_hz, on_char, m);
}
void modem_init(modem_t *m, const modem_config_t *config)
{
    memset(m, 0, sizeof(*m));
    m->config = *config;
    if (!modem_mode_supported(m->config.mode)) m->config.mode = MODEM_MODE_BPSK500;
    modem_set_rx_tone(m, m->config.rx_tone_hz);
}
void modem_set_mode(modem_t *m, modem_mode_t mode)
{
    if (!modem_mode_supported(mode)) return;
    m->config.mode = mode;
    modem_set_rx_tone(m, m->config.rx_tone_hz);
}
void modem_set_rx_tone(modem_t *m, int tone_hz)
{
    const int baud = 8000 / (int)modem_samples_per_symbol(m->config.mode);
    const int margin = baud + 100;
    const int low = margin;
    const int high = 4000 - margin < 3500 ? 4000 - margin : 3500;
    if (tone_hz < low) tone_hz = low;
    if (tone_hz > high) tone_hz = high;
    m->config.rx_tone_hz = tone_hz;
    reset(m);
}
modem_mode_t modem_get_mode(const modem_t *m) { return m->config.mode; }
int modem_get_rx_tone(const modem_t *m) { return m->config.rx_tone_hz; }
const char *modem_mode_name(modem_mode_t mode)
{
    return (unsigned)mode < MODEM_MODE_COUNT ? names[mode] : "?";
}
size_t modem_process(modem_t *m, const int16_t *samples, size_t count, char *out, size_t out_size)
{
    if (!out || !out_size) return 0;
    m->out = out; m->out_size = out_size; m->out_len = 0;
    out[0] = 0;
    bpsk_rx_feed(&m->rx, samples, count);
    m->sample_counter += count;
    return m->out_len;
}
