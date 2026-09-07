#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int16_t history[47];
    unsigned head, phase;
} audio_decimator_t;

/* Downsample mono 16 kHz by two. Caller reserves ceil(count / 2) outputs.
 * Filter and decimation phase persist across arbitrary input block sizes. */
size_t audio_decimator_feed(audio_decimator_t *state, const int16_t *mono,
                            size_t count, int16_t *out);
