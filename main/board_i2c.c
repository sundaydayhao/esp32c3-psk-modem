#include "board_i2c.h"

#include "board_config.h"
#include "esp_check.h"

static const char *TAG = "board_i2c";
static i2c_master_bus_handle_t s_i2c_bus;

esp_err_t board_i2c_get_bus(i2c_master_bus_handle_t *bus_handle)
{
    ESP_RETURN_ON_FALSE(bus_handle, ESP_ERR_INVALID_ARG, TAG, "bus_handle is NULL");

    if (!s_i2c_bus) {
        const i2c_master_bus_config_t conf = {
            .i2c_port = BOARD_TOUCH_I2C_PORT,
            .sda_io_num = BOARD_TOUCH_PIN_SDA,
            .scl_io_num = BOARD_TOUCH_PIN_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags = {
                .enable_internal_pullup = true,
            },
        };
        ESP_RETURN_ON_ERROR(i2c_new_master_bus(&conf, &s_i2c_bus), TAG, "i2c bus");
    }

    *bus_handle = s_i2c_bus;
    return ESP_OK;
}
