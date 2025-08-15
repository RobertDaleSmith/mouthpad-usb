/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <bluetooth/gatt_dm.h>
#include <zephyr/sys/byteorder.h>
#include <bluetooth/scan.h>
#include <bluetooth/services/hogp.h>

#include <zephyr/logging/log.h>
#include <dk_buttons_and_leds.h>

#include "ble_central.h"

LOG_MODULE_REGISTER(ble_central, LOG_LEVEL_INF);

/* Key used to accept or reject passkey value */
#define KEY_PAIRING_ACCEPT DK_BTN1_MSK
#define KEY_PAIRING_REJECT DK_BTN2_MSK

/* BLE Central variables */
static struct bt_conn *default_conn;
static struct bt_conn *auth_conn;
static struct k_work_delayable sec_retry_work;
static struct bt_conn *sec_retry_conn;

/* Callback registration */
static ble_connected_cb_t connected_callback = NULL;
static ble_disconnected_cb_t disconnected_callback = NULL;

/* Forward declarations */
static void gatt_discover(struct bt_conn *conn);
static bool le_param_req(struct bt_conn *conn, struct bt_le_conn_param *param);
static void le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency, uint16_t timeout);
static void sec_retry_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	if (!sec_retry_conn) {
		return;
	}
	int err = bt_conn_set_security(sec_retry_conn, BT_SECURITY_L1);
	printk("bt_conn_set_security retry -> %d\n", err);
	if (err == -EBUSY || err == -EINVAL) {
		k_work_reschedule(&sec_retry_work, K_MSEC(100));
		return;
	}
	bt_conn_unref(sec_retry_conn);
	sec_retry_conn = NULL;
}

/* Scan callback functions */
static void scan_filter_match(struct bt_scan_device_info *device_info,
			      struct bt_scan_filter_match *filter_match,
			      bool connectable)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (!filter_match->uuid.match ||
	    (filter_match->uuid.count != 1)) {

		printk("Invalid device connected\n");

		return;
	}

	const struct bt_uuid *uuid = filter_match->uuid.uuid[0];

	bt_addr_le_to_str(device_info->recv_info->addr, addr, sizeof(addr));

	printk("Filters matched on UUID 0x%04x.\nAddress: %s connectable: %s\n",
		BT_UUID_16(uuid)->val,
		addr, connectable ? "yes" : "no");
}

static void scan_connecting_error(struct bt_scan_device_info *device_info)
{
	printk("Connecting failed\n");
}

static void scan_connecting(struct bt_scan_device_info *device_info,
			    struct bt_conn *conn)
{
	ble_central_set_default_conn(bt_conn_ref(conn));
}

static void scan_filter_no_match(struct bt_scan_device_info *device_info,
				 bool connectable)
{
	int err;
	struct bt_conn *conn = NULL;
	char addr[BT_ADDR_LE_STR_LEN];

	if (device_info->recv_info->adv_type == BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
		bt_addr_le_to_str(device_info->recv_info->addr, addr,
				  sizeof(addr));
		printk("Direct advertising received from %s\n", addr);
		bt_scan_stop();

		err = bt_conn_le_create(device_info->recv_info->addr,
					BT_CONN_LE_CREATE_CONN,
					device_info->conn_param, &conn);

		if (!err) {
			ble_central_set_default_conn(bt_conn_ref(conn));
			bt_conn_unref(conn);
		}
	}
}

BT_SCAN_CB_INIT(scan_cb, scan_filter_match, scan_filter_no_match,
		scan_connecting_error, scan_connecting);

/* Scan initialization moved to after bt_enable() like HID Remapper */

int ble_central_init(void)
{
	LOG_INF("Initializing BLE Central...");
	k_work_init_delayable(&sec_retry_work, sec_retry_work_handler);
	/* Scan init moved to after bt_enable() */
	LOG_INF("✓ BLE Central initialized");
	return 0;
}

