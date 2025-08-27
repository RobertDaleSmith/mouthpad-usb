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
 * @brief Get the latest mouse data for display purposes
 * 
 * @param x Pointer to store X delta value (can be NULL)
 * @param y Pointer to store Y delta value (can be NULL)  
 * @param buttons Pointer to store button state (can be NULL)
 * @return true if new mouse data is available, false otherwise
 */
bool ble_hid_get_mouse_data(int16_t *x, int16_t *y, uint8_t *buttons);

/**
 * @brief Check if mouse data has been updated
 * 
 * @return true if new mouse data is available, false otherwise
 */
bool ble_hid_has_mouse_data(void);

/**
 * @brief Handle button events for HID functionality
 * 
 * @param button_state The button state
 * @param has_changed Which buttons have changed
 */
void ble_hid_handle_buttons(uint32_t button_state, uint32_t has_changed);

/* Callback function types */
typedef void (*ble_hid_data_received_cb_t)(const uint8_t *data, uint16_t len);
typedef void (*ble_hid_ready_cb_t)(void);

/**
 * @brief Register data received callback
 *
 * @param cb Callback function for data received events
 * @return 0 on success, negative error code on failure
 */
int ble_hid_register_data_received_cb(ble_hid_data_received_cb_t cb);

/**
 * @brief Register ready callback
 *
 * @param cb Callback function for ready events
 * @return 0 on success, negative error code on failure
 */
int ble_hid_register_ready_cb(ble_hid_ready_cb_t cb);

/**
 * @brief Manually trigger auto-detection and mode switching
 *
 * This function manually triggers the automatic protocol mode detection
 * and switching logic, similar to how operating systems handle HID
 * protocol mode optimization.
 *
 * @return 0 on success, negative error code on failure
 */
int ble_hid_auto_detect_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_HID_H */
