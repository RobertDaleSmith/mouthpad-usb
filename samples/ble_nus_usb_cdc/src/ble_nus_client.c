/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ble_nus_client.h"
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <bluetooth/services/nus_client.h>
#include <bluetooth/services/nus.h>
#include <bluetooth/gatt_dm.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

LOG_MODULE_REGISTER(ble_nus_client, LOG_LEVEL_INF);

/* NUS Client instance */
static struct bt_nus_client nus_client;

/* Callback functions for external modules */
static ble_nus_data_received_cb_t data_received_cb;
static ble_nus_data_sent_cb_t data_sent_cb;
static ble_nus_mtu_exchange_cb_t mtu_exchange_cb;
static ble_nus_discovery_complete_cb_t discovery_complete_cb;

/* Semaphore for NUS write operations */
K_SEM_DEFINE(nus_write_sem, 0, 1);

/* NUS Client callbacks */
static uint8_t nus_data_received(struct bt_nus_client *nus, const uint8_t *data, uint16_t len);
static void nus_data_sent(struct bt_nus_client *nus, uint8_t err, const uint8_t *const data, uint16_t len);

/* Discovery callbacks */
static void discovery_complete(struct bt_gatt_dm *dm, void *context);
static void discovery_service_not_found(struct bt_conn *conn, void *context);
static void discovery_error(struct bt_conn *conn, int err, void *context);

/* MTU exchange callback */
static void exchange_func(struct bt_conn *conn, uint8_t err, struct bt_gatt_exchange_params *params);

/* Discovery callback structure */
struct bt_gatt_dm_cb discovery_cb = {
	.completed = discovery_complete,
	.service_not_found = discovery_service_not_found,
	.error_found = discovery_error,
};

/* NUS Client callback implementations */
static uint8_t nus_data_received(struct bt_nus_client *nus, const uint8_t *data, uint16_t len)
{
	ARG_UNUSED(nus);

	// Call external data received callback if registered
	if (data_received_cb) {
		data_received_cb(data, len);
	}

	return BT_GATT_ITER_CONTINUE;
}

static void nus_data_sent(struct bt_nus_client *nus, uint8_t err, const uint8_t *const data, uint16_t len)
{
	ARG_UNUSED(nus);
	ARG_UNUSED(data);
	ARG_UNUSED(len);

	k_sem_give(&nus_write_sem);

	if (err) {
		LOG_WRN("ATT error code: 0x%02X", err);
	}

	// Call external data sent callback if registered
	if (data_sent_cb) {
		data_sent_cb(err);
	}
}

/* Discovery callback implementations */
static void discovery_complete(struct bt_gatt_dm *dm, void *context)
{
	struct bt_nus_client *nus = context;
	int err;
	
	LOG_INF("Service discovery completed");

	// Print detailed discovery data to see what's actually available
	LOG_INF("=== GATT DISCOVERY DATA ===");
	bt_gatt_dm_data_print(dm);
	LOG_INF("=== END GATT DISCOVERY DATA ===");

	// Check if we found any services at all
	const struct bt_gatt_dm_attr *service_attr = bt_gatt_dm_service_get(dm);
	if (!service_attr) {
		LOG_ERR("No services found in discovery data - device may not have any services");
		bt_gatt_dm_data_release(dm);
		return;
	}

	LOG_INF("Found at least one service, attempting NUS handle assignment");

	err = bt_nus_handles_assign(dm, nus);
	if (err) {
		LOG_ERR("Failed to assign NUS handles (err %d) - device may not have NUS TX/RX characteristics", err);
		LOG_ERR("This suggests the remote device doesn't have a proper NUS service implementation");
		LOG_ERR("The device might be advertising NUS UUID but not actually implementing the service");
		bt_gatt_dm_data_release(dm);
		return;
	}
	
	LOG_INF("NUS handles assigned successfully");

	// Don't attempt subscription immediately - follow HID pattern
	// Subscription will be handled later when needed

	err = bt_gatt_dm_data_release(dm);
	if (err) {
		LOG_ERR("Could not release the discovery data, error code: %d", err);
	}

	// Call external discovery complete callback if registered
	if (discovery_complete_cb) {
		discovery_complete_cb();
	}
}

