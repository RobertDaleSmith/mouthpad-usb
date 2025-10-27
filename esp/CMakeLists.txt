cmake_minimum_required(VERSION 3.16)

# Read VERSION file from repository root
set(VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION")
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

    # Convert to hex string for C define
    set(USB_BCD_DEVICE_HEX "0x${USB_BCD_DEVICE}")

    message(STATUS "Firmware version: ${FIRMWARE_VERSION_STRING} (USB bcdDevice: ${USB_BCD_DEVICE_HEX})")

    # Pass to compiler via component config
    idf_build_set_property(COMPILE_DEFINITIONS "-DFIRMWARE_VERSION_STRING=\"${FIRMWARE_VERSION_STRING}\"" APPEND)
    idf_build_set_property(COMPILE_DEFINITIONS "-DUSB_BCD_DEVICE=${USB_BCD_DEVICE_HEX}" APPEND)
  else()
    message(WARNING "Invalid VERSION format: ${FIRMWARE_VERSION_STRING}")
  endif()
else()
  message(WARNING "VERSION file not found at ${VERSION_FILE}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mouthpad_usb)
