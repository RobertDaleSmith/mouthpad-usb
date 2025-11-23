/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ble_central.h"
#include "ble_dis.h"
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

/* BLE Central connection state */
enum ble_central_state {
	BLE_CENTRAL_STATE_DISCONNECTED,
	BLE_CENTRAL_STATE_SCANNING,
	BLE_CENTRAL_STATE_CONNECTING,
	BLE_CENTRAL_STATE_CONNECTED
};

/* BLE Central state */
static struct bt_conn *default_conn;
static struct k_work scan_work;
static struct k_work_delayable scan_indicator_work;
static enum ble_central_state connection_state = BLE_CENTRAL_STATE_DISCONNECTED;

/* Scanning mode for multi-bond support */
typedef enum {
	SCAN_MODE_NORMAL,      /* Connect to any bonded device */
	SCAN_MODE_ADDITIONAL,  /* Only connect to NEW devices (not already bonded) */
} ble_scan_mode_t;

static ble_scan_mode_t scan_mode = SCAN_MODE_NORMAL;
static int64_t additional_scan_start_time = 0;
#define ADDITIONAL_SCAN_TIMEOUT_MS 10000  /* 10 second timeout for additional scan */

/* Track if any bonded devices are advertising in current scan session */
static bool bonded_device_seen_advertising = false;

/* Multi-bond device tracking */
/* MAX_BONDED_DEVICES and struct bonded_device are defined in ble_central.h */

static struct bonded_device bonded_devices[MAX_BONDED_DEVICES];
static uint8_t bonded_device_count = 0;
static K_MUTEX_DEFINE(bonded_devices_mutex);

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

/* Settings persistence for bonded device name */
static int bonded_name_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg);
static int bonded_name_commit(void);

SETTINGS_STATIC_HANDLER_DEFINE(ble_central, "ble_central", NULL, bonded_name_set, bonded_name_commit, NULL);

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

