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
#include <bluetooth/gatt_dm.h>
#include <bluetooth/services/bas_client.h>

#include "ble_bas.h"
#include "ble_central.h"

#define LOG_MODULE_NAME ble_bas
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

/* Battery Service client instance */
static struct bt_bas_client bas;
static bool bas_ready = false;

/* Battery Service callbacks following Nordic central_bas pattern */
static void battery_notify_cb(struct bt_bas_client *bas, uint8_t battery_level);
static void battery_discovery_completed_cb(struct bt_gatt_dm *dm, void *context);
static void battery_discovery_service_not_found_cb(struct bt_conn *conn, void *context);
static void battery_discovery_error_found_cb(struct bt_conn *conn, int err, void *context);

/* GATT Discovery Manager callback structure */
static struct bt_gatt_dm_cb battery_discovery_cb = {
	.completed = battery_discovery_completed_cb,
	.service_not_found = battery_discovery_service_not_found_cb,
	.error_found = battery_discovery_error_found_cb,
};

/* Public API implementations */
int ble_bas_init(void)
{
	LOG_INF("Initializing Battery Service client...");
	
	/* Initialize Battery Service client following Nordic pattern */
	bt_bas_client_init(&bas);
	
	LOG_INF("Battery Service client initialized successfully");
	return 0;
}

int ble_bas_discover(struct bt_conn *conn)
{
	int err;

	if (!conn || conn != ble_central_get_default_conn()) {
		LOG_WRN("Invalid connection for Battery Service discovery");
		return -EINVAL;
	}

	LOG_INF("Starting Battery Service discovery");

	err = bt_gatt_dm_start(conn, BT_UUID_BAS, &battery_discovery_cb, NULL);
	if (err) {
		LOG_ERR("Could not start battery discovery: %d", err);
		return err;
	}
	
	return 0;
}

bool ble_bas_is_ready(void)
{
	return bas_ready;
}

/* Battery Service callback implementations */
static void battery_notify_cb(struct bt_bas_client *bas, uint8_t battery_level)
{
	if (battery_level == BT_BAS_VAL_INVALID) {
		LOG_WRN("Battery notification aborted");
	} else {
		LOG_INF("=== BATTERY LEVEL: %d%% ===", battery_level);
	}
}

static void battery_discovery_completed_cb(struct bt_gatt_dm *dm, void *context)
{
	int err;

	LOG_INF("Battery Service discovery completed");

	bt_gatt_dm_data_print(dm);

	err = bt_bas_handles_assign(dm, &bas);
	if (err) {
		LOG_ERR("Could not assign BAS handles: %d", err);
		goto cleanup;
	}

	if (bt_bas_notify_supported(&bas)) {
		LOG_INF("Battery notifications supported - subscribing");
		err = bt_bas_subscribe_battery_level(&bas, battery_notify_cb);
		if (err) {
			LOG_WRN("Cannot subscribe to battery notifications (err: %d)", err);
		} else {
			bas_ready = true;
		}
	} else {
		LOG_INF("Battery notifications not supported - using periodic reads");
		err = bt_bas_start_per_read_battery_level(&bas, 10000, battery_notify_cb);
		if (err) {
			LOG_WRN("Could not start periodic battery reads: %d", err);
		} else {
			bas_ready = true;
		}
	}

cleanup:
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