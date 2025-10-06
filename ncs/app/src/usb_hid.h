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

/**
 * @brief Send HID report to USB HID device
 *
 * @param data HID report data
 * @param len Length of HID report data
 * @return 0 on success, negative error code on failure
 */
int usb_hid_send_report(const uint8_t *data, uint16_t len);

/**
 * @brief Send HID release-all report to clear any stuck inputs
 *
 * Sends reports with all buttons released and no movement to prevent
 * stuck inputs when BLE device disconnects.
 *
 * @return 0 on success, negative error code on failure
 */
int usb_hid_send_release_all(void);

#endif /* USB_H */
