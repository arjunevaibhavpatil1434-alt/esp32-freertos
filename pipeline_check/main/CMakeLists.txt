idf_component_register(
    SRCS "main.c" "dht11.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_lcd
)
