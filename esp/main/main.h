#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Clear all BLE bonds and disconnect active device
 *
 * This performs a full bond reset:
 * 1. Disconnects any currently connected BLE device
 * 2. Clears all stored bonds from NVS
 * 3. Clears cached device info
 * 4. Sets LED state to scanning
 *
 * Can be called from CDC reset command, button long press,
 * or protobuf ClearBondsWrite command.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t perform_bond_reset(void);

/**
 * @brief Check if BLE connection is fully ready to be reported as Connected
 *
 * Returns true when:
 * - NUS service is ready AND we have cached device info (fast path), OR
 * - DIS discovery is complete with firmware version (slow path)
 *
 * This allows reporting Connected state as soon as NUS is ready when we have
 * cached device info, without waiting for DIS discovery.
 *
 * @return true if connection should be reported as Connected
 */
bool ble_connection_is_fully_ready(void);

/**
 * @brief Callback invoked when NUS service is fully ready
 *
 * This is called from transport_uart when NUS CCCD write succeeds
 * and notifications are enabled. It checks if we have cached device info
 * and sets the connection state accordingly.
 */
void nus_ready_callback(void);

#ifdef __cplusplus
}
#endif
