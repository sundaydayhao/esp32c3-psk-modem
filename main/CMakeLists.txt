idf_component_register(
    SRCS
        "app_main.c"
        "audio_input.c"
        "display.c"
        "modem.c"
        "spectrum.c"
        "ui.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_timer esp_driver_spi esp_driver_i2c esp_driver_gpio esp_adc lvgl nuepsk espressif__esp_lcd_touch_ft5x06
)
