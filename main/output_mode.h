#pragma once
#include "esp_err.h"

typedef enum { OUTPUT_WIFI = 0, OUTPUT_BLE = 1 } output_mode_t;
esp_err_t output_mode_init(void);
output_mode_t output_mode_get(void);
/* Persist selection and reboot from a worker; never from a GATT/HTTP callback. */
esp_err_t output_mode_request(output_mode_t mode);
