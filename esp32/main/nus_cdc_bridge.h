#pragma once

#include "esp_err.h"
#include "esp_gattc_api.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the NUS to CDC bridge
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t nus_cdc_bridge_init(void);

/**
 * @brief Start the bridge service
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t nus_cdc_bridge_start(void);

/**
 * @brief Stop the bridge service
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t nus_cdc_bridge_stop(void);

/**
 * @brief Check if the bridge is active
 * 
 * @return true if active, false otherwise
 */
bool nus_cdc_bridge_is_active(void);

/**
 * @brief Trigger NUS service discovery on a connected device
 * 
 * @param gattc_if GATT client interface to use
 * @param conn_id Connection ID of the connected device
 * @return esp_err_t ESP_OK on success
 */
esp_err_t nus_cdc_bridge_discover_services(esp_gatt_if_t gattc_if, uint16_t conn_id);

/**
 * @brief Handle GATT client events (called from main GATT client callback)
 * 
 * @param event GATT client event
 * @param gattc_if GATT client interface
 * @param param Event parameters
 */
void nus_cdc_bridge_handle_gattc_event(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);

#ifdef __cplusplus
}
#endif
