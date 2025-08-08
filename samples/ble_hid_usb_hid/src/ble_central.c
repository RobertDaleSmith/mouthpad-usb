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
#include <dk_buttons_and_leds.h>

#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>

#include "ble_central.h"

LOG_MODULE_REGISTER(ble_central, LOG_LEVEL_INF);

/* Key used to accept or reject passkey value */
#define KEY_PAIRING_ACCEPT DK_BTN1_MSK
#define KEY_PAIRING_REJECT DK_BTN2_MSK

/* BLE Central variables */
static struct bt_conn *default_conn;
static struct bt_conn *auth_conn;

/* Callback registration */
static ble_connected_cb_t connected_callback = NULL;
static ble_disconnected_cb_t disconnected_callback = NULL;
static ble_security_changed_cb_t security_changed_callback = NULL;

/* Forward declarations */
static void gatt_discover(struct bt_conn *conn);

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

/* Scan callback functions */
static void scan_filter_match(struct bt_scan_device_info *device_info,
			      struct bt_scan_filter_match *filter_match,
			      bool connectable)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (!filter_match->uuid.match ||
	    (filter_match->uuid.count < 1)) {

		printk("Invalid device connected\n");

		return;
	}

	bt_addr_le_to_str(device_info->recv_info->addr, addr, sizeof(addr));

	/* Check if HID service is present */
	bool has_hid = false;
	for (int i = 0; i < filter_match->uuid.count; i++) {
		const struct bt_uuid *uuid = filter_match->uuid.uuid[i];
		if (uuid->type == BT_UUID_TYPE_16 && BT_UUID_16(uuid)->val == BT_UUID_HIDS_VAL) {
			has_hid = true;
			break;
		}
	}

	printk("Filters matched on device %s.\n", addr);
	if (has_hid) {
		printk("  - HID service found\n");
	}
	printk("Connectable: %s\n", connectable ? "yes" : "no");
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

BT_SCAN_CB_INIT(scan_cb, scan_filter_match, NULL,
		scan_connecting_error, scan_connecting);

static void scan_init(void)
{
	int err;

	struct bt_scan_init_param scan_init = {
		.connect_if_match = true,
		.scan_param = NULL,
		.conn_param = BT_LE_CONN_PARAM_DEFAULT
	};

	bt_scan_init(&scan_init);
	bt_scan_cb_register(&scan_cb);
}

int ble_central_init(void)
{
	int err;

	LOG_INF("Initializing BLE Central...");
	
	printk("Starting Bluetooth initialization...\n");
	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return err;
	}
	LOG_INF("Bluetooth initialized");
	printk("Bluetooth initialized successfully\n");

	/* Fixed passkey (if configured) is handled by Kconfig at build time */

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		printk("Loading settings...\n");
		settings_load();
		printk("Settings loaded\n");
	}

	scan_init();
	LOG_INF("✓ BLE Central initialized");
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

	/* Add HID service filter */
	err = bt_scan_filter_add(BT_SCAN_FILTER_TYPE_UUID, BT_UUID_HIDS);
	if (err) {
		LOG_ERR("HID UUID filter cannot be added (err %d", err);
		return err;
	}
	filter_mode |= BT_SCAN_UUID_FILTER;

	bt_foreach_bond(BT_ID_DEFAULT, try_add_address_filter, &filter_mode);

	err = bt_scan_filter_enable(filter_mode, false);
	if (err) {
		LOG_ERR("Filters cannot be turned on (err %d)", err);
		return err;
	}

	err = bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE);
	if (err) {
		LOG_ERR("Scanning failed to start (err %d)", err);
		return err;
	}

	LOG_INF("Scan started");
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
		printk("Failed to connect to %s, 0x%02x %s\n", addr, conn_err,
		       bt_hci_err_to_str(conn_err));
		if (conn == ble_central_get_default_conn()) {
			bt_conn_unref(ble_central_get_default_conn());
			ble_central_set_default_conn(NULL);

			/* This demo doesn't require active scan */
			err = ble_central_start_scan();
			if (err) {
				printk("Scanning failed to start (err %d)\n",
				       err);
			}
		}

		return;
	}

	printk("Connected: %s\n", addr);

	/* Stop scanning once connected */
	err = bt_scan_stop();
	if ((!err) && (err != -EALREADY)) {
		LOG_ERR("Stop LE scan failed (err %d)", err);
	}

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

	printk("Disconnected: %s, reason 0x%02x %s\n", addr, reason, bt_hci_err_to_str(reason));

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

	/* This demo doesn't require active scan */
	err = ble_central_start_scan();
	if (err) {
		printk("Scanning failed to start (err %d)\n", err);
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
	} else {
		printk("Security failed: %s level %u err %d %s\n", addr, level, err,
		       bt_security_err_to_str(err));
	}

	/* Call external security changed callback if registered */
	if (security_changed_callback) {
		security_changed_callback(conn, level, err);
	}

	/* GATT discovery will be handled by the transport layer */
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected        = connected,
	.disconnected     = disconnected,
	.security_changed = security_changed
};

