/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ble_central.h"
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/addr.h>
#include <bluetooth/gatt_dm.h>
#include <bluetooth/scan.h>
#include <bluetooth/services/nus.h>
#include <bluetooth/services/hogp.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(ble_central, LOG_LEVEL_INF);

/* BLE Central state */
static struct bt_conn *default_conn;
static struct k_work scan_work;
static struct k_work_delayable scan_indicator_work;
static bool scanning_active = false;

/* Bonded device tracking */
static bt_addr_le_t bonded_device_addr;
static bool has_bonded_device = false;

/* Device UUID tracking - to verify both HID and NUS across multiple packets */
struct device_uuid_state {
	bt_addr_le_t addr;
	bool has_hid;
	bool has_nus;
	int8_t rssi;
	int64_t timestamp;
};

static struct device_uuid_state tracked_devices[4];
static K_MUTEX_DEFINE(tracked_devices_mutex);

/* Device type detection */
static bool is_nus_device(const struct bt_scan_device_info *device_info);
static bool is_hid_device(const struct bt_scan_device_info *device_info);
static void log_device_type(const struct bt_scan_device_info *device_info, const char *addr);

/* Callback functions for external modules */
static ble_central_connected_cb_t connected_cb;
static ble_central_disconnected_cb_t disconnected_cb;

/* BLE Central callbacks */
static void connected(struct bt_conn *conn, uint8_t conn_err);
static void disconnected(struct bt_conn *conn, uint8_t reason);
static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err);

/* Scan callbacks */
static void scan_filter_match(struct bt_scan_device_info *device_info,
			      struct bt_scan_filter_match *filter_match,
			      bool connectable);
static void scan_connecting_error(struct bt_scan_device_info *device_info);
static void scan_connecting(struct bt_scan_device_info *device_info,
			    struct bt_conn *conn);

/* Device name extraction from advertising data */
static int extract_device_name_from_scan(struct bt_scan_device_info *device_info, char *name_buf, size_t buf_len);

/* Device name storage functions */
static void store_device_name_for_addr(const bt_addr_le_t *addr, const char *name);
static const char *get_stored_device_name_for_addr(const bt_addr_le_t *addr);

/* Background scan state */
static bool background_scan_active = false;

/* Authentication callbacks */
static void auth_cancel(struct bt_conn *conn);
static void pairing_complete(struct bt_conn *conn, bool bonded);
static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason);

/* Connection callbacks structure */
BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed
};

/* Forward declarations for background scanning */
static void scan_filter_match_background(struct bt_scan_device_info *device_info,
				         struct bt_scan_filter_match *filter_match,
				         bool connectable);
static void stop_background_scan(void);

/* Forward declaration for no_match callback */
static void scan_no_match(struct bt_scan_device_info *device_info, bool connectable);

/* Scan callbacks structure */
BT_SCAN_CB_INIT(scan_cb, scan_filter_match, scan_no_match,
		scan_connecting_error, scan_connecting);

/* Background scan callbacks structure for RSSI updates during connection */
BT_SCAN_CB_INIT(background_scan_cb, scan_filter_match_background, NULL,
		scan_connecting_error, scan_connecting);

/* Authentication callbacks */
static struct bt_conn_auth_cb conn_auth_callbacks = {
	.cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb conn_auth_info_callbacks = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed
};

