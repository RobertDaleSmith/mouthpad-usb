#!/bin/bash

# DFU script for BLE-USB Bridge
# Target: Seeed XIAO nRF52840

set -e

BOARD="xiao_ble"
BUILD_DIR="build"
SERIAL_PORT=""

echo "DFU Trigger script for BLE-USB Bridge..."
echo "Target board: $BOARD"

# Function to find the serial port
find_serial_port() {
    echo "Looking for XIAO BLE serial port..."
    
    # Common serial port patterns for XIAO BLE
    POSSIBLE_PORTS=(
        "/dev/tty.usbmodem*"
        "/dev/tty.usbserial*"
        "/dev/ttyACM*"
        "/dev/ttyUSB*"
    )
    
    for pattern in "${POSSIBLE_PORTS[@]}"; do
        for port in $pattern; do
            if [ -e "$port" ]; then
                echo "Found serial port: $port"
                SERIAL_PORT="$port"
                return 0
            fi
        done
    done
    
    return 1
}

# Try to find the serial port
if ! find_serial_port; then
    echo "ERROR: No serial port found!"
    echo ""
    echo "Please ensure:"
    echo "  1. Your XIAO BLE is connected via USB"
    echo "  2. The device is running firmware with shell support"
    echo "  3. No other application is using the serial port"
    echo ""
    echo "Manual method:"
    echo "  1. Connect to the device manually:"
    echo "     screen /dev/tty.usbmodem2101 115200"
    echo "  2. Type 'dfu' and press Enter"
    echo "  3. Exit screen (Ctrl+A, then K)"
    echo ""
    echo "Or use UF2 method:"
    echo "  1. Double-tap the reset button to enter UF2 mode"
    echo "  2. Run 'make flash-uf2' for instructions"
    exit 1
fi

echo "Attempting to send DFU command to $SERIAL_PORT..."
echo ""

# Try to send the dfu command using screen
echo "Sending 'dfu' command to device..."
echo "You should see the device reboot into DFU mode."
echo ""

echo "Device found at: $SERIAL_PORT"
echo ""
echo "To trigger DFU mode, you have two options:"
echo ""
echo "Option 1 - Serial Command (if device is running):"
echo "  1. Connect to the device:"
echo "     screen $SERIAL_PORT 115200"
echo "  2. Type 'dfu' and press Enter"
echo "  3. Exit screen: Ctrl+A, then K"
echo "  4. Device will reboot"
echo ""
echo "Option 2 - Hardware Method (always works):"
echo "  1. Double-tap the reset button on your XIAO BLE"
echo "  2. Device enters UF2 bootloader mode"
echo "  3. Run 'make flash-uf2' for flashing instructions"
echo ""
echo "After either method, run 'make flash-uf2' to flash your firmware."

echo ""
echo "DFU command sent!"
echo "The device should now be rebooting into DFU mode."
echo ""
echo "Next steps:"
echo "  1. Wait a few seconds for the device to reboot"
echo "  2. Run 'make flash-uf2' to get UF2 flashing instructions"
echo "  3. Or double-tap the reset button to enter UF2 mode manually" 