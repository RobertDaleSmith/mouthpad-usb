cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mouthpad_usb)

target_sources(app PRIVATE
  src/ble.c
  src/usb.c
  src/main.c
)
