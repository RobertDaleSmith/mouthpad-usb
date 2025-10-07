# MouthPad^ USB – ESP32 Firmware (ESP-IDF)

ESP-IDF port of the MouthPad^ USB bridge for ESP32-S3 boards. Functionally mirrors the Zephyr/nRF
firmware: it scans for the MouthPad^ over BLE, relays HID and NUS traffic to USB, exposes the same dual
CDC interfaces, and offers the same maintenance commands.

## Target hardware

* **Seeed Studio XIAO ESP32-S3** – validated board with user button and LED on GPIO 21.
* **LilyGo T-Display-S3** – buildable but the board lacks a dedicated user LED; the firmware disables LED
  status on that variant.

Select the board via `make xiao` or `make lilygo` (defaults to XIAO). The Makefile injects board-specific
`sdkconfig` fragments so switching targets does not leave stale settings behind.

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

The TinyUSB CDC maintenance port accepts the same commands as the nRF firmware:

| Command | Description |
|---------|-------------|
| `dfu`   | Disconnect serial, print confirmation, and reboot into the ROM serial downloader. Run `idf.py flash` while the port stays in download mode. |
| `reset` | Disconnect, clear stored bonds (via `ble_bonds_clear_all()`), and return to scanning. |
| `restart` | Restart firmware (software reset via `esp_restart()`). |
| `serial` | Display USB serial number (MAC address). |
| `version` | Display firmware build timestamp, ESP-IDF version, and chip information. |
| `device` | Display connected MouthPad^ device information from BLE Device Information Service (manufacturer, model, serial, firmware version, etc.). |

Logs arrive on the same port (typically `/dev/cu.usbmodem<serial>3` on macOS).

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

## Troubleshooting

* **LED stays off on XIAO** – ensure you ran `make clean` after switching from the LilyGo build so the
  correct `sdkconfig` was regenerated.
* **Device enumerates as plain serial** – you are in DFU mode. Reflash the application or power-cycle
the board.

