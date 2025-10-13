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

void ble_central_register_connected_cb(ble_central_connected_cb_t cb);
void ble_central_register_disconnected_cb(ble_central_disconnected_cb_t cb);

/* Get scan work for external scheduling */
struct k_work *ble_central_get_scan_work(void);

/* Device type detection functions */
bool ble_central_is_nus_device(const struct bt_scan_device_info *device_info);
bool ble_central_is_hid_device(const struct bt_scan_device_info *device_info);
const char *ble_central_get_device_type_string(const struct bt_scan_device_info *device_info);

/* Background scanning for RSSI updates during connections */
int ble_central_start_background_scan_for_rssi(void);

/* Bonding management */
void ble_central_clear_bonded_device(void);

/* Scanning state query */
bool ble_central_is_scanning(void);

#endif /* BLE_CENTRAL_H */
