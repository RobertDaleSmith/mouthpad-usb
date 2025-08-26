/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BLE_MULTI_CONN_H
#define BLE_MULTI_CONN_H

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <bluetooth/scan.h>
#include <stdint.h>
#include <stdbool.h>

/* Device type enumeration */
typedef enum {
    DEVICE_TYPE_UNKNOWN = 0,
    DEVICE_TYPE_MOUTHPAD,       /* NUS + HID services */
    DEVICE_TYPE_EVEN_G1_LEFT,   /* NUS only (left arm) */
    DEVICE_TYPE_EVEN_G1_RIGHT,  /* NUS only (right arm) */
    DEVICE_TYPE_NUS_GENERIC     /* Generic NUS device */
} ble_device_type_t;

/* Connection state for each device */
typedef struct {
    struct bt_conn *conn;
    ble_device_type_t type;
    char name[32];
    bt_addr_le_t addr;
    bool nus_ready;
    bool hid_ready;
    uint16_t mtu;
    int8_t rssi;
} ble_device_connection_t;

/* Maximum simultaneous connections */
#define MAX_BLE_CONNECTIONS 4

/* Multi-connection management functions */
int ble_multi_conn_init(void);
int ble_multi_conn_add(struct bt_conn *conn, ble_device_type_t type, const char *name);
int ble_multi_conn_remove(struct bt_conn *conn);
ble_device_connection_t *ble_multi_conn_get(struct bt_conn *conn);
ble_device_connection_t *ble_multi_conn_get_by_index(int index);
ble_device_connection_t *ble_multi_conn_get_by_type(ble_device_type_t type);
ble_device_connection_t *ble_multi_conn_get_mouthpad(void);
ble_device_connection_t *ble_multi_conn_get_even_g1_left(void);
ble_device_connection_t *ble_multi_conn_get_even_g1_right(void);
int ble_multi_conn_count(void);
bool ble_multi_conn_has_type(ble_device_type_t type);
void ble_multi_conn_set_nus_ready(struct bt_conn *conn, bool ready);
void ble_multi_conn_set_hid_ready(struct bt_conn *conn, bool ready);
void ble_multi_conn_set_mtu(struct bt_conn *conn, uint16_t mtu);
void ble_multi_conn_set_rssi(struct bt_conn *conn, int8_t rssi);

/* Device type detection from scan info */
ble_device_type_t ble_multi_conn_detect_device_type(const struct bt_scan_device_info *device_info, const char *name);

/* Check if we need more Even G1 connections */
bool ble_multi_conn_need_even_g1_pair(void);

/* Debug/status functions */
void ble_multi_conn_print_status(void);

#endif /* BLE_MULTI_CONN_H */