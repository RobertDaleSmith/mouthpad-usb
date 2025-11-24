# Protobuf Changes Needed for Firmware Cache Clearing

The dongle firmware now supports clearing cached firmware versions via protobuf.
This requires the following additions to the `mouthpad-proto` repository.

## Problem Being Solved

When a MouthPad firmware update is performed directly via system BLE:
1. User connects MouthPad to computer/phone via system BLE
2. User updates MouthPad firmware
3. MouthPad reconnects to dongle
4. Dongle reports **old cached firmware version** from DIS cache
5. App incorrectly shows "update needed" again

## Solution

App sends `ClearFirmwareCacheWrite` message before initiating firmware update flow.
Dongle clears only the firmware version field from DIS cache while keeping other data
(device name, VID/PID, hardware revision) intact. On next reconnection, dongle will
re-read the firmware version from DIS.

## Required Proto Definitions

Add to `src/proto/MouthpadRelay.proto`:

```protobuf
// Request to clear cached firmware versions for all bonded devices
// Sent by app when initiating firmware update flow
message ClearFirmwareCacheWrite {
    // Empty - no parameters needed
}

// Response to ClearFirmwareCacheWrite
message ClearFirmwareCacheResponse {
    bool success = 1;  // true if firmware cache was cleared successfully
}
```

Add to `AppToRelayMessage` oneof:
```protobuf
message AppToRelayMessage {
    oneof message_body {
        // ... existing messages ...
        ClearFirmwareCacheWrite clear_firmware_cache_write = 7;  // Use next available tag number
    }
}
```

Add to `RelayToAppMessage` oneof:
```protobuf
message RelayToAppMessage {
    oneof message_body {
        // ... existing messages ...
        ClearFirmwareCacheResponse clear_firmware_cache_response = 7;  // Use next available tag number
    }
}
```

## Implementation Status

✅ **Dongle firmware** - Handler implemented in `ncs/app/src/main.c:673-687`
✅ **DIS module** - Functions implemented in `ncs/app/src/ble_dis.c`
✅ **Shell command** - `clearfwcache` available for testing
⏳ **Proto definitions** - Need to be added to mouthpad-proto repo
⏳ **App integration** - Need to send message when initiating FW update

## Testing

Before proto integration, test via shell command:
```
clearfwcache
```

After proto integration, app should:
1. Detect MouthPad needs firmware update
2. Send `ClearFirmwareCacheWrite` message to dongle
3. Wait for `ClearFirmwareCacheResponse` with `success: true`
4. Prompt user to update MouthPad via system BLE
5. After update completes, dongle will re-read firmware version on reconnection

## Benefits

- ✅ Keeps fast reconnection optimization (cached DIS data)
- ✅ Prevents false "update needed" warnings after legitimate updates
- ✅ Only clears firmware version, keeps device name and other metadata
- ✅ Works for all bonded devices (multi-bond support)
- ✅ Persists across dongle reboots (clears from flash storage)
