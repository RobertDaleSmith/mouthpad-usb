# MouthPad^ USB

Firmware for turning the **MouthPad^** wearable into a wired USB HID device without changing the
BLE firmware that runs on the mouthpiece. The microcontroller sits between the tablet/PC and the
MouthPad^, bridges BLE HID + NUS traffic to USB HID + CDC, mirrors LED behaviour, and exposes
maintenance utilities over a CDC console.

The repository currently carries **two implementations that behave the same on the wire**:

| Firmware | MCU / SDK | Status | Notes |
|----------|-----------|--------|-------|
| [`ncs/app/`](ncs/app/README.md) | nRF52840 (Nordic Connect SDK / Zephyr) | primary | Ships on Seeed XIAO nRF52840 hardware. |
| [`esp/`](esp/README.md) | ESP32-S3 (ESP-IDF) | feature parity | Mirrors the bridge for ESP32-S3 based bring-up and antenna experiments. |

Both firmwares expose the same HID descriptors, CDC command set, LED semantics, and bond-management
behaviour so you can swap hardware without reconfiguring the host.

---

## Core Capabilities

* **BLE HID client** – Discovers the MouthPad^ BLE HID service, subscribes to notifications, and
  polls RSSI for diagnostics.
* **USB HID device** – Replays the HID reports over USB using the same report descriptors as the
  mouthpiece. Hosts see the bridge as a standard multi-interface HID mouse/consumer-control device.
* **USB CDC (dual port)** –
  * CDC0 exports a Protobuf/text tunnel that mirrors the BLE NUS link.
  * CDC1 hosts a maintenance console (`dfu`, `clear`, etc.) and forwards logs.
* **Bond management** – A long press on the hardware button or the `clear` shell command wipes stored
  bonds, disconnects the current peer, and returns the bridge to pairing mode.
* **DFU hand-off** – Type `dfu` on the maintenance port to reboot into the resident UF2/ROM bootloader.
  From there you can drag a UF2 onto the mass-storage volume (nRF) or flash with `idf.py` (ESP32).
* **Status feedback** – A single LED shows scan/connect/activity states. NeoPixel support on the nRF
  build drives battery-dependent colours when present.

---

## Repository Layout

```
.
├── ncs/                # Nordic Connect SDK workspace (nRF52840 + Zephyr RTOS)
│   ├── app/            # Application source code for nRF52840 boards
│   │   └── README.md   # Build, flash, and usage instructions
│   ├── Makefile        # West/UF2 helper targets
│   └── docker-compose.yml # Containerised Zephyr build targets
├── esp/                # ESP32-S3 firmware and workspace (ESP-IDF)
│   └── README.md       # Build, flash, and usage instructions
├── docs/               # Additional documentation
└── scripts/, web_interface/, … # Shared utilities
```

---

## Getting Started

Pick the firmware that matches your hardware:

* **nRF52840 (NCS/Zephyr)** – follow [`ncs/app/README.md`](ncs/app/README.md) for environment setup, build/flash
  commands (Docker or native), and UF2/RTT tips. All NCS development happens in the `ncs/` directory with its
  own West workspace. Additional Zephyr build flows are documented in [`docs/BUILD_METHODS.md`](docs/BUILD_METHODS.md).
* **ESP32-S3 (ESP-IDF)** – follow [`esp/README.md`](esp/README.md) for sourcing ESP-IDF, board
  selection (`make xiao`/`make lilygo`), and TinyUSB console usage.

Both readmes document the LED states, CDC port layout, console commands, and how to enter DFU.

---

## CDC Maintenance Commands

Irrespective of MCU, the secondary CDC interface enumerates as the maintenance console and accepts the
same commands:

| Command | Firmware | Description |
|---------|----------|-------------|
| `dfu`   | Both | Print a confirmation, delay long enough for the message to flush, then reboot into the UF2/ROM bootloader (nRF) or ROM serial downloader (ESP32). |
| `clear` | Both | Disconnect the active MouthPad^, erase stored BLE bonds, reset LED state to scanning. |
| `serial` | Zephyr only | Print the USB serial number that macOS/OpenBSD/Linux use when naming the `usbmodem*` device nodes. |

Console logs appear on the same port, so you can verify the command succeeded before reconnecting the
MouthPad^ for pairing.

---

## DFU & Updates

* **nRF build** – double-tap reset to mount the bootloader volume, drag the UF2 produced by the build
  (`ncs/build/app/zephyr/zephyr.uf2`) onto it, or run `dfu` on the maintenance port to reboot into the
  UF2 loader without touching the board.
* **ESP32 build** – run `dfu` to switch the TinyUSB stack into DFU mode and expose the ROM serial
  downloader. Flash new firmware with `idf.py flash` (or `make flash`) on that port.

---

## Contributing & Support

* See [`AGENTS.md`](AGENTS.md) for coding style and review expectations.
* Open an issue or pull request with reproduction steps; both firmware directories carry READMEs that
  outline their respective build steps.
* The project is licensed under the MIT License.
