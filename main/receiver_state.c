#include "receiver_state.h"
#include "rx_history.h"

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static rx_history_t history;
static receiver_status_t status;

void receiver_append(const char *data, size_t count)
{
    portENTER_CRITICAL(&lock);
    rx_history_append(&history, data, count);
    portEXIT_CRITICAL(&lock);
}
void receiver_update_status(const receiver_status_t *value)
{
    portENTER_CRITICAL(&lock);
    status = *value;
    portEXIT_CRITICAL(&lock);
}
void receiver_get_status(receiver_status_t *value)
{
    portENTER_CRITICAL(&lock);
    *value = status;
    portEXIT_CRITICAL(&lock);
}
size_t receiver_read(uint64_t since, uint8_t *out, size_t capacity, uint64_t *next, uint64_t *dropped)
{
    portENTER_CRITICAL(&lock);
    size_t count = rx_history_read(&history, since, out, capacity, next, dropped);
    portEXIT_CRITICAL(&lock);
    return count;
}
