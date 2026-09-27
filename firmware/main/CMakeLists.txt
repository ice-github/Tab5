idf_component_register(SRCS "main.c" "ina_diag.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_jpeg esp_lcd fatfs nvs_flash esp_pm esp_driver_sdmmc esp_driver_i2c
                    PRIV_REQUIRES espressif__esp_lcd_touch)
