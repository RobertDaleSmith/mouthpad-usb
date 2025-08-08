/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BLE_CENTRAL_H
#define BLE_CENTRAL_H

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <bluetooth/scan.h>

/* BLE Central initialization and control functions */
int ble_central_init(void);
int ble_central_start_scan(void);
int ble_central_stop_scan(void);

/* Connection management */
struct bt_conn *ble_central_get_default_conn(void);
void ble_central_set_default_conn(struct bt_conn *conn);

/* Callback registration for external modules */
typedef void (*ble_central_connected_cb_t)(struct bt_conn *conn);
typedef void (*ble_central_disconnected_cb_t)(struct bt_conn *conn, uint8_t reason);
typedef void (*ble_central_security_changed_cb_t)(struct bt_conn *conn, bt_security_t level, enum bt_security_err err);

void ble_central_register_connected_cb(ble_central_connected_cb_t cb);
void ble_central_register_disconnected_cb(ble_central_disconnected_cb_t cb);
void ble_central_register_security_changed_cb(ble_central_security_changed_cb_t cb);

/* Authentication handling */
void ble_central_num_comp_reply(bool accept);
struct bt_conn *ble_central_get_auth_conn(void);
void ble_central_set_auth_conn(struct bt_conn *conn);
void ble_central_handle_buttons(uint32_t button_state, uint32_t has_changed);

/* Additional initialization functions */
int ble_central_init_callbacks(void);
int ble_central_init_auth_callbacks(void);

/* Get scan work for external scheduling */
struct k_work *ble_central_get_scan_work(void);

#endif /* BLE_CENTRAL_H */ 