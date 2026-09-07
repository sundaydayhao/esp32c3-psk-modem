#include "rx_history.h"

void rx_history_append(rx_history_t *h, const char *data, size_t length)
{
    for (size_t i = 0; i < length; ++i) h->bytes[h->end++ % RX_HISTORY_CAPACITY] = (uint8_t)data[i];
}
size_t rx_history_read(const rx_history_t *h, uint64_t since, uint8_t *out,
                       size_t capacity, uint64_t *next, uint64_t *dropped)
{
    const uint64_t oldest = h->end > RX_HISTORY_CAPACITY ? h->end - RX_HISTORY_CAPACITY : 0;
    *dropped = since < oldest ? oldest - since : 0;
    if (since < oldest) since = oldest;
    if (since > h->end) since = oldest;
    size_t length = h->end - since > capacity ? capacity : (size_t)(h->end - since);
    for (size_t i = 0; i < length; ++i) out[i] = h->bytes[(since + i) % RX_HISTORY_CAPACITY];
    *next = since + length;
    return length;
}
