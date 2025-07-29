# MouthPad^ USB 

A Zephyr RTOS application for the **Seeed XIAO nRF52840** that acts as a bridge between BLE HID devices and USB HID, with full Device Firmware Upgrade (DFU) support.

<div style="text-align: center;">
   <img src="docs/images/mouthpad_usb.png" style="max-width: 420px">
</div>

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

This project supports **four different build methods**. For detailed instructions on each method, see [docs/BUILD_METHODS.md](docs/BUILD_METHODS.md).

### Method 1: Docker (Recommended)

The easiest way to build this project is using Docker, which provides a consistent environment and avoids local setup issues.

```bash
# Build all supported boards
docker-compose run --rm mouthpad-build

# Or start interactive development environment
docker-compose run --rm mouthpad-dev
```

### Method 2: Make Commands

For quick local builds using the included Makefile:

```bash
# Build the project
make build

# Flash to device
make flash

# Monitor serial output
make monitor
```

### Method 3: VS Code Extension

For visual development with integrated debugging:

1. Install "NRF Connect for VS Code" extension
2. Open project in VS Code
3. Use Command Palette: `NRF Connect: Add Existing Project`
4. Select the `/app` directory
5. Build using the VS Code interface

### Method 4: Traditional Zephyr Workspace

For full NCS workspace access:

```bash
# Create workspace
mkdir ~/zephyr_workspace
cd ~/zephyr_workspace
west init -m https://github.com/nrfconnect/sdk-nrf.git --mr main
west update

# Copy app and build
cp -r /path/to/mouthpad_usb/app projects/mouthpad_usb
west build -b xiao_ble projects/mouthpad_usb --pristine=always
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
├── app/                  # Application source code
│   ├── src/              # Source files
│   ├── prj.conf          # Zephyr configuration
│   └── CMakeLists.txt    # Build configuration
├── docs/                 # Documentation
│   ├── BUILD_METHODS.md  # Detailed build instructions
│   └── images/           # Project images
├── .github/workflows/    # CI/CD configuration
├── docker-compose.yml    # Docker build configuration
└── Makefile              # Local build commands
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

For detailed troubleshooting information, see [docs/BUILD_METHODS.md](docs/BUILD_METHODS.md).

### Common Issues

**Problem**: Build fails with environment issues
**Solution**: Use Docker method for consistent environment

**Problem**: USB device not accessible
**Solution**: Ensure proper permissions and device access

**Problem**: BLE device not connecting
**Solution**: Check device advertising and HID service availability

## 📚 Available Commands

```bash
# Docker commands
docker-compose run --rm mouthpad-build  # Build all boards
docker-compose run --rm mouthpad-dev     # Interactive development

# Make commands
make build      # Build with MCUboot bootloader
make clean      # Clean build directory
make flash      # Flash via west
make monitor    # Monitor serial output
make help       # Show all available commands
```

## 🔄 Development Workflow

### Local Development

1. **Make Changes**: Edit source files in `app/src/`
2. **Build**: Use any of the four build methods
3. **Test**: Monitor output and test functionality
4. **Flash**: Flash to device for testing
5. **Iterate**: Repeat as needed

### CI/CD Pipeline

This project includes GitHub Actions that automatically build and test your code:

- **Automatic Builds**: Every push to `main` triggers a build
- **Multi-board Support**: Builds for supported boards
- **Artifact Storage**: Build artifacts are automatically uploaded
- **Caching**: Uses ccache for faster builds

## 📖 Additional Resources

- [Build Methods Documentation](docs/BUILD_METHODS.md)
- [Seeed XIAO nRF52840 Documentation](https://wiki.seeedstudio.com/XIAO-BLE/)
- [MCUboot Documentation](https://docs.mcuboot.com/)
- [Zephyr RTOS Documentation](https://docs.zephyrproject.org/)
- [Nordic nRF Connect SDK](https://developer.nordicsemi.com/nRF_Connect_SDK/)

## 🤝 Contributing

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Test thoroughly using one of the build methods
5. Submit a pull request

## 📄 License

This project is licensed under the MIT License - see the LICENSE file for details.