/* Connection callback implementations */
static void connected(struct bt_conn *conn, uint8_t conn_err)
{
	char addr[BT_ADDR_LE_STR_LEN];
	int err;

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (conn_err) {
		LOG_ERR("CONNECTION FAILED: %s, error: 0x%02x (%s)", addr, conn_err,
			bt_hci_err_to_str(conn_err));

		if (default_conn == conn) {
			bt_conn_unref(default_conn);
			default_conn = NULL;

			(void)k_work_submit(&scan_work);
		}

		return;
	}

	LOG_INF("CONNECTED TO DEVICE: %s", addr);

	/* Store connection reference */
	default_conn = bt_conn_ref(conn);

	/* Stop scanning */
	scanning_active = false;
	err = bt_scan_stop();
	if ((!err) && (err != -EALREADY)) {
		LOG_ERR("Stop LE scan failed (err %d)", err);
	}

	/* Call external connected callback if registered */
	if (connected_cb) {
		connected_cb(conn);
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("DISCONNECTED FROM DEVICE: %s, reason: 0x%02x (%s)", addr, reason, bt_hci_err_to_str(reason));

	if (default_conn != conn) {
		return;
	}

	bt_conn_unref(default_conn);
	default_conn = NULL;

	/* Stop any background scanning */
	stop_background_scan();

	/* Call external disconnected callback if registered */
	if (disconnected_cb) {
		disconnected_cb(conn, reason);
	}

	LOG_INF("RESTARTING SCAN AFTER DISCONNECTION");
	(void)k_work_submit(&scan_work);
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (!err) {
		LOG_INF("Security changed: %s level %u", addr, level);
	} else {
		LOG_WRN("Security failed: %s level %u err %d %s", addr, level, err,
			bt_security_err_to_str(err));
	}
}

/* Scanning indicator work handler */
static void scan_indicator_handler(struct k_work *work)
{
	if (scanning_active) {
		if (has_bonded_device) {
			/* Format address without type suffix like "(random)" */
			char addr_str[18]; // "XX:XX:XX:XX:XX:XX\0"
			snprintf(addr_str, sizeof(addr_str), "%02X:%02X:%02X:%02X:%02X:%02X",
				bonded_device_addr.a.val[5], bonded_device_addr.a.val[4],
				bonded_device_addr.a.val[3], bonded_device_addr.a.val[2],
				bonded_device_addr.a.val[1], bonded_device_addr.a.val[0]);
			LOG_INF("Scanning for MouthPad (%s)...", addr_str);
		} else {
			LOG_INF("Scanning for MouthPad...");
		}
		/* Re-schedule for next message */
		k_work_schedule(&scan_indicator_work, K_SECONDS(1));
	}
}

/* Helper function to find or create device UUID state */
static struct device_uuid_state *find_or_create_device_state(const bt_addr_le_t *addr)
{
	int64_t now = k_uptime_get();
	int oldest_idx = 0;
	int64_t oldest_time = now;

	k_mutex_lock(&tracked_devices_mutex, K_FOREVER);

	/* First try to find existing entry */
	for (int i = 0; i < ARRAY_SIZE(tracked_devices); i++) {
		if (bt_addr_le_cmp(&tracked_devices[i].addr, addr) == 0) {
			k_mutex_unlock(&tracked_devices_mutex);
			return &tracked_devices[i];
		}
		/* Track oldest entry for potential reuse */
		if (tracked_devices[i].timestamp < oldest_time) {
			oldest_time = tracked_devices[i].timestamp;
			oldest_idx = i;
		}
	}

	/* Not found - reuse oldest entry */
	memset(&tracked_devices[oldest_idx], 0, sizeof(struct device_uuid_state));
	bt_addr_le_copy(&tracked_devices[oldest_idx].addr, addr);
	tracked_devices[oldest_idx].timestamp = now;

	k_mutex_unlock(&tracked_devices_mutex);
	return &tracked_devices[oldest_idx];
}

/* Scan callback implementations */
static void scan_filter_match(struct bt_scan_device_info *device_info,
			      struct bt_scan_filter_match *filter_match,
			      bool connectable)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(device_info->recv_info->addr, addr, sizeof(addr));

	/* If we have a bonded device, reject all other devices */
	if (has_bonded_device) {
		if (bt_addr_le_cmp(device_info->recv_info->addr, &bonded_device_addr) != 0) {
			LOG_DBG("Rejecting non-bonded device: %s", addr);
			return;
		}
	}

	int8_t rssi = device_info->recv_info->rssi;

	/* Find or create device state to track UUIDs across multiple packets */
	struct device_uuid_state *dev_state = find_or_create_device_state(device_info->recv_info->addr);

	/* Update UUID state from this packet */
	bool has_nus = is_nus_device(device_info);
	bool has_hid = is_hid_device(device_info);

	if (has_hid) dev_state->has_hid = true;
	if (has_nus) dev_state->has_nus = true;
	dev_state->rssi = rssi;
	dev_state->timestamp = k_uptime_get();

	/* Only connect to connectable packets (ignore scan responses) */
	if (!connectable) {
		return;
	}

	/* Verify device has both HID and NUS services (accumulated across packets) */
	if (!dev_state->has_hid || !dev_state->has_nus) {
		/* Still waiting for both UUIDs - don't log to reduce spam */
		return;
	}

	/* Device has both services - log and proceed with connection */
	LOG_INF("MouthPad device found: %s (RSSI: %d dBm)", addr, rssi);
	LOG_INF("CONNECTING TO MOUTHPAD DEVICE: %s", addr);

	/* Stop scanning indicator */
	scanning_active = false;

	/* Store RSSI for later use during connection */
	extern void ble_transport_set_rssi(int8_t rssi);
	ble_transport_set_rssi(rssi);

	/* Extract device name directly from advertising data */
	char device_name[32];
	if (extract_device_name_from_scan(device_info, device_name, sizeof(device_name)) == 0) {
		/* Successfully extracted name - truncate to 12 characters */
		LOG_INF("Extracted device name from advertising: '%s'", device_name);
		device_name[12] = '\0';  /* Truncate to 12 chars for display */
	} else {
		/* Fallback to checking stored names */
		const char *stored_name = get_stored_device_name_for_addr(device_info->recv_info->addr);
		if (stored_name) {
			strncpy(device_name, stored_name, 12);
			device_name[12] = '\0';
			LOG_INF("Using stored device name: '%s'", device_name);
		} else {
			/* Final fallback - use shortened address */
			char addr_str[BT_ADDR_LE_STR_LEN];
			bt_addr_le_to_str(device_info->recv_info->addr, addr_str, sizeof(addr_str));
			/* Use last 12 chars of address */
			int addr_len = strlen(addr_str);
			if (addr_len > 12) {
				strncpy(device_name, addr_str + addr_len - 12, 12);
			} else {
				strncpy(device_name, addr_str, 12);
			}
			device_name[12] = '\0';
			LOG_INF("No device name found, using address: '%s'", device_name);
		}
	}

	/* Store device name in transport layer */
	extern void ble_transport_set_device_name(const char *name);
	ble_transport_set_device_name(device_name);

	/* Update display to show device found */
	extern int oled_display_device_found(const char *device_name);
	oled_display_device_found(device_name);

	/* Manually initiate connection (since auto-connect is disabled) */
	int err = bt_scan_stop();
	if (err) {
		LOG_ERR("Failed to stop scan before connecting (err %d)", err);
	}

	/* Check if there's an existing connection to this device and clean it up */
	struct bt_conn *existing_conn = bt_conn_lookup_addr_le(BT_ID_DEFAULT, device_info->recv_info->addr);
	if (existing_conn) {
		LOG_DBG("Found existing connection object, unreferencing twice (lookup + original)...");
		bt_conn_unref(existing_conn); /* Unref from lookup */
		bt_conn_unref(existing_conn); /* Unref the actual connection */
	}

	/* Create connection with default parameters */
	struct bt_conn *conn = NULL;
	struct bt_le_conn_param *conn_param = BT_LE_CONN_PARAM_DEFAULT;

	err = bt_conn_le_create(device_info->recv_info->addr, BT_CONN_LE_CREATE_CONN,
				conn_param, &conn);
	if (err) {
		LOG_ERR("Failed to create connection (err %d)", err);
		/* Restart scanning on error */
		scanning_active = true;
		k_work_schedule(&scan_indicator_work, K_SECONDS(1));
		(void)k_work_submit(&scan_work);
		return;
	}

	/* bt_conn_le_create returns with a reference, but we don't store it here
	 * The 'connected' callback will get the connection and create its own reference
	 * So we need to unref this one to avoid leaking */
	bt_conn_unref(conn);

	LOG_INF("Connection creation initiated successfully");
}

