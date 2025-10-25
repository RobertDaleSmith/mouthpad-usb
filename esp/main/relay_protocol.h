#ifndef RELAY_PROTOCOL_H
#define RELAY_PROTOCOL_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the relay protocol handler
 *
 * Sets up protobuf message handling between USB CDC and BLE NUS.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t relay_protocol_init(void);

/**
 * @brief Process incoming data from USB CDC
 *
 * Decodes AppToRelayMessage and routes to appropriate handler.
 *
 * @param data Pointer to received data
 * @param len Length of received data
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t relay_protocol_handle_usb_data(const uint8_t *data, uint16_t len);

/**
 * @brief Process incoming data from BLE NUS
 *
 * Wraps data in PassThroughToApp and sends to USB CDC.
 *
 * @param data Pointer to received data
 * @param len Length of received data
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t relay_protocol_handle_ble_data(const uint8_t *data, uint16_t len);

/**
 * @brief Send a response message to the host via USB CDC
 *
 * Encodes RelayToAppMessage and sends via USB CDC with framing.
 *
 * @param message Pointer to RelayToAppMessage to send
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t relay_protocol_send_response(const void *message);

/**
 * @brief Notify relay protocol of BLE connection state change
 *
 * Updates internal connection state for status queries.
 *
 * @param connected true if connected, false if disconnected
 */
void relay_protocol_update_ble_connection(bool connected);

/**
 * @brief Notify relay protocol of BLE scanning state change
 *
 * Updates internal scanning state for status queries.
 *
 * @param scanning true if scanning, false if not scanning
 */
void relay_protocol_update_ble_scanning(bool scanning);

/**
 * @brief Update RSSI value for connected device
 *
 * Updates internal RSSI value for status queries.
 *
 * @param rssi RSSI value in dBm
 */
void relay_protocol_update_rssi(int32_t rssi);

#ifdef __cplusplus
}
#endif

#endif // RELAY_PROTOCOL_H
