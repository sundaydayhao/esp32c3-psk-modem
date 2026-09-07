#include "ble_protocol.h"
#include <stdio.h>
#include <string.h>

size_t ble_payload_limit(unsigned mtu)
{
    if (mtu < 23) return 0;
    return mtu - 3 < BLE_TEXT_MAX_PAYLOAD ? mtu - 3 : BLE_TEXT_MAX_PAYLOAD;
}
void ble_line_feed(ble_line_t *s, const uint8_t *data, size_t count, ble_line_cb_t callback, void *user)
{
    for (size_t n = 0; n < count; ++n) {
        uint8_t c = data[n];
        if (c == '\n') {
            if (s->discard) callback(NULL, user);
            else if (s->used) { s->line[s->used] = 0; callback(s->line, user); }
            s->used = 0; s->discard = false;
        } else if (c != '\r') {
            if (c < 32 || c > 126 || s->used + 1 >= sizeof(s->line)) s->discard = true;
            if (!s->discard) s->line[s->used++] = (char)c;
        }
    }
}
ble_command_t ble_command_parse(const char *line)
{
    ble_command_t result = {0};
    if (!line) return result;
    if (strcmp(line, "WIFI") == 0) { result.type = BLE_COMMAND_WIFI; return result; }
    if (strcmp(line, "REPLAY") == 0) { result.type = BLE_COMMAND_REPLAY; return result; }
    char name[16], extra;
    unsigned tone;
    if (strncmp(line, "SET ", 4) != 0 || sscanf(line + 4, "%15s %u %c", name, &tone, &extra) != 2) return result;
    for (unsigned mode = 0; mode < MODEM_MODE_COUNT; ++mode) {
        if (!modem_mode_supported((modem_mode_t)mode) || strcmp(name, modem_mode_name((modem_mode_t)mode))) continue;
        unsigned margin = 8000 / modem_samples_per_symbol((modem_mode_t)mode) + 100;
        if (tone < margin || tone > 3500 || tone > 4000 - margin) return result;
        result.type = BLE_COMMAND_SET;
        result.mode = (modem_mode_t)mode;
        result.tone_hz = (int)tone;
        return result;
    }
    return result;
}