static void scan_connecting_error(struct bt_scan_device_info *device_info)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(device_info->recv_info->addr, addr, sizeof(addr));
	LOG_ERR("SCAN CONNECTING ERROR: %s", addr);
	LOG_WRN("Connecting failed");
}

static void scan_connecting(struct bt_scan_device_info *device_info,
			    struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(device_info->recv_info->addr, addr, sizeof(addr));
	LOG_INF("SCAN CONNECTING: %s", addr);
	default_conn = bt_conn_ref(conn);
}

/* Authentication callback implementations */
static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Pairing cancelled: %s", addr);
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Pairing completed: %s, bonded: %d", addr, bonded);

	/* If bonded, store this as our bonded device */
	if (bonded) {
		bt_addr_le_copy(&bonded_device_addr, bt_conn_get_dst(conn));
		has_bonded_device = true;
		LOG_INF("Device bonded - will only reconnect to this MouthPad: %s", addr);
	}
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_WRN("Pairing failed conn: %s, reason %d %s", addr, reason,
		bt_security_err_to_str(reason));
}

/* Helper to check and store bonded device */
static void check_bonded_device(const struct bt_bond_info *info, void *user_data)
{
	/* Store the bonded device address (there should only be one with MAX_PAIRED=1) */
	bt_addr_le_copy(&bonded_device_addr, &info->addr);
	has_bonded_device = true;

	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(&info->addr, addr, sizeof(addr));
	LOG_INF("Found bonded device: %s", addr);
}

