/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/settings/settings.h>
#include <bluetooth/gatt_dm.h>
#include <string.h>

#include "ble_dis.h"
#include "ble_central.h"

#define LOG_MODULE_NAME ble_dis
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

/* Device Information Service UUIDs (Bluetooth SIG assigned numbers) */
/* Note: BT_UUID_DIS, BT_UUID_DIS_PNP_ID, and BT_UUID_GAP_DEVICE_NAME are already defined in zephyr/bluetooth/uuid.h */
#define BT_UUID_DIS_FW_REV_VAL    0x2A26  /* Firmware Revision String */
#define BT_UUID_DIS_HW_REV_VAL    0x2A27  /* Hardware Revision String */
#define BT_UUID_DIS_MFR_NAME_VAL  0x2A29  /* Manufacturer Name String */

#define BT_UUID_DIS_FW_REV  BT_UUID_DECLARE_16(BT_UUID_DIS_FW_REV_VAL)
#define BT_UUID_DIS_HW_REV  BT_UUID_DECLARE_16(BT_UUID_DIS_HW_REV_VAL)
#define BT_UUID_DIS_MFR_NAME BT_UUID_DECLARE_16(BT_UUID_DIS_MFR_NAME_VAL)

/* Device Information client state */
static ble_dis_info_t device_info;
static bool dis_ready = false;
static struct bt_conn *current_conn = NULL;
static struct bt_gatt_read_params read_params;

/* Characteristic handles */
static uint16_t fw_rev_handle = 0;
static uint16_t hw_rev_handle = 0;
static uint16_t mfr_name_handle = 0;
static uint16_t pnp_id_handle = 0;

/* Discovery completion callback */
static ble_dis_discovery_complete_cb_t discovery_complete_cb = NULL;

/* Forward declarations */
static void dis_discovery_completed_cb(struct bt_gatt_dm *dm, void *context);
static void dis_discovery_service_not_found_cb(struct bt_conn *conn, void *context);
static void dis_discovery_error_found_cb(struct bt_conn *conn, int err, void *context);
static uint8_t read_device_name_cb(struct bt_conn *conn, uint8_t err,
				   struct bt_gatt_read_params *params,
				   const void *data, uint16_t length);
static uint8_t read_pnp_id_cb(struct bt_conn *conn, uint8_t err,
			      struct bt_gatt_read_params *params,
			      const void *data, uint16_t length);

/* GATT Discovery Manager callback structure */
static struct bt_gatt_dm_cb dis_discovery_cb = {
	.completed = dis_discovery_completed_cb,
	.service_not_found = dis_discovery_service_not_found_cb,
	.error_found = dis_discovery_error_found_cb,
};

/* Settings storage for persistent DIS info across power cycles */
/* Build settings key for a specific device address */
static void build_dis_settings_key(const bt_addr_le_t *addr, char *key_buf, size_t buf_size)
{
	/* Create clean hex string from address bytes + type for settings key */
	/* Format: "ble_dis/<6 hex bytes><type>/info" e.g., "ble_dis/F01A5F522A3E_1/info" */
	snprintf(key_buf, buf_size, "ble_dis/%02X%02X%02X%02X%02X%02X_%d/info",
		 addr->a.val[5], addr->a.val[4], addr->a.val[3],
		 addr->a.val[2], addr->a.val[1], addr->a.val[0],
		 addr->type);
}

static int save_dis_info_to_settings(const bt_addr_le_t *addr)
{
	if (!IS_ENABLED(CONFIG_SETTINGS)) {
		return 0;
	}

	if (!addr) {
		LOG_ERR("Cannot save DIS info: no address provided");
		return -EINVAL;
	}

	char key[64];
	build_dis_settings_key(addr, key, sizeof(key));

	LOG_INF("Saving DIS info for device: has_fw=%d, fw='%s', has_name=%d, name='%s', has_pnp=%d, vid=0x%04X, pid=0x%04X",
		device_info.has_firmware_version, device_info.firmware_version,
		device_info.has_device_name, device_info.device_name,
		device_info.has_pnp_id, device_info.vendor_id, device_info.product_id);

	int err = settings_save_one(key, &device_info, sizeof(ble_dis_info_t));
	if (err) {
		LOG_ERR("Failed to save DIS info to settings (err %d)", err);
		return err;
	}

	LOG_INF("Saved DIS info to persistent storage: %s", key);
	return 0;
}

