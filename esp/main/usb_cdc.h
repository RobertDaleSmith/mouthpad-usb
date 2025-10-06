#pragma once

#include "esp_err.h"
#include "stdint.h"
#include "stdbool.h"

#ifdef __cplusplus
extern "C" {
#endif

// Callback function types
typedef void (*usb_cdc_data_received_cb_t)(const uint8_t *data, uint16_t len);
typedef void (*usb_cdc_data_sent_cb_t)(esp_err_t status);

// USB CDC configuration
typedef struct {
    usb_cdc_data_received_cb_t data_received_cb;
    usb_cdc_data_sent_cb_t data_sent_cb;
} usb_cdc_config_t;

/**
 * @brief Initialize USB CDC with configuration
 * 
 * @param config Configuration structure with callback functions
 * @return esp_err_t ESP_OK on success
 */
esp_err_t usb_cdc_init(const usb_cdc_config_t *config);

/**
 * @brief Send data through USB CDC with packet framing
 * 
 * @param data Data to send
 * @param len Length of data
 * @return esp_err_t ESP_OK on success
 */
esp_err_t usb_cdc_send_data(const uint8_t *data, uint16_t len);

/**
 * @brief Check if USB CDC is ready for communication
 * 
 * @return true if ready, false otherwise
 */
bool usb_cdc_is_ready(void);

/**
 * @brief Update CDC callbacks after initialization
 * 
 * @param config Configuration structure with new callback functions
 * @return esp_err_t ESP_OK on success
 */
esp_err_t usb_cdc_update_callbacks(const usb_cdc_config_t *config);

/**
 * @brief Calculate CRC-16 (CCITT) for data integrity checking
 * 
 * @param data Data to calculate CRC for
 * @param len Length of data
 * @return uint16_t CRC-16 value
 */
uint16_t usb_cdc_calculate_crc16(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif
