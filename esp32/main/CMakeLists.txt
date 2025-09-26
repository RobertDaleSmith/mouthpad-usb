idf_component_register(SRCS "ble_transport.c" "ble_central.c" "dev_dfu.c" "ble_dis.c" "ble_nus.c" "button.c" "ble_bas.c" "leds.c" "main.c" "usb_hid.c" "usb_cdc.c"
                       INCLUDE_DIRS "."
                       REQUIRES bt esp_hid nvs_flash esp_driver_uart)
