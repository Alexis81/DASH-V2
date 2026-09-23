idf_component_register(SRCS "main.c" "can.c" "leds.c" "toyota_logo.c" "rpm_font.c" "stop_font.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES nvs_flash esp_timer esp_driver_twai esp_driver_gpio
                                  esp_driver_i2c esp_lcd espressif__esp_lcd_touch_gt911
                                  espressif__led_strip lvgl__lvgl)

