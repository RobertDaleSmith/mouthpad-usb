# Protobuf Changes Completed for Firmware Cache Clearing

The dongle firmware now supports clearing cached firmware versions via protobuf.
The proto definitions have been added to the `mouthpad-proto` repository.

## Problem Solved

When a MouthPad firmware update is performed directly via system BLE:
1. User connects MouthPad to computer/phone via system BLE
2. User updates MouthPad firmware
3. MouthPad reconnects to dongle
4. Dongle would report **old cached firmware version** from DIS cache
5. App would incorrectly show "update needed" again

## Solution Implemented

App sends `ClearFirmwareCacheWrite` message before initiating firmware update flow.
Dongle clears only the firmware version field from both flash and in-memory cache.
On next reconnection, dongle will re-read the firmware version from DIS.

## Proto Definitions (COMPLETED ✅)

Added to `mouthpad-proto/src/proto/MouthpadRelay.proto`:

```protobuf
message ClearFirmwareCacheWrite {
  // Clears cached firmware versions for all bonded devices
  // Used when initiating firmware update to prevent false "update needed" warnings
}

message ClearFirmwareCacheResponse {
  bool success = 1;
}
```

Added to `AppToRelayMessage` (tag 7):
```protobuf
ClearFirmwareCacheWrite clear_firmware_cache_write = 7;
```

Added to `RelayToAppMessage` (tag 7):
```protobuf
ClearFirmwareCacheResponse clear_firmware_cache_response = 7;
```

## Implementation Status

✅ **Dongle firmware** - Handler implemented with multi-bond in-memory cache
✅ **Proto definitions** - Added to mouthpad-proto repo (branch: rds/clear-firmware-cache)
✅ **App integration** - Sends message when firmware update detected (branch: rds/clear-firmware-cache-on-update)
✅ **In-memory cache** - All bonded devices' DIS loaded at boot, no flash read on reconnection

## Architecture

### In-Memory DIS Cache
The dongle maintains an in-memory cache array (`dis_cache[4]`) for all bonded devices:
- **At boot**: Settings callback loads all cached DIS from flash into memory
- **On connection**: Device's cache entry copied into `device_info` for fast access
- **Check cache**: `ble_dis_has_cached_firmware()` checks `device_info` (no flash read)
- **DIS complete**: Updates both flash storage AND in-memory cache
- **clearfwcache**: Clears firmware field from both flash AND in-memory cache

### Connection Flow

**With cached firmware (fast path)**:
1. Device connects
2. Load device's DIS cache from memory → `device_info`
3. `ble_dis_has_cached_firmware()` returns true
4. Report CONNECTED immediately after HID + NUS ready
5. Start DIS discovery in background to refresh cache

**Without cached firmware (slow path)**:
1. Device connects
2. Load device's DIS cache from memory → `device_info` (empty/no firmware)
3. `ble_dis_has_cached_firmware()` returns false
4. Wait for DIS discovery to complete
5. Report CONNECTED after firmware retrieved

### Firmware Update Flow

**App initiates firmware update**:
1. App detects MouthPad needs firmware update
2. App sends `ClearFirmwareCacheWrite` to dongle
3. Dongle clears firmware field from flash AND in-memory cache for all bonded devices
4. Dongle responds with `ClearFirmwareCacheResponse(success: true)`
5. User updates MouthPad via system BLE
6. MouthPad reconnects to dongle
7. Dongle finds no cached firmware → takes slow path
8. Dongle re-reads firmware from DIS before reporting CONNECTED
9. App sees correct new firmware version

## Testing

Test via shell command:
```
clearfwcache
```

After integration test:
1. Connect MouthPad to dongle (should use fast path if firmware cached)
2. Send `ClearFirmwareCacheWrite` via app
3. Disconnect MouthPad
4. Reconnect MouthPad
5. Dongle should take slow path (wait for DIS) before reporting CONNECTED
6. After DIS completes, firmware cache is re-populated for next connection

## Benefits

- ✅ Keeps fast reconnection optimization (cached DIS data in memory)
- ✅ No flash reads on every connection (only at boot)
- ✅ Prevents false "update needed" warnings after legitimate updates
- ✅ Only clears firmware version, keeps device name and other metadata
- ✅ Works for all bonded devices (multi-bond support up to 4 devices)
- ✅ Persists across dongle reboots (both flash and memory updated)
- ✅ clearfwcache takes effect immediately on next reconnection

## Related Files

**Dongle firmware**:
- `ncs/app/src/ble_dis.c` - In-memory cache implementation
- `ncs/app/src/ble_dis.h` - Public API declarations
- `ncs/app/src/ble_transport.c` - Connection flow using cache
- `ncs/app/src/main.c` - Protobuf handler and shell command

**Proto repository**:
- `mouthpad-proto/src/proto/MouthpadRelay.proto` - Proto definitions
- Branch: `rds/clear-firmware-cache`

**App repository**:
- `mouthpad-utility/Shared/Controllers/MouthPadUSB.swift` - Send/receive messages
- `mouthpad-utility/Shared/Controllers/AppController.swift` - Auto-clear on update detected
- Branch: `rds/clear-firmware-cache-on-update`
