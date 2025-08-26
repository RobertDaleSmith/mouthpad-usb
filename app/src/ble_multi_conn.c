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

/* Helper to check if advertising data contains a specific UUID */
static bool adv_data_contains_uuid(const struct bt_data *data, const struct bt_uuid *uuid)
{
    if (data->type != BT_DATA_UUID16_ALL && data->type != BT_DATA_UUID16_SOME &&
        data->type != BT_DATA_UUID128_ALL && data->type != BT_DATA_UUID128_SOME) {
        return false;
    }

    /* Check for 16-bit UUID (HID service) */
    if (uuid->type == BT_UUID_TYPE_16) {
        struct bt_uuid_16 *uuid16 = (struct bt_uuid_16 *)uuid;
        uint16_t uuid_val = uuid16->val;
        
        /* Iterate through UUID list in advertising data */
        for (size_t i = 0; i + 1 < data->data_len; i += 2) {
            uint16_t found_uuid = sys_le16_to_cpu(*((uint16_t*)&data->data[i]));
            if (found_uuid == uuid_val) {
                return true;
            }
        }
    }
    /* Check for 128-bit UUID (NUS service) */
    else if (uuid->type == BT_UUID_TYPE_128) {
        struct bt_uuid_128 *uuid128 = (struct bt_uuid_128 *)uuid;
        
        /* For 128-bit UUIDs, we need at least 16 bytes */
        if (data->data_len >= 16) {
            /* Compare 128-bit UUIDs */
            if (memcmp(data->data, uuid128->val, 16) == 0) {
                return true;
            }
        }
    }

    return false;
}

/* Check advertising data for service UUIDs */
static bool check_adv_uuid(struct bt_data *data, void *user_data)
{
    struct {
        bool has_nus;
        bool has_hid;
    } *result = user_data;

    /* Nordic UART Service UUID */
    static const struct bt_uuid_128 nus_uuid = BT_UUID_INIT_128(
        BT_UUID_128_ENCODE(0x6e400001, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e));

    if (adv_data_contains_uuid(data, &nus_uuid.uuid)) {
        result->has_nus = true;
    }

    if (adv_data_contains_uuid(data, BT_UUID_HIDS)) {
        result->has_hid = true;
    }
    
    return true;  /* Continue parsing */
}

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
            
            LOG_INF("Added connection %d: type=%d, name=%s", i, type, 
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
    return ble_multi_conn_get_by_type(type) != NULL;
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
    struct {
        bool has_nus;
        bool has_hid;
    } services = {0};

    /* Parse advertising data for service UUIDs */
    bt_data_parse(device_info->adv_data, check_adv_uuid, &services);
    
    LOG_DBG("Device %s - NUS: %d, HID: %d", name ? name : "Unknown", 
            services.has_nus, services.has_hid);

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

    /* Determine device type based on services and name */
    if (services.has_nus && services.has_hid) {
        LOG_INF("Detected MouthPad device (NUS+HID)");
        return DEVICE_TYPE_MOUTHPAD;
    } else if (is_even_g1) {  /* Even G1 detection based on name alone */
        /* Determine if left or right arm based on device name */
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
    } else if (services.has_nus) {
        LOG_INF("Detected generic NUS device");
        return DEVICE_TYPE_NUS_GENERIC;
    }

    return DEVICE_TYPE_UNKNOWN;
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