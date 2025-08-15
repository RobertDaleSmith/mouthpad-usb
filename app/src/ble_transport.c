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
#include <string.h>

#include "ble_transport.h"
#include "ble_central.h"
#include "ble_nus_client.h"
#include "ble_hid.h"
#include "usb_cdc.h"
#include "usb_hid.h"
#include <bluetooth/services/bas_client.h>

#define LOG_MODULE_NAME ble_transport
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

/* USB CDC callback */
static usb_cdc_send_cb_t usb_cdc_send_callback = NULL;

/* NUS Bridge state */
static bool nus_client_ready = false;
static bool mtu_exchange_complete = false;
static bool bridging_started = false;

/* HID Bridge state */
static bool hid_client_ready = false;
static bool hid_discovery_complete = false;

/* Battery Service state */
static struct bt_bas_client bas;

/* Data activity tracking for LED indication */
static bool data_activity = false;
static int64_t last_data_time = 0;

/* HID Bridge callbacks */
static ble_data_callback_t hid_data_callback = NULL;
static ble_ready_callback_t hid_ready_callback = NULL;

/* Internal callback functions */
static void ble_nus_data_received_cb(const uint8_t *data, uint16_t len);
static void ble_nus_discovery_complete_cb(void);
static void ble_nus_mtu_exchange_cb(uint16_t mtu);
static void ble_hid_data_received_cb(const uint8_t *data, uint16_t len);
static void ble_hid_discovery_complete_cb(void);
static void ble_central_connected_cb(struct bt_conn *conn);
static void ble_central_disconnected_cb(struct bt_conn *conn, uint8_t reason);
static void gatt_discover(struct bt_conn *conn);

/* Battery Service callbacks following Nordic pattern */
static void battery_notify_cb(struct bt_bas_client *bas, uint8_t battery_level);
static void battery_discovery_completed_cb(struct bt_gatt_dm *dm, void *context);
static void battery_discovery_service_not_found_cb(struct bt_conn *conn, void *context);
static void battery_discovery_error_found_cb(struct bt_conn *conn, int err, void *context);
static void battery_gatt_discover(struct bt_conn *conn);

static bool nus_discovery_complete = false;

static void nus_discovery_completed_cb(void)
{
	LOG_INF("=== NUS DISCOVERY COMPLETED ===");
	nus_discovery_complete = true;
	
	/* Now start HID discovery after NUS is complete */
	LOG_INF("Starting HID service discovery after NUS completion...");
	int hid_discover_ret = ble_hid_discover(ble_central_get_default_conn());
	if (hid_discover_ret != 0) {
		LOG_ERR("BLE HID discovery failed (err %d)", hid_discover_ret);
		LOG_ERR("This might mean the connected device doesn't have HID services");
		LOG_ERR("Or there might be a GATT discovery conflict");
	} else {
		LOG_INF("BLE HID discovery started successfully");
	}
}

/* BLE Transport initialization */
int ble_transport_init(void)
{
	int err;

	/* Register BLE Central callbacks */
	ble_central_register_connected_cb(ble_central_connected_cb);
	ble_central_register_disconnected_cb(ble_central_disconnected_cb);

	/* Initialize BLE Central */
	err = ble_central_init();
	if (err != 0) {
		LOG_ERR("ble_central_init failed (err %d)", err);
		return err;
	}

	/* Register NUS Client callbacks */
	ble_nus_client_register_data_received_cb(ble_nus_data_received_cb);
	ble_nus_client_register_discovery_complete_cb(ble_nus_discovery_complete_cb);
	ble_nus_client_register_mtu_exchange_cb(ble_nus_mtu_exchange_cb);
	
	/* Register HID Client callbacks */
	LOG_INF("Registering BLE HID callbacks...");
	ble_hid_register_data_received_cb(ble_hid_data_received_cb);
	ble_hid_register_ready_cb(ble_hid_discovery_complete_cb);
	LOG_INF("BLE HID callbacks registered successfully");
	
	/* Register USB CDC callback */
	ble_transport_register_usb_cdc_callback((usb_cdc_send_cb_t)usb_cdc_send_data);

	/* Initialize NUS client */
	err = ble_nus_client_init();
	if (err != 0) {
		LOG_ERR("ble_nus_client_init failed (err %d)", err);
		return err;
	}
	LOG_INF("BLE NUS client initialized successfully");

	/* Initialize Battery Service client following Nordic pattern */
	bt_bas_client_init(&bas);
	LOG_INF("Battery Service client initialized successfully");

	/* Initialize HID client */
	err = ble_hid_init();
	if (err != 0) {
		LOG_ERR("ble_hid_init failed (err %d)", err);
		return err;
	}
	LOG_INF("BLE HID client initialized successfully");

	/* Start scanning */
	err = ble_central_start_scan();
	if (err) {
		LOG_ERR("Scan start failed (err %d)", err);
		return err;
	}

	return 0;
}

