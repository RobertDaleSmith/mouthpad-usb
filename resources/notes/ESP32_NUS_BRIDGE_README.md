# BLE HID + NUS Client to USB Bridge

This ESP32 firmware acts as a BLE central device that can connect to a single remote device offering both HID and NUS (Nordic UART Service) services, just like the nRF firmware implementation.

## Architecture
- **BLE Central**: Connects to a single device with both HID and NUS services
- **HID Bridge**: Forwards BLE HID reports to USB HID interface
- **NUS Bridge**: Forwards BLE NUS data to USB CDC with packet framing
- **Dual Service Support**: Both services work simultaneously on the same connection

## Configuration

The firmware supports both services by default. You can disable NUS support if not needed:

```c
// In main/app_config.h
#define ENABLE_HID_CENTRAL_MODE 1  // Always enabled
#define ENABLE_NUS_CLIENT_MODE 1   // Set to 0 to disable NUS support
```

## How It Works

1. **Device Discovery**: Scans for BLE devices with HID services
2. **Connection**: Connects to the best HID device found
3. **Service Discovery**: 
   - Discovers HID services (for HID reports)
   - Discovers NUS services (for serial data)
4. **Data Bridging**:
   - HID reports → USB HID interface
   - NUS data ↔ USB CDC interface (with packet framing)

## NUS Bridge Details

### Packet Format
```
[0xAA][0x55][Length_H][Length_L][Data...][CRC_H][CRC_L]
```

- **Start Markers**: `0xAA 0x55`
- **Length**: 2 bytes, big-endian (variable length)
- **Data**: Variable length payload (no 64-byte limit with proper framing)
- **CRC**: 2 bytes, CCITT CRC-16, big-endian

### BLE Service Details
- **Service UUID**: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
- **TX Characteristic**: Auto-detected by NOTIFY property (receives data FROM device)
- **RX Characteristic**: Auto-detected by WRITE property (sends data TO device)
- **Handle Detection**: Uses property-based discovery instead of hardcoded UUIDs

### Usage
1. Flash firmware (both HID and NUS clients are enabled by default)
2. Device scans for and connects to BLE devices with HID services
3. Once connected, it automatically discovers NUS services on the same device
4. Data flows bidirectionally:
   - HID reports: BLE → USB HID
   - NUS data: BLE ↔ USB CDC (with packet framing)

## Logging

### Separate Debug Channel
- **USB CDC**: Clean NUS bridge data only (no logs)
- **UART0**: Debug logs via TX|6 pin on XIAO expansion board servo connector
- **Connection**: Connect J-Link or USB-to-Serial to TX|6 pin at 115200 baud
- **Result**: Same clean separation as nRF RTT implementation

## Troubleshooting

### Device Not Enumerating
- Check USB cable connection
- Ensure firmware compiled and flashed successfully
- Check debug logs on TX|6 pin (servo connector) for initialization errors

### BLE Connection Issues
- Device acts as BLE central (client) for both HID and NUS services
- Make sure target device advertises HID services
- NUS service discovery happens automatically after HID connection
- Both services work on the same BLE connection
- Characteristics auto-detected by properties (NOTIFY for TX, WRITE for RX)

### NUS Data Issues
- Large packets (>64 bytes) are properly framed and sent as complete packets
- No fragmentation needed - packet framing handles variable lengths
- Check debug logs on TX|6 pin for detailed NUS communication info