int ble_central_start_scan(void)
{
	int err;

	err = bt_scan_start(BT_SCAN_TYPE_SCAN_PASSIVE);
	if (err) {
		printk("Scanning failed to start (err %d)\n", err);
		return err;
	}

	printk("Scanning successfully started\n");
	return 0;
}

int ble_central_stop_scan(void)
{
	bt_scan_stop();
	return 0;
}

struct bt_conn *ble_central_get_default_conn(void)
{
	return default_conn;
}

void ble_central_set_default_conn(struct bt_conn *conn)
{
	default_conn = conn;
}

struct bt_conn *ble_central_get_auth_conn(void)
{
	return auth_conn;
}

void ble_central_set_auth_conn(struct bt_conn *conn)
{
	auth_conn = conn;
}

/* Connection callback functions */
static void connected(struct bt_conn *conn, uint8_t conn_err)
{
	int err;
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (conn_err) {
		printk("Failed to connect to %s, 0x%02x (reason %d)\n", addr, conn_err, conn_err);
		if (conn == ble_central_get_default_conn()) {
			bt_conn_unref(ble_central_get_default_conn());
			ble_central_set_default_conn(NULL);

			/* Let bt_scan module handle restarting automatically */
			printk("Connection failed - bt_scan module will restart scan automatically\n");
		}

		return;
	}

	printk("Connected: %s\n", addr);

	/* Try different approach: Don't request security at all initially */
	/* Just proceed directly to GATT discovery and let the device request security if needed */
	printk("Skipping initial security request - proceeding to GATT discovery\n");

	/* Start GATT discovery immediately */
	gatt_discover(conn);

	/* Call transport layer callback if registered */
	if (connected_callback) {
		connected_callback(conn);
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];
	int err;

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (ble_central_get_auth_conn()) {
		bt_conn_unref(ble_central_get_auth_conn());
		ble_central_set_auth_conn(NULL);
	}

	printk("Disconnected: %s, reason 0x%02x (%d)\n", addr, reason, reason);

	/* Release HOGP if active - this will be handled by ble.c */
	extern struct bt_hogp *ble_hid_get_hogp(void);
	extern bool bt_hogp_assign_check(const struct bt_hogp *hogp);
	extern void bt_hogp_release(struct bt_hogp *hogp);
	
	struct bt_hogp *hogp = ble_hid_get_hogp();
	if (bt_hogp_assign_check(hogp)) {
		printk("HIDS client active - releasing");
		bt_hogp_release(hogp);
	}

	if (ble_central_get_default_conn() != conn) {
		return;
	}

	bt_conn_unref(ble_central_get_default_conn());
	ble_central_set_default_conn(NULL);

	/* Restart scan immediately like Nordic sample does */
	err = bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE);
	if (err) {
		printk("Scanning failed to start (err %d)\n", err);
	} else {
		printk("Scanning restarted successfully after disconnection\n");
	}

	/* Call transport layer callback if registered */
	if (disconnected_callback) {
		disconnected_callback(conn, reason);
	}
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (!err) {
		printk("Security changed: %s level %u\n", addr, level);

		/* Security upgrade successful - GATT discovery should already be running */
		/* This is just informational now since we start discovery immediately */
		if (level >= BT_SECURITY_L2) {
			printk("Encryption established (level %u)\n", level);
		}
	} else {
		printk("Security failed: %s level %u err %d\n", addr, level, err);

		/* Handle security failures */
		if (err == BT_SECURITY_ERR_AUTH_FAIL) {
			printk("Authentication failed - device may need re-pairing\n");
		} else if (err == BT_SECURITY_ERR_PIN_OR_KEY_MISSING) {
			printk("Bonding information missing\n");
		}
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected        = connected,
	.disconnected     = disconnected,
	.security_changed = security_changed,
	.le_param_req     = le_param_req,
	.le_param_updated = le_param_updated
};

