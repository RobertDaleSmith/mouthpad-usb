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
 * @brief Load device information for a specific bonded device address
 *
 * @param addr BLE address of the bonded device
 * @param out_info Output structure to fill with device info
 * @return 0 on success, negative error code if not found or on failure
 */
int ble_dis_load_info_for_addr(const bt_addr_le_t *addr, ble_dis_info_t *out_info);

/**
 * @brief Clear saved device information for a specific address
 *
 * @param addr BLE address of the device to clear
 */
void ble_dis_clear_saved_for_addr(const bt_addr_le_t *addr);

/**
 * @brief Clear all saved device information from persistent storage
 *
 * This should be called when bonds are cleared to remove cached DIS info.
 */
void ble_dis_clear_saved(void);

/**
 * @brief Clear cached firmware version for a specific device
 *
 * Loads the cached DIS info, clears only the firmware version field,
 * and saves it back. This forces firmware version to be re-read on
 * next connection (useful after MouthPad firmware update).
 *
 * @param addr BLE address of the device
 */
void ble_dis_clear_cached_firmware_for_addr(const bt_addr_le_t *addr);

/**
 * @brief Clear cached firmware version for all bonded devices
 *
 * This should be called when initiating a firmware update flow to ensure
 * the dongle re-reads firmware version on next connection.
 */
void ble_dis_clear_all_cached_firmware(void);

/**
 * @brief Load cached DIS info from in-memory cache for connected device
 *
 * Called when device connects to populate device_info from the in-memory cache.
 * The in-memory cache is loaded from flash at boot.
 *
 * @param addr BLE address of the connected device
 */
void ble_dis_load_cache_for_connected_device(const bt_addr_le_t *addr);

/**
 * @brief Check if we have cached device info with firmware version
 *
 * This is used to determine if we can report Connected state early
 * (when NUS is ready) or if we need to wait for DIS to retrieve firmware version.
 *
 * @return true if cached device info exists with valid firmware version
 */
bool ble_dis_has_cached_firmware(void);

/**
 * @brief Callback type for DIS discovery completion
 *
 * @param conn BLE connection
 */
typedef void (*ble_dis_discovery_complete_cb_t)(struct bt_conn *conn);

/**
 * @brief Set callback to be called when DIS discovery completes
 *
 * @param cb Callback function
 */
void ble_dis_set_discovery_complete_cb(ble_dis_discovery_complete_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* BLE_DIS_H_ */
