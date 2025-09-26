#pragma once

#include "esp_err.h"
#include "esp_gattc_api.h"
#include "esp_gap_ble_api.h"
#include "esp_bt_defs.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Device Information Service UUID (16-bit)
#define DIS_SERVICE_UUID                0x180A

// Device Information Service characteristic UUIDs (16-bit)
#define DIS_CHAR_MANUFACTURER_NAME_UUID 0x2A29
#define DIS_CHAR_MODEL_NUMBER_UUID      0x2A24
#define DIS_CHAR_SERIAL_NUMBER_UUID     0x2A25
#define DIS_CHAR_HARDWARE_REV_UUID      0x2A27
#define DIS_CHAR_FIRMWARE_REV_UUID      0x2A26
#define DIS_CHAR_SOFTWARE_REV_UUID      0x2A28
#define DIS_CHAR_PNP_ID_UUID            0x2A50

// Maximum length for device info strings
#define DIS_MAX_STRING_LEN              64

// PnP ID structure
typedef struct {
    uint8_t vendor_id_source;    // 0x01=Bluetooth SIG, 0x02=USB Forum
    uint16_t vendor_id;          // Vendor ID
    uint16_t product_id;         // Product ID
    uint16_t product_version;    // Product version
} ble_pnp_id_t;

// Device information structure
typedef struct {
    char device_name[DIS_MAX_STRING_LEN];        // Advertised device name
    char manufacturer_name[DIS_MAX_STRING_LEN];
    char model_number[DIS_MAX_STRING_LEN];
    char serial_number[DIS_MAX_STRING_LEN];
    char hardware_revision[DIS_MAX_STRING_LEN];
    char firmware_revision[DIS_MAX_STRING_LEN];
    char software_revision[DIS_MAX_STRING_LEN];
    ble_pnp_id_t pnp_id;
    bool has_pnp_id;
    bool info_complete;
} ble_device_info_t;

// Callback function types
typedef void (*ble_device_info_complete_cb_t)(const ble_device_info_t *device_info);

// Device info client configuration
typedef struct {
    ble_device_info_complete_cb_t info_complete_cb;
} ble_device_info_config_t;

/**
 * @brief Initialize the BLE device info client
 *
 * @param config Configuration structure with callback functions
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ble_device_info_init(const ble_device_info_config_t *config);

/**
 * @brief Start device info discovery for DIS on a connected device
 *
 * @param gattc_if GATT client interface to use
 * @param conn_id Connection ID of the connected device
 * @param server_bda BD address of the connected device
 * @param device_name Advertised device name (can be NULL)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ble_device_info_discover(esp_gatt_if_t gattc_if, uint16_t conn_id, const esp_bd_addr_t server_bda, const char *device_name);

/**
 * @brief Handle GATT client events (called from main GATT client callback)
 *
 * @param event GATT client event
 * @param gattc_if GATT client interface
 * @param param Event parameters
 */
void ble_device_info_handle_gattc_event(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);

/**
 * @brief Get the current device info (if available)
 *
 * @return const ble_device_info_t* Pointer to device info, NULL if not available
 */
const ble_device_info_t* ble_device_info_get_current(void);

/**
 * @brief Print device info to log
 *
 * @param device_info Device info structure to print
 */
void ble_device_info_print(const ble_device_info_t *device_info);

#ifdef __cplusplus
}
#endif