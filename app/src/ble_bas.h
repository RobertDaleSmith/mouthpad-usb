/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BLE_BAS_H_
#define BLE_BAS_H_

#include <zephyr/bluetooth/conn.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the Battery Service client
 * 
 * @return 0 on success, negative error code on failure
 */
int ble_bas_init(void);

/**
 * @brief Start Battery Service discovery on a BLE connection
 * 
 * @param conn BLE connection to discover Battery Service on
 * @return 0 on success, negative error code on failure
 */
int ble_bas_discover(struct bt_conn *conn);

/**
 * @brief Check if Battery Service is ready and operational
 * 
 * @return true if Battery Service is ready, false otherwise
 */
bool ble_bas_is_ready(void);

/**
 * @brief Reset Battery Service state on disconnect
 * 
 * Resets the battery service state and clears the battery level
 */
void ble_bas_reset(void);

/**
 * @brief Get the current battery level of the connected device
 * 
 * @return Battery level percentage (0-100), or 0xFF if invalid/unknown
 */
uint8_t ble_bas_get_battery_level(void);

/**
 * @brief Battery level color indication modes
 */
typedef enum {
	BAS_COLOR_MODE_DISCRETE,  /**< 4 discrete colors per quarter (green/yellow/orange/red) */
	BAS_COLOR_MODE_GRADIENT   /**< Smooth gradient from green to red based on percentage */
} ble_bas_color_mode_t;

/**
 * @brief RGB color structure for LED indication
 */
typedef struct {
	uint8_t red;    /**< Red component (0-255) */
	uint8_t green;  /**< Green component (0-255) */
	uint8_t blue;   /**< Blue component (0-255) */
} ble_bas_rgb_color_t;

/**
 * @brief Get LED color based on current battery level
 * 
 * @param mode Color mode (discrete quarters or smooth gradient)
 * @return RGB color structure representing the battery level
 */
ble_bas_rgb_color_t ble_bas_get_battery_color(ble_bas_color_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif /* BLE_BAS_H_ */