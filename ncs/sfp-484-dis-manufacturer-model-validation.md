# Plan: Add Manufacturer Name & Model Number to DIS Read Pipeline + Device Validation

## Ticket: SFP-484

## Context

The BLE DIS read pipeline (`on_connection_read_steps` in `ble_dis.c`) currently reads Firmware Revision, PnP ID, and Device Name on connection. The `ble_dis_info_t` struct already has `manufacturer_name`/`has_manufacturer_name` fields and the discovery code already finds the `mfr_name_handle` — but neither the Manufacturer Name nor the Model Number are actually read post-discovery.

We need to:
1. **Commit 1**: Add Manufacturer Name and Model Number read steps to the pipeline
2. **Commit 2**: Reject connections where Manufacturer Name ≠ "Augmental" or Model Number ≠ "MouthPad^"

---

## Critical Files

- `app/src/ble_dis.c` — primary file; contains pipeline, callbacks, discovery
- `app/src/ble_dis.h` — struct definitions and constants

---

## Step 0: Rename and Commit Plan File (must be first)

Rename `composed-roaming-adleman.md` → `sfp-484-dis-manufacturer-model-validation.md` and commit it. All other tasks are blocked by this step.

---

## Commit 1: Add Manufacturer Name + Model Number to Read Pipeline

### `app/src/ble_dis.h`

1. Add `#define BLE_DIS_MODEL_NUMBER_MAX_LEN 64`
2. Add two fields to `ble_dis_info_t`:
   ```c
   char model_number[BLE_DIS_MODEL_NUMBER_MAX_LEN];
   bool has_model_number;
   ```

### `app/src/ble_dis.c`

1. **New UUID define** (alongside existing `BT_UUID_DIS_MFR_NAME_VAL`):
   ```c
   #define BT_UUID_DIS_MODEL_NUMBER_VAL  0x2A24  /* Model Number String */
   #define BT_UUID_DIS_MODEL_NUMBER  BT_UUID_DECLARE_16(BT_UUID_DIS_MODEL_NUMBER_VAL)
   ```

2. **New handle variable** (alongside existing `mfr_name_handle`):
   ```c
   static uint16_t model_number_handle = 0;
   ```

3. **Reset `model_number_handle` in `ble_dis_reset()`** (alongside `mfr_name_handle = 0`).

4. **Add `read_mfr_name_cb`** (after `read_device_name_cb`, following the same pattern as `read_fw_rev_cb`):
   - On `!data`: call `advance_read_pipeline(step + 1)`, return `BT_GATT_ITER_STOP`
   - On `err`: log error, call `advance_read_pipeline(step + 1)`, return `BT_GATT_ITER_STOP`
   - On data: `memcpy` into `device_info.manufacturer_name`, set `has_manufacturer_name = true`, log, return `BT_GATT_ITER_CONTINUE`

5. **Add `read_model_number_cb`** (same pattern):
   - Populates `device_info.model_number` / `has_model_number`

6. **Discover Model Number in `dis_discovery_completed_cb`** (alongside existing Manufacturer Name discovery):
   ```c
   gatt_chrc = bt_gatt_dm_char_by_uuid(dm, BT_UUID_DIS_MODEL_NUMBER);
   if (gatt_chrc) {
       gatt_desc = bt_gatt_dm_desc_by_uuid(dm, gatt_chrc, BT_UUID_DIS_MODEL_NUMBER);
       if (gatt_desc) {
           model_number_handle = gatt_desc->handle;
       }
   }
   ```

7. **Add two new entries to `on_connection_read_steps`** (insert before the existing steps, or after — order doesn't matter functionally; place them first for clarity):
   ```c
   {
       .params     = { .handle_count = 1, .single = { .handle = 0, .offset = 0 } },
       .handle_ptr = &mfr_name_handle,
       .callback   = read_mfr_name_cb,
       .name       = "Manufacturer Name",
   },
   {
       .params     = { .handle_count = 1, .single = { .handle = 0, .offset = 0 } },
       .handle_ptr = &model_number_handle,
       .callback   = read_model_number_cb,
       .name       = "Model Number",
   },
   ```
   These are handle-based steps, so `advance_read_pipeline` will skip them automatically if the handle is 0.

---

## Commit 2: Reject Non-Augmental / Non-MouthPad^ Devices

### `app/src/ble_dis.c` — update `on_dis_reads_complete()`

Add a validation check before calling `discovery_complete_cb`:

```c
static void on_dis_reads_complete(void)
{
    LOG_INF("DIS read pipeline complete");
    dis_ready = true;

    if (current_conn) {
        save_dis_info_to_settings(bt_conn_get_dst(current_conn));
    }

    /* Validate device identity */
    bool mfr_ok = device_info.has_manufacturer_name &&
                  strcmp(device_info.manufacturer_name, "Augmental") == 0;
    bool model_ok = device_info.has_model_number &&
                    strcmp(device_info.model_number, "MouthPad^") == 0;

    if (!mfr_ok || !model_ok) {
        LOG_WRN("Device identity mismatch — mfr='%s' model='%s'; disconnecting",
                device_info.has_manufacturer_name ? device_info.manufacturer_name : "(none)",
                device_info.has_model_number      ? device_info.model_number      : "(none)");
        if (current_conn) {
            bt_conn_disconnect(current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        }
        return;
    }

    if (discovery_complete_cb && current_conn) {
        discovery_complete_cb(current_conn);
    }
}
```

`bt_conn_disconnect` is already used in `ble_central.c` and `ble_transport.c` with `BT_HCI_ERR_REMOTE_USER_TERM_CONN`; `ble_dis.c` already includes `<zephyr/bluetooth/conn.h>`.

---

## Verification

1. Build the firmware: `make build` (or equivalent from `Makefile`)
2. Flash to device and connect a MouthPad — confirm logs show "Manufacturer Name: Augmental" and "Model Number: MouthPad^", and that connection proceeds normally
3. Connect a non-MouthPad BLE device — confirm logs show the "Device identity mismatch" warning and the connection is terminated