/* GATT Discovery callback functions */
static void discovery_completed_cb(struct bt_gatt_dm *dm,
				   void *context)
{
	int err;
	struct bt_conn *conn = bt_gatt_dm_conn_get(dm);

	printk("The discovery procedure succeeded\n");

	bt_gatt_dm_data_print(dm);

	/* Check security level and request upgrade if needed */
	bt_security_t sec = bt_conn_get_security(conn);
	printk("Discovery complete - current security level: %d\n", sec);

	if (sec < BT_SECURITY_L2) {
		printk("Requesting L2 security before HOGP init...\n");
		err = bt_conn_set_security(conn, BT_SECURITY_L2);
		if (err) {
			printk("Security request failed: %d, continuing anyway\n", err);
		} else {
			printk("Security upgrade requested\n");
		}
	}

	printk("Proceeding with HOGP setup...\n");
	extern struct bt_hogp *ble_hid_get_hogp(void);
	struct bt_hogp *hogp = ble_hid_get_hogp();
	err = bt_hogp_handles_assign(dm, hogp);
	if (err) {
		printk("Could not init HIDS client object, error: %d\n", err);
	}

	err = bt_gatt_dm_data_release(dm);
	if (err) {
		printk("Could not release the discovery data, error "
		       "code: %d\n", err);
	}
}

static void discovery_service_not_found_cb(struct bt_conn *conn,
					   void *context)
{
	printk("The service could not be found during the discovery\n");
}

static void discovery_error_found_cb(struct bt_conn *conn,
				     int err,
				     void *context)
{
	printk("The discovery procedure failed with %d\n", err);
}

/* Connection parameter callbacks (mirror HID Remapper behavior) */
static bool le_param_req(struct bt_conn *conn, struct bt_le_conn_param *param)
{
	/* Keep max equal to min */
	param->interval_max = param->interval_min;
	return true;
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency, uint16_t timeout)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(interval);
	ARG_UNUSED(latency);
	ARG_UNUSED(timeout);
}

static const struct bt_gatt_dm_cb discovery_cb = {
	.completed = discovery_completed_cb,
	.service_not_found = discovery_service_not_found_cb,
	.error_found = discovery_error_found_cb,
};

static void gatt_discover(struct bt_conn *conn)
{
	int err;

	if (conn != ble_central_get_default_conn()) {
		return;
	}

	printk("Starting GATT discovery for HID service\n");
	err = bt_gatt_dm_start(conn, BT_UUID_HIDS, &discovery_cb, NULL);
	if (err) {
		printk("Could not start the discovery procedure, error code: %d\n", err);
		
		/* If discovery fails due to insufficient security, the stack will request upgrade */
		if (err == -EPERM) {
			printk("Discovery blocked by insufficient security - waiting for security upgrade\n");
		}
	} else {
		printk("GATT discovery started successfully\n");
	}
}

int ble_central_gatt_discover(struct bt_conn *conn)
{
	gatt_discover(conn);
	return 0;
}

/* Authentication callback functions */
static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Passkey for %s: %06u\n", addr, passkey);
}

static void auth_passkey_confirm(struct bt_conn *conn, unsigned int passkey)
{
	ARG_UNUSED(passkey);
	bt_conn_auth_passkey_confirm(conn);
	printk("Numeric comparison auto-accepted\n");
}

static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Pairing cancelled: %s\n", addr);
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Pairing completed: %s, bonded: %d\n", addr, bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Pairing failed conn: %s, reason %d\n", addr, reason);
}

static void pairing_confirm(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("Just Works pairing request from %s - auto-accepting\n", addr);
	bt_conn_auth_pairing_confirm(conn);
}

/* Use pairing_confirm to auto-accept Just Works pairing */
static struct bt_conn_auth_cb conn_auth_callbacks = {
	.passkey_display = auth_passkey_display,
	.pairing_confirm = pairing_confirm,  /* Auto-accept Just Works pairing */
	.passkey_confirm = auth_passkey_confirm,  /* Auto-accept numeric comparison */
	.cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb conn_auth_info_callbacks = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed
};

