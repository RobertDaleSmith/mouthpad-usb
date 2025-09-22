#pragma once

#include "esp_err.h"
#include "esp_gattc_api.h"
#include "esp_gap_ble_api.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Nordic UART Service UUID (128-bit)
#define NUS_SERVICE_UUID        0x6E400001B5A3F393E0A9E50E24DCCA9E
#define NUS_CHAR_TX_UUID        0x6E400002B5A3F393E0A9E50E24DCCA9E
#define NUS_CHAR_RX_UUID        0x6E400003B5A3F393E0A9E50E24DCCA9E

// Maximum data length for NUS packets
#define NUS_MAX_DATA_LEN        244  // Leave room for ATT headers

// Callback function types
typedef void (*ble_nus_client_data_received_cb_t)(const uint8_t *data, uint16_t len);
typedef void (*ble_nus_client_data_sent_cb_t)(esp_err_t status);
typedef void (*ble_nus_client_connected_cb_t)(void);
typedef void (*ble_nus_client_disconnected_cb_t)(void);

// NUS client configuration
typedef struct {
    ble_nus_client_data_received_cb_t data_received_cb;
    ble_nus_client_data_sent_cb_t data_sent_cb;
    ble_nus_client_connected_cb_t connected_cb;
    ble_nus_client_disconnected_cb_t disconnected_cb;
} ble_nus_client_config_t;

/**
 * @brief Initialize the BLE NUS client
 * 
 * @param config Configuration structure with callback functions
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ble_nus_client_init(const ble_nus_client_config_t *config);

/**
 * @brief Start service discovery for NUS on a connected device
 * 
 * @param gattc_if GATT client interface to use
 * @param conn_id Connection ID of the connected device
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ble_nus_client_discover_services(esp_gatt_if_t gattc_if, uint16_t conn_id);

/**
 * @brief Send data through the NUS RX characteristic
 * 
 * @param data Data to send
 * @param len Length of data
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ble_nus_client_send_data(const uint8_t *data, uint16_t len);

/**
 * @brief Check if NUS client is connected and ready
 * 
 * @return true if connected and ready, false otherwise
 */
bool ble_nus_client_is_ready(void);

/**
 * @brief Get the current connection ID
 * 
 * @return uint16_t Connection ID, 0xFFFF if not connected
 */
uint16_t ble_nus_client_get_conn_id(void);

/**
 * @brief Handle GATT client events (called from main GATT client callback)
 *
 * @param event GATT client event
 * @param gattc_if GATT client interface
 * @param param Event parameters
 */
void ble_nus_client_handle_gattc_event(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);

/**
 * @brief Print debug status of NUS client
 */
void ble_nus_client_debug_status(void);

#ifdef __cplusplus
}
#endif
