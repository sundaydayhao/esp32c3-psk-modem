#pragma once
#include "modem.h"

#define BLE_TEXT_MAX_PAYLOAD 180
typedef struct { char line[64]; size_t used; bool discard; } ble_line_t;
typedef void (*ble_line_cb_t)(const char *line, void *user); /* NULL = malformed/oversized */
typedef enum { BLE_COMMAND_INVALID, BLE_COMMAND_SET, BLE_COMMAND_REPLAY, BLE_COMMAND_WIFI } ble_command_type_t;
typedef struct { ble_command_type_t type; modem_mode_t mode; int tone_hz; } ble_command_t;

void ble_line_feed(ble_line_t *line, const uint8_t *data, size_t count, ble_line_cb_t callback, void *user);
ble_command_t ble_command_parse(const char *line);
size_t ble_payload_limit(unsigned mtu);
