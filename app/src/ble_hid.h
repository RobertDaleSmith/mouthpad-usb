/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BLE_HID_H
#define BLE_HID_H

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <bluetooth/services/hogp.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the BLE HID module
 * 
 * This function initializes the HOGP client and sets up HID-related callbacks.
 * 
 * @return 0 on success, negative error code on failure
 */
int ble_hid_init(void);

/**
 * @brief Discover HID services on a connected device
 * 
 * @param conn The BLE connection to discover services on
 * @return 0 on success, negative error code on failure
 */
int ble_hid_discover(struct bt_conn *conn);

/**
 * @brief Check if HID is ready for use
 * 
 * @return true if HID is ready, false otherwise
 */
bool ble_hid_is_ready(void);

/**
 * @brief Send a HID report
 * 
 * @param data The report data to send
 * @param len The length of the report data
 * @return 0 on success, negative error code on failure
 */
int ble_hid_send_report(const uint8_t *data, uint16_t len);

/**
 * @brief Get the HOGP instance
 * 
 * @return Pointer to the HOGP instance
 */
struct bt_hogp *ble_hid_get_hogp(void);

/**
 * @brief Get the HOGP notify callback function
 * 
 * @return Pointer to the HOGP notify callback function
 */
uint8_t (*ble_hid_get_notify_cb(void))(struct bt_hogp *, struct bt_hogp_rep_info *, uint8_t, const uint8_t *);

/**
 * @brief Get the HOGP boot mouse callback function
 * 
 * @return Pointer to the HOGP boot mouse callback function
 */
uint8_t (*ble_hid_get_boot_mouse_cb(void))(struct bt_hogp *, struct bt_hogp_rep_info *, uint8_t, const uint8_t *);

/**
 * @brief Get the HOGP boot keyboard callback function
 * 
 * @return Pointer to the HOGP boot keyboard callback function
 */
uint8_t (*ble_hid_get_boot_kbd_cb(void))(struct bt_hogp *, struct bt_hogp_rep_info *, uint8_t, const uint8_t *);

/**
 * @brief Handle button events for HID functionality
 * 
 * @param button_state The button state
 * @param has_changed Which buttons have changed
 */
void ble_hid_handle_buttons(uint32_t button_state, uint32_t has_changed);

#ifdef __cplusplus
}
#endif

#endif /* BLE_HID_H */ 