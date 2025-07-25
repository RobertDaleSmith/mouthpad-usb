/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VIRTUAL_MOUSE_H
#define VIRTUAL_MOUSE_H

#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the virtual mouse input device
 *
 * This function initializes the virtual mouse device that can generate
 * input events for the USB HID system.
 *
 * @return 0 on success, negative error code on failure
 */
int virtual_mouse_init(void);

/**
 * @brief Send a mouse event through the virtual mouse device
 *
 * This function sends mouse events (buttons, x, y, wheel) through the
 * virtual mouse device, which will be picked up by the input subsystem
 * and forwarded to the USB HID device.
 *
 * @param buttons Mouse button states (bit 0: left, bit 1: right, bit 2: middle)
 * @param x X-axis movement (signed 8-bit value)
 * @param y Y-axis movement (signed 8-bit value)
 * @param wheel Wheel movement (signed 8-bit value)
 * @return 0 on success, negative error code on failure
 */
int virtual_mouse_send_event(uint8_t buttons, int8_t x, int8_t y, int8_t wheel);

#ifdef __cplusplus
}
#endif

#endif /* VIRTUAL_MOUSE_H */ 