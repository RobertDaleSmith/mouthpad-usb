/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BLE_DIS_H_
#define BLE_DIS_H_

#include <zephyr/bluetooth/conn.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum lengths for DIS strings */
#define BLE_DIS_FIRMWARE_VERSION_MAX_LEN 64
#define BLE_DIS_DEVICE_NAME_MAX_LEN 64
#define BLE_DIS_HARDWARE_REVISION_MAX_LEN 64
#define BLE_DIS_MANUFACTURER_NAME_MAX_LEN 64

/**
 * @brief Device Information structure
 */
typedef struct {
	char firmware_version[BLE_DIS_FIRMWARE_VERSION_MAX_LEN];
	char device_name[BLE_DIS_DEVICE_NAME_MAX_LEN];
	char hardware_revision[BLE_DIS_HARDWARE_REVISION_MAX_LEN];
	char manufacturer_name[BLE_DIS_MANUFACTURER_NAME_MAX_LEN];
	uint16_t vendor_id;
	uint16_t product_id;
	bool has_firmware_version;
	bool has_device_name;
	bool has_hardware_revision;
	bool has_manufacturer_name;
	bool has_pnp_id;
} ble_dis_info_t;

/**
 * @brief Initialize the Device Information Service client
 *
 * @return 0 on success, negative error code on failure
 */
int ble_dis_init(void);

/**
 * @brief Start Device Information Service discovery on a BLE connection
 *
 * This will discover the DIS service and read all available characteristics
 * (Firmware Revision, Device Name, Hardware Revision, Manufacturer Name).
 *
 * @param conn BLE connection to discover DIS on
 * @return 0 on success, negative error code on failure
 */
int ble_dis_discover(struct bt_conn *conn);

/**
 * @brief Check if Device Information Service discovery is complete
 *
 * @return true if DIS discovery is complete and data is available, false otherwise
 */
bool ble_dis_is_ready(void);

/**
 * @brief Reset Device Information Service state on disconnect
 *
 * Clears all cached device information.
 */
void ble_dis_reset(void);

/**
 * @brief Get the cached device information
 *
 * @return Pointer to device information structure, or NULL if not available
 */
const ble_dis_info_t *ble_dis_get_info(void);

/**
 * @brief Clear saved device information from persistent storage
 *
 * This should be called when bonds are cleared to remove cached DIS info.
 */
void ble_dis_clear_saved(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_DIS_H_ */
