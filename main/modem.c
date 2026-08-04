#include "modem.h"

#include <string.h>

static const char *const s_mode_names[MODEM_MODE_COUNT] = {
    "BPSK31",
    "QPSK31-U",
    "QPSK31-L",
    "RTTY",
    "PSK63",
};

static nuepsk_mode_t to_nue_mode(modem_mode_t mode)
{
    switch (mode) {
    case MODEM_MODE_QPSK31_USB:
        return NUEPSK_MODE_QPSK31_USB;
    case MODEM_MODE_QPSK31_LSB:
        return NUEPSK_MODE_QPSK31_LSB;
    case MODEM_MODE_PSK63:
        return NUEPSK_MODE_PSK63;
    case MODEM_MODE_RTTY:
        return NUEPSK_MODE_RTTY;
    case MODEM_MODE_BPSK31:
    default:
        return NUEPSK_MODE_BPSK31;
    }
}

static void on_nue_char(char c, void *userdata)
{
    modem_t *m = (modem_t *)userdata;
    if (!m->out || m->out_len + 1 >= m->out_size) {
        return;
    }
    m->out[m->out_len++] = c;
    m->out[m->out_len] = '\0';
}

static void recreate_nue_rx(modem_t *m)
{
    nuepsk_config_t config = {
        .mode = to_nue_mode(m->config.mode),
        .sample_rate_hz = m->config.sample_rate_hz,
        .rx_tone_hz = m->config.rx_tone_hz,
        .char_cb = on_nue_char,
        .userdata = m,
    };
    nuepsk_rx_init(&m->nue_rx, &config);
    m->nue_rx.sq_thresh = m->config.squelch;
}

void modem_init(modem_t *m, const modem_config_t *config)
{
    memset(m, 0, sizeof(*m));
    m->config = *config;
    recreate_nue_rx(m);
}

void modem_set_mode(modem_t *m, modem_mode_t mode)
{
    if (mode >= MODEM_MODE_COUNT) {
        return;
    }
    m->config.mode = mode;
    recreate_nue_rx(m);
}

void modem_set_rx_tone(modem_t *m, int tone_hz)
{
    if (tone_hz < 100) {
        tone_hz = 100;
    } else if (tone_hz > 3500) {
        tone_hz = 3500;
    }
    m->config.rx_tone_hz = tone_hz;
    recreate_nue_rx(m);
}

modem_mode_t modem_get_mode(const modem_t *m)
{
    return m->config.mode;
}

const char *modem_mode_name(modem_mode_t mode)
{
    return mode < MODEM_MODE_COUNT ? s_mode_names[mode] : "?";
}

int modem_get_rx_tone(const modem_t *m)
{
    return m->config.rx_tone_hz;
}

size_t modem_process(modem_t *m, const int16_t *samples, size_t count, char *out, size_t out_size)
{
    if (out_size == 0) {
        return 0;
    }
    out[0] = '\0';
    m->out = out;
    m->out_size = out_size;
    m->out_len = 0;

    nuepsk_rx_feed(&m->nue_rx, samples, count);
    if (m->out_len > 0) {
        return m->out_len;
    }

    m->sample_counter += count;

    out[0] = '\0';
    return 0;
}
