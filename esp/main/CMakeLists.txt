idf_component_register(SRCS "usb_dfu.c" "ble_hid.c" "transport_hid.c" "transport_uart.c" "ble_central.c" "ble_dis.c" "ble_nus.c" "button.c" "ble_bas.c" "ble_bonds.c" "leds.c" "main.c" "usb_hid.c" "usb_cdc.c"
                            "packet_framing.c"
                            "mouthpad-proto/src/C/MouthpadRelay.pb.c"
                            "mouthpad-proto/nanopb/pb_common.c"
                            "mouthpad-proto/nanopb/pb_decode.c"
                            "mouthpad-proto/nanopb/pb_encode.c"
                       INCLUDE_DIRS "."
                                    "mouthpad-proto/nanopb"
                                    "mouthpad-proto/src/C"
                       REQUIRES bt esp_hid nvs_flash esp_driver_uart usb)

# Workaround for esp_tinyusb v2.0.1 bug: CMakeLists.txt has backwards logic for IDF 6.0
# It only adds "usb" component for IDF < 6, but IDF 6.0+ is what requires it
# Force esp_tinyusb to link against usb component
idf_component_get_property(esp_tinyusb_lib espressif__esp_tinyusb COMPONENT_LIB)
idf_component_get_property(usb_lib usb COMPONENT_LIB)
target_link_libraries(${esp_tinyusb_lib} PRIVATE ${usb_lib})
