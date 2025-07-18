cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(ble-usb-bridge)

# Suppress main function return type warning
target_compile_options(app PRIVATE -Wno-main)

# Add your source files
target_sources(app PRIVATE
    src/main.c
    src/ble_hid_c.c
) 