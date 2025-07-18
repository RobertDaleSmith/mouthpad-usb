#!/bin/bash

# UF2 Flash script for BLE-USB Bridge
# Target: Seeed XIAO nRF52840

set -e

BOARD="xiao_ble"
BUILD_DIR="build"

echo "UF2 Flash script for BLE-USB Bridge..."
echo "Target board: $BOARD"

# Check if build directory exists
if [ ! -d "$BUILD_DIR" ]; then
    echo "Build directory not found. Please run './build.sh' first."
    exit 1
fi

# Check if UF2 file exists
UF2_FILE="$BUILD_DIR/zephyr/zephyr.uf2"
if [ ! -f "$UF2_FILE" ]; then
    echo "UF2 file not found: $UF2_FILE"
    echo "Please ensure CONFIG_BUILD_OUTPUT_UF2=y is set in prj.conf"
    exit 1
fi

echo "UF2 file found: $UF2_FILE"
echo ""

# Try to find the mounted UF2 volume
UF2_VOLUME=""
POSSIBLE_VOLUMES=("XIAO-SENSE" "XIAO_NRF52840" "NRF52BOOT" "UF2BOOT" "RPI-RP2")

echo "Looking for UF2 bootloader volume..."

for volume in "${POSSIBLE_VOLUMES[@]}"; do
    if [ -d "/Volumes/$volume" ]; then
        UF2_VOLUME="$volume"
        echo "Found UF2 volume: /Volumes/$volume"
        break
    fi
done

if [ -z "$UF2_VOLUME" ]; then
    echo ""
    echo "No UF2 bootloader volume found."
    echo ""
    echo "To enter UF2 bootloader mode:"
    echo "  1. Double-tap the reset button on your Seeed XIAO nRF52840"
    echo "  2. Or hold the reset button while plugging in the USB cable"
    echo "  3. The device should appear as a USB mass storage device"
    echo ""
    echo "Once the device is in UF2 mode, run this script again."
    echo ""
    echo "Manual flashing instructions:"
    echo "  cp $UF2_FILE /Volumes/XIAO-SENSE/"
    echo ""
    exit 1
fi

# Copy the UF2 file to the device
echo "Copying UF2 file to /Volumes/$UF2_VOLUME/..."
cp "$UF2_FILE" "/Volumes/$UF2_VOLUME/"

if [ $? -eq 0 ]; then
    echo "Flash successful!"
    echo ""
    echo "The device will automatically restart with the new firmware."
    echo "You should see the device disappear and reappear as a serial device."
    echo ""
    echo "To monitor the device output:"
    echo "  make monitor"
    echo "  or"
    echo "  screen /dev/tty.usbmodem* 115200"
else
    echo "Flash failed!"
    echo "Make sure the device is in UF2 bootloader mode."
    exit 1
fi 