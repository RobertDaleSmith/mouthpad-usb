
# MouthPad BLE to USB Bridge

## 🎯 Goal

To build a firmware application for the **Seeed XIAO nRF52840** board that:

- Connects to a **custom BLE device** (e.g. a wireless mouse) which:
  - Transmits **BLE HID input reports**
  - Sends additional data over **BLE UART (Nordic UART Service or similar)**
- Forwards:
  - **BLE HID input reports** → over **USB HID** (to emulate the same HID device over USB)
  - **BLE UART data** → over **USB CDC Serial** (for monitoring or logging on a PC)

> This turns the Seeed XIAO into a **BLE-to-USB bridge**, capable of relaying both control inputs and auxiliary telemetry data from a wireless BLE device to a USB host.

---

## 🧱 Architecture

### BLE Side (Central Role)
- Scans for and connects to a specific **custom BLE peripheral**
- Subscribes to:
  - **HID Input Reports** (via Notification on a known characteristic)
  - **UART Data** (via Nordic UART Service or similar)
- Parses both streams in parallel

### USB Side (Device Role)
- Emulates:
  - A **USB HID device** (e.g. mouse, keyboard, or generic HID with same report format as the BLE device)
  - A **USB CDC (serial) device** for debugging or telemetry
- Forwards:
  - HID input reports directly as HID output to USB host
  - UART packets to USB serial output (optionally formatted)

---

## 🛠️ Target Hardware

- **MCU**: [Seeed XIAO nRF52840](https://wiki.seeedstudio.com/XIAO-BLE/)
- **Firmware SDK**: Nordic **nRF5 SDK v17.1.0**
- **Toolchain**:
  - `arm-none-eabi-gcc`
  - Nordic CLI Tools (`nrfjprog`)
  - Optional: SEGGER Embedded Studio for debugging

---

## 📦 Project Status

- Initial scaffold created with:
  - `src/` folder for application logic
  - Placeholder files for BLE UART, BLE HID, USB CDC, USB HID, and shared utils
  - `sdk_config.h` and `Makefile` placeholders
- Project not yet functional — no active BLE or USB handling code implemented yet

---

## 🔜 Remaining Work

1. **Set Up BLE Central Functionality**
   - Scan for and connect to the BLE device
   - Discover HID and UART services + characteristics
   - Subscribe to notifications and handle received data

2. **Set Up USB Device Classes**
   - Configure CDC (serial) output
   - Configure HID class with matching report descriptor
   - Implement USB transmit functions for BLE data

3. **Handle Forwarding Logic**
   - BLE UART → USB Serial
   - BLE HID → USB HID

4. **Device Filtering (optional)**
   - Filter only the target BLE device by name,diskutil list external address, or service UUID

5. **Robustness**
   - Handle disconnects and reconnections
   - Optional LED indicators for BLE/USB connection states

---

## 🧪 Stretch Goals

- Bidirectional UART forwarding (USB → BLE)
- Multiple BLE device support
- Configurable HID report remapping
- Logging and command console over USB

---

## 📂 File Overview (Scaffolded)

| File | Description |
|------|-------------|
| `src/main.c` | Entry point, init and loop |
| `src/ble_uart_c.*` | BLE UART Central handler |
| `src/ble_hid_c.*` | BLE HID Central handler |
| `src/usb_cdc.*` | USB Serial output logic |
| `src/usb_hid.*` | USB HID output logic |
| `src/utils.*` | Shared helpers |
| `sdk_config/sdk_config.h` | Nordic SDK configuration |
| `Makefile` | Build system (to be adapted from SDK examples) |
