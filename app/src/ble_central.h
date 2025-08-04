#ifndef BLE_CENTRAL_H
#define BLE_CENTRAL_H

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <bluetooth/scan.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize BLE Central functionality
 *
 * @return 0 on success, negative error code on failure
 */
int ble_central_init(void);

/**
 * @brief Start scanning for BLE devices
 *
 * @return 0 on success, negative error code on failure
 */
int ble_central_start_scan(void);

/**
 * @brief Stop scanning for BLE devices
 *
 * @return 0 on success, negative error code on failure
 */
int ble_central_stop_scan(void);

/**
 * @brief Get the default connection
 *
 * @return Pointer to the default connection, or NULL if not connected
 */
struct bt_conn *ble_central_get_default_conn(void);

/**
 * @brief Set the default connection
 *
 * @param conn Connection to set as default
 */
void ble_central_set_default_conn(struct bt_conn *conn);

/**
 * @brief Get the authentication connection
 *
 * @return Pointer to the authentication connection, or NULL if not in auth
 */
struct bt_conn *ble_central_get_auth_conn(void);

/**
 * @brief Set the authentication connection
 *
 * @param conn Connection to set as auth connection
 */
void ble_central_set_auth_conn(struct bt_conn *conn);

/**
 * @brief Initialize BLE Central connection callbacks
 *
 * @return 0 on success, negative error code on failure
 */
int ble_central_init_callbacks(void);

/**
 * @brief Start GATT discovery on a connection
 *
 * @param conn Connection to discover services on
 * @return 0 on success, negative error code on failure
 */
int ble_central_gatt_discover(struct bt_conn *conn);

/**
 * @brief Handle numeric comparison reply for pairing
 *
 * @param accept Whether to accept or reject the pairing
 */
void ble_central_num_comp_reply(bool accept);

/**
 * @brief Initialize BLE Central authentication callbacks
 *
 * @return 0 on success, negative error code on failure
 */
int ble_central_init_auth_callbacks(void);

/**
 * @brief Handle button events for BLE Central functionality
 *
 * @param button_state The button state
 * @param has_changed Which buttons have changed
 */
void ble_central_handle_buttons(uint32_t button_state, uint32_t has_changed);

#ifdef __cplusplus
}
#endif

#endif /* BLE_CENTRAL_H */ 