/* Address filter helper function */
static void try_add_address_filter(const struct bt_bond_info *info, void *user_data)
{
	int err;
	char addr[BT_ADDR_LE_STR_LEN];
	uint8_t *filter_mode = user_data;

	bt_addr_le_to_str(&info->addr, addr, sizeof(addr));

	struct bt_conn *conn = bt_conn_lookup_addr_le(BT_ID_DEFAULT, &info->addr);

	if (conn) {
		bt_conn_unref(conn);
		return;
	}

	err = bt_scan_filter_add(BT_SCAN_FILTER_TYPE_ADDR, &info->addr);
	if (err) {
		LOG_ERR("Address filter cannot be added (err %d): %s", err, addr);
		return;
	}

	LOG_INF("Address filter added: %s", addr);
	*filter_mode |= BT_SCAN_ADDR_FILTER;
}

/* Helper structure for UUID search */
struct uuid_search_context {
	bool found;
	const struct bt_uuid *target_uuid;
};

/* Callback for bt_data_parse to search for specific UUID */
static bool uuid_search_cb(struct bt_data *data, void *user_data)
{
	struct uuid_search_context *ctx = (struct uuid_search_context *)user_data;

	if (data->type == BT_DATA_UUID16_ALL || data->type == BT_DATA_UUID16_SOME) {
		/* Parse 16-bit UUIDs */
		if (ctx->target_uuid->type == BT_UUID_TYPE_16) {
			struct bt_uuid_16 *target = (struct bt_uuid_16 *)ctx->target_uuid;
			for (size_t i = 0; i < data->data_len; i += 2) {
				uint16_t uuid_val = sys_get_le16(&data->data[i]);
				if (uuid_val == target->val) {
					ctx->found = true;
					return false; /* Stop parsing */
				}
			}
		}
	} else if (data->type == BT_DATA_UUID128_ALL || data->type == BT_DATA_UUID128_SOME) {
		/* Parse 128-bit UUIDs */
		if (ctx->target_uuid->type == BT_UUID_TYPE_128) {
			struct bt_uuid_128 *target = (struct bt_uuid_128 *)ctx->target_uuid;
			for (size_t i = 0; i < data->data_len; i += 16) {
				if (memcmp(&data->data[i], target->val, 16) == 0) {
					ctx->found = true;
					return false; /* Stop parsing */
				}
			}
		}
	}

	return true; /* Continue parsing */
}

