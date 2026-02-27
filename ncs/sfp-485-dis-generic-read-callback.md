# Plan: Refactor DIS Read Pipeline to Generic Callback

## Context

The BLE DIS read pipeline currently has 5 nearly-identical `read_*_cb` functions (`read_fw_rev_cb`, `read_pnp_id_cb`, `read_device_name_cb`, `read_mfr_name_cb`, `read_model_number_cb`). Each one duplicates the same boilerplate: extract the step via `CONTAINER_OF`, handle `err`, handle `!data` (advance pipeline), then do a small unique data extraction. This refactor collapses all five into a single generic callback that delegates the unique part to a `process_fn` stored on the step itself.

---

## Critical Files

- `app/src/ble_dis.c` — the only file being changed

---

## Step 0: Rename and Commit Plan File (must be first)

Rename `composed-roaming-adleman.md` → `sfp-485-dis-generic-read-callback.md` and commit it. All other tasks are blocked by this step.

---

## Changes to `app/src/ble_dis.c`

### 1. New typedef + updated `dis_read_step_t`

Replace the existing `dis_read_step_t` typedef (and its comment block) with:

```c
/* Signature for per-characteristic data processing.
 * Called by dis_read_generic_cb only when data != NULL and err == 0.
 * offset — byte position of this fragment within the characteristic value
 *          (0 for first fragment, accumulated for subsequent fragments).
 *          Enables correct multi-fragment accumulation for long characteristics. */
typedef void (*dis_process_fn_t)(const void *data, uint16_t length, uint16_t offset);

/* Read pipeline step.
 * params      — fully pre-populated; .func is assigned to dis_read_generic_cb
 *               at runtime, and .single.handle is refreshed from *handle_ptr
 *               for handle-based steps.
 * handle_ptr  — points to the discovered handle variable for handle-based steps,
 *               NULL for by-UUID steps. A zero value at runtime causes the step
 *               to be skipped.
 * process_fn  — called with raw data when a fragment arrives; NULL is safe (no-op).
 * name        — human-readable label used in log messages. */
typedef struct {
    struct bt_gatt_read_params params;
    uint16_t *handle_ptr;
    dis_process_fn_t process_fn;
    const char *name;
} dis_read_step_t;
```

`bt_gatt_read_func_t callback` is replaced by `dis_process_fn_t process_fn`.

### 2. Add forward declaration for `dis_read_generic_cb`

Add alongside the existing forward declarations:

```c
static uint8_t dis_read_generic_cb(struct bt_conn *conn, uint8_t err,
                                   struct bt_gatt_read_params *params,
                                   const void *data, uint16_t length);
```

### 3. Delete 5 `read_*_cb` functions

Remove entirely:
- `read_fw_rev_cb`
- `read_pnp_id_cb`
- `read_device_name_cb`
- `read_mfr_name_cb`
- `read_model_number_cb`

### 4. Add 5 `process_*` functions + 1 generic callback

