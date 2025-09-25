idf_component_register(SRCS "button.c" "ble_nus.c" "nus_cdc_bridge.c" "ble_nus_client.c" "bootloader_trigger.c" "ble_bas.c" "leds.c" "main.c" "ble_transport.c" "usb_hid.c" "usb_cdc.c"
                       INCLUDE_DIRS "."
                       REQUIRES bt esp_hid nvs_flash esp_driver_uart)
