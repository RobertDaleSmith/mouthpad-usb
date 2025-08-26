/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ble_central.h"
#include "ble_multi_conn.h"
#include "even_g1.h"
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
static struct bt_conn *default_conn;  /* Keep for backward compatibility */
static struct k_work scan_work;
static bool even_g1_pairing_mode = false;  /* True when looking for second Even G1 arm */
static bool connection_in_progress = false;  /* Prevent multiple simultaneous connections */
static struct k_work_delayable connection_retry_work;
static int64_t last_connection_attempt_time = 0;  /* Track last attempt to prevent spam */
#define CONNECTION_ATTEMPT_DELAY_MS 2000  /* Wait 2 seconds between attempts */

/* Even G1 Dual-Arm Connection State Machine */
typedef enum {
    EVEN_G1_STATE_IDLE,
    EVEN_G1_STATE_CONNECTING_LEFT,
    EVEN_G1_STATE_LEFT_CONNECTED,
    EVEN_G1_STATE_LEFT_DISCOVERING,
    EVEN_G1_STATE_LEFT_MTU_EXCHANGING,
    EVEN_G1_STATE_LEFT_READY,
    EVEN_G1_STATE_CONNECTING_RIGHT,
    EVEN_G1_STATE_RIGHT_CONNECTED,
    EVEN_G1_STATE_RIGHT_DISCOVERING,
    EVEN_G1_STATE_RIGHT_MTU_EXCHANGING,
    EVEN_G1_STATE_BOTH_READY
} even_g1_connection_state_t;

static even_g1_connection_state_t even_g1_state = EVEN_G1_STATE_IDLE;
static struct bt_conn *even_g1_left_pending_conn = NULL;
static struct bt_conn *even_g1_right_pending_conn = NULL;

/* Even G1 discovery cache */
struct even_g1_discovered_device {
	bt_addr_le_t addr;
	ble_device_type_t device_type;
	char name[32];
	int8_t rssi;
	bool valid;
};

static struct even_g1_discovered_device even_g1_left_cache;
static struct even_g1_discovered_device even_g1_right_cache;

/* Device type detection */
static bool is_nus_device(const struct bt_scan_device_info *device_info);
static bool is_hid_device(const struct bt_scan_device_info *device_info);
static void log_device_type(const struct bt_scan_device_info *device_info, const char *addr);

/* Even G1 dual connection helper */
static void connect_to_both_even_g1_arms(void);

/* Even G1 state machine functions */
static void even_g1_advance_state_machine(void);
static void even_g1_handle_connection_complete(struct bt_conn *conn);
static void even_g1_handle_discovery_complete(struct bt_conn *conn);
static void even_g1_handle_mtu_exchange_complete(struct bt_conn *conn);
static void even_g1_reset_state_machine(void);
static void even_g1_start_left_connection(void);
static void even_g1_start_right_connection(void);

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

