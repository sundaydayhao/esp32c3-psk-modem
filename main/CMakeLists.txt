idf_component_register(
    SRCS
        "app_main.c"
        "audio_input.c"
        "audio_decimator.c"
        "board_i2c.c"
        "bpsk_rx.c"
        "display.c"
        "modem.c"
        "spectrum.c"
        "ui.c"
        "rx_history.c"
        "web_rx.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "web/index.html"
    REQUIRES esp_lcd esp_timer esp_driver_spi esp_driver_i2c esp_driver_i2s esp_driver_gpio lvgl nuepsk espressif__esp_lcd_touch_ft5x06 espressif__es8311 esp_wifi esp_netif esp_event esp_http_server nvs_flash
)
