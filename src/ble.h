#ifndef BLE_H
#define BLE_H

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <bluetooth/services/hogp.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the BLE stack and start scanning for HID devices
 * 
 * This function initializes the Bluetooth stack, sets up scanning for HID devices,
 * configures authentication callbacks, and starts the scanning process.
 * 
 * @return 0 on success, negative error code on failure
 */
int ble_init(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_H */