In place of the 5 deleted callbacks, add the process functions first (since they're referenced by the array, which precedes the generic callback in the file), followed by `dis_read_generic_cb`:

String process functions use `offset` to correctly place each fragment in the destination buffer, clamping at the buffer limit to safely handle unexpectedly long values from non-Augmental devices. `process_pnp_id` ignores offset since it's a fixed 7-byte binary field.

```c
static void process_mfr_name(const void *data, uint16_t length, uint16_t offset)
{
    if (offset >= BLE_DIS_MANUFACTURER_NAME_MAX_LEN - 1) {
        return;
    }
    size_t remaining = BLE_DIS_MANUFACTURER_NAME_MAX_LEN - 1 - offset;
    size_t copy_len = MIN(length, remaining);
    memcpy(device_info.manufacturer_name + offset, data, copy_len);
    device_info.manufacturer_name[offset + copy_len] = '\0';
    device_info.has_manufacturer_name = true;
}

static void process_model_number(const void *data, uint16_t length, uint16_t offset)
{
    if (offset >= BLE_DIS_MODEL_NUMBER_MAX_LEN - 1) {
        return;
    }
    size_t remaining = BLE_DIS_MODEL_NUMBER_MAX_LEN - 1 - offset;
    size_t copy_len = MIN(length, remaining);
    memcpy(device_info.model_number + offset, data, copy_len);
    device_info.model_number[offset + copy_len] = '\0';
    device_info.has_model_number = true;
}

static void process_fw_rev(const void *data, uint16_t length, uint16_t offset)
{
    if (offset >= BLE_DIS_FIRMWARE_VERSION_MAX_LEN - 1) {
        return;
    }
    size_t remaining = BLE_DIS_FIRMWARE_VERSION_MAX_LEN - 1 - offset;
    size_t copy_len = MIN(length, remaining);
    memcpy(device_info.firmware_version + offset, data, copy_len);
    device_info.firmware_version[offset + copy_len] = '\0';
    device_info.has_firmware_version = true;
}

static void process_pnp_id(const void *data, uint16_t length, uint16_t offset)
{
    ARG_UNUSED(offset);
    /* Parse PnP ID: 7 bytes total
     * Byte 0: Vendor ID Source
     * Bytes 1-2: Vendor ID (little-endian)
     * Bytes 3-4: Product ID (little-endian)
     * Bytes 5-6: Product Version (little-endian) */
    if (length >= 7) {
        const uint8_t *pnp_data = (const uint8_t *)data;
        device_info.vendor_id  = pnp_data[1] | (pnp_data[2] << 8);
        device_info.product_id = pnp_data[3] | (pnp_data[4] << 8);
        device_info.has_pnp_id = true;
        LOG_INF("PnP ID: VID=0x%04X, PID=0x%04X",
                device_info.vendor_id, device_info.product_id);
    } else {
        LOG_WRN("PnP ID data too short: %d bytes", length);
    }
}

static void process_device_name(const void *data, uint16_t length, uint16_t offset)
{
    if (offset >= BLE_DIS_DEVICE_NAME_MAX_LEN - 1) {
        return;
    }
    size_t remaining = BLE_DIS_DEVICE_NAME_MAX_LEN - 1 - offset;
    size_t copy_len = MIN(length, remaining);
    memcpy(device_info.device_name + offset, data, copy_len);
    device_info.device_name[offset + copy_len] = '\0';
    device_info.has_device_name = true;
}

static uint8_t dis_read_generic_cb(struct bt_conn *conn, uint8_t err,
                                   struct bt_gatt_read_params *params,
                                   const void *data, uint16_t length)
{
    dis_read_step_t *step = CONTAINER_OF(params, dis_read_step_t, params);

    if (err) {
        LOG_ERR("%s read failed: %d", step->name, err);
        advance_read_pipeline(step + 1);
        return BT_GATT_ITER_STOP;
    }

    if (!data) {
        LOG_INF("%s read complete", step->name);
        advance_read_pipeline(step + 1);
        return BT_GATT_ITER_STOP;
    }

    if (step->process_fn) {
        /* For handle-based steps (handle_count == 1), params->single.offset holds
         * the offset of the current fragment, updated by the GATT stack between
         * fragments. For by-UUID steps, pass 0. */
        uint16_t offset = (params->handle_count == 1) ? params->single.offset : 0;
        step->process_fn(data, length, offset);
    }

    return BT_GATT_ITER_CONTINUE;
}
```

### 5. Update `advance_read_pipeline`

Change the single line that assigns the callback:

```c
/* Before: */
step->params.func = step->callback;

/* After: */
step->params.func = dis_read_generic_cb;
```

### 6. Update `on_connection_read_steps`

Change `.callback = read_*_cb` → `.process_fn = process_*` in all 5 entries.

---

## Notes

- String field logging (e.g. "Manufacturer Name: Augmental") moves out of `process_*` and into the generic callback's `!data` completion branch, since `process_fn` is called per-fragment and the string may not be complete until the last fragment. The generic callback already logs `"%s read complete"` using `step->name`, which is sufficient for confirming the read finished. If per-field value logging is desired, it can be added to the completion path with access to `device_info.*` directly.
- The existing TODO comment about offset accumulation in `read_fw_rev_cb` is removed — it is now fixed by design.
- `process_pnp_id` uses `ARG_UNUSED(offset)` since PnP ID is always 7 bytes and never spans fragments.

## Verification

1. Build: `make build-april-dongle` — must produce zero warnings
2. Flash to device and confirm logs show all 5 characteristics read successfully on connection
