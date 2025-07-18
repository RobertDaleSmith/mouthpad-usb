# MouthPad^ USB 

### BLE-USB Bridge with MCUboot DFU Support

A Zephyr RTOS application for the **Seeed XIAO nRF52840** that acts as a bridge between BLE HID devices and USB HID, with full Device Firmware Upgrade (DFU) support.

## 🎯 Features

- **BLE Central**: Connects to BLE HID devices (mice, keyboards, etc.)
- **USB HID Device**: Emulates the same HID device over USB
- **BLE UART**: Forwards additional data over USB CDC serial
- **MCUboot DFU**: Full over-the-air firmware update support
- **UF2 Support**: Easy firmware updates via UF2 bootloader

## 🛠️ Hardware Requirements

- **Seeed XIAO nRF52840** board
- USB cable for programming and USB HID functionality
- BLE HID device to bridge (e.g., wireless mouse, keyboard)

## 🚀 Quick Start

### 1. Build the Project

```bash
# Build with MCUboot bootloader
make build
```

### 2. Flash the Device

You have three options for flashing:

#### Option A: MCUboot DFU (Recommended)
```bash
# Flash the initial firmware
make flash

# To update firmware later:
# 1. Connect to device via serial
make monitor
# 2. Type 'dfu' in the shell
# 3. Run 'make flash' again
```

#### Option B: UF2 Bootloader
```bash
# Get UF2 flashing instructions
make flash-uf2

# Then follow the instructions to copy the UF2 file
```

#### Option C: Manual UF2
1. Double-tap the reset button to enter UF2 mode
2. Copy `build/zephyr/app.uf2` to the `XIAO-SENSE` volume

### 3. Monitor Output

```bash
# Monitor serial output for debugging
make monitor
```

## 🔧 DFU (Device Firmware Upgrade)

This project includes full DFU support with two methods:

### Method 1: MCUboot DFU (Over-the-Air)

1. **Initial Setup**: Flash the device with `make flash` (includes MCUboot bootloader)
2. **Enter DFU Mode**: 
   - Connect via serial: `make monitor`
   - Type `dfu` in the shell
   - Device reboots into bootloader mode
3. **Update Firmware**: Run `make flash` again

### Method 2: UF2 Bootloader

1. **Enter UF2 Mode**: Double-tap the reset button
2. **Copy Firmware**: Copy `build/zephyr/app.uf2` to the `XIAO-SENSE` volume
3. **Automatic Restart**: Device restarts with new firmware

## 📁 Project Structure

```
├── src/                  # Application source code
│   ├── main.c            # Main application logic
│   ├── ble_hid_c.c       # BLE HID client
│   └── ble_uart_c.c      # BLE UART client
├── scripts/
│   ├── build.sh          # Build script with MCUboot
│   ├── flash.sh          # Flash script
│   ├── flash-uf2.sh      # UF2 flash instructions
│   ├── monitor.sh        # Serial monitor
│   └── 
├── prj.conf               # Zephyr configuration
└── Makefile               # Build system
```

## ⚙️ Configuration

### Flash Partitions (MCUboot)

The device uses the following flash layout:
- **0x00000-0x0FFFF**: MCUboot bootloader (64KB)
- **0x10000-0x4FFFF**: Application slot 0 (256KB)
- **0x50000-0x8FFFF**: Application slot 1 (256KB)
- **0x90000-0x9FFFF**: Scratch area (64KB)
- **0xA0000-0xFFFFF**: Storage (384KB)

### Key Configuration Options

- `CONFIG_BOOTLOADER_MCUBOOT=y`: Enable MCUboot bootloader
- `CONFIG_USB_DFU_CLASS=y`: Enable USB DFU support
- `CONFIG_BUILD_OUTPUT_UF2=y`: Generate UF2 files
- `CONFIG_BT_CENTRAL=y`: Enable BLE central mode
- `CONFIG_BT_PERIPHERAL=y`: Enable BLE peripheral mode

## 🔍 Troubleshooting

### DFU Issues

**Problem**: Device reboots but doesn't enter DFU mode
**Solution**: 
1. Ensure MCUboot is properly flashed: `make flash`
2. Check that the device tree overlay is correct
3. Verify flash partitions are properly configured

**Problem**: UF2 volume doesn't appear
**Solution**:
1. Double-tap the reset button quickly
2. Or hold reset while plugging in USB cable
3. Check that `CONFIG_BUILD_OUTPUT_UF2=y` is set

### Build Issues

**Problem**: Build fails with MCUboot errors
**Solution**:
1. Clean build: `make clean`
2. Rebuild: `make build`
3. Check that all required configurations are set in `prj.conf`

### Connection Issues

**Problem**: BLE device not connecting
**Solution**:
1. Check device is in range and advertising
2. Verify device has HID service
3. Monitor serial output: `make monitor`

## 📚 Available Commands

```bash
make build      # Build with MCUboot bootloader
make clean      # Clean build directory
make flash      # Flash via west (requires bootloader mode)
make flash-uf2  # Get UF2 flashing instructions
make monitor    # Monitor serial output
make dfu        # Trigger DFU mode via serial
make help       # Show all available commands
```

## 🔄 Development Workflow

1. **Make Changes**: Edit source files in `src/`
2. **Build**: `make build`
3. **Test**: `make monitor` to see output
4. **Flash**: `make flash` or use UF2 method
5. **Iterate**: Repeat as needed

## 📖 Additional Resources

- [Seeed XIAO nRF52840 Documentation](https://wiki.seeedstudio.com/XIAO-BLE/)
- [MCUboot Documentation](https://docs.mcuboot.com/)
- [Zephyr RTOS Documentation](https://docs.zephyrproject.org/)
- [Nordic nRF Connect SDK](https://developer.nordicsemi.com/nRF_Connect_SDK/)

## 🤝 Contributing

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Test thoroughly
5. Submit a pull request

## 📄 License

This project is licensed under the MIT License - see the LICENSE file for details.