/* Device type detection functions */
static bool is_nus_device(const struct bt_scan_device_info *device_info)
{
	/* Check if NUS service UUID is present in advertising data */
	struct bt_uuid_128 nus_uuid = BT_UUID_INIT_128(BT_UUID_NUS_VAL);
	struct uuid_search_context ctx = {
		.found = false,
		.target_uuid = (const struct bt_uuid *)&nus_uuid,
	};

	if (device_info->adv_data) {
		struct net_buf_simple_state state;
		net_buf_simple_save(device_info->adv_data, &state);
		bt_data_parse(device_info->adv_data, uuid_search_cb, &ctx);
		net_buf_simple_restore(device_info->adv_data, &state);
	}

	return ctx.found;
}

static bool is_hid_device(const struct bt_scan_device_info *device_info)
{
	/* Check if HID service UUID is present in advertising data */
	struct bt_uuid_16 hid_uuid = BT_UUID_INIT_16(BT_UUID_HIDS_VAL);
	struct uuid_search_context ctx = {
		.found = false,
		.target_uuid = (const struct bt_uuid *)&hid_uuid,
	};

	if (device_info->adv_data) {
		struct net_buf_simple_state state;
		net_buf_simple_save(device_info->adv_data, &state);
		bt_data_parse(device_info->adv_data, uuid_search_cb, &ctx);
		net_buf_simple_restore(device_info->adv_data, &state);
	}

	return ctx.found;
}


/* Scan work handler */
static void scan_work_handler(struct k_work *item)
{
	ARG_UNUSED(item);
	(void)ble_central_start_scan();
}

/* Public API implementations */
int ble_central_init(void)
{
	int err;

	LOG_INF("Starting Bluetooth initialization...");
	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return err;
	}
	LOG_INF("Bluetooth initialized successfully");

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		LOG_INF("Loading settings...");
		settings_load();
		LOG_INF("Settings loaded");
	}

	/* Register authentication callbacks */
	err = bt_conn_auth_cb_register(&conn_auth_callbacks);
	if (err) {
		LOG_ERR("Failed to register authorization callbacks.");
		return err;
	}
	LOG_INF("Authorization callbacks registered");

	err = bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
	if (err) {
		LOG_ERR("Failed to register authorization info callbacks.");
		return err;
	}
	LOG_INF("Authorization info callbacks registered");

	/* Check if we have any bonded devices on startup */
	has_bonded_device = false;
	bt_foreach_bond(BT_ID_DEFAULT, check_bonded_device, NULL);
	if (has_bonded_device) {
		char addr[BT_ADDR_LE_STR_LEN];
		bt_addr_le_to_str(&bonded_device_addr, addr, sizeof(addr));
		LOG_INF("Found existing bond at startup: %s", addr);
	}

	/* Initialize scan without auto-connect so we can verify both HID+NUS before connecting */
	struct bt_scan_init_param scan_init = {
		.connect_if_match = false,  /* Disable auto-connect, we'll connect manually after verification */
	};

	bt_scan_init(&scan_init);
	bt_scan_cb_register(&scan_cb);

	/* Initialize scan work */
	k_work_init(&scan_work, scan_work_handler);
	k_work_init_delayable(&scan_indicator_work, scan_indicator_handler);
	LOG_INF("Scan module initialized");

	return 0;
}

