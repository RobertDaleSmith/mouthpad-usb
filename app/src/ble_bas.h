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

#ifdef __cplusplus
}
#endif

#endif /* BLE_BAS_H_ */