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

	LOG_INF("BLE Transport sending %d bytes to HID", len);
	int err = ble_hid_send_report(data, len);
	if (err) {
		LOG_ERR("BLE Transport HID send failed: %d", err);
	} else {
		LOG_INF("BLE Transport HID send successful");
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
	LOG_INF("=== BLE HID DATA RECEIVED ===");
	LOG_INF("HID data received: %d bytes", len);
	LOG_INF("HID discovery status: ready=%d, complete=%d", hid_client_ready, hid_discovery_complete);
	
	// Only process data after HID discovery is complete
	if (!hid_discovery_complete) {
		LOG_DBG("Skipping HID data - HID discovery not complete");
		return;
	}
	
	// Debug: Log the first few bytes to see what we're getting
	if (len > 0) {
		LOG_INF("HID First bytes: %02x %02x %02x %02x", 
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
	
	// Bridge HID data directly to USB HID
	if (hid_data_callback) {
		LOG_INF("Calling USB HID callback with %d bytes", len);
		hid_data_callback(data, len);
	} else {
		LOG_WRN("No USB HID callback registered!");
	}
	
	// Also try to send directly to USB HID device
	extern int usb_hid_send_report(const uint8_t *data, uint16_t len);
	int ret = usb_hid_send_report(data, len);
	if (ret != 0) {
		LOG_ERR("Failed to send HID report to USB device (err %d)", ret);
	} else {
		LOG_INF("HID report sent to USB device successfully");
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
	ARG_UNUSED(reason);
	
	// Reset ready states for both NUS and HID
	nus_client_ready = false;
	hid_client_ready = false;
	hid_discovery_complete = false;
	mtu_exchange_complete = false;
	
	LOG_INF("BLE Central disconnected - resetting both NUS and HID bridge states");
}