/* GATT Discovery callback functions */
static void discovery_completed_cb(struct bt_gatt_dm *dm,
				   void *context)
{
	int err;

	printk("The discovery procedure succeeded\n");

	LOG_INF("=== GATT DISCOVERY DATA ===");
	bt_gatt_dm_data_print(dm);
	LOG_INF("=== END GATT DISCOVERY DATA ===");

	extern struct bt_hogp *ble_hid_get_hogp(void);
	struct bt_hogp *hogp = ble_hid_get_hogp();
	err = bt_hogp_handles_assign(dm, hogp);
	if (err) {
		printk("Could not init HIDS client object, error: %d\n", err);
		LOG_ERR("HOGP handles assignment failed (err %d)", err);
		LOG_ERR("Device may not have a complete HID service");
		
		/* Release the discovery data even if HOGP assignment failed */
		err = bt_gatt_dm_data_release(dm);
		if (err) {
			LOG_ERR("Failed to release discovery data (err %d)", err);
		}
		return;
	}

	err = bt_gatt_dm_data_release(dm);
	if (err) {
		printk("Could not release the discovery data, error "
		       "code: %d\n", err);
		LOG_ERR("Failed to release discovery data (err %d)", err);
	}

	/* Signal to transport layer that HID discovery is complete */
	extern void ble_hid_discovery_complete(void);
	LOG_INF("Calling HID discovery complete callback");
	ble_hid_discovery_complete();
}

static void discovery_service_not_found_cb(struct bt_conn *conn,
					   void *context)
{
	printk("The service could not be found during the discovery\n");
	LOG_ERR("HID service not found on device");
	LOG_ERR("This might indicate the device doesn't have HID services");
}

static void discovery_error_found_cb(struct bt_conn *conn,
				     int err,
				     void *context)
{
	printk("The discovery procedure failed with %d\n", err);
	LOG_ERR("HID service discovery failed (err %d)", err);
	LOG_ERR("This might indicate the device doesn't have HID services or connection issues");
}

static const struct bt_gatt_dm_cb discovery_cb = {
	.completed = discovery_completed_cb,
	.service_not_found = discovery_service_not_found_cb,
	.error_found = discovery_error_found_cb,
};

static void gatt_discover(struct bt_conn *conn)
{
	static int retry_count = 0;
	int err;

	if (conn != ble_central_get_default_conn()) {
		return;
	}

	/* Check if connection is valid */
	if (!conn) {
		LOG_ERR("Invalid connection, cannot start discovery");
		return;
	}

	LOG_INF("Starting HID service discovery... (attempt %d)", retry_count + 1);
	err = bt_gatt_dm_start(conn, BT_UUID_HIDS, &discovery_cb, NULL);
	if (err) {
		LOG_ERR("Could not start HID discovery (err %d)", err);
		LOG_ERR("This might be due to connection not being ready or security issues");
		
		/* Retry up to 2 times with shorter delays */
		if (retry_count < 2) {
			retry_count++;
			LOG_INF("Retrying discovery in %d ms...", retry_count * 500);
			k_sleep(K_MSEC(retry_count * 500));
			gatt_discover(conn);
		} else {
			LOG_ERR("GATT discovery failed after %d attempts", retry_count);
			retry_count = 0; // Reset for next connection
		}
	} else {
		LOG_INF("HID service discovery started successfully");
		retry_count = 0; // Reset on success
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
	/* Display only; confirmation is handled automatically in auth_passkey_confirm */
}

static void auth_pairing_confirm(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("Auto-confirming Just Works pairing for %s\n", addr);
	bt_conn_auth_pairing_confirm(conn);
}

static void auth_passkey_confirm(struct bt_conn *conn, unsigned int passkey)
{
	char addr[BT_ADDR_LE_STR_LEN];

	ble_central_set_auth_conn(bt_conn_ref(conn));

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Passkey for %s: %06u\n", addr, passkey);
	printk("Auto-confirming numeric comparison\n");
	bt_conn_auth_passkey_confirm(conn);
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

	printk("Pairing failed conn: %s, reason %d %s\n", addr, reason,
	       bt_security_err_to_str(reason));
}

static struct bt_conn_auth_cb conn_auth_callbacks = {
	.passkey_display = auth_passkey_display,
	.passkey_confirm = auth_passkey_confirm,
	.pairing_confirm = auth_pairing_confirm,
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

	err = bt_conn_auth_cb_register(&conn_auth_callbacks);
	if (err) {
		printk("failed to register authorization callbacks.\n");
		return err;
	}

	err = bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
	if (err) {
		printk("Failed to register authorization info callbacks.\n");
		return err;
	}

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

int ble_central_register_security_changed_cb(ble_security_changed_cb_t cb)
{
	security_changed_callback = cb;
	return 0;
} 