/* Forward declaration for additional scan timeout check */
static void check_additional_scan_timeout(void);

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

		/* Connection failed - return to disconnected state */
		connection_state = BLE_CENTRAL_STATE_DISCONNECTED;
		LOG_INF("*** STATE SET TO DISCONNECTED (connection failed) ***");

		/* Clean up connection reference if it was stored */
		if (default_conn == conn) {
			bt_conn_unref(default_conn);
			default_conn = NULL;
		}

		/* Always restart scanning after connection failure */
		LOG_INF("Restarting scan after connection failure");
		(void)k_work_submit(&scan_work);

		return;
	}

	LOG_INF("CONNECTED TO DEVICE: %s", addr);

	/* Store connection reference */
	default_conn = bt_conn_ref(conn);

	/* Update state to CONNECTING - will transition to CONNECTED when services are ready */
	connection_state = BLE_CENTRAL_STATE_CONNECTING;
	LOG_INF("*** STATE SET TO CONNECTING (waiting for service discovery) ***");

	/* Stop scanning */
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

	/* Update state to disconnected */
	connection_state = BLE_CENTRAL_STATE_DISCONNECTED;
	LOG_INF("*** STATE SET TO DISCONNECTED (device disconnected) ***");

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
	if (connection_state == BLE_CENTRAL_STATE_SCANNING) {
		/* Check for additional scan timeout */
		check_additional_scan_timeout();

		if (scan_mode == SCAN_MODE_ADDITIONAL) {
			int64_t elapsed = k_uptime_get() - additional_scan_start_time;
			int remaining = (ADDITIONAL_SCAN_TIMEOUT_MS - elapsed) / 1000;
			LOG_INF("Scanning for ADDITIONAL MouthPad (%ds remaining, %d bonded)...",
				remaining, bonded_device_count);
		} else if (bonded_device_count > 0) {
			if (bonded_device_seen_advertising) {
				LOG_INF("Scanning for bonded MouthPad (%d bonded, bonded device found)...",
					bonded_device_count);
			} else {
				LOG_INF("Scanning for MouthPad (prefer bonded, %d bonded)...",
					bonded_device_count);
			}
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

	/* Check if device is already bonded */
	bool is_bonded = ble_central_is_device_bonded(device_info->recv_info->addr);

	/* Handle different scan modes */
	if (scan_mode == SCAN_MODE_ADDITIONAL) {
		/* ADDITIONAL mode: Only connect to NEW devices (not already bonded) */
		if (is_bonded) {
			LOG_DBG("ADDITIONAL scan: Skipping already-bonded device: %s", addr);
			return;
		}
		LOG_INF("ADDITIONAL scan: Found NEW device: %s", addr);
	} else {
		/* NORMAL mode: Always prefer bonded, but accept unbonded if no bonded devices advertising */
		if (is_bonded) {
			/* Mark that we've seen a bonded device advertising */
			bonded_device_seen_advertising = true;
			LOG_DBG("NORMAL scan: Found bonded device: %s", addr);
		} else if (bonded_device_count > 0) {
			/* We have bonds but this device isn't bonded */
			if (bonded_device_seen_advertising) {
				/* We've seen bonded devices advertising - skip unbonded */
				LOG_DBG("NORMAL scan: Skipping non-bonded device (bonded devices available): %s", addr);
				return;
			}
			/* No bonded devices seen advertising - accept unbonded as fallback */
			LOG_INF("NORMAL scan: No bonded devices found - accepting unbonded device: %s", addr);
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

	/* CRITICAL: Set connecting state IMMEDIATELY before any logging or processing
	 * This ensures status queries return CONNECTING as soon as we decide to connect */
	connection_state = BLE_CENTRAL_STATE_CONNECTING;
	LOG_INF("*** STATE SET TO CONNECTING ***");

	/* Device has both services - log and proceed with connection */
	LOG_INF("MouthPad device found: %s (RSSI: %d dBm)", addr, rssi);
	LOG_INF("CONNECTING TO MOUTHPAD DEVICE: %s", addr);


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
		connection_state = BLE_CENTRAL_STATE_DISCONNECTED;
		LOG_INF("*** STATE SET TO DISCONNECTED (bt_conn_le_create failed) ***");
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

	/* If bonded, add to bonded devices list */
	if (bonded) {
		/* Get device name from transport layer */
		extern const char *ble_transport_get_device_name(void);
		const char *device_name = ble_transport_get_device_name();

		/* Add to bonded devices (this also saves to settings) */
		int err = ble_central_add_bonded_device(bt_conn_get_dst(conn), device_name);
		if (err == 0) {
			LOG_INF("Device bonded and added to list - name: '%s', address: %s",
				device_name ? device_name : "(no name)", addr);
		} else {
			LOG_ERR("Failed to add bonded device to list (err %d)", err);
		}

		/* Switch back to NORMAL scan mode if we were in ADDITIONAL mode */
		if (scan_mode == SCAN_MODE_ADDITIONAL) {
			LOG_INF("New bond complete - switching back to NORMAL scan mode");
			scan_mode = SCAN_MODE_NORMAL;
		}
	}
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_WRN("Pairing failed conn: %s, reason %d %s", addr, reason,
		bt_security_err_to_str(reason));
}

/* Settings handlers for persisting bonded device name */
static int bonded_name_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	int rc;
	int bond_idx;
	char temp_buf[64];

	LOG_INF("Settings callback: name='%s', len=%d", name, len);

	/* Parse bond_X/name or bond_X/addr patterns */
	if (sscanf(name, "bond_%d", &bond_idx) == 1) {
		if (bond_idx < 0 || bond_idx >= MAX_BONDED_DEVICES) {
			LOG_WRN("Invalid bond index in settings: %d", bond_idx);
			return -EINVAL;
		}

		/* Find the subkey (name or addr) */
		const char *slash = strchr(name, '/');
		if (!slash) {
			return -ENOENT;
		}
		slash++; /* Skip the '/' */

		if (strcmp(slash, "name") == 0) {
			/* Load device name */
			if (len > sizeof(temp_buf) - 1) {
				LOG_ERR("Bonded name too long: %d bytes", len);
				return -EINVAL;
			}

			rc = read_cb(cb_arg, temp_buf, len);
			if (rc >= 0) {
				temp_buf[rc] = '\0';

				k_mutex_lock(&bonded_devices_mutex, K_FOREVER);
				/* Load name regardless of is_valid (address might load after name) */
				strncpy(bonded_devices[bond_idx].name, temp_buf,
					sizeof(bonded_devices[bond_idx].name) - 1);
				bonded_devices[bond_idx].name[sizeof(bonded_devices[bond_idx].name) - 1] = '\0';
				LOG_INF("Loaded bond %d name: '%s'", bond_idx, temp_buf);
				k_mutex_unlock(&bonded_devices_mutex);
				return 0;
			}

			LOG_ERR("Failed to read bond %d name (err %d)", bond_idx, rc);
			return rc;

		} else if (strcmp(slash, "addr") == 0) {
			/* Load device address from settings */
			bt_addr_le_t addr;
			if (len != sizeof(bt_addr_le_t)) {
				LOG_ERR("Invalid address size: %d bytes", len);
				return -EINVAL;
			}

			rc = read_cb(cb_arg, &addr, len);
			if (rc >= 0) {
				k_mutex_lock(&bonded_devices_mutex, K_FOREVER);
				/* Check if this address is already loaded */
				bool already_loaded = false;
				for (int i = 0; i < MAX_BONDED_DEVICES; i++) {
					if (bonded_devices[i].is_valid &&
					    bt_addr_le_eq(&bonded_devices[i].addr, &addr)) {
						already_loaded = true;
						break;
					}
				}

				if (!already_loaded && !bonded_devices[bond_idx].is_valid) {
					/* Load into this slot */
					bt_addr_le_copy(&bonded_devices[bond_idx].addr, &addr);
					bonded_devices[bond_idx].is_valid = true;
					bonded_devices[bond_idx].last_seen = 0;
					/* Don't clear name - it may have already been loaded from settings */
					bonded_device_count++;

					char addr_str[BT_ADDR_LE_STR_LEN];
					bt_addr_le_to_str(&addr, addr_str, sizeof(addr_str));
					LOG_INF("Loaded bond %d address from settings: %s (count now %d)",
						bond_idx, addr_str, bonded_device_count);
				}
				k_mutex_unlock(&bonded_devices_mutex);
				return 0;
			}

			LOG_ERR("Failed to read bond %d address (err %d)", bond_idx, rc);
			return rc;
		}
	}

	return -ENOENT;
}

/* Display all bonded devices with names and addresses */
static void display_bonded_devices(void)
{
	k_mutex_lock(&bonded_devices_mutex, K_FOREVER);

	if (bonded_device_count == 0) {
		LOG_INF("No bonded devices");
	} else {
		LOG_INF("=== Bonded Devices (%d/%d) ===", bonded_device_count, MAX_BONDED_DEVICES);
		for (int i = 0; i < MAX_BONDED_DEVICES; i++) {
			if (bonded_devices[i].is_valid) {
				char addr_str[BT_ADDR_LE_STR_LEN];
				bt_addr_le_to_str(&bonded_devices[i].addr, addr_str, sizeof(addr_str));

				if (bonded_devices[i].name[0] != '\0') {
					LOG_INF("  [%d] %s - %s", i, bonded_devices[i].name, addr_str);
				} else {
					LOG_INF("  [%d] (no name) - %s", i, addr_str);
				}
			}
		}
		LOG_INF("===========================");
	}

	k_mutex_unlock(&bonded_devices_mutex);
}

static int bonded_name_commit(void)
{
	/* Settings have been loaded - names are now populated in bonded_devices array */
	LOG_INF("Settings loaded for %d bonded devices", bonded_device_count);

	/* Display all bonded devices */
	display_bonded_devices();

	return 0;
}

/* Save bonded device address to persistent settings */
static void save_bonded_device_addr(int bond_idx, const bt_addr_le_t *addr)
{
	if (!IS_ENABLED(CONFIG_SETTINGS)) {
		return;
	}

	if (bond_idx < 0 || bond_idx >= MAX_BONDED_DEVICES) {
		return;
	}

	char key[32];
	snprintf(key, sizeof(key), "ble_central/bond_%d/addr", bond_idx);

	int err = settings_save_one(key, addr, sizeof(bt_addr_le_t));
	if (err) {
		LOG_ERR("Failed to save bond %d address to settings (err %d)", bond_idx, err);
	} else {
		char addr_str[BT_ADDR_LE_STR_LEN];
		bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
		LOG_DBG("Saved bond %d address to settings: %s", bond_idx, addr_str);
	}
}

/* Save bonded device name to persistent settings */
static void save_bonded_device_name(int bond_idx, const char *name)
{
	if (!IS_ENABLED(CONFIG_SETTINGS)) {
		return;
	}

	if (bond_idx < 0 || bond_idx >= MAX_BONDED_DEVICES) {
		return;
	}

	char key[32];
	snprintf(key, sizeof(key), "ble_central/bond_%d/name", bond_idx);

	int err = settings_save_one(key, name, name ? strlen(name) : 0);
	if (err) {
		LOG_ERR("Failed to save bond %d name to settings (err %d)", bond_idx, err);
	} else {
		LOG_INF("Saved bond %d name to settings: '%s'", bond_idx, name ? name : "");
	}
}

/* Helper to check and store bonded device */
static void check_bonded_device(const struct bt_bond_info *info, void *user_data)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(&info->addr, addr, sizeof(addr));

	/* Load DIS info for this bonded device to get the name */
	ble_dis_info_t dis_info;
	memset(&dis_info, 0, sizeof(dis_info));

	extern int ble_dis_load_info_for_addr(const bt_addr_le_t *addr, ble_dis_info_t *out_info);
	int err = ble_dis_load_info_for_addr(&info->addr, &dis_info);
	if (err != 0) {
		LOG_WRN("Failed to load DIS info for %s (err: %d)", addr, err);
	}

	k_mutex_lock(&bonded_devices_mutex, K_FOREVER);

	/* Find empty slot or existing entry */
	for (int i = 0; i < MAX_BONDED_DEVICES; i++) {
		if (!bonded_devices[i].is_valid) {
			/* Empty slot - add new bond */
			bt_addr_le_copy(&bonded_devices[i].addr, &info->addr);
			bonded_devices[i].is_valid = true;
			bonded_devices[i].last_seen = k_uptime_get_32();

			/* Get device name from DIS info if available */
			if (err == 0 && dis_info.has_device_name) {
				strncpy(bonded_devices[i].name, dis_info.device_name,
					sizeof(bonded_devices[i].name) - 1);
				bonded_devices[i].name[sizeof(bonded_devices[i].name) - 1] = '\0';
				LOG_INF("Found bonded device %d/%d: %s (%s)", bonded_device_count + 1,
					MAX_BONDED_DEVICES, addr, bonded_devices[i].name);
			} else {
				bonded_devices[i].name[0] = '\0';
				LOG_INF("Found bonded device %d/%d: %s (no name)", bonded_device_count + 1,
					MAX_BONDED_DEVICES, addr);
			}

			bonded_device_count++;
			break;
		} else if (bt_addr_le_eq(&bonded_devices[i].addr, &info->addr)) {
			/* Already in list */
			LOG_DBG("Bonded device already tracked: %s", addr);
			break;
		}
	}

	if (bonded_device_count >= MAX_BONDED_DEVICES) {
		LOG_WRN("Maximum bonded devices (%d) reached", MAX_BONDED_DEVICES);
	}

	k_mutex_unlock(&bonded_devices_mutex);
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

	/* Initialize bonded devices array to zero */
	k_mutex_lock(&bonded_devices_mutex, K_FOREVER);
	memset(bonded_devices, 0, sizeof(bonded_devices));
	bonded_device_count = 0;
	k_mutex_unlock(&bonded_devices_mutex);

	/* Load settings BEFORE enumerating bonds (fixes bond restoration after reboot) */
	if (IS_ENABLED(CONFIG_SETTINGS)) {
		LOG_INF("Loading settings...");
		settings_load();
	}

	/* Check if we have any bonded devices (AFTER settings loaded) */
	bt_foreach_bond(BT_ID_DEFAULT, check_bonded_device, NULL);
	LOG_INF("Found %d bonded device(s) at startup", bonded_device_count);

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

	/* Don't start scanning if we're currently connecting - let the connection complete */
	if (connection_state == BLE_CENTRAL_STATE_CONNECTING) {
		LOG_DBG("Cannot start scan: connection in progress");
		return -EBUSY;
	}

	/* Don't start scanning if we're already connected */
	if (connection_state == BLE_CENTRAL_STATE_CONNECTED) {
		LOG_DBG("Cannot start scan: already connected");
		return -EALREADY;
	}

	err = bt_scan_stop();
	if (err) {
		LOG_ERR("Failed to stop scanning (err %d)", err);
		return err;
	}

	bt_scan_filter_remove_all();

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

	/* Enable UUID filters - bond checking happens in scan_filter_match */
	if (scan_mode == SCAN_MODE_ADDITIONAL) {
		LOG_INF("Enabling HID+NUS UUID filter in ADDITIONAL scan mode (%d bonded, looking for NEW device)",
			bonded_device_count);
	} else if (bonded_device_count > 0) {
		/* Reset bonded device advertising flag for new scan session */
		bonded_device_seen_advertising = false;
		LOG_INF("Enabling HID+NUS UUID filter in NORMAL scan mode (%d bonded devices, prefer bonded)",
			bonded_device_count);
	} else {
		LOG_INF("Enabling HID+NUS UUID filter (no bonded devices - will pair with first MouthPad found)");
	}

	/* Enable filters with match_all=false (OR logic) so we get callbacks for devices with HID or NUS
	 * We'll manually verify both UUIDs are present in scan_filter_match callback */
	err = bt_scan_filter_enable(BT_SCAN_UUID_FILTER, false);
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

	/* Update state to scanning */
	connection_state = BLE_CENTRAL_STATE_SCANNING;
	LOG_INF("*** STATE SET TO SCANNING ***");

	/* Start scanning indicator */
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

/* Start additional scan mode - scan for NEW devices (not already bonded) */
int ble_central_start_additional_scan(void)
{
	LOG_INF("Starting ADDITIONAL scan mode (scan for NEW device, ignore %d bonded)", bonded_device_count);

	/* Disconnect current connection if any (but keep the bond!) */
	if (default_conn) {
		LOG_INF("Disconnecting current connection to search for additional device");
		int err = bt_conn_disconnect(default_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		if (err && err != -ENOTCONN) {
			LOG_ERR("Failed to disconnect (err %d)", err);
		}
	}

	/* Switch to ADDITIONAL scan mode */
	scan_mode = SCAN_MODE_ADDITIONAL;
	additional_scan_start_time = k_uptime_get();

	/* Start scanning */
	return ble_central_start_scan();
}

/* Check and handle additional scan timeout */
static void check_additional_scan_timeout(void)
{
	if (scan_mode != SCAN_MODE_ADDITIONAL) {
		return;
	}

	int64_t elapsed = k_uptime_get() - additional_scan_start_time;
	if (elapsed > ADDITIONAL_SCAN_TIMEOUT_MS) {
		LOG_INF("ADDITIONAL scan timeout (%lld ms) - resuming NORMAL scan mode", elapsed);
		scan_mode = SCAN_MODE_NORMAL;

		/* Restart scan in NORMAL mode */
		if (connection_state == BLE_CENTRAL_STATE_SCANNING) {
			ble_central_start_scan();
		}
	}
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

/* ==== Multi-Bond Management Functions ==== */

/* Check if a device is already bonded */
bool ble_central_is_device_bonded(const bt_addr_le_t *addr)
{
	bool found = false;

	k_mutex_lock(&bonded_devices_mutex, K_FOREVER);

	for (int i = 0; i < MAX_BONDED_DEVICES; i++) {
		if (bonded_devices[i].is_valid && bt_addr_le_eq(&bonded_devices[i].addr, addr)) {
			found = true;
			break;
		}
	}

	k_mutex_unlock(&bonded_devices_mutex);

	return found;
}

/* Add a device to the bonded list (called during pairing) */
int ble_central_add_bonded_device(const bt_addr_le_t *addr, const char *name)
{
	int ret = -ENOMEM;

	k_mutex_lock(&bonded_devices_mutex, K_FOREVER);

	/* Check if already bonded */
	for (int i = 0; i < MAX_BONDED_DEVICES; i++) {
		if (bonded_devices[i].is_valid && bt_addr_le_eq(&bonded_devices[i].addr, addr)) {
			/* Already bonded - update name and timestamp */
			if (name) {
				strncpy(bonded_devices[i].name, name, sizeof(bonded_devices[i].name) - 1);
				bonded_devices[i].name[sizeof(bonded_devices[i].name) - 1] = '\0';
				save_bonded_device_name(i, name);
			}
			bonded_devices[i].last_seen = k_uptime_get_32();

			/* Save address to ensure it persists across reboots */
			save_bonded_device_addr(i, addr);

			char addr_str[BT_ADDR_LE_STR_LEN];
			bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
			LOG_INF("Updated existing bonded device: %s (%s)", addr_str, name ? name : "no name");

			ret = 0;
			goto unlock;
		}
	}

	/* Find empty slot */
	for (int i = 0; i < MAX_BONDED_DEVICES; i++) {
		if (!bonded_devices[i].is_valid) {
			bt_addr_le_copy(&bonded_devices[i].addr, addr);
			bonded_devices[i].is_valid = true;
			bonded_devices[i].last_seen = k_uptime_get_32();

			if (name) {
				strncpy(bonded_devices[i].name, name, sizeof(bonded_devices[i].name) - 1);
				bonded_devices[i].name[sizeof(bonded_devices[i].name) - 1] = '\0';
			} else {
				bonded_devices[i].name[0] = '\0';
			}

			bonded_device_count++;

			char addr_str[BT_ADDR_LE_STR_LEN];
			bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
			LOG_INF("Added new bonded device %d/%d: %s (%s)",
				bonded_device_count, MAX_BONDED_DEVICES, addr_str, name ? name : "no name");

			/* Save to persistent settings */
			save_bonded_device_addr(i, addr);
			if (name) {
				save_bonded_device_name(i, name);
			}

			ret = 0;
			goto unlock;
		}
	}

	/* No empty slot - find and remove oldest bonded device */
	LOG_WRN("Maximum bonded devices reached (%d) - removing oldest", MAX_BONDED_DEVICES);

	int oldest_idx = -1;
	uint32_t oldest_time = UINT32_MAX;

	for (int i = 0; i < MAX_BONDED_DEVICES; i++) {
		if (bonded_devices[i].is_valid && bonded_devices[i].last_seen < oldest_time) {
			oldest_time = bonded_devices[i].last_seen;
			oldest_idx = i;
		}
	}

	if (oldest_idx >= 0) {
		char old_addr_str[BT_ADDR_LE_STR_LEN];
		bt_addr_le_to_str(&bonded_devices[oldest_idx].addr, old_addr_str, sizeof(old_addr_str));
		LOG_INF("Removing oldest bonded device [%d]: %s (%s)",
			oldest_idx, old_addr_str,
			bonded_devices[oldest_idx].name[0] ? bonded_devices[oldest_idx].name : "no name");

		/* Clear DIS info for the device being removed */
		extern void ble_dis_clear_saved_for_addr(const bt_addr_le_t *addr);
		ble_dis_clear_saved_for_addr(&bonded_devices[oldest_idx].addr);

		/* Unbond from BT stack */
		int err = bt_unpair(BT_ID_DEFAULT, &bonded_devices[oldest_idx].addr);
		if (err) {
			LOG_ERR("Failed to unpair device (err %d)", err);
		}

		/* Clear settings for this slot */
		char key[32];
		snprintf(key, sizeof(key), "ble_central/bond_%d/name", oldest_idx);
		settings_delete(key);
		snprintf(key, sizeof(key), "ble_central/bond_%d/addr", oldest_idx);
		settings_delete(key);

		/* Replace with new device */
		bt_addr_le_copy(&bonded_devices[oldest_idx].addr, addr);
		bonded_devices[oldest_idx].is_valid = true;
		bonded_devices[oldest_idx].last_seen = k_uptime_get_32();

		if (name) {
			strncpy(bonded_devices[oldest_idx].name, name, sizeof(bonded_devices[oldest_idx].name) - 1);
			bonded_devices[oldest_idx].name[sizeof(bonded_devices[oldest_idx].name) - 1] = '\0';
		} else {
			bonded_devices[oldest_idx].name[0] = '\0';
		}

		char new_addr_str[BT_ADDR_LE_STR_LEN];
		bt_addr_le_to_str(addr, new_addr_str, sizeof(new_addr_str));
		LOG_INF("Replaced with new bonded device [%d]: %s (%s)",
			oldest_idx, new_addr_str, name ? name : "no name");

		/* Save to persistent settings */
		save_bonded_device_addr(oldest_idx, addr);
		if (name) {
			save_bonded_device_name(oldest_idx, name);
		}

		ret = 0;
	}

unlock:
	k_mutex_unlock(&bonded_devices_mutex);
	return ret;
}

/* Remove a specific device from the bonded list */
int ble_central_remove_bonded_device(const bt_addr_le_t *addr)
{
	int ret = -ENOENT;

	k_mutex_lock(&bonded_devices_mutex, K_FOREVER);

	for (int i = 0; i < MAX_BONDED_DEVICES; i++) {
		if (bonded_devices[i].is_valid && bt_addr_le_eq(&bonded_devices[i].addr, addr)) {
			char addr_str[BT_ADDR_LE_STR_LEN];
			bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
			LOG_INF("Removing bonded device: %s (%s)", addr_str, bonded_devices[i].name);

			/* Clear DIS info for the device being removed */
			extern void ble_dis_clear_saved_for_addr(const bt_addr_le_t *addr);
			ble_dis_clear_saved_for_addr(addr);

			/* Clear settings for this slot */
			if (IS_ENABLED(CONFIG_SETTINGS)) {
				char key[32];
				snprintf(key, sizeof(key), "ble_central/bond_%d/name", i);
				settings_delete(key);
				snprintf(key, sizeof(key), "ble_central/bond_%d/addr", i);
				settings_delete(key);
			}

			/* Clear in-memory data */
			bonded_devices[i].is_valid = false;
			memset(&bonded_devices[i].addr, 0, sizeof(bonded_devices[i].addr));
			bonded_devices[i].name[0] = '\0';
			bonded_devices[i].last_seen = 0;
			bonded_device_count--;

			ret = 0;
			break;
		}
	}

	k_mutex_unlock(&bonded_devices_mutex);

	return ret;
}

/* Get all bonded devices (for app query) */
int ble_central_get_bonded_devices(struct bonded_device *out_list, size_t max_count)
{
	int count = 0;

	if (!out_list || max_count == 0) {
		return -EINVAL;
	}

	/* Zero the output array to prevent garbage data in unused slots */
	memset(out_list, 0, max_count * sizeof(struct bonded_device));

	k_mutex_lock(&bonded_devices_mutex, K_FOREVER);

	for (int i = 0; i < MAX_BONDED_DEVICES && count < max_count; i++) {
		if (bonded_devices[i].is_valid) {
			memcpy(&out_list[count], &bonded_devices[i], sizeof(struct bonded_device));
			count++;
		}
	}

	k_mutex_unlock(&bonded_devices_mutex);

	return count;
}

/* Clear ALL bonded device tracking (called when bonds are cleared) */
void ble_central_clear_bonded_device(void)
{
	k_mutex_lock(&bonded_devices_mutex, K_FOREVER);

	LOG_INF("Clearing all bonded device tracking (%d devices)", bonded_device_count);

	/* Log each bonded device being cleared */
	for (int i = 0; i < MAX_BONDED_DEVICES; i++) {
		if (bonded_devices[i].is_valid) {
			char addr_str[BT_ADDR_LE_STR_LEN];
			bt_addr_le_to_str(&bonded_devices[i].addr, addr_str, sizeof(addr_str));
			LOG_INF("Clearing bond: %s (%s)", addr_str, bonded_devices[i].name);
		}
	}

	/* Clear all bonds */
	memset(bonded_devices, 0, sizeof(bonded_devices));
	bonded_device_count = 0;

	k_mutex_unlock(&bonded_devices_mutex);

	/* Reset scan mode to NORMAL (in case it was in ADDITIONAL mode) */
	scan_mode = SCAN_MODE_NORMAL;

	/* Delete saved device names from persistent settings */
	if (IS_ENABLED(CONFIG_SETTINGS)) {
		for (int i = 0; i < MAX_BONDED_DEVICES; i++) {
			char key[32];
			snprintf(key, sizeof(key), "ble_central/bond_%d/name", i);
			settings_delete(key);

			snprintf(key, sizeof(key), "ble_central/bond_%d/addr", i);
			settings_delete(key);

			/* Yield to prevent blocking too long during flash operations */
			k_yield();
		}
		LOG_INF("Deleted all bonded device names from persistent storage");
	}

	LOG_INF("All bonded device tracking cleared - will pair with any MouthPad");
}

/* Get bonded device address and name if one exists (returns first bonded device for backwards compatibility) */
bool ble_central_get_bonded_device_addr(bt_addr_le_t *out_addr, char *out_name, size_t name_size)
{
	if (!out_addr) {
		return false;
	}

	k_mutex_lock(&bonded_devices_mutex, K_FOREVER);

	/* Find first valid bonded device */
	for (int i = 0; i < MAX_BONDED_DEVICES; i++) {
		if (bonded_devices[i].is_valid) {
			bt_addr_le_copy(out_addr, &bonded_devices[i].addr);

			/* Copy cached device name if requested */
			if (out_name && name_size > 0) {
				if (bonded_devices[i].name[0] != '\0') {
					strncpy(out_name, bonded_devices[i].name, name_size - 1);
					out_name[name_size - 1] = '\0';
					LOG_INF("Returning bonded device with name: '%s'", out_name);
				} else {
					out_name[0] = '\0';
					LOG_INF("Returning bonded device with NO cached name");
				}
			}

			k_mutex_unlock(&bonded_devices_mutex);
			return true;
		}
	}

	k_mutex_unlock(&bonded_devices_mutex);

	LOG_INF("No bonded devices found");
	return false;
}

/* Query if actively scanning for devices */
bool ble_central_is_scanning(void)
{
	bool result = connection_state == BLE_CENTRAL_STATE_SCANNING;
	LOG_INF("is_scanning query: state=%d, returning %d", connection_state, result);
	return result;
}

/* Query if connection attempt is in progress */
bool ble_central_is_connecting(void)
{
	bool result = connection_state == BLE_CENTRAL_STATE_CONNECTING;
	LOG_INF("is_connecting query: state=%d, returning %d", connection_state, result);
	return result;
}

/* Query if connected to a device */
bool ble_central_is_connected(void)
{
	bool result = connection_state == BLE_CENTRAL_STATE_CONNECTED;
	LOG_INF("is_connected query: state=%d, returning %d", connection_state, result);
	return result;
}

/* Mark that GATT services are ready - transitions from CONNECTING to CONNECTED */
void ble_central_mark_services_ready(void)
{
	if (connection_state == BLE_CENTRAL_STATE_CONNECTING) {
		connection_state = BLE_CENTRAL_STATE_CONNECTED;
		LOG_INF("*** STATE SET TO CONNECTED (services ready) ***");
	} else {
		LOG_WRN("mark_services_ready called but state is not CONNECTING (state=%d)", connection_state);
	}
}
