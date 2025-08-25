# ESP-IDF MouthPad^ USB Bridge

This directory contains a minimal ESP-IDF application that turns a
Seeed XIAO ESP32-S3 Sense into a BLE-to-USB HID bridge for the
MouthPad^ mouse. The firmware automatically scans for a BLE peripheral
advertising both the HID service (0x1812) and the Nordic UART Service
(NUS) (`6E400001-B5A3-F393-E0A9-E50E24DCCA9E`). Once connected it
forwards raw HID reports from the MouthPad^ to the host computer over
USB using TinyUSB, preserving the report ID so each report reaches its
matching USB input endpoint.

The goal of this initial version is to evaluate BLE performance using
the external antenna available on the ESP32-S3 Sense board.

## Building

```bash
cd esp32
make build
```

## Flashing

```bash
make flash PORT=/dev/ttyACM0
make monitor PORT=/dev/ttyACM0
```

The application uses default USB descriptors for a generic HID mouse.
