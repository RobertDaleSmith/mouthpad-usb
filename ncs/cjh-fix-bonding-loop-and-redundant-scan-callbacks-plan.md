# Plan: Fix Bonding Loop and Redundant Scan Callbacks

## Context

Two issues were introduced by the recent manufacturer data advertising check commit:

**Issue 1 — Bonding loop**: The new scan-time mfr data check (`has_mfr_data`) applies to ALL
devices, including ones already bonded. A device bonded before this commit was added (i.e., a
genuine MouthPad^ that has already passed DIS identity) can now fail the check if its
`tracked_devices` entry was cleared (e.g., power cycle) and the mfr data hasn't been
re-accumulated yet. Since `bonded_device_seen_advertising = true` is set when the bonded device
is seen (even though the mfr check then rejects it), no other device can connect either. The
dongle gets stuck scanning forever without ever connecting.

A second variant of this loop occurs if a device manages to pass the mfr data check but then
fails the post-connection DIS identity check (line 586–596 of `ble_dis.c`). The DIS check calls
`bt_conn_disconnect()` but does NOT call `bt_unpair()`. So the device remains bonded. On the
next scan cycle, `tracked_devices` still has all flags set (the mfr data was accumulated from
the previous scan), the device passes all checks, connects again, fails DIS again, and loops.
`extract_device_name_from_scan()` is called once per loop iteration — this is the source of
issue 2.

**Issue 2 — `extract_device_name_from_scan()` called many times**: Direct consequence of the
DIS-failure loop described above. The function is also called in `scan_no_match()` for every
non-matching nearby BLE device (expected behavior), which may add to the log noise.

---

## Critical Files

- `app/src/ble_central.c` — two changes
- `app/src/ble_dis.c` — one change

---

## Changes

### 1. Skip mfr data check for bonded devices (`ble_central.c`, ~line 378)

Bonded devices have already been verified as genuine MouthPad^ via DIS during initial pairing.
Requiring them to re-pass the scan-time check is a regression that blocks legitimate reconnects.
The DIS check (which still runs post-connection) remains the authoritative identity check.

Replace:
```c
if (!dev_state->has_mfr_data) {
    LOG_DBG("Skipping device %s: missing expected manufacturer data", addr);
    return;
}
```

With:
```c
if (!is_bonded && !dev_state->has_mfr_data) {
    LOG_DBG("Skipping device %s: missing expected manufacturer data", addr);
    return;
}
```

### 2. Unpair on confirmed DIS identity failure (`ble_dis.c`, `verify_device_identity()`, ~line 586)

If a device's DIS manufacturer name or model number doesn't match, the device is not a genuine
MouthPad^ and its bond must be removed. Without unpairing, the dongle loops indefinitely:
connect → DIS fails → disconnect → [device still bonded] → reconnect → DIS fails → ...

Only unpair when at least one DIS characteristic was successfully read (to avoid discarding a
bond due to a transient GATT read failure where no data was received at all).

Replace:
```c
if (!mfr_ok || !model_ok) {
    LOG_WRN("Device identity mismatch — mfr='%s' model='%s'; disconnecting",
            device_info.has_manufacturer_name ? device_info.manufacturer_name : "(none)",
            device_info.has_model_number ? device_info.model_number : "(none)");
    if (current_conn) {
        bt_conn_disconnect(current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
    return;
}
```

With:
```c
if (!mfr_ok || !model_ok) {
    bool got_reads = device_info.has_manufacturer_name || device_info.has_model_number;
    LOG_WRN("Device identity mismatch — mfr='%s' model='%s'; disconnecting%s",
            device_info.has_manufacturer_name ? device_info.manufacturer_name : "(none)",
            device_info.has_model_number ? device_info.model_number : "(none)",
            got_reads ? " and unpairing" : " (read error, not unpairing)");
    if (current_conn) {
        if (got_reads) {
            bt_unpair(BT_ID_DEFAULT, bt_conn_get_dst(current_conn));
        }
        bt_conn_disconnect(current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
    return;
}
```

### 3. Guard `scan_filter_match()` against non-scanning state (`ble_central.c`, ~line 317)

Add an early return at the top of `scan_filter_match()` if the connection state is not
`BLE_CENTRAL_STATE_SCANNING`. This prevents redundant processing and redundant calls to
`extract_device_name_from_scan()` if pending advertising events are delivered after the
connection attempt has already been initiated (e.g., due to events already in the HCI queue
when `bt_scan_stop()` is called).

Add after the opening brace of `scan_filter_match()`, before `bt_addr_le_to_str(...)`:
```c
/* Ignore callbacks if we are no longer actively scanning */
if (connection_state != BLE_CENTRAL_STATE_SCANNING) {
    return;
}
```

---

## Verification

1. Build: `make build-april-dongle` — zero warnings
2. Flash and confirm in logs:
   - Genuine bonded MouthPad^ reconnects successfully even after a power cycle (mfr data check
     bypassed for bonded device, DIS identity passes)
   - A device with matching UUIDs + mfr advertising data but wrong DIS identity is disconnected,
     unpaired (log shows "disconnecting and unpairing"), and does NOT reconnect in a loop
   - After the DIS failure, the dongle returns to scanning without looping
