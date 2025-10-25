#
# Copyright (c) 2018 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mouthpad_usb)

target_include_directories(app PRIVATE
  src/mouthpad-proto/nanopb
  src/mouthpad-proto/src/C
)

# NORDIC SDK APP START
  target_sources(app PRIVATE
    src/ble_transport.c
    src/ble_central.c
    src/ble_nus_client.c
    src/ble_hid.c
    src/ble_bas.c
    src/ble_dis.c
    src/usb_cdc.c
    src/usb_hid.c
    src/sample_usbd_init.c
    src/oled_display.c
    src/buzzer.c
    src/leds.c
    src/button.c
    src/main.c
    src/mouthpad-proto/src/C/MouthpadRelay.pb.c
    src/mouthpad-proto/nanopb/pb_common.c
    src/mouthpad-proto/nanopb/pb_decode.c
    src/mouthpad-proto/nanopb/pb_encode.c
  )
# NORDIC SDK APP END
