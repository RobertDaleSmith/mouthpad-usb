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
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>

#include "ble_hid.h"
#include "ble_central.h"
#include "ble_transport.h"

/* Forward declarations for direct USB access */
extern const struct device *hid_dev;
extern struct k_sem ep_write_sem;

LOG_MODULE_REGISTER(ble_transport, LOG_LEVEL_INF);

/* USB HID callback */
static usb_hid_send_cb_t usb_hid_send_callback = NULL;

/* HID Bridge state */
static bool hid_client_ready = false;
static bool bridging_started = false;

/* Future NUS Bridge state */
static ble_data_callback_t nus_data_callback = NULL;
static ble_ready_callback_t nus_ready_callback = NULL;
static bool nus_client_ready = false;

/* Internal callback functions */
static void ble_hid_data_received_cb(const uint8_t *data, uint16_t len);
static void ble_hid_ready_cb(void);
static void ble_central_connected_cb(struct bt_conn *conn);
static void ble_central_disconnected_cb(struct bt_conn *conn, uint8_t reason);
static void button_handler(uint32_t button_state, uint32_t has_changed);

/* BLE Transport initialization */
int ble_transport_init(void)
{
	int err;

	LOG_INF("Initializing BLE Transport...");

	/* Register BLE Central callbacks */
	ble_central_register_connected_cb(ble_central_connected_cb);
	ble_central_register_disconnected_cb(ble_central_disconnected_cb);

	/* Initialize BLE Central */
	err = ble_central_init();
	if (err != 0) {
		LOG_ERR("ble_central_init failed (err %d)", err);
		return err;
	}

	/* Register HID Client callbacks */
	ble_hid_register_data_received_cb(ble_hid_data_received_cb);
	ble_hid_register_ready_cb(ble_hid_ready_cb);

	/* Initialize HID client */
	err = ble_hid_init();
	if (err != 0) {
		LOG_ERR("ble_hid_init failed (err %d)", err);
		return err;
	}

	/* Initialize BLE Central callbacks */
	err = ble_central_init_callbacks();
	if (err != 0) {
		LOG_ERR("ble_central_init_callbacks failed (err %d)", err);
		return err;
	}

	/* Initialize BLE Central authentication callbacks */
	err = ble_central_init_auth_callbacks();
	if (err != 0) {
		LOG_ERR("ble_central_init_auth_callbacks failed (err %d)", err);
		return err;
	}

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return err;
	}

	LOG_INF("Bluetooth initialized");

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}

	err = dk_buttons_init(button_handler);
	if (err) {
		LOG_ERR("Failed to initialize buttons (err %d)", err);
		return err;
	}

	/* Start scanning */
	err = ble_central_start_scan();
	if (err) {
		LOG_ERR("Scan start failed (err %d)", err);
		return err;
	}

	LOG_INF("BLE Transport initialized successfully");
	return 0;
}

/* Transport registration functions */
int ble_transport_register_usb_hid_callback(usb_hid_send_cb_t cb)
{
	usb_hid_send_callback = cb;
	return 0;
}

int ble_transport_start_bridging(void)
{
	bridging_started = true;
	LOG_INF("BLE Transport bridging started");
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
		LOG_ERR("BLE Transport send failed: %d", err);
	} else {
		LOG_INF("BLE Transport send successful");
	}
	return err;
}

bool ble_transport_is_hid_ready(void)
{
	return hid_client_ready;
}

/* Future NUS Transport functions */
int ble_transport_register_nus_data_callback(ble_data_callback_t cb)
{
	nus_data_callback = cb;
	return 0;
}

int ble_transport_register_nus_ready_callback(ble_ready_callback_t cb)
{
	nus_ready_callback = cb;
	return 0;
}

int ble_transport_send_nus_data(const uint8_t *data, uint16_t len)
{
	if (!nus_client_ready) {
		LOG_WRN("NUS client not ready");
		return -ENOTCONN;
	}

	// TODO: Implement NUS client send when NUS bridge is added
	return -ENOSYS;
}

bool ble_transport_is_nus_ready(void)
{
	return nus_client_ready;
}

/* Internal callback functions */
static void ble_hid_data_received_cb(const uint8_t *data, uint16_t len)
{
	LOG_INF("HID data received: %d bytes", len);

	// Bridge HID data directly to USB HID
	if (usb_hid_send_callback) {
		usb_hid_send_callback(data, len);
	}
}

static void ble_hid_ready_cb(void)
{
	LOG_INF("HID client ready - service discovery complete");
	hid_client_ready = true;
	LOG_INF("HID client ready - bridge operational");
}

static void ble_central_connected_cb(struct bt_conn *conn)
{
	LOG_INF("BLE Central connected - starting setup");

	int err = bt_conn_set_security(conn, BT_SECURITY_L2);
	if (err) {
		LOG_WRN("Failed to set security: %d", err);
		ble_central_gatt_discover(conn);
	} else {
		LOG_INF("Security setup successful");
		ble_central_gatt_discover(conn);
	}
}

static void ble_central_disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(reason);

	// Reset ready states
	hid_client_ready = false;
	nus_client_ready = false;

	LOG_INF("BLE Central disconnected - resetting bridge states");
}

/* Button handler for transport layer */
static void button_handler(uint32_t button_state, uint32_t has_changed)
{
	/* Handle BLE Central button events (pairing) */
	ble_central_handle_buttons(button_state, has_changed);

	/* Handle HID-related button events */
	ble_hid_handle_buttons(button_state, has_changed);
}
