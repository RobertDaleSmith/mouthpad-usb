# Plan: Reject Connections Missing Expected Advertising Manufacturer Data

## Context

The dongle currently filters devices at two stages: (1) at scan time by requiring both HID
and NUS service UUIDs, and (2) post-connection via GATT DIS reads that verify manufacturer
name and model number. A device that passes the UUID check but is not an Augmental MouthPad^
still gets connected to before being rejected, wasting connection time and power.

Adding a manufacturer-specific advertising data check at scan time (at the existing TODO on
line 372 of `ble_central.c`) lets us reject non-MouthPad devices before any connection is
attempted. The expected manufacturer data is company ID `0x4147` with additional payload
`"MouthPad^"`.

---

## Critical Files

- `app/src/ble_central.c` — only file being changed

---

## Changes

### 1. Add constants near the top of `ble_central.c`

Alongside the existing `#define`s (near `MAX_BONDED_DEVICES`, etc.):

```c
#define MOUTHPAD_COMPANY_ID        0x4147
#define MOUTHPAD_MFR_DATA_PAYLOAD  "MouthPad^"
```

### 2. Add `has_mfr_data` to `device_uuid_state`

```c
struct device_uuid_state {
    bt_addr_le_t addr;
    bool has_hid;
    bool has_nus;
    bool has_mfr_data;   /* <-- add this */
    int8_t rssi;
    int64_t timestamp;
};
```

### 3. Add `is_mouthpad_manufacturer_data()` — modelled on `is_nus_device()`/`is_hid_device()`

BLE manufacturer-specific data format: bytes 0–1 are the company ID (little-endian),
bytes 2+ are the additional payload.

```c
static bool mfr_data_search_cb(struct bt_data *data, void *user_data)
{
    bool *found = (bool *)user_data;

    if (data->type == BT_DATA_MANUFACTURER_DATA) {
        if (data->data_len >= 2 + sizeof(MOUTHPAD_MFR_DATA_PAYLOAD) - 1 &&
            sys_get_le16(data->data) == MOUTHPAD_COMPANY_ID &&
            memcmp(data->data + 2, MOUTHPAD_MFR_DATA_PAYLOAD,
                   sizeof(MOUTHPAD_MFR_DATA_PAYLOAD) - 1) == 0) {
            *found = true;
            return false; /* stop parsing */
        }
    }

    return true; /* continue parsing */
}

static bool is_mouthpad_manufacturer_data(const struct bt_scan_device_info *device_info)
{
    bool found = false;

    if (device_info->adv_data) {
        struct net_buf_simple_state state;
        net_buf_simple_save(device_info->adv_data, &state);
        bt_data_parse(device_info->adv_data, mfr_data_search_cb, &found);
        net_buf_simple_restore(device_info->adv_data, &state);
    }

    return found;
}
```

Note: The correct Zephyr constant is `BT_DATA_MANUFACTURER_DATA` (value `0xFF`).

### 4. Accumulate `has_mfr_data` in `scan_filter_match()` alongside HID/NUS

After `dev_state->timestamp = k_uptime_get();` (line ~359):

```c
if (is_mouthpad_manufacturer_data(device_info)) dev_state->has_mfr_data = true;
```

### 5. Replace TODO with manufacturer data check (line ~372)

Replace:
```c
// TODO - Is this where we can check manufacturer info on the advertising packets.
```

With:
```c
if (!dev_state->has_mfr_data) {
    LOG_DBG("Skipping device %s: missing expected manufacturer data", addr);
    return;
}
```

---

## Verification

1. Build: `make build-april-dongle` — zero warnings
2. Flash and confirm in logs that:
   - A genuine MouthPad^ passes the manufacturer data check and connects
   - A device with matching UUIDs but wrong/missing manufacturer data is skipped with the `LOG_DBG` message above