/* Transport registration functions */
int ble_transport_register_usb_cdc_callback(usb_cdc_send_cb_t cb)
{
	usb_cdc_send_callback = cb;
	return 0;
}

int ble_transport_register_usb_hid_callback(ble_data_callback_t cb)
{
	hid_data_callback = cb;
	return 0;
}

int ble_transport_start_bridging(void)
{
	bridging_started = true;
	LOG_INF("BLE Transport bridging started");
	return 0;
}

int ble_transport_send_nus_data(const uint8_t *data, uint16_t len)
{
	if (!nus_client_ready) {
		LOG_WRN("NUS client not ready");
		return -ENOTCONN;
	}

	LOG_INF("BLE Transport sending %d bytes to NUS", len);
	int err = ble_nus_client_send_data(data, len);
	if (err) {
		LOG_ERR("BLE Transport send failed: %d", err);
	} else {
		LOG_INF("BLE Transport send successful");
		data_activity = true;  // Mark data activity for LED indication
	}
	return err;
}

bool ble_transport_is_nus_ready(void)
{
	return nus_client_ready;
}

/* Future HID Transport functions */
int ble_transport_register_hid_data_callback(ble_data_callback_t cb)
{
	hid_data_callback = cb;
	return 0;
}

int ble_transport_register_hid_ready_callback(ble_ready_callback_t cb)
{
	hid_ready_callback = cb;
	return 0;
}

int ble_transport_send_hid_data(const uint8_t *data, uint16_t len)
{
	if (!hid_client_ready) {
		LOG_WRN("HID client not ready");
		return -ENOTCONN;
	}

	LOG_DBG("BLE Transport sending %d bytes to HID", len);
	int err = ble_hid_send_report(data, len);
	if (err) {
		LOG_ERR("BLE Transport HID send failed: %d", err);
	} else {
		LOG_DBG("BLE Transport HID send successful");
		data_activity = true;  // Mark data activity for LED indication
	}
	return err;
}

bool ble_transport_is_hid_ready(void)
{
	return hid_client_ready;
}

/* Internal callback functions */
static void ble_nus_data_received_cb(const uint8_t *data, uint16_t len)
{
	LOG_INF("NUS data received: %d bytes", len);
	
	// Only process data after MTU exchange is complete
	if (!mtu_exchange_complete) {
		LOG_DBG("Skipping data - MTU exchange not complete");
		return;
	}
	
	// Debug: Log the first few bytes to see what we're getting
	if (len > 0) {
		LOG_INF("First bytes: %02x %02x %02x %02x", 
			data[0], len > 1 ? data[1] : 0, len > 2 ? data[2] : 0, len > 3 ? data[3] : 0);
	}
	
	// Filter out 2-byte echo responses (73 XX format)
	if (len == 2 && data[0] == 0x73) {
		LOG_DBG("Skipping 2-byte echo: 73 %02x", data[1]);
		return;
	}
	
	// Don't echo back single characters (likely echo from our input)
	if (len == 1) {
		LOG_DBG("Skipping single character echo");
		return;
	}
	
	// For larger packets, try to identify the structure
	if (len >= 4) {
		LOG_INF("PACKET STRUCTURE: Type=0x%02x, Length=%d", data[0], len);
	}
	
	// Mark data activity for LED indication
	data_activity = true;
	last_data_time = k_uptime_get();
	LOG_DBG("=== DATA ACTIVITY MARKED ===");
	
	// Bridge NUS data directly to USB CDC
	if (usb_cdc_send_callback) {
		usb_cdc_send_callback(data, len);
	}
}

