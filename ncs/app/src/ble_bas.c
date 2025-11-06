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
static uint8_t current_battery_level = 0xFF; /* Invalid/unknown initially */

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

void ble_bas_reset(void)
{
	bas_ready = false;
	current_battery_level = 0xFF; /* Reset to invalid/unknown */
	LOG_DBG("Battery service reset");
}

uint8_t ble_bas_get_battery_level(void)
{
	return current_battery_level;
}

ble_bas_rgb_color_t ble_bas_get_battery_color(ble_bas_color_mode_t mode)
{
	ble_bas_rgb_color_t color = {0, 0, 0}; /* Default to off */
	
	/* If battery level is invalid/unknown, default to green (assume full battery) */
	if (current_battery_level == 0xFF || current_battery_level > 100) {
		color.green = 255;
		return color;
	}
	
	if (mode == BAS_COLOR_MODE_DISCRETE) {
		/* Segmented colors optimized for GPIO LEDs */
		if (current_battery_level >= 50) {
			/* Green: 100-50% */
			color.green = 255;
		} else if (current_battery_level >= 10) {
			/* Yellow: 49-10% */
			color.red = 255;
			color.green = 255;
		} else {
			/* Red: 10-0% */
			color.red = 255;
		}
	} else {
		/* Gradient mode: smooth transition from green to red */
		if (current_battery_level >= 50) {
			/* Green to yellow gradient (100% to 50%) */
			/* Green stays at 255, red increases from 0 to 255 */
			color.green = 255;
			color.red = (uint8_t)(255 * (100 - current_battery_level) / 50);
		} else {
			/* Yellow to red gradient (50% to 0%) */
			/* Red stays at 255, green decreases from 255 to 0 */
			color.red = 255;
			color.green = (uint8_t)(255 * current_battery_level / 50);
		}
	}
	
	return color;
}

/* Battery Service callback implementations */
static void battery_notify_cb(struct bt_bas_client *bas, uint8_t battery_level)
{
	if (battery_level == BT_BAS_VAL_INVALID) {
		LOG_WRN("Battery notification aborted");
		current_battery_level = 0xFF; /* Mark as invalid */
	} else {
		LOG_INF("=== BATTERY LEVEL: %d%% ===", battery_level);
		current_battery_level = battery_level; /* Store current level */
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

	/* DIS discovery is now started earlier in gatt_discover() for faster firmware/VID/PID retrieval */
	LOG_DBG("Battery Service discovery complete (DIS already started)");
}

static void battery_discovery_service_not_found_cb(struct bt_conn *conn, void *context)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(context);
	LOG_INF("Battery Service not found during discovery");
	/* DIS discovery is now started earlier in gatt_discover() */
}

static void battery_discovery_error_found_cb(struct bt_conn *conn, int err, void *context)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(context);
	LOG_ERR("Battery Service discovery failed: %d", err);
	/* DIS discovery is now started earlier in gatt_discover() */
}