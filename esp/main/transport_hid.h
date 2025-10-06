#pragma once

#include "esp_err.h"
#include "esp_hidh.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the HID transport bridge (BLE HID to USB HID)
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t transport_hid_init(void);

/**
 * @brief Start the HID bridge service
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t transport_hid_start(void);

/**
 * @brief Stop the HID bridge service
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t transport_hid_stop(void);

/**
 * @brief Check if the HID bridge is active
 *
 * @return true if active, false otherwise
 */
bool transport_hid_is_active(void);

/**
 * @brief Set the active HID device for the bridge
 *
 * @param dev HID device handle
 * @param bda Bluetooth device address
 * @return esp_err_t ESP_OK on success
 */
esp_err_t transport_hid_set_device(esp_hidh_dev_t *dev, const uint8_t *bda);

/**
 * @brief Clear the active HID device
 */
void transport_hid_clear_device(void);

/**
 * @brief Get the active device's Bluetooth address
 *
 * @param addr Buffer to store the address (6 bytes)
 * @return ESP_OK if active device exists, ESP_ERR_INVALID_STATE if no active device
 */
esp_err_t transport_hid_get_active_address(uint8_t *addr);

/**
 * @brief Handle device disconnection and release all stuck inputs
 *
 * This function releases any stuck HID inputs (buttons, movement, scroll)
 * by sending neutral reports to USB HID. Call when BLE device disconnects.
 */
void transport_hid_handle_disconnect(void);

/**
 * @brief Handle HID input report (called from BLE HID client)
 *
 * @param report_id HID report ID
 * @param data Report data
 * @param length Data length
 * @return esp_err_t ESP_OK on success
 */
esp_err_t transport_hid_handle_input(uint8_t report_id, const uint8_t *data, uint16_t length);

#ifdef __cplusplus
}
#endif