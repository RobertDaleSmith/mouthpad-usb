/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/version.h>
#include "ble_transport.h"
#include "usb_hid.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void)
{
    printk("MouthPad^USB :: HID Sample Started\n");
    printk("Build: %s %s %s\n", KERNEL_VERSION_STRING, __DATE__, __TIME__);

	/* Initialize BLE Transport */
	if (ble_transport_init() != 0) {
		printk("BLE Transport initialization failed\n");
		return 0;
	}

	/* Initialize USB */
	if (usb_init() != 0) {
		printk("USB initialization failed\n");
		return 0;
	}

	printk("MouthPad^USB Bridge initialization complete\n");
}