void ble_central_num_comp_reply(bool accept)
{
	if (accept) {
		bt_conn_auth_passkey_confirm(ble_central_get_auth_conn());
		printk("Numeric Match, conn %p\n", ble_central_get_auth_conn());
	} else {
		bt_conn_auth_cancel(ble_central_get_auth_conn());
		printk("Numeric Reject, conn %p\n", ble_central_get_auth_conn());
	}

	bt_conn_unref(ble_central_get_auth_conn());
	ble_central_set_auth_conn(NULL);
}

int ble_central_init_auth_callbacks(void)
{
	int err;

	/* Check if callbacks are already registered */
	static bool callbacks_registered = false;
	if (callbacks_registered) {
		LOG_WRN("Auth callbacks already registered");
		return 0;
	}

	/* Register auth callbacks */
	err = bt_conn_auth_cb_register(&conn_auth_callbacks);
	if (err) {
		printk("Failed to register authorization callbacks (err %d).\n", err);
		
		/* -EINVAL can mean callbacks already registered */
		if (err == -EINVAL) {
			printk("Note: Callbacks might already be registered or invalid\n");
		}
		return err;
	}

	err = bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
	if (err) {
		printk("Failed to register authorization info callbacks (err %d).\n", err);
		return err;
	}

	callbacks_registered = true;
	LOG_INF("BLE Central auth callbacks initialized");
	return 0;
}

void ble_central_handle_buttons(uint32_t button_state, uint32_t has_changed)
{
	uint32_t button = button_state & has_changed;

	if (ble_central_get_auth_conn()) {
		if (button & KEY_PAIRING_ACCEPT) {
			ble_central_num_comp_reply(true);
		}

		if (button & KEY_PAIRING_REJECT) {
			ble_central_num_comp_reply(false);
		}

		return;
	}
}

int ble_central_init_callbacks(void)
{
	LOG_INF("BLE Central callbacks initialized");
	return 0;
}

int ble_central_register_connected_cb(ble_connected_cb_t cb)
{
	connected_callback = cb;
	return 0;
}

int ble_central_register_disconnected_cb(ble_disconnected_cb_t cb)
{
	disconnected_callback = cb;
	return 0;
}

int ble_central_init_scan(void)
{
	LOG_INF("Initializing BLE scan after Bluetooth enabled...");
	
	/* Match HID Remapper conn params: 7.5 ms interval, latency 44, timeout 4 s */
	static const struct bt_le_conn_param custom_conn_param = {
		.interval_min = 6,
		.interval_max = 6,
		.latency = 44,
		.timeout = 400,
	};

	struct bt_scan_init_param scan_init = {
		.connect_if_match = 1,
		.scan_param = NULL,
        .conn_param = &custom_conn_param
	};

	/* Initialize scan module (like HID Remapper does after bt_enable) */
	bt_scan_init(&scan_init);
	bt_scan_cb_register(&scan_cb);

	LOG_INF("BLE scan initialized successfully");
	return 0;
}

int ble_central_setup_scan_filters(void)
{
	int err;

	LOG_INF("Setting up scan filters...");
	
	/* Remove all existing filters first (like HID Remapper) */
	bt_scan_filter_remove_all();

	/* Add UUID filter for HID service (like HID Remapper) */
	err = bt_scan_filter_add(BT_SCAN_FILTER_TYPE_UUID, BT_UUID_HIDS);
	if (err) {
		LOG_ERR("Cannot add UUID scan filter (err %d)", err);
		return err;
	}

	/* Enable UUID filter with match-all flag (like HID Remapper) */
	err = bt_scan_filter_enable(BT_SCAN_UUID_FILTER, true);
	if (err) {
		LOG_ERR("Cannot enable scan filters (err %d)", err);
		return err;
	}

	LOG_INF("Scan filters set up successfully");
	return 0;
}