int ble_central_start_scan(void)
{
	int err;
	uint8_t filter_mode = 0;

	err = bt_scan_stop();
	if (err) {
		LOG_ERR("Failed to stop scanning (err %d)", err);
		return err;
	}

	bt_scan_filter_remove_all();

	/* Check if we have a bonded device */
	has_bonded_device = false;
	bt_foreach_bond(BT_ID_DEFAULT, check_bonded_device, NULL);

	/* Add UUID filters for both HID and NUS services (MouthPad has both) */
	err = bt_scan_filter_add(BT_SCAN_FILTER_TYPE_UUID, BT_UUID_HIDS);
	if (err) {
		LOG_ERR("Cannot add HID UUID scan filter (err %d)", err);
		return err;
	}

	err = bt_scan_filter_add(BT_SCAN_FILTER_TYPE_UUID, BT_UUID_NUS_SERVICE);
	if (err) {
		LOG_ERR("Cannot add NUS UUID scan filter (err %d)", err);
		return err;
	}

	/* If we have a bonded device, only scan for that specific address */
	if (has_bonded_device) {
		bt_foreach_bond(BT_ID_DEFAULT, try_add_address_filter, &filter_mode);
	}

	/* Always enable UUID filter, optionally add address filters */
	uint8_t enable_filters = BT_SCAN_UUID_FILTER;
	if (filter_mode != 0) {
		enable_filters |= filter_mode;
		char addr[BT_ADDR_LE_STR_LEN];
		bt_addr_le_to_str(&bonded_device_addr, addr, sizeof(addr));
		LOG_INF("Enabling HID+NUS UUID + address filter for bonded device: %s", addr);
	} else {
		LOG_INF("Enabling HID+NUS UUID filter (no bonded devices - will pair with first MouthPad found)");
	}

	/* Enable filters with match_all=false (OR logic) so we get callbacks for devices with HID or NUS
	 * We'll manually verify both UUIDs are present in scan_filter_match callback */
	err = bt_scan_filter_enable(enable_filters, false);
	if (err) {
		LOG_ERR("Filters cannot be turned on (err %d)", err);
		return err;
	}

	/* Set aggressive scan parameters for faster device discovery
	 * Interval: 0x0010 = 16 * 0.625ms = 10ms (how often to start scanning)
	 * Window: 0x0010 = 16 * 0.625ms = 10ms (how long to scan each time)
	 * Setting interval == window means continuous scanning for fastest pairing
	 */
	struct bt_le_scan_param scan_param = {
		.type = BT_LE_SCAN_TYPE_ACTIVE,
		.options = BT_LE_SCAN_OPT_FILTER_DUPLICATE,
		.interval = 0x0010,  /* 10ms */
		.window = 0x0010,    /* 10ms - continuous scanning */
	};

	err = bt_scan_params_set(&scan_param);
	if (err) {
		LOG_WRN("Failed to set scan parameters (err %d), using defaults", err);
	} else {
		LOG_INF("Set aggressive scan parameters: interval=10ms, window=10ms (continuous)");
	}

	err = bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE);
	if (err) {
		LOG_ERR("Scanning failed to start (err %d)", err);
		return err;
	}

	/* Start scanning indicator */
	scanning_active = true;
	k_work_schedule(&scan_indicator_work, K_NO_WAIT);

	/* Update display to show scanning status */
	extern int oled_display_scanning(void);
	oled_display_scanning();
	
	return 0;
}

int ble_central_stop_scan(void)
{
	return bt_scan_stop();
}

struct bt_conn *ble_central_get_default_conn(void)
{
	return default_conn;
}

void ble_central_set_default_conn(struct bt_conn *conn)
{
	default_conn = conn;
}

void ble_central_register_connected_cb(ble_central_connected_cb_t cb)
{
	connected_cb = cb;
}

void ble_central_register_disconnected_cb(ble_central_disconnected_cb_t cb)
{
	disconnected_cb = cb;
}

struct k_work *ble_central_get_scan_work(void)
{
	return &scan_work;
}

