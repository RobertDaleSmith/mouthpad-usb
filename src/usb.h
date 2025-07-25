/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef USB_H
#define USB_H

#include <zephyr/kernel.h>

/**
 * @brief Initialize USB HID device
 *
 * This function initializes the USB HID device as a mouse,
 * sets up the necessary callbacks, and starts the main loop
 * for handling mouse input events.
 *
 * @return 0 on success, negative error code on failure
 */
int usb_init(void);

// Mouse events are now handled through the Zephyr input subsystem
// Use virtual_mouse_send_event() from virtual_mouse.h instead

#endif /* USB_H */
