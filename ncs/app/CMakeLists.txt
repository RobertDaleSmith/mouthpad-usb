#
# Copyright (c) 2018 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mouthpad_usb)

# Read VERSION file from repository root (two levels up from ncs/app)
set(VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION")
if(EXISTS "${VERSION_FILE}")
  file(READ "${VERSION_FILE}" FIRMWARE_VERSION_STRING)
  string(STRIP "${FIRMWARE_VERSION_STRING}" FIRMWARE_VERSION_STRING)

  # Parse version string (e.g., "0.1.0" -> major=0, minor=1, patch=0)
  string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" VERSION_MATCH "${FIRMWARE_VERSION_STRING}")
  if(VERSION_MATCH)
    set(VERSION_MAJOR ${CMAKE_MATCH_1})
    set(VERSION_MINOR ${CMAKE_MATCH_2})
    set(VERSION_PATCH ${CMAKE_MATCH_3})

    # Convert to BCD format for USB descriptor (e.g., 0.1.0 -> 0x0010)
    math(EXPR USB_BCD_DEVICE "(${VERSION_MAJOR} << 8) | (${VERSION_MINOR} << 4) | ${VERSION_PATCH}")

    message(STATUS "Firmware version: ${FIRMWARE_VERSION_STRING} (USB bcdDevice: 0x${USB_BCD_DEVICE})")

    # Pass to compiler
    add_definitions(-DFIRMWARE_VERSION_STRING="${FIRMWARE_VERSION_STRING}")
    add_definitions(-DUSB_BCD_DEVICE=${USB_BCD_DEVICE})
  else()
    message(WARNING "Invalid VERSION format: ${FIRMWARE_VERSION_STRING}")
  endif()
else()
  message(WARNING "VERSION file not found at ${VERSION_FILE}")
endif()

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

# Add PSELRESET erase support only for MakerDiary nRF52840 MDK dongle
# This board needs PSELRESET erased to use P0.18 as button GPIO instead of reset pin
if(CONFIG_DONGLE_VARIANT_STRING STREQUAL "makerdiary_nrf52840mdk")
  target_sources(app PRIVATE
    src/pselreset_erase.c
  )
endif()

# NORDIC SDK APP END
