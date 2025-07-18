#!/bin/bash

# Build script for BLE-USB Bridge
# Target: Seeed XIAO nRF52840

set -e

BOARD="xiao_ble"
BUILD_DIR="build"

echo "Building BLE-USB Bridge..."
echo "Target board: $BOARD"
echo "Build directory: $BUILD_DIR"

# Clean previous build
if [ -d "$BUILD_DIR" ]; then
    echo "Cleaning previous build..."
    rm -rf "$BUILD_DIR"
fi

# Build the application
echo "Building application..."
west build -b "$BOARD" -d "$BUILD_DIR"

if [ $? -eq 0 ]; then
    echo "Build successful!"
    echo ""
    echo "Generated files:"
    echo "  - Application: $BUILD_DIR/zephyr/zephyr.elf"
    echo "  - UF2: $BUILD_DIR/zephyr/zephyr.uf2"
    echo ""
    echo "To flash the device:"
    echo "  make flash-uf2"
    echo ""
    echo "To enter DFU mode:"
    echo "  1. Connect to the device via serial"
    echo "  2. Type 'dfu' in the shell"
    echo "  3. The device will reboot"
    echo "  4. Use 'make flash-uf2' to update firmware"
else
    echo "Build failed!"
    exit 1
fi 