/* Smart scanning state for button-triggered scans */
static bool smart_scan_active = false;

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
		
		/* Reset connection in progress flag */
		connection_in_progress = false;

		if (default_conn == conn) {
			bt_conn_unref(default_conn);
			default_conn = NULL;

			(void)k_work_submit(&scan_work);
		}

		return;
	}

	printk("*** CONNECTED TO DEVICE: %s ***\n", addr);
	LOG_INF("Connected: %s", addr);
	
	/* Reset connection in progress flag */
	connection_in_progress = false;

	/* Handle Even G1 state machine updates */
	even_g1_handle_connection_complete(conn);

	/* Call external connected callback if registered */
	if (connected_cb) {
		connected_cb(conn);
	}

	/* Stop scanning once all desired connections are established */
	if (!ble_multi_conn_need_even_g1_pair() && !even_g1_pairing_mode) {
		err = bt_scan_stop();
		if ((!err) && (err != -EALREADY)) {
			LOG_ERR("Stop LE scan failed (err %d)", err);
		}
		LOG_INF("All desired devices connected, scan stopped");
		even_g1_pairing_mode = false;
	} else {
		LOG_INF("Continuing scan for remaining devices (Even G1 pair needed: %s)", 
		        ble_multi_conn_need_even_g1_pair() ? "yes" : "no");
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("*** DISCONNECTED FROM DEVICE: %s, reason: 0x%02x ***\n", addr, reason);
	LOG_INF("Disconnected: %s, reason 0x%02x %s", addr, reason, bt_hci_err_to_str(reason));

	/* Check if this was an Even G1 device and reset state machine if needed */
	if (conn == even_g1_left_pending_conn || conn == even_g1_right_pending_conn) {
		LOG_WRN("Even G1 device disconnected - resetting state machine");
		even_g1_reset_state_machine();
	}

	/* Always call external disconnected callback first for any connection */
	if (disconnected_cb) {
		disconnected_cb(conn, reason);
	}

	/* Handle cleanup for default connection */
	if (default_conn == conn) {
		bt_conn_unref(default_conn);
		default_conn = NULL;

		/* Stop any background scanning */
		stop_background_scan();

		printk("*** RESTARTING SCAN AFTER DISCONNECTION ***\n");
		(void)k_work_submit(&scan_work);
	} else {
		/* For non-default connections, still restart scan to look for replacements */
		LOG_INF("Non-default connection disconnected, checking if scan restart needed");
		
		/* Check if we need to restart scanning (e.g., for Even G1 pairing) */
		int active_connections = ble_multi_conn_count();
		LOG_INF("Active connections remaining: %d", active_connections);
		
		/* If this was an Even G1 device, check if we need to keep looking for its pair */
		if (ble_multi_conn_need_even_g1_pair()) {
			LOG_INF("Even G1 pair still needed, ensuring scan is active");
			k_work_submit(&scan_work);
		}
	}
	
	/* Reset connection progress flag in case it was stuck */
	connection_in_progress = false;
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (!err) {
		LOG_INF("Security changed: %s level %u", addr, level);
		
		/* Notify Even G1 module if this is an Even G1 connection */
		ble_device_connection_t *dev = ble_multi_conn_get(conn);
		if (dev && (dev->type == DEVICE_TYPE_EVEN_G1_LEFT || 
		           dev->type == DEVICE_TYPE_EVEN_G1_RIGHT)) {
			even_g1_security_changed(conn, level);
		}
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
	char device_name[32] = {0};

	bt_addr_le_to_str(device_info->recv_info->addr, addr, sizeof(addr));

	/* Log RSSI information */
	int8_t rssi = device_info->recv_info->rssi;
	
	/* Extract device name first */
	if (extract_device_name_from_scan(device_info, device_name, sizeof(device_name)) != 0) {
		/* Fallback to stored name or address */
		const char *stored_name = get_stored_device_name_for_addr(device_info->recv_info->addr);
		if (stored_name) {
			strncpy(device_name, stored_name, sizeof(device_name) - 1);
		} else {
			strncpy(device_name, "Unknown", sizeof(device_name) - 1);
		}
	}
	
	/* Detect device type using multi-connection manager first */
	ble_device_type_t device_type = ble_multi_conn_detect_device_type(device_info, device_name);
	
	/* Only log devices we're interested in to reduce spam */
	if (device_type == DEVICE_TYPE_MOUTHPAD || 
	    device_type == DEVICE_TYPE_EVEN_G1_LEFT || 
	    device_type == DEVICE_TYPE_EVEN_G1_RIGHT) {
		printk("*** DEVICE FOUND: %s (%s) connectable: %d RSSI: %d dBm ***\n", 
		       addr, device_name, connectable, rssi);
		LOG_INF("Device found: %s (%s) RSSI: %d dBm", addr, device_name, rssi);
	}
	
	/* Check if we should connect to this device */
	bool should_connect = false;
	const char *connect_reason = NULL;
	
	/* Smart scan filtering: if button-triggered, only look for missing device types */
	if (smart_scan_active) {
		bool has_mouthpad = ble_multi_conn_has_type(DEVICE_TYPE_MOUTHPAD);
		bool has_even_g1 = ble_multi_conn_has_type(DEVICE_TYPE_EVEN_G1_LEFT) || 
		                   ble_multi_conn_has_type(DEVICE_TYPE_EVEN_G1_RIGHT);
		
		/* If we have Even G1 connected, only look for MouthPad */
		if (has_even_g1 && device_type != DEVICE_TYPE_MOUTHPAD) {
			LOG_DBG("Smart scan: Have Even G1, ignoring non-MouthPad device (%s)", device_name);
			return;
		}
		
		/* If we have MouthPad connected, only look for Even G1 */
		if (has_mouthpad && device_type != DEVICE_TYPE_EVEN_G1_LEFT && device_type != DEVICE_TYPE_EVEN_G1_RIGHT) {
			LOG_DBG("Smart scan: Have MouthPad, ignoring non-Even G1 device (%s)", device_name);
			return;
		}
		
		/* Check if this is the same device we're already connected to */
		ble_device_connection_t *existing_conn;
		if (device_type == DEVICE_TYPE_MOUTHPAD) {
			existing_conn = ble_multi_conn_get_mouthpad();
		} else if (device_type == DEVICE_TYPE_EVEN_G1_LEFT) {
			existing_conn = ble_multi_conn_get_even_g1_left();
		} else if (device_type == DEVICE_TYPE_EVEN_G1_RIGHT) {
			existing_conn = ble_multi_conn_get_even_g1_right();
		} else {
			existing_conn = NULL;
		}
		
		if (existing_conn && bt_addr_le_cmp(device_info->recv_info->addr, &existing_conn->addr) == 0) {
			LOG_INF("Smart scan: Ignoring identical device already connected (%s)", device_name);
			return;
		}
		
		LOG_INF("Smart scan: Targeting %s device (%s)", 
		        device_type == DEVICE_TYPE_MOUTHPAD ? "MouthPad" : "Even G1", device_name);
	}
	
	switch (device_type) {
	case DEVICE_TYPE_MOUTHPAD:
		if (!ble_multi_conn_has_type(DEVICE_TYPE_MOUTHPAD)) {
			should_connect = true;
			connect_reason = "MouthPad device";
		} else {
			LOG_INF("Already have a MouthPad connected, skipping");
		}
		break;
		
	case DEVICE_TYPE_EVEN_G1_LEFT:
		if (!ble_multi_conn_has_type(DEVICE_TYPE_EVEN_G1_LEFT)) {
			/* Cache this device instead of connecting immediately */
			even_g1_left_cache.addr = *device_info->recv_info->addr;
			even_g1_left_cache.device_type = device_type;
			strncpy(even_g1_left_cache.name, device_name, sizeof(even_g1_left_cache.name) - 1);
			even_g1_left_cache.rssi = rssi;
			even_g1_left_cache.valid = true;
			LOG_INF("Cached Even G1 Left Arm for dual connection");
			even_g1_pairing_mode = true;
		} else {
			LOG_INF("Already have Even G1 left arm connected, skipping");
		}
		break;
		
	case DEVICE_TYPE_EVEN_G1_RIGHT:
		if (!ble_multi_conn_has_type(DEVICE_TYPE_EVEN_G1_RIGHT)) {
			/* Cache this device instead of connecting immediately */
			even_g1_right_cache.addr = *device_info->recv_info->addr;
			even_g1_right_cache.device_type = device_type;
			strncpy(even_g1_right_cache.name, device_name, sizeof(even_g1_right_cache.name) - 1);
			even_g1_right_cache.rssi = rssi;
			even_g1_right_cache.valid = true;
			LOG_INF("Cached Even G1 Right Arm for dual connection");
			even_g1_pairing_mode = true;
		} else {
			LOG_INF("Already have Even G1 right arm connected, skipping");
		}
		break;
		
	case DEVICE_TYPE_NUS_GENERIC:
		/* For now, skip generic NUS devices unless in special mode */
		LOG_DBG("Generic NUS device found, skipping for now");
		break;
		
	default:
		/* Don't log for unknown device types to reduce spam */
		break;
	}
	
	/* Check if we have both Even G1 arms cached and should connect to both */
	if (even_g1_left_cache.valid && even_g1_right_cache.valid && 
	    !ble_multi_conn_has_type(DEVICE_TYPE_EVEN_G1_LEFT) && 
	    !ble_multi_conn_has_type(DEVICE_TYPE_EVEN_G1_RIGHT)) {
		connect_to_both_even_g1_arms();
		return;  /* Early return - dual connection handled */
	}
	
	if (should_connect) {
		printk("*** CONNECTING TO %s: %s (RSSI: %d dBm) ***\n", 
		       connect_reason, addr, rssi);
		LOG_INF("Connecting to %s: %s (RSSI: %d dBm)", 
		        connect_reason, addr, rssi);
		
		/* Store RSSI for later use during connection */
		extern void ble_transport_set_rssi(int8_t rssi);
		ble_transport_set_rssi(rssi);
		
		/* Store device name in transport layer */
		extern void ble_transport_set_device_name(const char *name);
		ble_transport_set_device_name(device_name);
		
		/* Update display to show device found */
		extern int oled_display_device_found(const char *device_name);
		oled_display_device_found(device_name);
		
		/* For MouthPad devices, let the scan module handle connection automatically */
		if (device_type == DEVICE_TYPE_MOUTHPAD) {
			LOG_INF("MouthPad connection will be handled by scan module");
		}
		/* Note: Even G1 devices are handled by dual connection logic above */
	} else {
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
	char device_name[32] = {0};
	bt_addr_le_to_str(device_info->recv_info->addr, addr, sizeof(addr));
	printk("*** SCAN CONNECTING: %s ***\n", addr);
	
	/* Try to get device name from stored names or scan data */
	const char *stored_name = get_stored_device_name_for_addr(device_info->recv_info->addr);
	if (stored_name) {
		strncpy(device_name, stored_name, sizeof(device_name) - 1);
	} else if (extract_device_name_from_scan(device_info, device_name, sizeof(device_name)) != 0) {
		strncpy(device_name, "Unknown", sizeof(device_name) - 1);
	}
	
	/* Detect device type and set it in transport layer */
	ble_device_type_t device_type = ble_multi_conn_detect_device_type(device_info, device_name);
	
	/* Store device type and name in transport layer for connection callback */
	extern void ble_transport_set_device_type(ble_device_type_t type);
	extern void ble_transport_set_device_name(const char *name);
	ble_transport_set_device_type(device_type);
	ble_transport_set_device_name(device_name);
	
	LOG_INF("Device connecting via filter: %s (%s) type=%d", addr, device_name, device_type);
	
	/* Reset smart scan mode when connection succeeds */
	if (smart_scan_active) {
		LOG_INF("Smart scan: Connection successful, disabling smart scan mode");
		smart_scan_active = false;
	}
	
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

	/* Skip address filters for Even G1 devices - force them through device detection */
	if (strstr(addr, "F8:21:E9:32:7B:9B") != NULL || strstr(addr, "F4:7B:D2:69:81:C6") != NULL) {
		LOG_INF("Skipping address filter for Even G1 device: %s (forcing through device detection)", addr);
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

	/* Add HID filter for MouthPad devices, Even G1 will be caught in scan_no_match */
	err = bt_scan_filter_add(BT_SCAN_FILTER_TYPE_UUID, BT_UUID_HIDS);
	if (err) {
		LOG_WRN("Cannot add HID UUID scan filter (err %d), scanning all devices", err);
	} else {
		LOG_INF("Added HID UUID filter for MouthPad devices");
	}

	bt_foreach_bond(BT_ID_DEFAULT, try_add_address_filter, &filter_mode);

	/* Enable UUID and address filters */
	uint8_t enable_filters = 0;
	if (err == 0) {  /* HID filter was added successfully */
		enable_filters |= BT_SCAN_UUID_FILTER;
	}
	if (filter_mode != 0) {
		enable_filters |= filter_mode;
	}
	
	if (enable_filters != 0) {
		err = bt_scan_filter_enable(enable_filters, false);
		if (err) {
			LOG_ERR("Filters cannot be turned on (err %d)", err);
			return err;
		}
		LOG_INF("Enabled filters: HID=%d, Address=%d", 
		        (enable_filters & BT_SCAN_UUID_FILTER) ? 1 : 0,
		        (filter_mode != 0) ? 1 : 0);
	} else {
		LOG_INF("No filters enabled - scanning all devices");
	}

	err = bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE);
	if (err) {
		LOG_ERR("Scanning failed to start (err %d)", err);
		return err;
	}

	LOG_INF("Scan started (checking for MouthPad and Even G1 devices)");
	
	/* Update display to show scanning status */
	extern int oled_display_scanning(void);
	oled_display_scanning();
	
	return 0;
}

int ble_central_start_scan_for_missing_devices(void)
{
	/* Check what devices we already have */
	bool has_mouthpad = ble_multi_conn_has_type(DEVICE_TYPE_MOUTHPAD);
	bool has_even_g1 = ble_multi_conn_has_type(DEVICE_TYPE_EVEN_G1_LEFT) || 
	                   ble_multi_conn_has_type(DEVICE_TYPE_EVEN_G1_RIGHT);
	
	if (has_mouthpad && has_even_g1) {
		LOG_INF("Smart scan: Already have both MouthPad and Even G1 connected - nothing to scan for");
		return 0;
	}
	
	if (has_mouthpad) {
		LOG_INF("Smart scan: Have MouthPad, scanning for Even G1 devices only");
	} else if (has_even_g1) {
		LOG_INF("Smart scan: Have Even G1, scanning for MouthPad devices only");
	} else {
		LOG_INF("Smart scan: No devices connected, scanning for any compatible device");
	}
	
	/* Enable smart scan mode and start regular scan */
	smart_scan_active = true;
	int err = ble_central_start_scan();
	if (err) {
		smart_scan_active = false;
		return err;
	}
	
	return 0;
}

int ble_central_stop_scan(void)
{
	smart_scan_active = false;
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
	char addr[BT_ADDR_LE_STR_LEN];
	char device_name[32] = {0};

	bt_addr_le_to_str(device_info->recv_info->addr, addr, sizeof(addr));
	
	/* Extract device name */
	if (extract_device_name_from_scan(device_info, device_name, sizeof(device_name)) != 0) {
		const char *stored_name = get_stored_device_name_for_addr(device_info->recv_info->addr);
		if (stored_name) {
			strncpy(device_name, stored_name, sizeof(device_name) - 1);
		} else {
			strncpy(device_name, "Unknown", sizeof(device_name) - 1);
		}
	}
	
	/* Store device name for future reference */
	store_device_name_for_addr(device_info->recv_info->addr, device_name);
	
	/* Check if this could be an Even G1 device (NUS service but no HID) */
	ble_device_type_t device_type = ble_multi_conn_detect_device_type(device_info, device_name);
	
	/* Only log devices we're interested in */
	if (device_type == DEVICE_TYPE_MOUTHPAD || 
	    device_type == DEVICE_TYPE_EVEN_G1_LEFT || 
	    device_type == DEVICE_TYPE_EVEN_G1_RIGHT) {
		LOG_INF("No filter match: %s (%s) - type %d", addr, device_name, device_type);
	}
	
	if (device_type == DEVICE_TYPE_EVEN_G1_LEFT || device_type == DEVICE_TYPE_EVEN_G1_RIGHT) {
		int8_t rssi = device_info->recv_info->rssi;
		
		/* Cache Even G1 devices instead of connecting immediately */
		if (device_type == DEVICE_TYPE_EVEN_G1_LEFT && !ble_multi_conn_has_type(DEVICE_TYPE_EVEN_G1_LEFT)) {
			/* Cache left arm */
			even_g1_left_cache.addr = *device_info->recv_info->addr;
			even_g1_left_cache.device_type = device_type;
			strncpy(even_g1_left_cache.name, device_name, sizeof(even_g1_left_cache.name) - 1);
			even_g1_left_cache.rssi = rssi;
			even_g1_left_cache.valid = true;
			LOG_INF("Cached Even G1 Left Arm for dual connection");
			even_g1_pairing_mode = true;
		} else if (device_type == DEVICE_TYPE_EVEN_G1_RIGHT && !ble_multi_conn_has_type(DEVICE_TYPE_EVEN_G1_RIGHT)) {
			/* Cache right arm */
			even_g1_right_cache.addr = *device_info->recv_info->addr;
			even_g1_right_cache.device_type = device_type;
			strncpy(even_g1_right_cache.name, device_name, sizeof(even_g1_right_cache.name) - 1);
			even_g1_right_cache.rssi = rssi;
			even_g1_right_cache.valid = true;
			LOG_INF("Cached Even G1 Right Arm for dual connection");
			even_g1_pairing_mode = true;
		}
		
		/* Check if we have both arms cached and should connect to both */
		if (even_g1_left_cache.valid && even_g1_right_cache.valid && 
		    !ble_multi_conn_has_type(DEVICE_TYPE_EVEN_G1_LEFT) && 
		    !ble_multi_conn_has_type(DEVICE_TYPE_EVEN_G1_RIGHT)) {
			connect_to_both_even_g1_arms();
			return;  /* Early return - dual connection handled */
		}
		/* All Even G1 connection logic is now handled by the dual-arm caching above */
	}
}

/* Even G1 State Machine Implementation */

static void even_g1_reset_state_machine(void)
{
	LOG_INF("Resetting Even G1 state machine");
	even_g1_state = EVEN_G1_STATE_IDLE;
	
	if (even_g1_left_pending_conn) {
		bt_conn_unref(even_g1_left_pending_conn);
		even_g1_left_pending_conn = NULL;
	}
	
	if (even_g1_right_pending_conn) {
		bt_conn_unref(even_g1_right_pending_conn);
		even_g1_right_pending_conn = NULL;
	}
	
	/* Clear cache */
	even_g1_left_cache.valid = false;
	even_g1_right_cache.valid = false;
	even_g1_pairing_mode = false;
}

static void even_g1_start_left_connection(void)
{
	if (!even_g1_left_cache.valid) {
		LOG_ERR("Cannot start left connection - no cached left arm");
		even_g1_reset_state_machine();
		return;
	}
	
	LOG_INF("Starting Even G1 left arm connection");
	
	extern void ble_transport_set_rssi(int8_t rssi);
	extern void ble_transport_set_device_name(const char *name);
	extern void ble_transport_set_device_type(ble_device_type_t type);
	extern int oled_display_device_found(const char *device_name);
	
	/* Set transport layer for left arm */
	ble_transport_set_device_type(even_g1_left_cache.device_type);
	ble_transport_set_rssi(even_g1_left_cache.rssi);
	ble_transport_set_device_name(even_g1_left_cache.name);
	oled_display_device_found(even_g1_left_cache.name);
	
	even_g1_state = EVEN_G1_STATE_CONNECTING_LEFT;
	
	int err = bt_conn_le_create(&even_g1_left_cache.addr, 
	                            BT_CONN_LE_CREATE_CONN,
	                            BT_LE_CONN_PARAM_DEFAULT, 
	                            &even_g1_left_pending_conn);
	if (err) {
		LOG_ERR("Failed to create connection to left arm: %d", err);
		even_g1_reset_state_machine();
	} else {
		LOG_INF("Connection initiated to Even G1 left arm");
		/* Set as default connection for MTU exchange callbacks */
		default_conn = bt_conn_ref(even_g1_left_pending_conn);
	}
}

static void even_g1_start_right_connection(void)
{
	if (!even_g1_right_cache.valid) {
		LOG_ERR("Cannot start right connection - no cached right arm");
		even_g1_reset_state_machine();
		return;
	}
	
	LOG_INF("Starting Even G1 right arm connection");
	
	extern void ble_transport_set_rssi(int8_t rssi);
	extern void ble_transport_set_device_name(const char *name);
	extern void ble_transport_set_device_type(ble_device_type_t type);
	extern int oled_display_device_found(const char *device_name);
	
	/* Set transport layer for right arm */
	ble_transport_set_device_type(even_g1_right_cache.device_type);
	ble_transport_set_rssi(even_g1_right_cache.rssi);
	ble_transport_set_device_name(even_g1_right_cache.name);
	oled_display_device_found(even_g1_right_cache.name);
	
	even_g1_state = EVEN_G1_STATE_CONNECTING_RIGHT;
	
	int err = bt_conn_le_create(&even_g1_right_cache.addr, 
	                            BT_CONN_LE_CREATE_CONN,
	                            BT_LE_CONN_PARAM_DEFAULT, 
	                            &even_g1_right_pending_conn);
	if (err) {
		LOG_ERR("Failed to create connection to right arm: %d", err);
		even_g1_reset_state_machine();
	} else {
		LOG_INF("Connection initiated to Even G1 right arm");
		/* Update default connection for right arm */
		if (default_conn) {
			bt_conn_unref(default_conn);
		}
		default_conn = bt_conn_ref(even_g1_right_pending_conn);
	}
}

static void even_g1_advance_state_machine(void)
{
	LOG_INF("*** ADVANCING STATE MACHINE: current state = %d ***", even_g1_state);
	
	switch (even_g1_state) {
	case EVEN_G1_STATE_IDLE:
		/* Start with left arm */
		LOG_INF("State: IDLE - checking if both arms cached");
		if (even_g1_left_cache.valid && even_g1_right_cache.valid) {
			LOG_INF("Both arms cached, starting left connection");
			even_g1_start_left_connection();
		}
		break;
		
	case EVEN_G1_STATE_LEFT_READY:
		/* Left arm is ready, now connect to right arm */
		LOG_INF("State: LEFT_READY - starting right arm connection");
		even_g1_start_right_connection();
		break;
		
	case EVEN_G1_STATE_RIGHT_MTU_EXCHANGING:
		/* Right arm MTU exchange complete, we should be fully ready */
		even_g1_state = EVEN_G1_STATE_BOTH_READY;
		LOG_INF("*** BOTH EVEN G1 ARMS CONNECTED AND READY ***");
		printk("*** BOTH EVEN G1 ARMS CONNECTED AND READY ***\\n");
		
		/* Stop scanning since we have both arms */
		int scan_err = bt_scan_stop();
		if (scan_err && scan_err != -EALREADY) {
			LOG_WRN("Failed to stop scan: %d", scan_err);
		}
		
		/* Clear pairing mode */
		even_g1_pairing_mode = false;
		break;
		
	default:
		/* No action needed for other states */
		LOG_INF("State: %d - no action needed", even_g1_state);
		break;
	}
}

static void even_g1_handle_connection_complete(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	
	LOG_INF("*** CONNECTION COMPLETE CALLBACK: %s, state=%d ***", addr, even_g1_state);
	
	if (even_g1_state == EVEN_G1_STATE_CONNECTING_LEFT && conn == even_g1_left_pending_conn) {
		LOG_INF("Even G1 left arm connected: %s", addr);
		even_g1_state = EVEN_G1_STATE_LEFT_CONNECTED;
		LOG_INF("*** LEFT ARM CONNECTED - STATE NOW %d ***", even_g1_state);
	} else if (even_g1_state == EVEN_G1_STATE_CONNECTING_RIGHT && conn == even_g1_right_pending_conn) {
		LOG_INF("Even G1 right arm connected: %s", addr);
		even_g1_state = EVEN_G1_STATE_RIGHT_CONNECTED;
		LOG_INF("*** RIGHT ARM CONNECTED - STATE NOW %d ***", even_g1_state);
	}
}

static void even_g1_handle_discovery_complete(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	
	LOG_INF("*** DISCOVERY COMPLETE CALLBACK: %s, state=%d ***", addr, even_g1_state);
	
	if (even_g1_state == EVEN_G1_STATE_LEFT_CONNECTED && conn == even_g1_left_pending_conn) {
		LOG_INF("Even G1 left arm service discovery complete: %s", addr);
		even_g1_state = EVEN_G1_STATE_LEFT_MTU_EXCHANGING;
		LOG_INF("*** LEFT ARM DISCOVERY COMPLETE - STATE NOW %d (waiting for MTU) ***", even_g1_state);
	} else if (even_g1_state == EVEN_G1_STATE_RIGHT_CONNECTED && conn == even_g1_right_pending_conn) {
		LOG_INF("Even G1 right arm service discovery complete: %s", addr);
		even_g1_state = EVEN_G1_STATE_RIGHT_MTU_EXCHANGING;
		LOG_INF("*** RIGHT ARM DISCOVERY COMPLETE - STATE NOW %d (waiting for MTU) ***", even_g1_state);
	}
}

static void even_g1_handle_mtu_exchange_complete(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	
	LOG_INF("*** STATE MACHINE MTU CALLBACK: %s, state=%d, left_pending=%p, right_pending=%p ***", 
	        addr, even_g1_state, even_g1_left_pending_conn, even_g1_right_pending_conn);
	
	if ((even_g1_state == EVEN_G1_STATE_LEFT_CONNECTED || even_g1_state == EVEN_G1_STATE_LEFT_DISCOVERING || even_g1_state == EVEN_G1_STATE_LEFT_MTU_EXCHANGING) && 
	    conn == even_g1_left_pending_conn) {
		LOG_INF("Even G1 left arm MTU exchange complete: %s", addr);
		even_g1_state = EVEN_G1_STATE_LEFT_READY;
		LOG_INF("*** ADVANCING STATE MACHINE TO CONNECT RIGHT ARM ***");
		even_g1_advance_state_machine(); /* This will trigger right arm connection */
	} else if ((even_g1_state == EVEN_G1_STATE_RIGHT_CONNECTED || even_g1_state == EVEN_G1_STATE_RIGHT_DISCOVERING || even_g1_state == EVEN_G1_STATE_RIGHT_MTU_EXCHANGING) && 
	           conn == even_g1_right_pending_conn) {
		LOG_INF("Even G1 right arm MTU exchange complete: %s", addr);
		/* Mark right arm ready and advance to final state */
		even_g1_state = EVEN_G1_STATE_RIGHT_MTU_EXCHANGING;
		even_g1_advance_state_machine(); /* This will mark both arms ready */
	} else {
		LOG_WRN("MTU exchange callback - state/connection mismatch!");
		LOG_WRN("Current state: %d", even_g1_state);
		LOG_WRN("Connection: %p vs left: %p vs right: %p", conn, even_g1_left_pending_conn, even_g1_right_pending_conn);
	}
}

/* Even G1 dual connection helper - now uses state machine */
static void connect_to_both_even_g1_arms(void)
{
	printk("*** BOTH EVEN G1 ARMS FOUND - STARTING STATE MACHINE ***\n");
	LOG_INF("Both Even G1 arms discovered, starting sequential connection state machine");
	
	/* Stop scanning to avoid conflicts with connection creation */
	LOG_INF("Stopping scan for dual connection attempt");
	int scan_err = bt_scan_stop();
	if (scan_err && scan_err != -EALREADY) {
		LOG_WRN("Failed to stop scan: %d", scan_err);
	}
	
	/* Small delay to let scan stop complete */
	k_msleep(100);
	
	/* Start the state machine */
	even_g1_advance_state_machine();
}

/* Public API for Even G1 state machine callbacks */
void ble_central_even_g1_discovery_complete(struct bt_conn *conn)
{
	even_g1_handle_discovery_complete(conn);
}

void ble_central_even_g1_mtu_exchange_complete(struct bt_conn *conn)
{
	even_g1_handle_mtu_exchange_complete(conn);
}