static int settings_set_cb(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	LOG_DBG("DIS settings_set_cb called: name='%s', len=%d", name, len);

	/* Settings are now per-device (ble_dis/<addr>/info), loaded on-demand */
	/* Just acknowledge all DIS settings without loading them globally */
	if (len == sizeof(ble_dis_info_t)) {
		/* Valid DIS info size, consume it but don't load globally */
		ble_dis_info_t temp_info;
		ssize_t bytes_read = read_cb(cb_arg, &temp_info, sizeof(ble_dis_info_t));
		if (bytes_read == sizeof(ble_dis_info_t)) {
			LOG_DBG("Found DIS info in settings: %s", name);
			return 0;
		}
	}

	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(ble_dis, "ble_dis", NULL, settings_set_cb, NULL, NULL);

/* Public API implementations */
int ble_dis_init(void)
{
	LOG_INF("Initializing Device Information Service client...");
	/* Note: Don't clear device_info here - it may already be loaded from
	 * persistent storage via settings_set_cb() when settings subsystem initialized
	 */
	dis_ready = false;

	LOG_INF("DIS init - device_info state: has_fw=%d, fw='%s', has_pnp=%d, vid=0x%04X, pid=0x%04X",
		device_info.has_firmware_version, device_info.firmware_version,
		device_info.has_pnp_id, device_info.vendor_id, device_info.product_id);

	LOG_INF("Device Information Service client initialized successfully");
	return 0;
}

int ble_dis_discover(struct bt_conn *conn)
{
	int err;

	if (!conn || conn != ble_central_get_default_conn()) {
		LOG_WRN("Invalid connection for Device Information Service discovery");
		return -EINVAL;
	}

	LOG_INF("Starting Device Information Service discovery");
	current_conn = conn;

	/* Start GATT discovery for DIS service */
	err = bt_gatt_dm_start(conn, BT_UUID_DIS, &dis_discovery_cb, NULL);
	if (err) {
		LOG_ERR("Could not start DIS discovery: %d", err);
		return err;
	}

	return 0;
}

bool ble_dis_is_ready(void)
{
	return dis_ready;
}

void ble_dis_reset(void)
{
	/* Clear connection state and handles, but preserve cached device_info
	 * so it can be reported even when disconnected (matches ESP firmware behavior)
	 */
	dis_ready = false;
	current_conn = NULL;
	fw_rev_handle = 0;
	hw_rev_handle = 0;
	mfr_name_handle = 0;
	pnp_id_handle = 0;
	/* Note: device_info is NOT cleared - it persists across disconnections
	 * This allows the host to query bonded device info even when disconnected
	 */
	LOG_DBG("Device Information Service reset (device_info preserved)");
}

const ble_dis_info_t *ble_dis_get_info(void)
{
	/* Return cached device_info even when disconnected (dis_ready == false)
	 * Check if any meaningful data is present before returning
	 */
	if (device_info.has_firmware_version || device_info.has_pnp_id) {
		LOG_INF("ble_dis_get_info returning data: has_fw=%d, fw='%s', has_pnp=%d, vid=0x%04X, pid=0x%04X",
			device_info.has_firmware_version, device_info.firmware_version,
			device_info.has_pnp_id, device_info.vendor_id, device_info.product_id);
		return &device_info;
	}
	LOG_WRN("ble_dis_get_info returning NULL (no data available)");
	return NULL;
}

int ble_dis_load_info_for_addr(const bt_addr_le_t *addr, ble_dis_info_t *out_info)
{
	if (!addr || !out_info) {
		return -EINVAL;
	}

	if (!IS_ENABLED(CONFIG_SETTINGS)) {
		return -ENOTSUP;
	}

	char key[64];
	build_dis_settings_key(addr, key, sizeof(key));

	/* Use settings_load_one to load the data directly */
	ssize_t len = settings_load_one(key, out_info, sizeof(ble_dis_info_t));
	if (len <= 0) {
		LOG_DBG("No DIS info found for device (key: %s, len: %d)", key, (int)len);
		return -ENOENT;
	}

	if (out_info->has_device_name) {
		LOG_DBG("Loaded DIS info for device: name='%s'", out_info->device_name);
	}

	return 0;
}

void ble_dis_clear_saved_for_addr(const bt_addr_le_t *addr)
{
	if (!addr || !IS_ENABLED(CONFIG_SETTINGS)) {
		return;
	}

	char key[64];
	build_dis_settings_key(addr, key, sizeof(key));

	LOG_INF("Clearing DIS info for device: %s", key);

	int err = settings_delete(key);
	if (err && err != -ENOENT) {
		LOG_ERR("Failed to delete DIS info (err %d)", err);
	} else {
		LOG_DBG("Cleared DIS info from storage");
	}
}

void ble_dis_clear_saved(void)
{
	LOG_INF("Clearing all saved DIS info");

	/* Clear in-memory cache */
	memset(&device_info, 0, sizeof(device_info));

	/* Note: Per-device DIS info in settings will be cleaned up when bonds are cleared */
	/* This function now just clears the global cache */
}

void ble_dis_clear_cached_firmware_for_addr(const bt_addr_le_t *addr)
{
	if (!addr || !IS_ENABLED(CONFIG_SETTINGS)) {
		return;
	}

	/* Load existing DIS info */
	ble_dis_info_t dis_info;
	int err = ble_dis_load_info_for_addr(addr, &dis_info);

	if (err != 0) {
		LOG_DBG("No cached DIS info to clear firmware from");
		return;
	}

	/* Clear only the firmware version field */
	dis_info.has_firmware_version = false;
	dis_info.firmware_version[0] = '\0';

	/* Save back to storage */
	char key[64];
	build_dis_settings_key(addr, key, sizeof(key));

	err = settings_save_one(key, &dis_info, sizeof(ble_dis_info_t));
	if (err) {
		LOG_ERR("Failed to save DIS info after clearing firmware (err %d)", err);
	} else {
		char addr_str[BT_ADDR_LE_STR_LEN];
		bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
		LOG_INF("Cleared cached firmware version for device: %s", addr_str);
	}
}

void ble_dis_clear_all_cached_firmware(void)
{
	LOG_INF("Clearing cached firmware version for all bonded devices");

	/* Get list of bonded devices from ble_central */
	extern int ble_central_get_bonded_devices(struct bonded_device *out_list, size_t max_count);

	struct bonded_device bonds[4]; /* MAX_BONDED_DEVICES */
	int count = ble_central_get_bonded_devices(bonds, 4);

	if (count <= 0) {
		LOG_DBG("No bonded devices to clear firmware from");
		return;
	}

	/* Clear firmware version for each bonded device */
	for (int i = 0; i < count; i++) {
		if (bonds[i].is_valid) {
			ble_dis_clear_cached_firmware_for_addr(&bonds[i].addr);
		}
	}

	LOG_INF("Cleared cached firmware for %d device(s)", count);
}

bool ble_dis_has_cached_firmware(void)
{
	/* Check if device_info has valid firmware version
	 * This info is loaded from persistent storage on init via settings callback,
	 * so it persists across reboots
	 */
	return device_info.has_firmware_version && (device_info.firmware_version[0] != '\0');
}

/* GATT read callbacks for each characteristic */
static uint8_t read_fw_rev_cb(struct bt_conn *conn, uint8_t err,
			      struct bt_gatt_read_params *params,
			      const void *data, uint16_t length)
{
	if (err) {
		LOG_ERR("Firmware Revision read failed: %d", err);
		return BT_GATT_ITER_STOP;
	}

	if (!data) {
		LOG_DBG("Firmware Revision read complete");

		/* Read PnP ID if available */
		if (pnp_id_handle) {
			LOG_DBG("Reading PnP ID...");
			memset(&read_params, 0, sizeof(read_params));
			read_params.func = (bt_gatt_read_func_t)read_pnp_id_cb;
			read_params.handle_count = 1;
			read_params.single.handle = pnp_id_handle;
			read_params.single.offset = 0;

			int err = bt_gatt_read(current_conn, &read_params);
			if (err) {
				LOG_ERR("PnP ID read failed to start: %d", err);
				/* Continue to read device name anyway */
				goto read_device_name;
			}
		} else {
read_device_name:
			/* Read device name from GAP service */
			LOG_DBG("Reading device name from GAP...");
			memset(&read_params, 0, sizeof(read_params));
			read_params.func = read_device_name_cb;
			read_params.by_uuid.uuid = BT_UUID_GAP_DEVICE_NAME;
			read_params.by_uuid.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
			read_params.by_uuid.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;

			int err = bt_gatt_read(current_conn, &read_params);
			if (err) {
				LOG_ERR("Device name read failed to start: %d", err);
				dis_ready = true;  /* Mark ready even without device name */
			}
		}

		return BT_GATT_ITER_STOP;
	}

	/* Copy firmware revision string */
	size_t copy_len = MIN(length, BLE_DIS_FIRMWARE_VERSION_MAX_LEN - 1);
	memcpy(device_info.firmware_version, data, copy_len);
	device_info.firmware_version[copy_len] = '\0';
	device_info.has_firmware_version = true;

	LOG_INF("Firmware Revision: %s", device_info.firmware_version);

	return BT_GATT_ITER_CONTINUE;
}

static uint8_t read_pnp_id_cb(struct bt_conn *conn, uint8_t err,
			      struct bt_gatt_read_params *params,
			      const void *data, uint16_t length)
{
	if (err) {
		LOG_ERR("PnP ID read failed: %d", err);
		/* Continue to read device name */
		goto read_device_name;
	}

	if (!data) {
		LOG_DBG("PnP ID read complete");
		/* Continue to read device name */
		goto read_device_name;
	}

	/* Parse PnP ID: 7 bytes total
	 * Byte 0: Vendor ID Source (0x01=Bluetooth SIG, 0x02=USB Implementer's Forum)
	 * Bytes 1-2: Vendor ID (little-endian)
	 * Bytes 3-4: Product ID (little-endian)
	 * Bytes 5-6: Product Version (little-endian) */
	if (length >= 7) {
		const uint8_t *pnp_data = (const uint8_t *)data;
		device_info.vendor_id = pnp_data[1] | (pnp_data[2] << 8);
		device_info.product_id = pnp_data[3] | (pnp_data[4] << 8);
		device_info.has_pnp_id = true;
		LOG_INF("PnP ID: VID=0x%04X, PID=0x%04X", device_info.vendor_id, device_info.product_id);
	} else {
		LOG_WRN("PnP ID data too short: %d bytes", length);
	}

read_device_name:
	/* Read device name from GAP service */
	LOG_DBG("Reading device name from GAP...");
	memset(&read_params, 0, sizeof(read_params));
	read_params.func = read_device_name_cb;
	read_params.by_uuid.uuid = BT_UUID_GAP_DEVICE_NAME;
	read_params.by_uuid.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	read_params.by_uuid.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;

	int read_err = bt_gatt_read(current_conn, &read_params);
	if (read_err) {
		LOG_ERR("Device name read failed to start: %d", read_err);
		dis_ready = true;  /* Mark ready even without device name */
	}

	return BT_GATT_ITER_STOP;
}

static uint8_t read_device_name_cb(struct bt_conn *conn, uint8_t err,
				   struct bt_gatt_read_params *params,
				   const void *data, uint16_t length)
{
	if (err) {
		LOG_ERR("Device Name read failed: %d", err);
		dis_ready = true;  /* Mark ready even without device name */
		LOG_INF("Device Information Service ready (device name unavailable)");

		/* Save DIS info even if device name failed - we have firmware/PnP ID */
		if (current_conn) {
			save_dis_info_to_settings(bt_conn_get_dst(current_conn));
		}

		/* Trigger discovery complete callback */
		if (discovery_complete_cb && current_conn) {
			discovery_complete_cb(current_conn);
		}

		return BT_GATT_ITER_STOP;
	}

	if (!data) {
		LOG_DBG("Device Name read complete");
		dis_ready = true;
		LOG_INF("Device Information Service ready");

		/* Save DIS info to persistent storage now that discovery is complete */
		if (current_conn) {
			save_dis_info_to_settings(bt_conn_get_dst(current_conn));
		}

		/* Trigger discovery complete callback */
		if (discovery_complete_cb && current_conn) {
			discovery_complete_cb(current_conn);
		}

		return BT_GATT_ITER_STOP;
	}

	/* Copy device name string */
	size_t copy_len = MIN(length, BLE_DIS_DEVICE_NAME_MAX_LEN - 1);
	memcpy(device_info.device_name, data, copy_len);
	device_info.device_name[copy_len] = '\0';
	device_info.has_device_name = true;

	LOG_INF("Device Name: %s", device_info.device_name);
	return BT_GATT_ITER_CONTINUE;
}

/* DIS Discovery callbacks */
static void dis_discovery_completed_cb(struct bt_gatt_dm *dm, void *context)
{
	int err;
	const struct bt_gatt_dm_attr *gatt_chrc;
	const struct bt_gatt_dm_attr *gatt_desc;

	LOG_INF("Device Information Service discovery completed");
	bt_gatt_dm_data_print(dm);

	/* Find Firmware Revision characteristic */
	gatt_chrc = bt_gatt_dm_char_by_uuid(dm, BT_UUID_DIS_FW_REV);
	if (gatt_chrc) {
		gatt_desc = bt_gatt_dm_desc_by_uuid(dm, gatt_chrc, BT_UUID_DIS_FW_REV);
		if (gatt_desc) {
			fw_rev_handle = gatt_desc->handle;
			LOG_INF("Found Firmware Revision characteristic (handle: %d)", fw_rev_handle);
		}
	}

	/* Find Hardware Revision characteristic */
	gatt_chrc = bt_gatt_dm_char_by_uuid(dm, BT_UUID_DIS_HW_REV);
	if (gatt_chrc) {
		gatt_desc = bt_gatt_dm_desc_by_uuid(dm, gatt_chrc, BT_UUID_DIS_HW_REV);
		if (gatt_desc) {
			hw_rev_handle = gatt_desc->handle;
			LOG_INF("Found Hardware Revision characteristic (handle: %d)", hw_rev_handle);
		}
	}

	/* Find Manufacturer Name characteristic */
	gatt_chrc = bt_gatt_dm_char_by_uuid(dm, BT_UUID_DIS_MFR_NAME);
	if (gatt_chrc) {
		gatt_desc = bt_gatt_dm_desc_by_uuid(dm, gatt_chrc, BT_UUID_DIS_MFR_NAME);
		if (gatt_desc) {
			mfr_name_handle = gatt_desc->handle;
			LOG_INF("Found Manufacturer Name characteristic (handle: %d)", mfr_name_handle);
		}
	}

	/* Find PnP ID characteristic */
	gatt_chrc = bt_gatt_dm_char_by_uuid(dm, BT_UUID_DIS_PNP_ID);
	if (gatt_chrc) {
		gatt_desc = bt_gatt_dm_desc_by_uuid(dm, gatt_chrc, BT_UUID_DIS_PNP_ID);
		if (gatt_desc) {
			pnp_id_handle = gatt_desc->handle;
			LOG_INF("Found PnP ID characteristic (handle: %d)", pnp_id_handle);
		}
	}

	/* Release discovery data */
	err = bt_gatt_dm_data_release(dm);
	if (err) {
		LOG_ERR("Could not release DIS discovery data: %d", err);
	}

	/* Start reading characteristics - begin with Firmware Revision */
	if (fw_rev_handle) {
		LOG_DBG("Reading Firmware Revision...");
		memset(&read_params, 0, sizeof(read_params));
		read_params.func = (bt_gatt_read_func_t)read_fw_rev_cb;
		read_params.handle_count = 1;
		read_params.single.handle = fw_rev_handle;
		read_params.single.offset = 0;

		err = bt_gatt_read(current_conn, &read_params);
		if (err) {
			LOG_ERR("Firmware Revision read failed to start: %d", err);
			dis_ready = true;  /* Mark ready even if reads fail */
		}
	} else {
		LOG_WRN("No DIS characteristics found");
		dis_ready = true;
	}
}

static void dis_discovery_service_not_found_cb(struct bt_conn *conn, void *context)
{
	LOG_INF("Device Information Service not found during discovery");

	/* Still try to read device name from GAP */
	LOG_DBG("Reading device name from GAP...");
	current_conn = conn;
	memset(&read_params, 0, sizeof(read_params));
	read_params.func = read_device_name_cb;
	read_params.by_uuid.uuid = BT_UUID_GAP_DEVICE_NAME;
	read_params.by_uuid.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	read_params.by_uuid.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;

	int err = bt_gatt_read(conn, &read_params);
	if (err) {
		LOG_ERR("Device name read failed to start: %d", err);
		dis_ready = true;  /* Mark ready even without device name */
	}
}

static void dis_discovery_error_found_cb(struct bt_conn *conn, int err, void *context)
{
	LOG_ERR("Device Information Service discovery failed: %d", err);
	dis_ready = true;  /* Mark ready to unblock, but with no data */

	/* Trigger discovery complete callback even on error so NUS can continue */
	if (discovery_complete_cb && conn) {
		discovery_complete_cb(conn);
	}
}

void ble_dis_set_discovery_complete_cb(ble_dis_discovery_complete_cb_t cb)
{
	discovery_complete_cb = cb;
}