/* Device type checking functions for external modules */
bool ble_central_is_nus_device(const struct bt_scan_device_info *device_info)
{
	return is_nus_device(device_info);
}

bool ble_central_is_hid_device(const struct bt_scan_device_info *device_info)
{
	return is_hid_device(device_info);
}

const char *ble_central_get_device_type_string(const struct bt_scan_device_info *device_info)
{
	bool has_nus = is_nus_device(device_info);
	bool has_hid = is_hid_device(device_info);
	
	if (has_nus && has_hid) {
		return "MOUTHPAD";
	} else if (has_nus) {
		return "NUS_ONLY";
	} else if (has_hid) {
		return "HID_ONLY";
	} else {
		return "UNKNOWN";
	}
}

/* Background scan callback - only updates RSSI for already connected devices */
static void scan_filter_match_background(struct bt_scan_device_info *device_info,
				         struct bt_scan_filter_match *filter_match,
				         bool connectable)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(device_info->recv_info->addr, addr, sizeof(addr));
	
	/* Only process if we're connected and this is our connected device */
	if (default_conn) {
		const bt_addr_le_t *connected_addr = bt_conn_get_dst(default_conn);
		if (bt_addr_le_cmp(device_info->recv_info->addr, connected_addr) == 0) {
			/* This is advertising from our connected device - update RSSI! */
			int8_t rssi = device_info->recv_info->rssi;
			LOG_INF("Background RSSI update for connected device %s: %d dBm", addr, rssi);
			
			/* Update RSSI in transport layer */
			extern void ble_transport_set_rssi(int8_t rssi);
			ble_transport_set_rssi(rssi);
		}
	}
}

/* Start background scanning for RSSI updates while connected */
int ble_central_start_background_scan_for_rssi(void)
{
	if (background_scan_active) {
		LOG_DBG("Background scan already active");
		return 0;
	}
	
	if (!default_conn) {
		LOG_WRN("No active connection for background scanning");
		return -ENOTCONN;
	}
	
	/* Use passive scanning to avoid interfering with connection */
	background_scan_active = true;
	
	LOG_INF("Starting background scan for RSSI updates");
	
	/* Register the background scan callback */
	bt_scan_cb_register(&background_scan_cb);
	
	int err = bt_scan_start(BT_SCAN_TYPE_SCAN_PASSIVE);
	if (err) {
		LOG_ERR("Failed to start background scan (err %d)", err);
		background_scan_active = false;
		return err;
	}
	
	return 0;
}

/* Stop background scanning */
static void stop_background_scan(void)
{
	if (background_scan_active) {
		LOG_INF("Stopping background scan for RSSI updates");
		bt_scan_stop();
		/* Note: No need to unregister callback as it's statically defined */
		background_scan_active = false;
	}
}

/* Parse advertising data callback for bt_data_parse() */
static bool parse_device_name_cb(struct bt_data *data, void *user_data)
{
	struct {
		char *name_buf;
		size_t buf_len;
		bool found;
	} *name_info = user_data;
	
	if (data->type == BT_DATA_NAME_COMPLETE || data->type == BT_DATA_NAME_SHORTENED) {
		/* Copy the name to our buffer */
		size_t name_len = MIN(data->data_len, name_info->buf_len - 1);
		memcpy(name_info->name_buf, data->data, name_len);
		name_info->name_buf[name_len] = '\0';
		name_info->found = true;
		return false; /* Stop parsing, we found the name */
	}
	
	return true; /* Continue parsing */
}

