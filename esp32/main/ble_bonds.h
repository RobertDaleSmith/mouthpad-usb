#pragma once

#include "esp_err.h"
#include "esp_bt_defs.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the BLE bonding system
 *
 * This module manages pairing with a single MouthPad device and stores
 * the bond information persistently.
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ble_bonds_init(void);

/**
 * @brief Check if we have a bonded device
 *
 * @return true if we have a bonded device stored
 */
bool ble_bonds_has_bonded_device(void);

/**
 * @brief Get the bonded device address
 *
 * @param[out] bda Buffer to store the bonded device address
 * @return esp_err_t ESP_OK if bonded device exists, ESP_ERR_NOT_FOUND if no bond
 */
esp_err_t ble_bonds_get_bonded_device(esp_bd_addr_t bda);

/**
 * @brief Check if a device address matches our bonded device
 *
 * @param bda Device address to check
 * @return true if this device matches our bonded device
 */
bool ble_bonds_is_bonded_device(const esp_bd_addr_t bda);

/**
 * @brief Store a new bonded device (replaces any existing bond)
 *
 * @param bda Device address to bond with
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ble_bonds_store_device(const esp_bd_addr_t bda);

/**
 * @brief Clear all stored bonds
 *
 * This removes the bonded device from storage and clears all BLE bonds.
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ble_bonds_clear_all(void);

/**
 * @brief Get bonded device info as string for debugging
 *
 * @param[out] buffer Buffer to store the string (should be at least 24 bytes)
 * @param buffer_size Size of the buffer
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ble_bonds_get_info_string(char *buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif