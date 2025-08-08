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
#include <bluetooth/gatt_dm.h>
#include <bluetooth/scan.h>
#include <bluetooth/services/nus.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <dk_buttons_and_leds.h>

LOG_MODULE_REGISTER(ble_central, LOG_LEVEL_INF);

/* Button definitions for pairing */
#define KEY_PAIRING_ACCEPT DK_BTN1_MSK
#define KEY_PAIRING_REJECT DK_BTN2_MSK

/* BLE Central state */
static struct bt_conn *default_conn;
static struct k_work scan_work;

/* Callback functions for external modules */
static ble_central_connected_cb_t connected_cb;
static ble_central_disconnected_cb_t disconnected_cb;
static ble_central_security_changed_cb_t security_changed_cb;

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

/* Scan callbacks structure */
BT_SCAN_CB_INIT(scan_cb, scan_filter_match, NULL,
		scan_connecting_error, scan_connecting);

/* Authentication callbacks - will be defined after function implementations */
static struct bt_conn_auth_cb conn_auth_callbacks;
static struct bt_conn_auth_info_cb conn_auth_info_callbacks;

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

	/* Call external security changed callback if registered */
	if (security_changed_cb) {
		security_changed_cb(conn, level, err);
	}
}

/* Scan callback implementations */
static void scan_filter_match(struct bt_scan_device_info *device_info,
			      struct bt_scan_filter_match *filter_match,
			      bool connectable)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(device_info->recv_info->addr, addr, sizeof(addr));

	printk("*** DEVICE FOUND WITH NUS: %s connectable: %d ***\n", addr, connectable);
	LOG_INF("Filters matched. Address: %s connectable: %d",
		addr, connectable);
	
	// Log additional device info for debugging
	LOG_INF("RSSI: %d", device_info->recv_info->rssi);
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
static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey);
static void auth_passkey_confirm(struct bt_conn *conn, unsigned int passkey);
static void auth_cancel(struct bt_conn *conn);

static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Passkey for %s: %06u\n", addr, passkey);
}

static void auth_passkey_confirm(struct bt_conn *conn, unsigned int passkey)
{
	char addr[BT_ADDR_LE_STR_LEN];

	ble_central_set_auth_conn(bt_conn_ref(conn));

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Passkey for %s: %06u\n", addr, passkey);
	printk("Press Button 1 to confirm, Button 2 to reject.\n");
}

static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Pairing cancelled: %s", addr);
}

/* Initialize authentication callback structures */
static void init_auth_callbacks(void)
{
	conn_auth_callbacks.passkey_display = auth_passkey_display;
	conn_auth_callbacks.passkey_confirm = auth_passkey_confirm;
	conn_auth_callbacks.cancel = auth_cancel;
	
	conn_auth_info_callbacks.pairing_complete = pairing_complete;
	conn_auth_info_callbacks.pairing_failed = pairing_failed;
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

/* Authentication handling functions */
static struct bt_conn *auth_conn = NULL;

void ble_central_num_comp_reply(bool accept)
{
	if (accept) {
		bt_conn_auth_passkey_confirm(auth_conn);
		printk("Numeric Match, conn %p\n", auth_conn);
	} else {
		bt_conn_auth_cancel(auth_conn);
		printk("Numeric Reject, conn %p\n", auth_conn);
	}

	bt_conn_unref(auth_conn);
	auth_conn = NULL;
}

struct bt_conn *ble_central_get_auth_conn(void)
{
	return auth_conn;
}

void ble_central_set_auth_conn(struct bt_conn *conn)
{
	auth_conn = conn;
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

	/* Authentication callbacks will be registered by ble_central_init_auth_callbacks() */

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

	err = bt_scan_filter_add(BT_SCAN_FILTER_TYPE_UUID, BT_UUID_NUS_SERVICE);
	if (err) {
		LOG_ERR("UUID filter cannot be added (err %d", err);
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

void ble_central_register_security_changed_cb(ble_central_security_changed_cb_t cb)
{
	security_changed_cb = cb;
}

struct k_work *ble_central_get_scan_work(void)
{
	return &scan_work;
}

/* Button handling for authentication */
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

/* Additional initialization functions like HID firmware */
int ble_central_init_callbacks(void)
{
	LOG_INF("BLE Central callbacks initialized");
	return 0;
}

int ble_central_init_auth_callbacks(void)
{
	int err;

	err = bt_conn_auth_cb_register(&conn_auth_callbacks);
	if (err) {
		LOG_ERR("Failed to register authorization callbacks (err %d)", err);
		return err;
	}

	err = bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
	if (err) {
		LOG_ERR("Failed to register authorization info callbacks (err %d)", err);
		return err;
	}

	LOG_INF("BLE Central auth callbacks initialized");
	return 0;
} 