/* Extract device name from BLE advertising data */
static int extract_device_name_from_scan(struct bt_scan_device_info *device_info, char *name_buf, size_t buf_len)
{
	if (!device_info || !name_buf || buf_len == 0) {
		LOG_WRN("Invalid parameters for name extraction");
		return -EINVAL;
	}
	
	if (!device_info->adv_data || device_info->adv_data->len == 0) {
		LOG_DBG("No advertising data available for device");
		return -ENOENT;
	}
	
	/* Initialize with default name */
	strncpy(name_buf, "Unknown Device", buf_len - 1);
	name_buf[buf_len - 1] = '\0';
	
	/* Use bt_data_parse to find the device name */
	struct {
		char *name_buf;
		size_t buf_len;
		bool found;
	} name_info = {
		.name_buf = name_buf,
		.buf_len = buf_len,
		.found = false
	};
	
	/* bt_data_parse expects a net_buf_simple */
	bt_data_parse(device_info->adv_data, parse_device_name_cb, &name_info);
	
	if (name_info.found) {
		// LOG_INF("Extracted device name from advertising: '%s'", name_buf);
		return 0;
	}
	
	LOG_DBG("No device name found in advertising data");
	return -ENOENT;
}

/* Store device names from advertising data for later use */
static char pending_device_names[8][32]; /* Store up to 8 device names */
static bt_addr_le_t pending_device_addrs[8];
static int pending_device_count = 0;

/* Store device name for a specific address */
static void store_device_name_for_addr(const bt_addr_le_t *addr, const char *name)
{
	/* Check if we already have this device */
	for (int i = 0; i < pending_device_count; i++) {
		if (bt_addr_le_cmp(&pending_device_addrs[i], addr) == 0) {
			/* Update existing entry */
			strncpy(pending_device_names[i], name, sizeof(pending_device_names[i]) - 1);
			pending_device_names[i][sizeof(pending_device_names[i]) - 1] = '\0';
			return;
		}
	}
	
	/* Add new entry if we have space */
	if (pending_device_count < ARRAY_SIZE(pending_device_names)) {
		pending_device_addrs[pending_device_count] = *addr;
		strncpy(pending_device_names[pending_device_count], name, 
		        sizeof(pending_device_names[pending_device_count]) - 1);
		pending_device_names[pending_device_count][sizeof(pending_device_names[pending_device_count]) - 1] = '\0';
		pending_device_count++;
	}
}

/* Get stored device name for a specific address */
static const char *get_stored_device_name_for_addr(const bt_addr_le_t *addr)
{
	for (int i = 0; i < pending_device_count; i++) {
		if (bt_addr_le_cmp(&pending_device_addrs[i], addr) == 0) {
			return pending_device_names[i];
		}
	}
	return NULL;
}

/* Scan no_match callback to capture device names from all advertising packets */
static void scan_no_match(struct bt_scan_device_info *device_info, bool connectable)
{
	/* Extract and store device name for potential future connection */
	char device_name[32];
	if (extract_device_name_from_scan(device_info, device_name, sizeof(device_name)) == 0) {
		/* Successfully extracted name, store it */
		store_device_name_for_addr(device_info->recv_info->addr, device_name);

		char addr_str[BT_ADDR_LE_STR_LEN];
		bt_addr_le_to_str(device_info->recv_info->addr, addr_str, sizeof(addr_str));
		// LOG_INF("*** FOUND DEVICE NAME '%s' from %s ***", device_name, addr_str);
	}
}

/* Clear bonded device tracking (called when bonds are cleared) */
void ble_central_clear_bonded_device(void)
{
	if (has_bonded_device) {
		char addr_str[18];
		snprintf(addr_str, sizeof(addr_str), "%02X:%02X:%02X:%02X:%02X:%02X",
			bonded_device_addr.a.val[5], bonded_device_addr.a.val[4],
			bonded_device_addr.a.val[3], bonded_device_addr.a.val[2],
			bonded_device_addr.a.val[1], bonded_device_addr.a.val[0]);
		LOG_INF("Clearing bonded device tracking: %s", addr_str);
	}

	has_bonded_device = false;
	memset(&bonded_device_addr, 0, sizeof(bonded_device_addr));
	LOG_INF("Bonded device tracking cleared - will pair with any MouthPad");
}
