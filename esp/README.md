# MouthPad^ USB – ESP32 Firmware (ESP-IDF)

ESP-IDF port of the MouthPad^ USB bridge for ESP32-S3 boards. Functionally mirrors the Zephyr/nRF
firmware with 1:1 feature parity: it scans for the MouthPad^ over BLE, relays HID and NUS traffic to USB,
exposes the same dual CDC interfaces, and offers the same maintenance commands.

## Target hardware

* **Seeed Studio XIAO ESP32-S3** – validated board with user button and LED on GPIO 21.
* **LilyGo T-Display-S3** – buildable but the board lacks a dedicated user LED; the firmware disables LED
  status on that variant.

Select the board via `make xiao` or `make lilygo` (defaults to XIAO). The Makefile injects board-specific
`sdkconfig` fragments so switching targets does not leave stale settings behind.

### External antenna setup (XIAO ESP32-S3)

For improved BLE range, use the **XIAO ESP32-S3 Sense** variant with external antenna connector:

**Required components:**
- [8dBi WiFi RP-SMA antenna (2.4GHz/5.8GHz dual band)](https://www.amazon.com/dp/B07R21LN5P) with U.FL/IPEX to RP-SMA pigtail cable
- [3D printable case for XIAO + SMA antenna](https://www.printables.com/model/1367918-xiao-esp32-sma-antenna-case)

The external antenna provides significantly better range than the PCB antenna, especially useful for extended distance operation or through obstacles.

## Features

**Core functionality (matches nRF firmware 1:1):**
* Bridges BLE HID and Nordic UART Service (NUS) traffic to USB HID + dual CDC ACM
* Automatic MouthPad^ discovery and reconnection
* LED status indicators (slow blink scanning, solid connected, fast flicker on activity)
* Maintenance console on CDC port 2 with shell commands (see below)
* Persistent BLE bonding across power cycles
* USB bcdDevice version automatically set from VERSION file

**Platform-specific features:**
* ESP-IDF toolchain and build system
* esptool-based flashing (USB serial downloader ROM)
* BLE device information query via `device` command
* GPIO 21 LED control on XIAO ESP32-S3

## Prerequisites

1. Install ESP-IDF v5.2+ (the project assumes `~/esp-idf`).
2. Run the helper to activate the correct Python environment before building:
   ```bash
   cd esp32
   . ./env.sh
   ```
3. Optional: edit `sdkconfig.board.<board>` if you need custom GPIO assignments.

## Build & flash

```bash
# Build for the XIAO ESP32-S3 target
make xiao

# Flash the firmware
make flash

# Monitor console output (auto-detects CDC port 1, exit with Ctrl+C)
make monitor

# Or use standard ESP-IDF commands
source env.sh
idf.py build flash monitor

# Clean generated build artefacts
make clean
```

**Note:** `idf.py monitor` does **not work** with TinyUSB CDC ports (it expects a traditional USB-serial chip with hardware flow control). Use `make monitor` instead, which uses `pyserial miniterm` with proper CDC support and line ending handling.

The Makefile sets `SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.board.xiao"` (or the LilyGo variant)
so the active configuration always matches the chosen board. If you switch boards, run `make clean`
before building to regenerate `sdkconfig` with the new defaults.

## CDC console commands

The TinyUSB CDC maintenance port (typically `/dev/cu.usbmodem<serial>3` on macOS) accepts these commands:

| Command | Description |
|---------|-------------|
| `dfu`   | Disconnect serial, print confirmation, and reboot into ROM serial downloader (run `idf.py flash` to reflash). |
| `reset` | Disconnect, clear all BLE bonds, and return to pairing mode. |
| `restart` | Restart firmware (software reset). |
| `serial` | Display USB serial number (derived from MAC address). |
| `version` | Display firmware build timestamp, ESP-IDF version, chip info, and VERSION file. |
| `device` | Display connected MouthPad^ device information (manufacturer, model, serial, firmware version, PnP ID). |

**Note:** The `device` command is ESP32-specific and not yet available in the nRF firmware.

## LED status

* **XIAO ESP32-S3** – GPIO 21 LED blinks while scanning, stays solid when connected, and pulses on HID
  traffic.
* **LilyGo T-Display-S3** – the firmware disables LED status because the board does not route an LED to
  a spare GPIO.

## Directory layout

```
esp32/
├── components/           # Local overrides (e.g. esp_tinyusb adjustments)
├── main/                 # Application sources
│   ├── ble_*             # BLE transport helpers
│   ├── usb_*             # TinyUSB HID/CDC glue
│   ├── leds.c            # LED state machine
│   └── main.c            # Bridge orchestration
├── Makefile              # Board-aware build/flash helpers
├── env.sh                # Helper to source ESP-IDF with the pinned Python env
├── sdkconfig.defaults    # Common ESP-IDF defaults
└── sdkconfig.board.*     # Board-specific overrides (LED GPIO/polarity)
```

## DFU workflow

1. Connect a terminal to the maintenance CDC port and type `dfu`, then close the terminal.
2. The firmware enters DFU pending state and resets into the ROM serial downloader.
3. Flash with `idf.py flash` (or `make flash`) targeting the ROM port.
4. The firmware restarts and reconnects to the MouthPad^.

## Version management

The firmware version is controlled by the `VERSION` file at the repository root. When you build:
- The VERSION file (e.g., `0.1.0`) is parsed by CMake
- USB bcdDevice descriptor is set automatically (e.g., `0.1.0` → `0x0010`)
- Version is logged at startup and visible in `version` command output

To release a new version, update the VERSION file and push to main. GitHub Actions will automatically build and create a release with firmware files named `mp_usb_{version}_{board}.bin`.

## Troubleshooting

* **LED stays off on XIAO** – ensure you ran `make clean` after switching from the LilyGo build so the
  correct `sdkconfig` was regenerated.
* **Device enumerates as plain serial** – you are in DFU mode. Reflash the application or power-cycle
the board.
* **Wrong version reported** – run `make clean` to regenerate build with current VERSION file.

