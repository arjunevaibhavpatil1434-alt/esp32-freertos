idf_component_register(SRCS "app_hf_msg_set.c"
                            "app_av_msg_set.c"
                            "bt_app_core.c"
                            "bt_app_hf.c"
                            "bt_app_av.c"
                            "bt_app_pbac.c"
                            "main.c"
                            "oled_status.c"
                            "spk_i2s.c"
                            "contacts.c"
                            "dht11.c"
                            "touch_ctl.c"
                    PRIV_REQUIRES bt nvs_flash console esp_ringbuf esp_driver_i2s esp_driver_i2c esp_driver_gpio esp_driver_touch_sens esp_lcd
                    INCLUDE_DIRS ".")
