#ifndef BLE_HID_C_H
#define BLE_HID_C_H

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

// HID report callback function type
typedef void (*ble_hid_report_callback_t)(const uint8_t *data, uint16_t len);

// HID connection state
typedef enum {
    BLE_HID_STATE_DISCONNECTED,
    BLE_HID_STATE_DISCOVERING,
    BLE_HID_STATE_CONNECTED,
    BLE_HID_STATE_ERROR
} ble_hid_state_t;

// HID device information
typedef struct {
    bt_addr_le_t addr;
    char name[32];
    struct bt_conn *conn;
    uint16_t hid_service_handle;
    uint16_t hid_report_handle;
    uint16_t hid_report_cccd_handle;
    bool notifications_enabled;
} ble_hid_device_t;

// Function declarations
void ble_hid_c_init(ble_hid_report_callback_t callback);
void ble_hid_c_start_scan(void);
void ble_hid_c_stop_scan(void);
void ble_hid_c_disconnect(void);
ble_hid_state_t ble_hid_c_get_state(void);
bool ble_hid_c_is_connected(void);
const ble_hid_device_t* ble_hid_c_get_device(void);

// Event handlers (called from main BLE event handler)
void ble_hid_c_on_ble_evt(struct bt_conn *conn, uint8_t evt_type, struct bt_gatt_attr *attr, void *buf);

// Connection handlers (called from main.c)
void ble_hid_c_on_connected(struct bt_conn *conn);
void ble_hid_c_on_disconnected(struct bt_conn *conn, uint8_t reason);

// Configuration
void ble_hid_c_set_target_name(const char *name);
void ble_hid_c_set_target_address(const bt_addr_le_t *addr);

#endif // BLE_HID_C_H