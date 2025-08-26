# Even G1 Integration Plan

## Overview
This document outlines the implementation plan for integrating Even G1 smart glasses with the MouthPad^USB firmware, enabling multi-device BLE connections to support both MouthPad^ and Even G1 simultaneously.

## Current State Analysis

### Existing Infrastructure
- **Connection Support**: CONFIG_BT_MAX_CONN=8 (supports up to 8 simultaneous connections)
- **Single Connection Model**: Currently uses `default_conn` pointer for single device
- **Service Discovery**: Sequential NUS → HID discovery pattern
- **Scan Filters**: Currently filters for HID UUID only (MouthPad^ detection)

### Required Changes
1. **Multi-connection management**: Track multiple bt_conn pointers
2. **Device type detection**: Identify Even G1 vs MouthPad^ during scanning
3. **Parallel service discovery**: Handle multiple devices with different services
4. **Protocol handling**: Implement Even G1 specific protocol

## Implementation Architecture

### 1. Connection Management Refactor

#### Data Structures
```c
// ble_transport.h additions
typedef enum {
    DEVICE_TYPE_UNKNOWN = 0,
    DEVICE_TYPE_MOUTHPAD,      // NUS + HID
    DEVICE_TYPE_EVEN_G1_LEFT,  // NUS only (left arm)
    DEVICE_TYPE_EVEN_G1_RIGHT, // NUS only (right arm)
    DEVICE_TYPE_NUS_GENERIC    // Generic NUS device
} ble_device_type_t;

typedef struct {
    struct bt_conn *conn;
    ble_device_type_t type;
    char name[32];
    bool nus_ready;
    bool hid_ready;
    uint16_t mtu;
} ble_device_connection_t;

// Track up to 4 connections (MouthPad + Even G1 Left + Even G1 Right + spare)
#define MAX_BLE_CONNECTIONS 4
static ble_device_connection_t connections[MAX_BLE_CONNECTIONS];
```

### 2. Device Detection Strategy

#### Scan Detection Logic
1. **Even G1 Detection**:
   - Check device name contains "Even" or "G1"
   - Advertises NUS UUID but NOT HID UUID
   - Two devices with similar names (Left/Right arms)

2. **MouthPad Detection**:
   - Advertises both NUS and HID UUIDs
   - Device name contains "MouthPad" or similar

3. **Scan Filter Updates**:
   ```c
   // Add NUS UUID to scan filters
   bt_scan_filter_add(BT_SCAN_FILTER_TYPE_UUID, BT_UUID_NUS);
   // Keep existing HID filter
   bt_scan_filter_add(BT_SCAN_FILTER_TYPE_UUID, BT_UUID_HIDS);
   ```

### 3. Even G1 Module Structure

#### New Files
- `app/src/even_g1.c/h` - Main Even G1 protocol handler
- `app/src/even_g1_nus.c/h` - Even G1 specific NUS client (handles both arms)
- `app/src/even_g1_protocol.c/h` - Protocol encoding/decoding

#### Key Components
```c
// even_g1.h
typedef struct {
    struct bt_conn *left_conn;
    struct bt_conn *right_conn;
    bool left_ready;
    bool right_ready;
    uint16_t left_mtu;
    uint16_t right_mtu;
} even_g1_state_t;

// Protocol functions
int even_g1_send_text(const char *text);
int even_g1_send_bitmap(const uint8_t *data, size_t len);
int even_g1_handle_event(uint8_t opcode, const uint8_t *data, size_t len);
```

### 4. Implementation Phases

#### Phase 1: Multi-Connection Support (Current Task)
1. Refactor `ble_central.c` to support multiple connections
2. Update scan callbacks to detect device types
3. Modify `ble_transport.c` to manage connection array
4. Add device type identification logic

#### Phase 2: Even G1 Basic Connection
1. Detect Even G1 during scan
2. Connect to both arms sequentially
3. Discover NUS service on both arms
4. Exchange MTU (request 247 bytes)
5. Enable notifications

#### Phase 3: Even G1 Protocol Implementation
1. Implement text display (`0x4E` opcode)
2. Add ACK handling between left/right arms
3. Create simple test message display

#### Phase 4: Data Bridge Display
1. Show MouthPad connection status on Even G1
2. Display mouse movement indicators
3. Show battery level if available
4. Display click events

### 5. Scan and Connection Flow

```
Start Scan
    ↓
Device Found
    ↓
Check Device Type:
    - Has NUS + HID → MouthPad
    - Has NUS only + "Even"/"G1" name → Even G1
    ↓
MouthPad Path:              Even G1 Path:
- Connect                   - Connect to first arm
- Discover NUS              - Store as left_conn
- Discover HID              - Continue scanning
- Start bridging            - Find second arm
                           - Connect to second arm
                           - Store as right_conn
                           - Discover NUS on both
                           - Exchange MTU on both
                           - Enable notifications
                           - Send init sequence
```

### 6. User Button Behavior

- **Short Press**: Toggle scan if no device connected OR restart scan for additional device
- **Long Press**: Disconnect all devices and restart scan
- **Double Press**: Cycle display information on Even G1

### 7. Testing Plan

1. **Connection Test**:
   - Verify both Even G1 arms connect
   - Confirm MouthPad can connect after Even G1
   - Test Even G1 connection after MouthPad

2. **Protocol Test**:
   - Send "Hello G1" text to display
   - Verify ACK handling
   - Test notification reception

3. **Integration Test**:
   - Display MouthPad status on Even G1
   - Show real-time mouse events
   - Verify USB bridge still works

## Risk Mitigation

1. **GATT Discovery Conflicts**: Use sequential discovery with completion callbacks
2. **Memory Constraints**: Limit to 4 simultaneous connections
3. **MTU Negotiation**: Always exchange MTU before enabling CCCD
4. **Connection Stability**: Implement reconnection logic for dropped connections

## Success Criteria

- [x] Plan documented
- [ ] Connect to both Even G1 arms simultaneously
- [ ] Maintain MouthPad connection alongside Even G1
- [ ] Display text on Even G1 screen
- [ ] User button controls scanning for additional devices
- [ ] Stable multi-device operation

## References
- Even G1 Protocol: `/docs/EVEN_G1.md`
- EvenDemoApp: https://github.com/even-realities/EvenDemoApp
- MentraOS: https://github.com/Mentra-Community/MentraOS