# MouthPad^ USB – nRF Firmware (Zephyr)

Zephyr application that runs on Nordic nRF52840 boards (tested primarily on the Seeed XIAO nRF52840)
and turns a MouthPad^ wearable into a wired USB HID device without touching the MouthPad^ firmware.

## Supported hardware

| Board | Status | Notes |
|-------|--------|-------|
| Seeed XIAO nRF52840 (`xiao_ble`) | ✅ Production | Primary shipping target. Button + single-colour LED. |
| Adafruit Feather nRF52840 (`adafruit_feather_nrf52840`) | ✅ Production | Requires custom bootloader config. |
| Nordic nRF52840 Dongle (`nrf52840dongle`) | ✅ Production | PCA10059 with stock Nordic LED pins. |
| April Brothers nRF52840 Dongle (`nrf52840dongle`) | ✅ Production | PCA10059 with non-standard LED wiring. |

The default overlays assume a single status LED and a user button. Dongle variants use different overlays
for Nordic (stock pins) vs April Brothers (non-standard LED wiring).

## Features

**Core functionality (matches ESP32 firmware 1:1):**
* Bridges BLE HID and Nordic UART Service (NUS) traffic to USB HID + dual CDC ACM
* Automatic MouthPad^ discovery and reconnection
* LED status indicators (slow blink scanning, solid connected, fast flicker on activity)
* Maintenance console on CDC port 2 with shell commands (see below)
* Persistent BLE bonding across power cycles
* USB bcdDevice version automatically set from VERSION file

**Platform-specific features:**
* UF2 bootloader support (drag-and-drop firmware updates)
* NeoPixel RGB LED support on Adafruit Feather (gradient battery colors)
* Docker-based build environment (no local toolchain required)
* RTT logging support for debugging

## Build & flash

You can build the Zephyr firmware inside the provided Docker image or in your own Zephyr/NCS workspace.

### Option 1 – Docker (no local toolchain)

```bash
cd ncs  # Navigate to NCS workspace directory

# Build the XIAO nRF52840 target and drop UF2 artefacts in build/
docker-compose run --rm mouthpad-build

# Start an interactive shell with the Zephyr toolchains mounted
docker-compose run --rm mouthpad-dev
```

The automated build copies `zephyr.uf2` to `build/mouthpad_usb_xiao_ble_<date>.uf2` so you can drag it
directly to the bootloader volume.

### Option 2 – Local workspace (`west`/`make`)

Prerequisites:

* nRF Connect SDK / Zephyr toolchain installed locally.
* `west` on your `PATH` and environment variables (`ZEPHYR_BASE`, `ZEPHYR_SDK_INSTALL_DIR`, …) set.

Workflow:

```bash
# First time only (initialises NCS workspace)
make init

# Build for the default board (xiao_ble)
make build

# Flash via J-Link
make flash

# Copy the UF2 to a mounted bootloader drive
make flash-uf2

# Open an RTT console (requires Segger tools, see Makefile for paths)
make monitor
```

Override the board with `BOARD=<board>` when calling `make build`.

## Firmware layout & DFU

* **UF2 updates:** double-tap reset to mount the bootloader drive (`XIAO-SENSE` on macOS/Linux), then
  copy `build/app/zephyr/zephyr.uf2`. You can also type `dfu` on the maintenance CDC port to reboot into
  the bootloader without touching the board.
* **CDC console commands:** available on `/dev/cu.usbmodem<serial>3` (macOS) / `/dev/ttyACM1` (Linux).

| Command | Description |
|---------|-------------|
| `dfu`   | Reboot into the UF2 bootloader (drag-and-drop firmware update mode). |
| `reset` | Disconnect and clear all BLE bonds, then return to pairing mode. |
| `restart` | Restart firmware (software reset). |
| `serial` | Print the USB serial number used in the `usbmodem` node name. |
| `version` | Display firmware build timestamp, Zephyr version, and VERSION file. |

**Note:** The ESP32 firmware also includes a `device` command to display MouthPad^ device information (manufacturer, model, firmware version). This command is not yet implemented in the nRF firmware.

## LED behaviour

GPIO LEDs use discrete colours; NeoPixels (if present) use gradients. States:

| State | Behaviour |
|-------|-----------|
| `LED_STATE_SCANNING` | Slow blink while seeking the MouthPad^. |
| `LED_STATE_CONNECTED` | Solid on. |
| `LED_STATE_DATA_ACTIVITY` | Fast flicker when forwarding HID traffic. |

## Version management

The firmware version is controlled by the `VERSION` file at the repository root. When you build:
- The VERSION file (e.g., `0.1.0`) is parsed by CMake
- USB bcdDevice descriptor is set automatically (e.g., `0.1.0` → `0x0010`)
- Version is logged at startup and visible in `version` command output

To release a new version, update the VERSION file and push to main. GitHub Actions will automatically build and create a release with firmware files named `mp_usb_{version}_{board}.uf2`.

## Directory notes

```
app/
├── boards/        # Board overlays (.conf/.overlay) for supported targets
├── src/           # Application sources
├── prj.conf       # Common Kconfig fragments
└── CMakeLists.txt
```

Common build tips and alternative workflows (VS Code, standalone west workspace) live in
[`resources/notes/BUILD_METHODS.md`](../../resources/notes/BUILD_METHODS.md).

