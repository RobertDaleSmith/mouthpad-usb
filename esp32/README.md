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

# (First time only) fetch TinyUSB device stack component
idf.py add-dependency espressif/esp_tinyusb^1.4.2

# Initialise ESP-IDF for this shell
make init

# Build the firmware (defaults to target esp32s3)
make build

# Flash the device (override ESP32_PORT if the default wildcard does not match)
ESP32_PORT=/dev/cu.usbserial-0001 make flash

# Attach a serial monitor (Ctrl+] to quit)
ESP32_PORT=/dev/cu.usbserial-0001 make monitor
```

`make init` sources `$(IDF_PATH)/export.sh` (default `~/esp-idf`) so subsequent
targets run in the ESP-IDF environment. Override `IDF_PATH` when ESP-IDF lives
elsewhere.

### Entering DFU without the BOOT button

Open the TinyUSB CDC console (e.g., `screen /dev/cu.usbmodemXXXX 115200`) and send
`dfu` followed by Enter. Close the terminal so the port is free. The firmware
reboots, hands USB back to the ROM loader, and enumerates as the USB-Serial/JTAG
download port. Run `idf.py flash` (or `make flash`) against that port—because the
flasher no longer tries to reset before flashing, the ROM stays available until
the transfer completes, after which the application restarts normally.

`make flash-monitor` combines flashing and monitoring for quicker cycles. Reports are truncated to 32
bytes for readability; adjust the helper in `main/main.c` if full reports are needed.

## Expected Output

Once powered, the firmware scans continuously. When a BLE HID device is detected, the logs show the peer
address, connection events, and HID reports alongside their RSSI value. Connected reports are forwarded
verbatim to the TinyUSB HID interface using the same descriptor and report IDs as the nRF firmware, so a
host sees identical mouse/consumer-control behaviour. A CDC ACM interface is enumerated in parallel and
serves as the default console (the device now appears as both a HID mouse and USB serial port). Use these
logs to compare signal strength under different antenna setups. If you need richer diagnostics, cross-
reference the upstream `esp_hid_host` README for optional features (battery events, Classic Bluetooth)
that can be re-enabled here as needed.