static void ble_nus_mtu_exchange_cb(uint16_t mtu)
{
	LOG_INF("MTU exchange completed: %d bytes", mtu);
	mtu_exchange_complete = true;
}

static void ble_nus_discovery_complete_cb(void)
{
	LOG_INF("NUS client ready - service discovery complete");
	nus_client_ready = true;
	LOG_INF("NUS client ready - bridge operational");
	
	/* Trigger HID discovery after NUS discovery completes */
	nus_discovery_completed_cb();
}

static void ble_hid_data_received_cb(const uint8_t *data, uint16_t len)
{
	LOG_DBG("=== BLE HID DATA RECEIVED ===");
	LOG_DBG("HID data received: %d bytes", len);
	LOG_DBG("HID discovery status: ready=%d, complete=%d", hid_client_ready, hid_discovery_complete);
	
	// Only process data after HID discovery is complete
	if (!hid_discovery_complete) {
		LOG_DBG("Skipping HID data - HID discovery not complete");
		return;
	}
	
	// Debug: Log the first few bytes to see what we're getting
	if (len > 0) {
		LOG_DBG("HID First bytes: %02x %02x %02x %02x", 
			data[0], len > 1 ? data[1] : 0, len > 2 ? data[2] : 0, len > 3 ? data[3] : 0);
	}
	
	// Filter out 2-byte echo responses (73 XX format)
	if (len == 2 && data[0] == 0x73) {
		LOG_DBG("Skipping 2-byte echo: 73 %02x", data[1]);
		return;
	}
	
	// Don't echo back single characters (likely echo from our input)
	if (len == 1) {
		LOG_DBG("Skipping single character echo");
		return;
	}
	
	// For larger packets, try to identify the structure
	if (len >= 4) {
		LOG_INF("HID PACKET STRUCTURE: Type=0x%02x, Length=%d", data[0], len);
	}
	
	// Mark data activity for LED indication
	data_activity = true;
	last_data_time = k_uptime_get();
	LOG_DBG("=== DATA ACTIVITY MARKED ===");
	
	// Bridge HID data directly to USB HID
	// Note: HID data is already sent directly to USB in ble_hid.c for zero latency
	// No need to duplicate the USB sending here to avoid semaphore conflicts
	if (hid_data_callback) {
		LOG_DBG("Calling USB HID callback with %d bytes", len);
		hid_data_callback(data, len);
	} else {
		LOG_DBG("No USB HID callback registered (normal - direct USB sending used)");
	}
}

static void ble_hid_discovery_complete_cb(void)
{
	LOG_INF("=== BLE HID DISCOVERY COMPLETE ===");
	LOG_INF("HID client ready - service discovery complete");
	hid_client_ready = true;
	hid_discovery_complete = true;
	LOG_INF("HID client ready - bridge operational");
	LOG_INF("BLE HID discovery status: ready=%d, complete=%d", hid_client_ready, hid_discovery_complete);
	
	/* Start Battery Service discovery after HID is complete */
	battery_gatt_discover(ble_central_get_default_conn());
}

static void gatt_discover(struct bt_conn *conn)
{
	if (conn != ble_central_get_default_conn()) {
		return;
	}

	LOG_INF("Starting GATT discovery for both NUS and HID services");
	LOG_INF("Connected device address: %s", bt_addr_le_str(bt_conn_get_dst(conn)));

	/* Reset discovery state */
	nus_discovery_complete = false;

	/* Discover NUS service first */
	LOG_INF("Starting NUS service discovery...");
	ble_nus_client_discover(conn);
	
	/* HID discovery will be started in nus_discovery_completed_cb */
}

static void ble_central_connected_cb(struct bt_conn *conn)
{
	int err;

	LOG_INF("BLE Central connected - starting setup");

	// Perform MTU exchange using the NUS client module
	err = ble_nus_client_exchange_mtu(conn);
	if (err) {
		LOG_WRN("MTU exchange failed (err %d)", err);
	} else {
		LOG_INF("MTU exchange initiated successfully");
	}

	err = bt_conn_set_security(conn, BT_SECURITY_L2);
	if (err) {
		LOG_WRN("Failed to set security: %d", err);
		gatt_discover(conn);
	} else {
		LOG_INF("Security setup successful");
		gatt_discover(conn);
	}
}

