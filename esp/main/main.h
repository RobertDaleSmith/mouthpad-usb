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

#ifdef __cplusplus
}
#endif
