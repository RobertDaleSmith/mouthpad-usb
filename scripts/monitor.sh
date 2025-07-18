#!/bin/bash

# Monitor script for BLE-USB Bridge
# Target: Seeed XIAO nRF52840

set -e

BOARD="xiao_ble"
BUILD_DIR="build"

echo "Starting serial monitor for BLE-USB Bridge..."
echo "Target board: $BOARD"

# Check if build directory exists
if [ ! -d "$BUILD_DIR" ]; then
    echo "Build directory not found. Please run './build.sh' first."
    exit 1
fi

echo "Starting west monitor..."
echo "Press Ctrl+C to exit"
echo ""

# Start the monitor
# screen west monitor -d "$BUILD_DIR" 
screen /dev/tty.usbmodem* 115200