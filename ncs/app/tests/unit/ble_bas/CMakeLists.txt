cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr COMPONENTS unittest REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_ble_bas)

target_sources(testbinary PRIVATE
  src/test_ble_bas_standalone.c
)
