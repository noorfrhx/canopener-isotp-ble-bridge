idf_component_register(SRCS "main.c" "isotp_bridge.c" "isotp.c" "ble_server.c" "elm_ble.c" "ws2812_control.c" "twai.c" "persist.c" "isotp_link_containers.c" "led.c" "eeprom.c" "uart.c" "connection_handler.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_twai esp_driver_gpio esp_driver_uart esp_driver_usb_serial_jtag esp_timer bt nvs_flash esp_pm)
