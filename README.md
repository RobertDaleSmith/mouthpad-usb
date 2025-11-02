# MouthPad^USB

Firmware for turning the **MouthPad^** wearable into a wired USB HID device. The microcontroller sits between the tablet/PC and the MouthPad^, bridging BLE HID + NUS traffic to USB HID + CDC, without changing the MouthPad^ firmware.

## Overview

This repository contains **two platform implementations** that expose identical USB interfaces:

| Platform | MCU / SDK | Status | Documentation |
|----------|-----------|--------|---------------|
| **Nordic nRF52840** | NCS / Zephyr RTOS | ✅ Production | [ncs/README.md](ncs/README.md) |
| **ESP32-S3** | ESP-IDF | ✅ Feature parity | [esp/README.md](esp/README.md) |

Both firmwares expose the same HID descriptors, CDC command set, LED semantics, and bond-management behavior, so you can swap hardware without reconfiguring the host.

## Quick Start

Pick the platform that matches your hardware:

### nRF52840 (Production Hardware)

Ships on **Seeed XIAO nRF52840**. Also supports Adafruit Feather nRF52840 and Nordic/April dongles.

```bash
cd ncs
docker-compose run --rm mouthpad-build
# Flash: drag build/app/zephyr/zephyr.uf2 to bootloader drive
```

**[👉 Full NCS documentation](ncs/README.md)**

### ESP32-S3

Alternative implementation for **Seeed XIAO ESP32-S3** and LilyGo T-Dongle-S3.

```bash
cd esp
source env.sh          # Source ESP-IDF environment
make xiao              # Build for XIAO ESP32-S3
make flash             # Flash firmware
```

**[👉 Full ESP32 documentation](esp/README.md)**

## Core Features

### USB Interfaces

- **USB HID** – Mouse + consumer control (standard multi-interface HID device)
- **USB CDC Port 0** – Protobuf/text tunnel mirroring BLE NUS link
- **USB CDC Port 1** – Maintenance console (`dfu`, `clear`, `serial`) + logs

### BLE Bridge

- **BLE HID client** – Discovers and connects to MouthPad^ HID service
- **Nordic UART Service (NUS)** – Bidirectional data tunnel for configuration/telemetry
- **Bond management** – Stores pairing and auto-reconnects
- **RSSI monitoring** – Tracks connection quality

### Status Feedback

- **LED indicators** – Scanning (blink), connected (solid), activity (flicker)
- **NeoPixel support** (nRF) – Battery-level colors when available
- **Console logging** – Real-time diagnostics on CDC port 1

### Device Firmware Update (DFU)

| Platform | Method |
|----------|--------|
| nRF52840 | UF2 bootloader (drag-and-drop) or type `dfu` command |
| ESP32-S3 | ROM serial downloader via `dfu` command or `idf.py flash` |

## Repository Structure

```
.
├── ncs/                    # Nordic Connect SDK workspace (nRF52840)
│   ├── app/                # Application source
│   ├── Makefile            # Build helpers
│   ├── docker-compose.yml  # Containerized builds
│   └── README.md           # 👈 nRF52840 documentation
├── esp/                    # ESP-IDF workspace (ESP32-S3)
│   ├── main/               # Application source
│   ├── Makefile            # Build helpers
│   └── README.md           # 👈 ESP32-S3 documentation
├── resources/              # Additional documentation and assets
│   ├── notes/              # Technical documentation
│   └── images/             # Diagrams and photos
└── web/                    # Web-based configuration tool
```

## CDC Maintenance Commands

Both firmwares expose a maintenance console on the second CDC port:

| Command | Description |
|---------|-------------|
| `dfu` | Reboot into bootloader (UF2 for nRF, ROM downloader for ESP32) |
| `reset` | Disconnect MouthPad^, erase BLE bonds, return to pairing mode |
| `restart` | Restart firmware (software reset) |
| `serial` | Print USB serial number |
| `version` | Display firmware version, build timestamp, and platform info |

**Port names:**
- **macOS:** `/dev/cu.usbmodem<serial>3`
- **Linux:** `/dev/ttyACM1`
- **Windows:** `COM<n>` (check Device Manager)

## Supported Hardware

### nRF52840 Boards

| Board | Status | Notes | Purchase |
|-------|--------|-------|----------|
| Seeed XIAO nRF52840 | ✅ Production | Primary shipping target ([expansion shield](https://www.seeedstudio.com/Seeeduino-XIAO-Expansion-board-p-4746.html) supported) | [Seeed Studio](https://www.seeedstudio.com/Seeed-XIAO-BLE-nRF52840-p-5201.html) |
| Adafruit Feather nRF52840 Express | ✅ Production | Requires custom bootloader config | [Adafruit](https://www.adafruit.com/product/4062) |
| Nordic nRF52840 Dongle (PCA10059) | ✅ Production | Stock Nordic LED pins | [Digi-Key](https://www.digikey.com/en/products/detail/nordic-semiconductor-asa/NRF52840-DONGLE/9491124) |
| April Brothers nRF52840 Dongle (PCA10059) | ✅ Production | Non-standard LED wiring | [Apr Brother](https://store.aprbrother.com/product/usb-dongle-nrf52840) |
| Raytac MDBT50Q-RX Dongle | ✅ Production | Single LED | [Raytac](https://www.raytac.com/product/ins.php?index_id=89) |
| MakerDiary nRF52840 MDK USB Dongle | ✅ Production | RGB LED support | [MakerDiary](https://wiki.makerdiary.com/nrf52840-mdk-usb-dongle/purchase/) |

### ESP32-S3 Boards

| Board | Status | Notes | Purchase |
|-------|--------|-------|----------|
| Seeed XIAO ESP32-S3 | ✅ Supported | Mirrors nRF feature set | [Seeed Studio](https://www.seeedstudio.com/XIAO-ESP32S3-p-5627.html) |
| LilyGo T-Dongle-S3 | ✅ Supported | Alternative form factor | [Amazon](https://www.amazon.com/dp/B0BF542H39) |

## CI/CD

GitHub Actions builds firmware for all board variants on every push to `main`:

**nRF52840 builds:**
- `mouthpad^usb_seeed_xiao_nrf52840_<commit>.uf2`
- `mouthpad^usb_adafruit_feather_nrf52840_<commit>.uf2`
- `mouthpad^usb_nordic_nrf52840dongle_<commit>.uf2`
- `mouthpad^usb_aprbrother_nrf52840_<commit>.uf2`
- `mouthpad^usb_raytac_mdbt50q_rx_<commit>.uf2`
- `mouthpad^usb_makerdiary_nrf52840mdk_<commit>.uf2`

Artifacts are available in the Actions tab for 90 days.

## Contributing

1. **Choose your platform** – Work in either `ncs/` or `esp/` directory
2. **Follow build instructions** – See platform-specific README
3. **Test on hardware** – Verify USB enumeration and BLE connection
4. **Submit PR** – Include board(s) tested in description

Both platforms should maintain behavioral parity for USB/BLE interfaces.

## Getting Help

- **Platform-specific issues:** See [ncs/README.md](ncs/README.md) or [esp/README.md](esp/README.md)
- **General questions:** Open an issue with reproduction steps
- **Build problems:** Check platform-specific troubleshooting sections

## License

MIT License – See [LICENSE](LICENSE) for details.
