idf_component_register(SRCS "bootloader_trigger.c" "ble_bas.c" "leds.c" "main.c" "ble_transport.c" "usb_hid.c" "usb_cdc.c"
                       INCLUDE_DIRS "."
                       REQUIRES bt esp_hid nvs_flash)
