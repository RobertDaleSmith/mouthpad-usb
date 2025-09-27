# Board Configuration

This project supports multiple ESP32-S3 development boards through ESP-IDF's Kconfig system.

## Supported Boards

### XIAO ESP32-S3 (Default)
- **LED**: GPIO 21 (built-in)
- **Button**: GPIO 0 (boot button)
- **Features**: Compact form factor with built-in LED

### LilyGo T-Display-S3
- **LED**: Disabled (no built-in LED)
- **Button**: GPIO 0 (boot button)
- **Display**: 1.9" LCD (pins reserved for future implementation)
- **Features**: Built-in color display

## Configuration Methods

### Method 1: Using Pre-configured Files (Recommended)

Copy the appropriate configuration file:

```bash
# For LilyGo T-Display-S3
cp sdkconfig.lilygo_t_display_s3 sdkconfig.board
cat sdkconfig.board >> sdkconfig

# For XIAO ESP32-S3
cp sdkconfig.xiao_esp32s3 sdkconfig.board
cat sdkconfig.board >> sdkconfig
```

### Method 2: Using menuconfig

```bash
idf.py menuconfig
```

Navigate to: `MouthPad Configuration → Board Type`

Select your target board and save.

### Method 3: Command Line Configuration

```bash
# Set board type directly
idf.py set-config CONFIG_MOUTHPAD_BOARD_LILYGO_T_DISPLAY_S3=y
idf.py set-config CONFIG_MOUTHPAD_LED_GPIO=-1

# Or for XIAO ESP32-S3
idf.py set-config CONFIG_MOUTHPAD_BOARD_XIAO_ESP32S3=y
idf.py set-config CONFIG_MOUTHPAD_LED_GPIO=21
```

## Build Process

After configuration:

```bash
idf.py build
idf.py flash
idf.py monitor
```

## Configuration Files

- `sdkconfig.xiao_esp32s3` - XIAO ESP32-S3 board settings
- `sdkconfig.lilygo_t_display_s3` - LilyGo T-Display-S3 board settings
- `main/Kconfig.projbuild` - Kconfig menu definitions
- `main/board_config.h` - Board-specific pin definitions

## Adding New Boards

1. Add new board choice in `main/Kconfig.projbuild`
2. Update `main/board_config.h` with GPIO definitions
3. Create a new `sdkconfig.board_name` file
4. Update this README

## Technical Details

The board configuration uses ESP-IDF's standard Kconfig system:

- **Board Selection**: `CONFIG_MOUTHPAD_BOARD_*` choice options
- **LED GPIO**: `CONFIG_MOUTHPAD_LED_GPIO` (-1 disables LED)
- **Button GPIO**: `CONFIG_MOUTHPAD_BUTTON_GPIO`
- **Board Name**: `CONFIG_MOUTHPAD_BOARD_NAME` (for logging)

This approach follows ESP-IDF best practices and allows external configuration without modifying source code.