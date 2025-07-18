make# BLE Device Selection Guide

## Current Behavior
The updated firmware now provides detailed information about **HID devices only** and allows you to target specific devices. Non-HID devices are automatically filtered out to reduce noise.

## How to Select Your BLE HID Mouse

### Step 1: Flash and Test
1. Flash the current firmware to your device
2. Monitor the serial output to see all discovered **HID devices**
3. Look for your mouse in the device list

### Step 2: Identify Your Device
The firmware will now show detailed information for each **HID device**:
```
BLE: HID Device #1: AA:BB:CC:DD:EE:FF
BLE:   Name: MyMouse
BLE:   RSSI: -45
BLE:   Services: HID=YES, UART=YES
```

**Note**: Only devices with HID services are shown. Non-HID devices (phones, speakers, etc.) are automatically filtered out.

### Step 3: Configure Target Device

#### Option A: Target by MAC Address (Recommended)
1. Edit `src/ble_config.h`
2. Uncomment and set the MAC address:
```c
#define TARGET_DEVICE_ADDR "AA:BB:CC:DD:EE:FF"  // Replace with your mouse's MAC
```
3. Comment out the auto-connect option:
```c
// #define AUTO_CONNECT_HID_UART
```

#### Option B: Target by Device Name
1. Edit `src/ble_config.h`
2. Uncomment and set the device name:
```c
#define TARGET_DEVICE_NAME "MyMouse"  // Replace with your mouse's name
```
3. Comment out the auto-connect option:
```c
// #define AUTO_CONNECT_HID_UART
```

#### Option C: Auto-connect to HID+UART Devices (Current)
The current configuration will automatically connect to any device that has both HID and UART services.

### Step 4: Rebuild and Flash
```bash
west build
west flash
```

## Device Information Explained

### Services
- **HID=YES**: Device supports Human Interface Device (mouse/keyboard) - **Always YES for shown devices**
- **UART=YES**: Device supports Nordic UART Service (for data communication)

### RSSI
- Signal strength indicator
- Higher (less negative) values = stronger signal
- Good range: -30 to -60 dBm

### Device Name
- May be shortened or complete
- Some devices don't broadcast names

## Filtering Behavior

### What's Shown
- ✅ **HID devices** (mice, keyboards, game controllers)
- ✅ **Devices with HID services** in their advertising data

### What's Filtered Out
- ❌ **Non-HID devices** (phones, speakers, headphones)
- ❌ **Devices without HID services**
- ❌ **Generic BLE devices** (sensors, beacons, etc.)

This filtering reduces noise and makes it easier to find your target device.

## Troubleshooting

### Device Not Found
1. Ensure the device is in pairing/discovery mode
2. Check if the device is advertising HID services
3. Verify the device is within range
4. **Note**: Non-HID devices won't appear in the list

### Wrong Device Connected
1. Use MAC address targeting for most reliable results
2. Check the device name carefully (case sensitive)
3. Verify the services match your expectations

### Connection Issues
1. The firmware now uses conservative connection parameters
2. Connection timeout increased to 10 seconds
3. Automatic retry logic (up to 3 attempts)

## Example Configuration

For a mouse named "Logitech MX Master 3" with MAC address "12:34:56:78:9A:BC":

```c
// In src/ble_config.h
#define TARGET_DEVICE_ADDR "12:34:56:78:9A:BC"
// #define TARGET_DEVICE_NAME "Logitech MX Master 3"  // Alternative
// #define AUTO_CONNECT_HID_UART  // Comment out for specific targeting
```

## Current Status
The firmware is currently configured to auto-connect to any HID+UART device. Only HID devices are shown in the scan results. To target your specific mouse, follow Step 3 above. 