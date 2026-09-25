idf_component_register(SRCS "app_hf_msg_set.c"
                            "app_av_msg_set.c"
                            "bt_app_core.c"
                            "bt_app_hf.c"
                            "bt_app_av.c"
                            "bt_app_pbac.c"
                            "main.c"
                    PRIV_REQUIRES bt nvs_flash console esp_ringbuf esp_driver_i2s esp_driver_dac
                    INCLUDE_DIRS ".")