static void discovery_service_not_found(struct bt_conn *conn, void *context)
{
	LOG_INF("Service not found");
}

static void discovery_error(struct bt_conn *conn, int err, void *context)
{
	LOG_WRN("Error while discovering GATT database: (%d)", err);
}

/* MTU exchange callback implementation */
static void exchange_func(struct bt_conn *conn, uint8_t err, struct bt_gatt_exchange_params *params)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(params);

	if (!err) {
		uint16_t mtu = bt_gatt_get_mtu(conn);
		printk("*** MTU EXCHANGE SUCCESSFUL: MTU = %d bytes ***\n", mtu);
		LOG_INF("MTU exchange done, MTU: %d", mtu);

		// Call external MTU exchange callback if registered
		if (mtu_exchange_cb) {
			mtu_exchange_cb(mtu);
		}
	} else {
		printk("*** MTU EXCHANGE FAILED: error = %d ***\n", err);
		LOG_WRN("MTU exchange failed (err %" PRIu8 ")", err);
	}
}

/* Public API implementations */
int ble_nus_client_init(void)
{
	int err;
	struct bt_nus_client_init_param init = {
		.cb = {
			.received = nus_data_received,
			.sent = nus_data_sent,
		}
	};

	err = bt_nus_client_init(&nus_client, &init);
	if (err) {
		LOG_ERR("NUS Client initialization failed (err %d)", err);
		return err;
	}

	LOG_INF("NUS Client module initialized");
	return err;
}

int ble_nus_client_send_data(const uint8_t *data, uint16_t len)
{
	if (!data || len == 0) {
		return -EINVAL;
	}

	return bt_nus_client_send(&nus_client, data, len);
}

int ble_nus_client_subscribe(void)
{
	int err;
	
	LOG_INF("Attempting to subscribe to NUS receive");
	
	err = bt_nus_subscribe_receive(&nus_client);
	if (err) {
		LOG_ERR("Failed to subscribe to NUS receive (err %d)", err);
		return err;
	}
	
	LOG_INF("NUS subscription successful");
	return 0;
}

void ble_nus_client_discover(struct bt_conn *conn)
{
	int err;

	err = bt_gatt_dm_start(conn,
			       BT_UUID_NUS_SERVICE,
			       &discovery_cb,
			       &nus_client);
	if (err) {
		LOG_ERR("could not start the discovery procedure, error "
			"code: %d", err);
	}
}

void ble_nus_client_register_data_received_cb(ble_nus_data_received_cb_t cb)
{
	data_received_cb = cb;
}

void ble_nus_client_register_data_sent_cb(ble_nus_data_sent_cb_t cb)
{
	data_sent_cb = cb;
}

void ble_nus_client_register_mtu_exchange_cb(ble_nus_mtu_exchange_cb_t cb)
{
	mtu_exchange_cb = cb;
}

void ble_nus_client_register_discovery_complete_cb(ble_nus_discovery_complete_cb_t cb)
{
	discovery_complete_cb = cb;
}

struct bt_nus_client *ble_nus_client_get_instance(void)
{
	return &nus_client;
}

/* Helper function to perform MTU exchange */
int ble_nus_client_exchange_mtu(struct bt_conn *conn)
{
	int err;
	static struct bt_gatt_exchange_params exchange_params;

	exchange_params.func = exchange_func;
	// Request maximum MTU (247 bytes) to handle bigger packets
	err = bt_gatt_exchange_mtu(conn, &exchange_params);
	if (err) {
		LOG_WRN("MTU exchange failed (err %d)", err);
	}

	return err;
} 