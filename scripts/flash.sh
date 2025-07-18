#!/bin/bash

# Flash script for BLE-USB Bridge with MCUboot DFU support
# Target: Seeed XIAO nRF52840

set -e

BOARD="xiao_ble"
BUILD_DIR="build"

echo "Flashing BLE-USB Bridge with MCUboot DFU support..."
echo "Target board: $BOARD"

# Check if build directory exists
if [ ! -d "$BUILD_DIR" ]; then
    echo "Build directory not found. Please run './build.sh' first."
    exit 1
fi

# Flash the combined image (bootloader + application)
echo "Flashing combined image (bootloader + application)..."
west flash -d "$BUILD_DIR"

if [ $? -eq 0 ]; then
    echo "Flash successful!"
    echo ""
    echo "Device is now running with MCUboot bootloader."
    echo ""
    echo "To enter DFU mode:"
    echo "  1. Connect to the device via serial (115200 baud)"
    echo "  2. Type 'dfu' in the shell"
    echo "  3. The device will reboot into bootloader mode"
    echo "  4. Use 'make flash' or this script to update firmware"
    echo ""
    echo "To monitor the device:"
    echo "  west espressif monitor -d $BUILD_DIR"
else
    echo "Flash failed!"
    exit 1
fi 