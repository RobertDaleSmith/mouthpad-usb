#!/bin/bash

# Simple UF2 Flash script for BLE-USB Bridge
# Target: Seeed XIAO nRF52840

set -e

BOARD="xiao_ble"
BUILD_DIR="build"

echo "Simple UF2 Flash script for BLE-USB Bridge..."
echo "Target board: $BOARD"
echo ""

# Check if build directory exists
if [ ! -d "$BUILD_DIR" ]; then
    echo "Build directory not found. Please run 'make build' first."
    exit 1
fi

# Check if UF2 file exists
UF2_FILE="$BUILD_DIR/zephyr/zephyr.uf2"
if [ ! -f "$UF2_FILE" ]; then
    echo "UF2 file not found: $UF2_FILE"
    echo "Please ensure CONFIG_BUILD_OUTPUT_UF2=y is set in prj.conf"
    exit 1
fi

echo "✅ UF2 file found: $UF2_FILE"
echo ""

# Check if device is already in UF2 mode
UF2_VOLUME=""
POSSIBLE_VOLUMES=("XIAO-SENSE" "XIAO_NRF52840" "NRF52BOOT" "UF2BOOT" "RPI-RP2")

echo "🔍 Checking for UF2 bootloader volume..."

for volume in "${POSSIBLE_VOLUMES[@]}"; do
    if [ -d "/Volumes/$volume" ]; then
        UF2_VOLUME="$volume"
        echo "✅ Found UF2 volume: /Volumes/$volume"
        break
    fi
done

if [ -z "$UF2_VOLUME" ]; then
    echo ""
    echo "❌ No UF2 bootloader volume found."
    echo ""
    echo "📋 To enter UF2 bootloader mode:"
    echo "   1. Double-tap the reset button on your Seeed XIAO nRF52840"
    echo "   2. The device should appear as a USB mass storage device"
    echo "   3. Run this script again"
    echo ""
    echo "💡 Alternative methods:"
    echo "   • Hold the reset button while plugging in the USB cable"
    echo "   • Or try the serial DFU command: make dfu"
    echo ""
    echo "🔧 Manual flashing (if device is in UF2 mode):"
    echo "   cp $UF2_FILE /Volumes/XIAO-SENSE/"
    echo ""
    exit 1
fi

# Copy the UF2 file to the device
echo "📤 Copying UF2 file to /Volumes/$UF2_VOLUME/..."
cp "$UF2_FILE" "/Volumes/$UF2_VOLUME/"

if [ $? -eq 0 ]; then
    echo ""
    echo "✅ Flash successful!"
    echo ""
    echo "🔄 The device will automatically restart with the new firmware."
    echo "   You should see the device disappear and reappear as a serial device."
    echo ""
    echo "📺 To monitor the device output:"
    echo "   make monitor"
    echo "   or"
    echo "   screen /dev/tty.usbmodem* 115200"
    echo ""
    echo "🎉 Your BLE-USB Bridge is now updated!"
else
    echo ""
    echo "❌ Flash failed!"
    echo "   Make sure the device is in UF2 bootloader mode."
    echo "   Try double-tapping the reset button again."
    exit 1
fi 