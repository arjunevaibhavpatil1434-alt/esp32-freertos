idf_component_register(
    SRCS "main.c" "dht11.c" "ui.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bt nvs_flash esp_driver_gpio esp_driver_i2c esp_lcd esp_timer
)
