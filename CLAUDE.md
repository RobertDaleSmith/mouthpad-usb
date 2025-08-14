# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

MouthPad^USB is a Zephyr RTOS application for the **Seeed XIAO nRF52840** that acts as a bridge between BLE HID devices and USB HID, with Device Firmware Upgrade (DFU) support. The system enables wireless BLE devices (mice, keyboards) to be used as wired USB devices.

## Build Commands

### Primary Build Method (Make)
```bash
make build      # Build with west for xiao_ble board (pristine)
make flash      # Flash firmware to device
make monitor    # Monitor serial output via west
make clean      # Remove build directory
make fullclean  # Remove entire workspace (requires re-init)
```

### Alternative Build Methods
1. **Docker**: `docker-compose run --rm mouthpad-build`
2. **VS Code**: Use NRF Connect extension with `/app` directory
3. **Traditional West Workspace**: 
   ```bash
   west build -b xiao_ble app --pristine=always
   west flash
   west monitor
   ```

### Workspace Initialization
If workspace is not initialized:
```bash
west init -m https://github.com/nrfconnect/sdk-nrf.git --mr main
west update
```

## Architecture Overview

The application follows a modular transport bridge architecture with three main layers:

### Core Components

1. **BLE Transport Layer** (`ble_transport.c/h`)
   - Central connection management
   - BLE NUS (Nordic UART Service) client 
   - BLE HID client for future integration
   - Connection state management and bridging coordination

2. **USB Interfaces**
   - **USB HID** (`usb_hid.c/h`): Mouse/keyboard emulation with custom descriptors
   - **USB CDC** (`usb_cdc.c/h`): Serial communication bridge for BLE NUS data

3. **BLE Clients**
   - **BLE Central** (`ble_central.c/h`): Device discovery and connection
   - **BLE NUS Client** (`ble_nus_client.c/h`): Nordic UART Service communication
   - **BLE HID** (`ble_hid.c/h`): HID device communication

### Data Flow
```
BLE Device → BLE Transport → USB Interface → Host Computer
   ↑                                           ↓
   ←← ←← ←← ←← ←← ←← ←← ←← ←← ←← ←← ←← ←← ←← ←← ←←
```

## Key Configuration

### Target Hardware
- **Primary Board**: `xiao_ble` (Seeed XIAO nRF52840)
- **Flash Layout**: MCUboot with dual application slots
- **Bootloader**: MCUboot + UF2 support for easy updates

### Important Config Options (prj.conf)
- `CONFIG_BT_CENTRAL=y` - BLE central role
- `CONFIG_USB_DEVICE_HID=y` - USB HID device
- `CONFIG_USB_CDC_ACM=y` - USB CDC serial
- `CONFIG_BUILD_OUTPUT_UF2=y` - UF2 bootloader support
- `CONFIG_USE_SEGGER_RTT=y` - RTT for debug output

### Build Output Locations
- **UF2 File**: `build/zephyr/app.uf2` (for UF2 bootloader)
- **HEX File**: `build/zephyr/merged.hex` (for direct flashing)

## Development Workflow

### Making Changes
1. Edit source files in `app/src/`
2. Build with `make build` (always uses --pristine)
3. Flash with `make flash`  
4. Monitor with `make monitor` or RTT viewer

### Testing
- Serial debug output via RTT (Segger RTT Viewer)
- USB CDC creates virtual serial port for BLE NUS data
- USB HID emulates mouse/keyboard on host system

### DFU Updates
1. **UF2 Method**: Double-tap reset → copy `app.uf2` to `XIAO-SENSE` volume
2. **Standard DFU**: Use MCUboot DFU procedures

## Architecture Notes

### Transport Bridge Pattern
The system uses a centralized transport bridge (`ble_transport`) that:
- Manages BLE connections and service discovery
- Routes data between BLE services and USB interfaces  
- Handles connection state and error recovery
- Provides callbacks for bidirectional data flow

### Future HID Integration
Current implementation focuses on BLE NUS ↔ USB CDC bridging. HID bridging infrastructure exists but is not fully implemented. The `ble_transport` layer already provides HID callback registration for future expansion.

### Memory Layout
- MCUboot bootloader: 0x00000-0x0FFFF (64KB)
- App slot 0: 0x10000-0x4FFFF (256KB)  
- App slot 1: 0x50000-0x8FFFF (256KB)
- Storage: 0xA0000-0xFFFFF (384KB)

## Common Issues

- Build environment issues → Use Docker method
- USB permissions → Check device access rights  
- BLE connection problems → Verify device advertising and pairing
- Always use `--pristine=always` builds for reliability