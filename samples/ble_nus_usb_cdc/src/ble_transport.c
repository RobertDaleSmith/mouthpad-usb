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
#include "usb_cdc.h"

#define LOG_MODULE_NAME ble_transport
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

/* USB CDC callback */
static usb_cdc_send_cb_t usb_cdc_send_callback = NULL;

/* NUS Bridge state */
static bool nus_client_ready = false;
static bool mtu_exchange_complete = false;
static bool bridging_started = false;
static bool security_established = false;
static struct k_timer discovery_timeout_timer;

/* Future HID Bridge callbacks and state */
static ble_data_callback_t hid_data_callback = NULL;
static ble_ready_callback_t hid_ready_callback = NULL;
static bool hid_client_ready = false;

/* Internal callback functions */
static void ble_nus_data_received_cb(const uint8_t *data, uint16_t len);
static void ble_nus_discovery_complete_cb(void);
static void ble_nus_mtu_exchange_cb(uint16_t mtu);
static void ble_central_connected_cb(struct bt_conn *conn);
static void ble_central_disconnected_cb(struct bt_conn *conn, uint8_t reason);
static void ble_central_security_changed_cb(struct bt_conn *conn, bt_security_t level, enum bt_security_err err);
static void gatt_discover(struct bt_conn *conn);
static void discovery_timeout_handler(struct k_timer *timer);

/* BLE Transport initialization */
int ble_transport_init(void)
{
	int err;

	/* Register BLE Central callbacks */
	ble_central_register_connected_cb(ble_central_connected_cb);
	ble_central_register_disconnected_cb(ble_central_disconnected_cb);
	ble_central_register_security_changed_cb(ble_central_security_changed_cb);

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
	
	/* Register USB CDC callback */
	ble_transport_register_usb_cdc_callback((usb_cdc_send_cb_t)usb_cdc_send_data);

	/* Initialize NUS client */
	err = ble_nus_client_init();
	if (err != 0) {
		LOG_ERR("ble_nus_client_init failed (err %d)", err);
		return err;
	}

	/* Initialize BLE Central callbacks (like HID firmware) */
	err = ble_central_init_callbacks();
	if (err != 0) {
		LOG_ERR("ble_central_init_callbacks failed (err %d)", err);
		return err;
	}

	/* Initialize BLE Central authentication callbacks (like HID firmware) */
	err = ble_central_init_auth_callbacks();
	if (err != 0) {
		LOG_ERR("ble_central_init_auth_callbacks failed (err %d)", err);
		return err;
	}

	/* Initialize discovery timeout timer */
	k_timer_init(&discovery_timeout_timer, discovery_timeout_handler, NULL);

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

	// Check if the NUS client instance is properly initialized
	struct bt_nus_client *nus = ble_nus_client_get_instance();
	if (!nus) {
		LOG_ERR("NUS client instance not available");
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

	// TODO: Implement HID client send when HID bridge is added
	return -ENOSYS;
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

static void ble_central_security_changed_cb(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	if (!err) {
		LOG_INF("Security established successfully (level %u)", level);
		security_established = true;
	} else {
		LOG_WRN("Security failed (level %u, err %d)", level, err);
		security_established = false;
		
		// Log what the security error means
		switch (err) {
		case BT_SECURITY_ERR_AUTH_FAIL:
			LOG_WRN("Authentication failed - device may require pairing");
			break;
		case BT_SECURITY_ERR_PIN_OR_KEY_MISSING:
			LOG_WRN("PIN or key missing - device may require bonding");
			break;
		case BT_SECURITY_ERR_OOB_NOT_AVAILABLE:
			LOG_WRN("OOB not available");
			break;
		case BT_SECURITY_ERR_PAIR_NOT_SUPPORTED:
			LOG_WRN("Pairing not supported by device");
			break;
		default:
			LOG_WRN("Unknown security error: %d", err);
			break;
		}
		
		// For error 4 (BT_SECURITY_ERR_AUTH_FAIL), try to continue anyway
		if (err == BT_SECURITY_ERR_AUTH_FAIL) {
			LOG_INF("Continuing with discovery despite authentication failure");
		}
	}
}

static void ble_nus_discovery_complete_cb(void)
{
	LOG_INF("NUS client ready - service discovery complete");
	
	// Stop the discovery timeout timer
	k_timer_stop(&discovery_timeout_timer);
	
	if (!security_established) {
		LOG_WRN("NUS discovery completed but security not established - bridge may not work properly");
	}
	
	nus_client_ready = true;
	
	// Attempt subscription after a short delay to ensure connection is stable
	k_sleep(K_MSEC(500));
	
	int err = ble_nus_client_subscribe();
	if (err) {
		LOG_WRN("NUS subscription failed (err %d) - bridge may not receive data", err);
	} else {
		LOG_INF("NUS subscription successful");
	}
	
	LOG_INF("NUS client ready - bridge operational");
}

static void gatt_discover(struct bt_conn *conn)
{
	if (conn != ble_central_get_default_conn()) {
		return;
	}

	LOG_INF("Starting GATT discovery with 10 second timeout");
	
	// Start discovery timeout timer
	k_timer_start(&discovery_timeout_timer, K_SECONDS(10), K_NO_WAIT);
	
	ble_nus_client_discover(conn);
}

static void discovery_timeout_handler(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	
	LOG_ERR("GATT discovery timeout - device may not have proper NUS service");
	
	// Reset ready states since discovery failed
	nus_client_ready = false;
	mtu_exchange_complete = false;
	security_established = false;
	
	// Disconnect to try again
	struct bt_conn *conn = ble_central_get_default_conn();
	if (conn) {
		LOG_INF("Disconnecting due to discovery timeout");
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
}

static void ble_central_connected_cb(struct bt_conn *conn)
{
	int err;

	LOG_INF("BLE Central connected - starting setup");

	// Reset security state
	security_established = false;

	// Perform MTU exchange using the NUS client module
	err = ble_nus_client_exchange_mtu(conn);
	if (err) {
		LOG_WRN("MTU exchange failed (err %d)", err);
	} else {
		LOG_INF("MTU exchange initiated successfully");
	}

	err = bt_conn_set_security(conn, BT_SECURITY_L3);
	if (err) {
		LOG_WRN("Failed to set security: %d", err);
		// Try discovery anyway, but log the security issue
		gatt_discover(conn);
	} else {
		LOG_INF("Security setup successful");
		security_established = true;
		gatt_discover(conn);
	}
}

static void ble_central_disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(reason);
	
	// Stop discovery timeout timer
	k_timer_stop(&discovery_timeout_timer);
	
	// Reset ready states
	nus_client_ready = false;
	hid_client_ready = false;
	mtu_exchange_complete = false;
	security_established = false;
	
	LOG_INF("BLE Central disconnected - resetting bridge states");
} 