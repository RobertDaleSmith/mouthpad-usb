/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ble_multi_conn.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/sys/byteorder.h>
#include <bluetooth/scan.h>
#include <string.h>

LOG_MODULE_REGISTER(ble_multi_conn, LOG_LEVEL_INF);

/* Connection storage */
static ble_device_connection_t connections[MAX_BLE_CONNECTIONS];
static struct k_mutex conn_mutex;


int ble_multi_conn_init(void)
{
    k_mutex_init(&conn_mutex);
    memset(connections, 0, sizeof(connections));
    LOG_INF("Multi-connection manager initialized");
    return 0;
}

int ble_multi_conn_add(struct bt_conn *conn, ble_device_type_t type, const char *name)
{
    k_mutex_lock(&conn_mutex, K_FOREVER);
    
    /* Find empty slot */
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (connections[i].conn == NULL) {
            connections[i].conn = bt_conn_ref(conn);
            connections[i].type = type;
            if (name) {
                strncpy(connections[i].name, name, sizeof(connections[i].name) - 1);
            }
            const bt_addr_le_t *addr = bt_conn_get_dst(conn);
            if (addr) {
                memcpy(&connections[i].addr, addr, sizeof(bt_addr_le_t));
            }
            
            LOG_INF("Added connection %d: type=%d (%s), name=%s", i, type,
                    type == DEVICE_TYPE_MOUTHPAD ? "MOUTHPAD" :
                    type == DEVICE_TYPE_EVEN_G1_LEFT ? "EVEN_G1_LEFT" :
                    type == DEVICE_TYPE_EVEN_G1_RIGHT ? "EVEN_G1_RIGHT" :
                    type == DEVICE_TYPE_NUS_GENERIC ? "NUS_GENERIC" : "UNKNOWN",
                    name ? name : "unknown");
            k_mutex_unlock(&conn_mutex);
            return i;
        }
    }
    
    k_mutex_unlock(&conn_mutex);
    LOG_ERR("No free connection slots");
    return -ENOMEM;
}

int ble_multi_conn_remove(struct bt_conn *conn)
{
    k_mutex_lock(&conn_mutex, K_FOREVER);
    
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (connections[i].conn == conn) {
            bt_conn_unref(connections[i].conn);
            LOG_INF("Removed connection %d: type=%d, name=%s", 
                    i, connections[i].type, connections[i].name);
            memset(&connections[i], 0, sizeof(ble_device_connection_t));
            k_mutex_unlock(&conn_mutex);
            return 0;
        }
    }
    
    k_mutex_unlock(&conn_mutex);
    return -ENOENT;
}

ble_device_connection_t *ble_multi_conn_get(struct bt_conn *conn)
{
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (connections[i].conn == conn) {
            return &connections[i];
        }
    }
    return NULL;
}

ble_device_connection_t *ble_multi_conn_get_by_index(int index)
{
    if (index < 0 || index >= MAX_BLE_CONNECTIONS) {
        return NULL;
    }
    
    if (connections[index].conn) {
        return &connections[index];
    }
    
    return NULL;
}

ble_device_connection_t *ble_multi_conn_get_by_type(ble_device_type_t type)
{
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (connections[i].conn && connections[i].type == type) {
            return &connections[i];
        }
    }
    return NULL;
}

ble_device_connection_t *ble_multi_conn_get_mouthpad(void)
{
    return ble_multi_conn_get_by_type(DEVICE_TYPE_MOUTHPAD);
}

ble_device_connection_t *ble_multi_conn_get_even_g1_left(void)
{
    return ble_multi_conn_get_by_type(DEVICE_TYPE_EVEN_G1_LEFT);
}

ble_device_connection_t *ble_multi_conn_get_even_g1_right(void)
{
    return ble_multi_conn_get_by_type(DEVICE_TYPE_EVEN_G1_RIGHT);
}

int ble_multi_conn_count(void)
{
    int count = 0;
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (connections[i].conn) {
            count++;
        }
    }
    return count;
}

bool ble_multi_conn_has_type(ble_device_type_t type)
{
    bool result = ble_multi_conn_get_by_type(type) != NULL;
    
    /* Debug logging commented out - too frequent in main loop
    if (type == DEVICE_TYPE_MOUTHPAD) {
        LOG_INF("DEBUG: ble_multi_conn_has_type(MOUTHPAD) = %d", result);
        for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
            if (connections[i].conn) {
                LOG_INF("  Connection %d: type=%d (%s)", i, connections[i].type,
                        connections[i].type == DEVICE_TYPE_MOUTHPAD ? "MOUTHPAD" :
                        connections[i].type == DEVICE_TYPE_EVEN_G1_LEFT ? "EVEN_G1_LEFT" :
                        connections[i].type == DEVICE_TYPE_EVEN_G1_RIGHT ? "EVEN_G1_RIGHT" :
                        connections[i].type == DEVICE_TYPE_NUS_GENERIC ? "NUS_GENERIC" : "UNKNOWN");
            }
        }
    }
    */
    
    return result;
}

