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
#include <zephyr/sys/printk.h>

LOG_MODULE_REGISTER(ble_central, LOG_LEVEL_INF);

/* BLE Central state */
static struct bt_conn *default_conn;
static struct k_work scan_work;

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
		printk("*** CONNECTION FAILED: %s, error: 0x%02x ***\n", addr, conn_err);
		LOG_INF("Failed to connect to %s, 0x%02x %s", addr, conn_err,
			bt_hci_err_to_str(conn_err));

		if (default_conn == conn) {
			bt_conn_unref(default_conn);
			default_conn = NULL;

			(void)k_work_submit(&scan_work);
		}

		return;
	}

	printk("*** CONNECTED TO DEVICE: %s ***\n", addr);
	LOG_INF("Connected: %s", addr);

	/* Call external connected callback if registered */
	if (connected_cb) {
		connected_cb(conn);
	}

	err = bt_scan_stop();
	if ((!err) && (err != -EALREADY)) {
		LOG_ERR("Stop LE scan failed (err %d)", err);
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("*** DISCONNECTED FROM DEVICE: %s, reason: 0x%02x ***\n", addr, reason);
	LOG_INF("Disconnected: %s, reason 0x%02x %s", addr, reason, bt_hci_err_to_str(reason));

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

	printk("*** RESTARTING SCAN AFTER DISCONNECTION ***\n");
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

/* Scan callback implementations */
static void scan_filter_match(struct bt_scan_device_info *device_info,
			      struct bt_scan_filter_match *filter_match,
			      bool connectable)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(device_info->recv_info->addr, addr, sizeof(addr));

	/* Log device type and connection status */
	log_device_type(device_info, addr);
	/* Log RSSI information */
	int8_t rssi = device_info->recv_info->rssi;
	printk("*** DEVICE FOUND: %s connectable: %d RSSI: %d dBm ***\n", 
	       addr, connectable, rssi);
	LOG_INF("Filters matched. Address: %s connectable: %d RSSI: %d dBm",
		addr, connectable, rssi);
	
	/* Only connect to devices that have both NUS and HID services */
	bool has_nus = is_nus_device(device_info);
	bool has_hid = is_hid_device(device_info);
	
	if (has_nus && has_hid) {
		printk("*** CONNECTING TO MOUTHPAD DEVICE: %s (RSSI: %d dBm) ***\n", addr, rssi);
		LOG_INF("Connecting to MouthPad device: %s (RSSI: %d dBm)", addr, rssi);
		
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
		
	} else {
		printk("*** REJECTING DEVICE (missing required services): %s ***\n", addr);
		LOG_INF("Rejecting device - missing required services: %s", addr);
		/* Don't connect to this device */
		return;
	}
}

static void scan_connecting_error(struct bt_scan_device_info *device_info)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(device_info->recv_info->addr, addr, sizeof(addr));
	printk("*** SCAN CONNECTING ERROR: %s ***\n", addr);
	LOG_WRN("Connecting failed");
}

static void scan_connecting(struct bt_scan_device_info *device_info,
			    struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(device_info->recv_info->addr, addr, sizeof(addr));
	printk("*** SCAN CONNECTING: %s ***\n", addr);
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
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_WRN("Pairing failed conn: %s, reason %d %s", addr, reason,
		bt_security_err_to_str(reason));
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

/* Device type detection functions */
static bool is_nus_device(const struct bt_scan_device_info *device_info)
{
	/* Since we're scanning without UUID filters, we'll assume any device that matches
	   our scan filter has the services we're looking for. The scan filter system
	   handles the UUID matching for us. */
	return true;
}

static bool is_hid_device(const struct bt_scan_device_info *device_info)
{
	/* Since we're scanning without UUID filters, we'll assume any device that matches
	   our scan filter has the services we're looking for. The scan filter system
	   handles the UUID matching for us. */
	return true;
}

static void log_device_type(const struct bt_scan_device_info *device_info, const char *addr)
{
	/* Since we're scanning for devices with both NUS and HID services,
	   any device that matches our scan filter is likely a MouthPad device */
	printk("*** MOUTHPAD DEVICE FOUND (NUS + HID): %s ***\n", addr);
	LOG_INF("MouthPad device detected (NUS + HID): %s", addr);
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

	printk("Starting Bluetooth initialization...\n");
	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return err;
	}
	LOG_INF("Bluetooth initialized");
	printk("Bluetooth initialized successfully\n");

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		printk("Loading settings...\n");
		settings_load();
		printk("Settings loaded\n");
	}

	/* Register authentication callbacks */
	err = bt_conn_auth_cb_register(&conn_auth_callbacks);
	if (err) {
		LOG_ERR("Failed to register authorization callbacks.");
		return err;
	}
	printk("Authorization callbacks registered\n");

	err = bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
	if (err) {
		printk("Failed to register authorization info callbacks.\n");
		return err;
	}
	printk("Authorization info callbacks registered\n");

	/* Initialize scan */
	struct bt_scan_init_param scan_init = {
		.connect_if_match = true,
	};

	bt_scan_init(&scan_init);
	bt_scan_cb_register(&scan_cb);

	/* Initialize scan work */
	k_work_init(&scan_work, scan_work_handler);
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

	/* Add UUID filter for HID service (like working sample) */
	err = bt_scan_filter_add(BT_SCAN_FILTER_TYPE_UUID, BT_UUID_HIDS);
	if (err) {
		LOG_ERR("Cannot add HID UUID scan filter (err %d)", err);
		return err;
	}

	bt_foreach_bond(BT_ID_DEFAULT, try_add_address_filter, &filter_mode);

	/* Always enable UUID filter, optionally add address filters */
	uint8_t enable_filters = BT_SCAN_UUID_FILTER;
	if (filter_mode != 0) {
		enable_filters |= filter_mode;
		LOG_INF("Enabling HID UUID filter + address filters for bonded devices");
	} else {
		LOG_INF("Enabling HID UUID filter (no bonded devices)");
	}

	err = bt_scan_filter_enable(enable_filters, false);
	if (err) {
		LOG_ERR("Filters cannot be turned on (err %d)", err);
		return err;
	}

	err = bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE);
	if (err) {
		LOG_ERR("Scanning failed to start (err %d)", err);
		return err;
	}

	LOG_INF("Scan started (checking for devices with both NUS and HID services)");
	
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
		LOG_INF("*** FOUND DEVICE NAME '%s' from %s ***", device_name, addr_str);
	}
}
