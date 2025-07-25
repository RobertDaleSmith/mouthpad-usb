/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/input/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/logging/log.h>

/* Define INPUT_EV_SYN since it's not in Zephyr's input event codes */
#define INPUT_EV_SYN 0x00

LOG_MODULE_REGISTER(virtual_mouse, LOG_LEVEL_INF);

/* Public API functions */
int virtual_mouse_init(void)
{
	LOG_INF("Virtual mouse initialized");
	return 0;
}

int virtual_mouse_send_event(uint8_t buttons, int8_t x, int8_t y, int8_t wheel)
{
	/* Send button events */
	input_report_key(NULL, INPUT_BTN_LEFT, (buttons & 0x01) ? 1 : 0, false, K_FOREVER);
	input_report_key(NULL, INPUT_BTN_RIGHT, (buttons & 0x02) ? 1 : 0, false, K_FOREVER);
	input_report_key(NULL, INPUT_BTN_MIDDLE, (buttons & 0x04) ? 1 : 0, false, K_FOREVER);

	/* Send movement events */
	if (x != 0) {
		input_report_rel(NULL, INPUT_REL_X, x, false, K_FOREVER);
	}
	if (y != 0) {
		input_report_rel(NULL, INPUT_REL_Y, y, false, K_FOREVER);
	}
	if (wheel != 0) {
		input_report_rel(NULL, INPUT_REL_WHEEL, wheel, false, K_FOREVER);
	}

	/* Send a sync event to complete the input report */
	input_report(NULL, INPUT_EV_SYN, 0, 0, true, K_FOREVER);

	LOG_DBG("Virtual mouse event: buttons=0x%02x, x=%d, y=%d, wheel=%d", buttons, x, y, wheel);
	return 0;
}

/* No device needed - we use NULL device for input events */ 