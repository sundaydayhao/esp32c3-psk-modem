#pragma once
#include "receiver_state.h"

esp_err_t ble_rx_start(QueueHandle_t controls);
const char *ble_rx_name(void);
