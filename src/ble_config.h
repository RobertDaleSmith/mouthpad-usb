#ifndef BLE_CONFIG_H
#define BLE_CONFIG_H

// BLE Device Configuration
// Uncomment and modify these to target a specific device

// Option 1: Target by MAC address (most reliable for specific device)
// #define TARGET_DEVICE_ADDR "AA:BB:CC:DD:EE:FF"

// Option 2: Target by device name (less reliable, but easier)
// #define TARGET_DEVICE_NAME "MyMouse"

// Option 3: Auto-connect to MouthPad devices (RECOMMENDED for distribution)
#define AUTO_CONNECT_MOUTHPAD

// Option 4: Auto-connect to any HID+UART device (fallback)
// #define AUTO_CONNECT_HID_UART

// Option 5: Specific device configurations
// Uncomment one of these for your specific device:

// For RDSMouthPad device (for testing)
// #define TARGET_DEVICE_ADDR "F0:1A:5F:52:2A:3E"
// #define TARGET_DEVICE_NAME "RDSMouthPad"

// Connection preferences
#define CONNECTION_TIMEOUT_MS 10000  // 10 seconds
#define MAX_CONNECTION_ATTEMPTS 3

// Scan preferences
#define SCAN_DURATION_SECONDS 30     // How long to scan before restarting

#endif // BLE_CONFIG_H 