void ble_multi_conn_set_nus_ready(struct bt_conn *conn, bool ready)
{
    ble_device_connection_t *dev = ble_multi_conn_get(conn);
    if (dev) {
        dev->nus_ready = ready;
        LOG_INF("Connection NUS ready=%d for %s", ready, dev->name);
    }
}

void ble_multi_conn_set_hid_ready(struct bt_conn *conn, bool ready)
{
    ble_device_connection_t *dev = ble_multi_conn_get(conn);
    if (dev) {
        dev->hid_ready = ready;
        LOG_INF("Connection HID ready=%d for %s", ready, dev->name);
    }
}

void ble_multi_conn_set_mtu(struct bt_conn *conn, uint16_t mtu)
{
    ble_device_connection_t *dev = ble_multi_conn_get(conn);
    if (dev) {
        dev->mtu = mtu;
        LOG_INF("Connection MTU=%d for %s", mtu, dev->name);
    }
}

void ble_multi_conn_set_rssi(struct bt_conn *conn, int8_t rssi)
{
    ble_device_connection_t *dev = ble_multi_conn_get(conn);
    if (dev) {
        dev->rssi = rssi;
    }
}

ble_device_type_t ble_multi_conn_detect_device_type(const struct bt_scan_device_info *device_info, const char *name)
{
    /* Check device name for Even G1 */
    bool is_even_g1 = false;
    if (name) {
        /* Check for Even G1 indicators in name */
        if (strstr(name, "Even") || strstr(name, "G1") || 
            strstr(name, "even") || strstr(name, "g1")) {
            is_even_g1 = true;
            LOG_INF("Detected Even G1 device by name: %s", name);
        }
    }

    if (is_even_g1) {
        /* Even G1 detection based on name alone */
        /* Even G1 devices have names like "Even G1_22_L_327B9B" or "Even G1_22_R_6981C6" */
        bool is_left_arm = false;
        bool is_right_arm = false;
        
        if (name) {
            /* Check for _L_ or _R_ in the name */
            if (strstr(name, "_L_")) {
                is_left_arm = true;
                LOG_INF("Detected Even G1 Left Arm by name suffix");
            } else if (strstr(name, "_R_")) {
                is_right_arm = true;
                LOG_INF("Detected Even G1 Right Arm by name suffix");
            } else {
                /* Fallback: check for standalone L or R */
                const char *last_underscore = strrchr(name, '_');
                if (last_underscore && *(last_underscore + 1) == 'L') {
                    is_left_arm = true;
                    LOG_INF("Detected Even G1 Left Arm by L suffix");
                } else if (last_underscore && *(last_underscore + 1) == 'R') {
                    is_right_arm = true;
                    LOG_INF("Detected Even G1 Right Arm by R suffix");
                }
            }
        }
        
        /* Return appropriate type based on detection */
        if (is_left_arm) {
            return DEVICE_TYPE_EVEN_G1_LEFT;
        } else if (is_right_arm) {
            return DEVICE_TYPE_EVEN_G1_RIGHT;
        } else {
            /* Couldn't determine from name, use order-based approach */
            if (ble_multi_conn_has_type(DEVICE_TYPE_EVEN_G1_LEFT)) {
                LOG_INF("Detected Even G1 Right Arm (second device, no name suffix)");
                return DEVICE_TYPE_EVEN_G1_RIGHT;
            } else {
                LOG_INF("Detected Even G1 Left Arm (first device, no name suffix)");
                return DEVICE_TYPE_EVEN_G1_LEFT;
            }
        }
    } else {
        /* Since we use HID UUID filter in scanning, any non-Even G1 device that gets here
           should be a MouthPad (has HID service). Trust the scan filter like main branch did. */
        LOG_INF("Detected MouthPad device (passed HID UUID filter): %s", name ? name : "Unknown");
        return DEVICE_TYPE_MOUTHPAD;
    }
}

bool ble_multi_conn_need_even_g1_pair(void)
{
    /* Check if we have one Even G1 arm but not both */
    bool has_left = ble_multi_conn_has_type(DEVICE_TYPE_EVEN_G1_LEFT);
    bool has_right = ble_multi_conn_has_type(DEVICE_TYPE_EVEN_G1_RIGHT);
    
    return (has_left && !has_right) || (!has_left && has_right);
}

void ble_multi_conn_print_status(void)
{
    LOG_INF("=== Multi-Connection Status ===");
    int active = 0;
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (connections[i].conn) {
            char addr_str[BT_ADDR_LE_STR_LEN];
            bt_addr_le_to_str(&connections[i].addr, addr_str, sizeof(addr_str));
            LOG_INF("Slot %d: %s (%s) - NUS:%d HID:%d MTU:%d RSSI:%d", 
                    i, connections[i].name, addr_str,
                    connections[i].nus_ready, connections[i].hid_ready,
                    connections[i].mtu, connections[i].rssi);
            active++;
        }
    }
    LOG_INF("Active connections: %d/%d", active, MAX_BLE_CONNECTIONS);
}