#include "audio_decimator.h"
#include "audio_filter.h"

size_t audio_decimator_feed(audio_decimator_t *s, const int16_t *mono, size_t count, int16_t *out)
{
    size_t produced = 0;
    for (size_t n = 0; n < count; ++n) {
        s->history[s->head] = mono[n] / 2;
        if (++s->phase == 2) {
            s->phase = 0;
            int32_t sum = 0;
            unsigned p = s->head;
            for (unsigned tap = 0; tap < 47; ++tap) {
                sum += s->history[p] * audio_lpf_q15[tap];
                if (++p == 47) p = 0;
            }
            int32_t value = sum >> 14;
            if (value > INT16_MAX) value = INT16_MAX;
            if (value < INT16_MIN) value = INT16_MIN;
            out[produced++] = (int16_t)value;
        }
        s->head = s->head ? s->head - 1 : 46;
    }
    return produced;
}
