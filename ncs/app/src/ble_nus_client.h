/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BLE_NUS_CLIENT_H
#define BLE_NUS_CLIENT_H

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <bluetooth/services/nus_client.h>
#include <bluetooth/gatt_dm.h>

/* NUS Client initialization and control functions */
int ble_nus_client_init(void);
int ble_nus_client_send_data(const uint8_t *data, uint16_t len);

/* Service discovery */
void ble_nus_client_discover(struct bt_conn *conn);

/* Callback registration for external modules */
typedef void (*ble_nus_data_received_cb_t)(const uint8_t *data, uint16_t len);
typedef void (*ble_nus_data_sent_cb_t)(uint8_t err);

void ble_nus_client_register_data_received_cb(ble_nus_data_received_cb_t cb);
void ble_nus_client_register_data_sent_cb(ble_nus_data_sent_cb_t cb);

/* Discovery complete callback */
typedef void (*ble_nus_discovery_complete_cb_t)(void);

void ble_nus_client_register_discovery_complete_cb(ble_nus_discovery_complete_cb_t cb);

/* Get NUS client instance for external use */
struct bt_nus_client *ble_nus_client_get_instance(void);

/* MTU exchange callback */
typedef void (*ble_nus_mtu_exchange_cb_t)(uint16_t mtu);

void ble_nus_client_register_mtu_exchange_cb(ble_nus_mtu_exchange_cb_t cb);

/* Helper function to perform MTU exchange */
int ble_nus_client_exchange_mtu(struct bt_conn *conn);

#endif /* BLE_NUS_CLIENT_H */
