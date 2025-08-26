/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BLE_NUS_MULTI_CLIENT_H
#define BLE_NUS_MULTI_CLIENT_H

#include <zephyr/bluetooth/conn.h>
#include <stdint.h>
#include <stdbool.h>

/* Callback function types */
typedef void (*ble_nus_multi_data_received_cb_t)(struct bt_conn *conn, const uint8_t *data, uint16_t len);
typedef void (*ble_nus_multi_discovery_complete_cb_t)(struct bt_conn *conn);
typedef void (*ble_nus_multi_mtu_exchange_cb_t)(struct bt_conn *conn, uint16_t mtu);

/* Initialize multi-connection NUS client */
int ble_nus_multi_client_init(void);

/* Add a new NUS connection */
int ble_nus_multi_client_add_connection(struct bt_conn *conn);

/* Remove a NUS connection */
int ble_nus_multi_client_remove_connection(struct bt_conn *conn);

/* Start service discovery for a connection */
void ble_nus_multi_client_discover(struct bt_conn *conn);

/* Send data to a specific connection */
int ble_nus_multi_client_send_data(struct bt_conn *conn, const uint8_t *data, uint16_t len);

/* Check if NUS is discovered for a connection */
bool ble_nus_multi_client_is_discovered(struct bt_conn *conn);

/* Exchange MTU for a specific connection */
int ble_nus_multi_client_exchange_mtu(struct bt_conn *conn);

/* Register callbacks */
void ble_nus_multi_client_register_data_received_cb(ble_nus_multi_data_received_cb_t cb);
void ble_nus_multi_client_register_discovery_complete_cb(ble_nus_multi_discovery_complete_cb_t cb);
void ble_nus_multi_client_register_mtu_exchange_cb(ble_nus_multi_mtu_exchange_cb_t cb);

/* Get active connection count */
int ble_nus_multi_client_get_connection_count(void);

#endif /* BLE_NUS_MULTI_CLIENT_H */