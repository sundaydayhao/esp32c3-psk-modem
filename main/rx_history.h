#pragma once
#include <stddef.h>
#include <stdint.h>

#define RX_HISTORY_CAPACITY 4096
typedef struct {
    uint8_t bytes[RX_HISTORY_CAPACITY];
    uint64_t end;
} rx_history_t;

/* Caller owns synchronization. Readers have independent cursors. */
void rx_history_append(rx_history_t *history, const char *data, size_t length);
size_t rx_history_read(const rx_history_t *history, uint64_t since, uint8_t *out,
                       size_t capacity, uint64_t *next, uint64_t *dropped);
