#ifndef BLE_UART_C_H
#define BLE_UART_C_H

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

// UART data callback function type
typedef void (*ble_uart_data_callback_t)(const uint8_t *data, uint16_t len);

// UART connection state
typedef enum {
    BLE_UART_STATE_DISCONNECTED,
    BLE_UART_STATE_DISCOVERING,
    BLE_UART_STATE_CONNECTED,
    BLE_UART_STATE_ERROR
} ble_uart_state_t;

// UART device information
typedef struct {
    bt_addr_le_t addr;
    char name[32];
    struct bt_conn *conn;
    uint16_t nus_service_handle;
    uint16_t nus_tx_handle;
    uint16_t nus_tx_cccd_handle;
    uint16_t nus_rx_handle;
    bool notifications_enabled;
} ble_uart_device_t;

// Function declarations
void ble_uart_c_init(ble_uart_data_callback_t callback);
void ble_uart_c_start_scan(void);
void ble_uart_c_stop_scan(void);
bool ble_uart_c_connect(const bt_addr_le_t *addr);
void ble_uart_c_disconnect(void);
ble_uart_state_t ble_uart_c_get_state(void);
bool ble_uart_c_is_connected(void);
const ble_uart_device_t* ble_uart_c_get_device(void);
bool ble_uart_c_send_data(const uint8_t *data, uint16_t len);

// Event handlers (called from main BLE event handler)
void ble_uart_c_on_ble_evt(struct bt_conn *conn, uint8_t evt_type, struct bt_gatt_attr *attr, void *buf);

// Configuration
void ble_uart_c_set_target_name(const char *name);
void ble_uart_c_set_target_address(const bt_addr_le_t *addr);

#endif // BLE_UART_C_H