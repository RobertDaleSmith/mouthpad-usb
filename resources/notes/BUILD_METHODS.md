# MouthPad USB Build Methods

This document describes the four different ways to build the MouthPad USB firmware.

## Method 1: Zephyr Workspace (Traditional NCS)

**Best for:** Development, debugging, and when you need full NCS workspace access.

### Setup:
```bash
# Create a Zephyr workspace
mkdir ~/zephyr_workspace
cd ~/zephyr_workspace

# Initialize with NCS
west init -m https://github.com/nrfconnect/sdk-nrf.git --mr main
west update

# Copy the mouthpad_usb app into the workspace
cp -r /path/to/mouthpad_usb/app projects/mouthpad_usb
```

### Build:
```bash
# From the workspace root
west build -b xiao_ble projects/mouthpad_usb --pristine=always
```

### Flash:
```bash
west flash --runner jlink --build-dir build/mouthpad_usb
```

## Method 2: Standalone Workspace (Make Commands)

**Best for:** Quick builds and CI/CD integration.

### Setup:
The project includes a `Makefile` with convenient commands:

```bash
# Build for xiao_ble
make build

# Flash the firmware
make flash

# Open serial monitor
make monitor
```

### Requirements:
- Zephyr SDK installed locally
- `west` tool installed
- Proper environment variables set

## Method 3: VS Code NRF Connect Extension

**Best for:** Visual development with integrated debugging.

### Setup:
1. Install the "NRF Connect for VS Code" extension
2. Open the project folder in VS Code
3. Use Command Palette: `NRF Connect: Add Existing Project`
4. Select the `/app` directory
5. Create a build configuration:
   - Board: `xiao_ble`
   - Build directory: `build`
   - Configuration: Use project's `prj.conf`

### Build:
- Use the VS Code build panel
- Or Command Palette: `NRF Connect: Build Project`

## Method 4: Docker (Recommended)

**Best for:** Consistent builds, CI/CD, and avoiding local environment setup.

### Prerequisites:
- Docker and Docker Compose installed
- No local Zephyr SDK required

### Quick Build:
```bash
# Build all boards (currently only xiao_ble active)
docker-compose run --rm mouthpad-build
```

### Interactive Development:
```bash
# Start development container
docker-compose run --rm mouthpad-dev

# Inside container:
cd /zephyr_workspace
west build -b xiao_ble app --pristine=always
```

### Build Artifacts:
- UF2 files are automatically generated with date stamps
- Located in `./build/` directory
- Named: `mouthpad_usb_xiao_ble_YYYYMMDD.uf2`

## Build Output Comparison

| Method | Speed | Setup Complexity | Debugging | CI/CD Ready |
|--------|-------|------------------|-----------|-------------|
| Zephyr Workspace | Medium | High | Excellent | Good |
| Make Commands | Fast | Medium | Good | Excellent |
| VS Code Extension | Medium | Low | Excellent | Poor |
| Docker | Fast | Low | Good | Excellent |

## Troubleshooting

### Docker Build Issues:
```bash
# Clear all caches
docker-compose down -v
docker system prune -f

# Rebuild from scratch
docker-compose run --rm mouthpad-build
```

### Local Build Issues:
```bash
# Clean build directory
rm -rf build/

# Update west workspace
west update

# Rebuild
west build -b xiao_ble app --pristine=always
```

### VS Code Extension Issues:
- Ensure the correct board is selected
- Check that the `/app` directory is properly recognized
- Verify `prj.conf` is in the app directory

## Environment Variables

For local builds, ensure these are set:
```bash
export ZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-sdk
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export PATH=$ZEPHYR_SDK_INSTALL_DIR/sysroots/x86_64-pokysdk-linux/usr/bin:$PATH
```

## Board Support

Currently supported boards:
- `xiao_ble` (primary target)
- `nrf52840dongle` (commented out due to build issues)
- `nrf52840dk/nrf52840` (commented out due to build issues)

To enable additional boards, uncomment the relevant sections in `docker-compose.yml`. 