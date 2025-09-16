# ESP32 BLE HID Central Prototype

This directory hosts an ESP-IDF project that scans for BLE HID peripherals (e.g., the MouthPad mouse),
connects to the first device discovered, and logs incoming HID reports with the received signal strength
indicator (RSSI). It is adapted from the official `bluetooth/esp_hid_host` example in ESP-IDF; unnecessary
USB/Bluetooth Classic handling has been trimmed so you can focus on BLE performance testing with the
ESP32-S3 and an external antenna.

## Prerequisites

1. Install ESP-IDF v5.1 or later and export the environment (`. $IDF_PATH/export.sh`).
2. Connect the ESP32-S3 board over USB and note its serial port (e.g., `/dev/cu.usbserial-0001`).
3. Optional: adjust defaults in `sdkconfig.defaults` for scan timing, stack size, or other HID host
   parameters mirrored from the upstream example.

## Build, Flash, and Monitor

```bash
# Activate ESP-IDF with the pinned Python environment
. ./env.sh

# Build the firmware (defaults to target esp32s3)
make build

# Flash the device (override ESP32_PORT if the default wildcard does not match)
ESP32_PORT=/dev/cu.usbserial-0001 make flash

# Attach a serial monitor (Ctrl+] to quit)
ESP32_PORT=/dev/cu.usbserial-0001 make monitor
```

`make flash-monitor` combines flashing and monitoring for quicker cycles. Reports are truncated to 32
bytes for readability; adjust the helper in `main/main.c` if full reports are needed.

## Expected Output

Once powered, the firmware scans continuously. When a BLE HID device is detected, the logs show the peer
address, connection events, and all incoming HID reports alongside their RSSI value. Use these logs to
compare signal strength under different antenna setups. If you need richer diagnostics, cross-reference
the upstream `esp_hid_host` README for optional features (battery events, Classic Bluetooth) that can be
re-enabled here as needed.
