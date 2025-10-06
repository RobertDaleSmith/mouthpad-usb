/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BLE_TRANSPORT_H
#define BLE_TRANSPORT_H

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/conn.h>

/* Callback function types */
typedef void (*ble_data_callback_t)(const uint8_t *data, uint16_t len);
typedef void (*ble_ready_callback_t)(void);

/* BLE Transport initialization and control functions */
int ble_transport_init(void);
int ble_transport_start_bridging(void);

/* USB HID callback type */
typedef void (*usb_hid_send_cb_t)(const uint8_t *data, uint16_t len);

/* Transport registration functions */
int ble_transport_register_usb_hid_callback(usb_hid_send_cb_t cb);

/* HID Transport functions */
int ble_transport_send_hid_data(const uint8_t *data, uint16_t len);
bool ble_transport_is_hid_ready(void);

/* Future NUS Transport functions (for later integration) */
int ble_transport_register_nus_data_callback(ble_data_callback_t cb);
int ble_transport_register_nus_ready_callback(ble_ready_callback_t cb);
int ble_transport_send_nus_data(const uint8_t *data, uint16_t len);
bool ble_transport_is_nus_ready(void);

/* Internal transport state management */
void ble_transport_handle_connection(struct bt_conn *conn);
void ble_transport_handle_disconnection(struct bt_conn *conn, uint8_t reason);

#endif /* BLE_TRANSPORT_H */ 