static void ble_central_disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	
	LOG_INF("BLE Central disconnected (reason: 0x%02x) - cleaning up and resetting states", reason);
	
	// Reset ready states for both NUS and HID
	nus_client_ready = false;
	hid_client_ready = false;
	hid_discovery_complete = false;
	mtu_exchange_complete = false;
	nus_discovery_complete = false;
	
	// Release HOGP if active - like Nordic sample does
	extern struct bt_hogp *ble_hid_get_hogp(void);
	extern bool bt_hogp_assign_check(const struct bt_hogp *hogp);
	extern void bt_hogp_release(struct bt_hogp *hogp);
	
	struct bt_hogp *hogp = ble_hid_get_hogp();
	if (bt_hogp_assign_check(hogp)) {
		printk("HIDS client active - releasing");
		bt_hogp_release(hogp);
	}
	
	LOG_INF("BLE Central disconnected - cleanup complete, ready for new connection");
}

bool ble_transport_is_connected(void)
{
	return nus_client_ready || hid_client_ready;
}

bool ble_transport_has_data_activity(void)
{
	// Check if we've had data activity within the last 100ms
	int64_t current_time = k_uptime_get();
	if (data_activity && (current_time - last_data_time) < 100) {
		LOG_DBG("BLE data activity detected (time diff: %lld ms)", current_time - last_data_time);
		return true;
	}
	
	// Reset if it's been too long
	if ((current_time - last_data_time) >= 100) {
		data_activity = false;
	}
	
	return false;
}

void ble_transport_mark_data_activity(void)
{
	data_activity = true;
	last_data_time = k_uptime_get();
	LOG_DBG("=== DATA ACTIVITY MARKED (DIRECT) ===");
}

/* Battery Service implementation following Nordic central_bas pattern */
static void battery_notify_cb(struct bt_bas_client *bas, uint8_t battery_level)
{
	if (battery_level == BT_BAS_VAL_INVALID) {
		LOG_WRN("Battery notification aborted");
	} else {
		LOG_INF("=== BATTERY LEVEL: %d%% ===", battery_level);
	}
}

static struct bt_gatt_dm_cb battery_discovery_cb = {
	.completed = battery_discovery_completed_cb,
	.service_not_found = battery_discovery_service_not_found_cb,
	.error_found = battery_discovery_error_found_cb,
};

static void battery_discovery_completed_cb(struct bt_gatt_dm *dm, void *context)
{
	int err;

	LOG_INF("Battery Service discovery completed");

	bt_gatt_dm_data_print(dm);

	err = bt_bas_handles_assign(dm, &bas);
	if (err) {
		LOG_ERR("Could not assign BAS handles: %d", err);
	}

	if (bt_bas_notify_supported(&bas)) {
		LOG_INF("Battery notifications supported - subscribing");
		err = bt_bas_subscribe_battery_level(&bas, battery_notify_cb);
		if (err) {
			LOG_WRN("Cannot subscribe to battery notifications (err: %d)", err);
		}
	} else {
		LOG_INF("Battery notifications not supported - using periodic reads");
		err = bt_bas_start_per_read_battery_level(&bas, 10000, battery_notify_cb);
		if (err) {
			LOG_WRN("Could not start periodic battery reads: %d", err);
		}
	}

	err = bt_gatt_dm_data_release(dm);
	if (err) {
		LOG_ERR("Could not release battery discovery data: %d", err);
	}
}

static void battery_discovery_service_not_found_cb(struct bt_conn *conn, void *context)
{
	LOG_INF("Battery Service not found during discovery");
}

static void battery_discovery_error_found_cb(struct bt_conn *conn, int err, void *context)
{
	LOG_ERR("Battery Service discovery failed: %d", err);
}

static void battery_gatt_discover(struct bt_conn *conn)
{
	int err;

	if (!conn || conn != ble_central_get_default_conn()) {
		return;
	}

	LOG_INF("Starting Battery Service discovery");

	err = bt_gatt_dm_start(conn, BT_UUID_BAS, &battery_discovery_cb, NULL);
	if (err) {
		LOG_ERR("Could not start battery discovery: %d", err);
	}
}
