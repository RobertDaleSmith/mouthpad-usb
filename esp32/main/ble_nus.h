#pragma once

#include "esp_err.h"
#include "esp_gatts_api.h"
#include "esp_gattc_api.h"
#include "esp_gap_ble_api.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"

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
typedef void (*ble_nus_data_received_cb_t)(const uint8_t *data, uint16_t len);
typedef void (*ble_nus_data_sent_cb_t)(esp_err_t status);
typedef void (*ble_nus_connected_cb_t)(void);
typedef void (*ble_nus_disconnected_cb_t)(void);

// NUS service configuration
typedef struct {
    ble_nus_data_received_cb_t data_received_cb;
    ble_nus_data_sent_cb_t data_sent_cb;
    ble_nus_connected_cb_t connected_cb;
    ble_nus_disconnected_cb_t disconnected_cb;
} ble_nus_config_t;

/**
 * @brief Initialize the BLE NUS service
 * 
 * @param config Configuration structure with callback functions
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ble_nus_init(const ble_nus_config_t *config);

/**
 * @brief Start advertising the NUS service
 * 
 * @param device_name Name to advertise
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ble_nus_start_advertising(const char *device_name);

/**
 * @brief Stop advertising
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ble_nus_stop_advertising(void);

/**
 * @brief Send data through the NUS TX characteristic
 * 
 * @param data Data to send
 * @param len Length of data
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ble_nus_send_data(const uint8_t *data, uint16_t len);

/**
 * @brief Check if NUS service is connected
 * 
 * @return true if connected, false otherwise
 */
bool ble_nus_is_connected(void);

/**
 * @brief Get the current connection ID
 * 
 * @return uint16_t Connection ID, 0xFFFF if not connected
 */
uint16_t ble_nus_get_conn_id(void);

#ifdef __cplusplus
}
#endif
