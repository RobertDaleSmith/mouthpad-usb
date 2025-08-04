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

/* Forward declarations for direct USB access */
extern const struct device *hid_dev;
extern struct k_sem ep_write_sem;

LOG_MODULE_REGISTER(ble_mouthpad, LOG_LEVEL_INF);

static void button_handler(uint32_t button_state, uint32_t has_changed)
{
	/* Handle BLE Central button events (pairing) */
	ble_central_handle_buttons(button_state, has_changed);

	/* Handle HID-related button events */
	ble_hid_handle_buttons(button_state, has_changed);
}

int ble_init(void)
{
	LOG_INF("Initializing BLE stack...");
	int err;

	printk("Starting Bluetooth Central HIDS\n");

	/* Initialize BLE HID module */
	err = ble_hid_init();
	if (err) {
		printk("BLE HID initialization failed (err %d)\n", err);
		return 0;
	}

	/* Initialize BLE Central module */
	err = ble_central_init();
	if (err) {
		printk("BLE Central initialization failed (err %d)\n", err);
		return 0;
	}

	/* Initialize BLE Central callbacks */
	err = ble_central_init_callbacks();
	if (err) {
		printk("BLE Central callbacks initialization failed (err %d)\n", err);
		return 0;
	}

	/* Initialize BLE Central authentication callbacks */
	err = ble_central_init_auth_callbacks();
	if (err) {
		printk("BLE Central auth callbacks initialization failed (err %d)\n", err);
		return 0;
	}

	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return 0;
	}

	printk("Bluetooth initialized\n");

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}

	err = dk_buttons_init(button_handler);
	if (err) {
		printk("Failed to initialize buttons (err %d)\n", err);
		return 0;
	}

	err = ble_central_start_scan();
	if (err) {
		printk("Scanning failed to start (err %d)\n", err);
		return 0;
	}

	return 0;
}
