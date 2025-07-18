# MouthPad Distribution Guide

## Overview
This firmware is designed to automatically detect and connect to any MouthPad device, regardless of the specific name or MAC address. It's perfect for distribution to users who have their own MouthPad devices.

## How It Works

### 🎯 **Smart MouthPad Detection**
The firmware automatically detects MouthPad devices by name patterns:

**Supported Name Variations:**
- `MouthPad`, `mouthpad`, `MOUTHPAD`
- `Mouth Pad`, `mouth pad`, `MOUTH PAD`
- `Mouth-Pad`, `mouth-pad`, `MOUTH-PAD`
- `Mouth_Pad`, `mouth_pad`, `MOUTH_PAD`
- `RDSMouthPad`, `rdsmouthpad`, `RDSMOUTHPAD`
- `RDS MouthPad`, `rds mouthpad`, `RDS MOUTHPAD`

### 🔄 **Connection Priority**
1. **Priority 1**: MouthPad devices (highest priority)
2. **Priority 2**: HID devices with UART services
3. **Priority 3**: Other HID devices by name pattern

## Configuration for Distribution

### ✅ **Recommended Configuration (Default)**
The firmware is pre-configured for distribution. In `src/ble_config.h`:
```c
// Auto-connect to MouthPad devices (RECOMMENDED for distribution)
#define AUTO_CONNECT_MOUTHPAD
```

### 🔧 **Alternative Configurations**

#### For Specific Device Testing
```c
// Comment out MouthPad auto-connect
// #define AUTO_CONNECT_MOUTHPAD

// Uncomment for specific device
#define TARGET_DEVICE_ADDR "F0:1A:5F:52:2A:3E"
```

#### For HID+UART Devices Only
```c
// Comment out MouthPad auto-connect
// #define AUTO_CONNECT_MOUTHPAD

// Uncomment for HID+UART devices
#define AUTO_CONNECT_HID_UART
```

## User Experience

### 📱 **What Users Will See**
```
BLE: Auto-connecting to MouthPad devices (recommended for distribution)
BLE: HID Device #1: AA:BB:CC:DD:EE:FF
BLE:   Name: RDSMouthPad
BLE:   RSSI: -45
BLE:   Services: HID=YES, UART=NO
BLE: Found MouthPad device - attempting connection
BLE: Device connected successfully!
```

### 🚀 **Automatic Operation**
1. **Plug in the device** - Firmware starts automatically
2. **Scan for devices** - Only HID devices are shown
3. **Auto-detect MouthPad** - Recognizes any MouthPad device
4. **Auto-connect** - Establishes connection automatically
5. **Ready to use** - No user configuration required

## Distribution Benefits

### ✅ **Universal Compatibility**
- Works with any MouthPad device
- No need to know specific MAC addresses
- Handles different naming conventions
- Case-insensitive detection

### 🔧 **Zero Configuration**
- Users don't need to configure anything
- Works out of the box
- No technical knowledge required
- Automatic device discovery

### 🛡️ **Robust Detection**
- Multiple name pattern matching
- Fallback to HID+UART devices
- Graceful error handling
- Automatic retry logic

## Building for Distribution

### 🏗️ **Standard Build**
```bash
west build
west flash
```

### 📦 **Distribution Package**
1. Build the firmware: `west build`
2. The UF2 file is ready for distribution: `build/zephyr.uf2`
3. Users can flash it directly to their device

## Troubleshooting

### Device Not Found
- Ensure MouthPad is in pairing mode
- Check if device name contains "MouthPad" or variations
- Verify device is within range

### Wrong Device Connected
- MouthPad detection has highest priority
- Only MouthPad devices will auto-connect
- Other devices require manual configuration

### Connection Issues
- Conservative connection parameters for compatibility
- 10-second connection timeout
- Automatic retry (up to 3 attempts)

## Customization

### Adding New Name Patterns
Edit the `is_mouthpad_device()` function in `src/main.c`:
```c
// Add new patterns here
if (strstr(device_name, "newpattern") || strstr(device_name, "NewPattern")) {
    return true;
}
```

### Changing Priority
Modify the connection logic in the scan callback:
```c
// Adjust priority order as needed
if (is_mouthpad_device(device_name)) {
    // Highest priority
} else if (has_hid_service && has_uart_service) {
    // Medium priority
} else if (other_patterns) {
    // Lower priority
}
```

## Version Information
- **Firmware Version**: 1.0
- **BLE Stack**: Zephyr RTOS
- **Target Platform**: nRF52840
- **Compatibility**: Any MouthPad device
- **Auto-Detection**: Enabled by default 