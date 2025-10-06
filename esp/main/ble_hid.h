#pragma once

#include "esp_err.h"
#include "esp_hidh.h"
#include "esp_event.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// BLE HID client configuration structure
typedef struct {
    void (*connected_cb)(esp_hidh_dev_t *dev, const uint8_t *bda);
    void (*disconnected_cb)(esp_hidh_dev_t *dev);
    void (*input_cb)(uint8_t report_id, const uint8_t *data, uint16_t length);
    void (*feature_cb)(esp_hidh_dev_t *dev, uint8_t report_id, const uint8_t *data, uint16_t length, esp_hid_usage_t usage);
    void (*battery_cb)(uint8_t level, esp_err_t status);
} ble_hid_client_config_t;

/**
 * @brief Initialize the BLE HID client
 *
 * @param config Configuration structure with callbacks
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ble_hid_client_init(const ble_hid_client_config_t *config);

/**
 * @brief Handle ESP HID host events (called from main HID callback)
 *
 * @param handler_args Handler arguments (unused)
 * @param base Event base
 * @param id Event ID
 * @param event_data Event data
 */
void ble_hid_client_handle_event(void *handler_args, esp_event_base_t base, int32_t id, void *event_data);

/**
 * @brief Get the currently active HID device
 *
 * @return esp_hidh_dev_t* Active device or NULL if none
 */
esp_hidh_dev_t *ble_hid_client_get_active_device(void);

/**
 * @brief Check if a HID device is currently connected
 *
 * @return true if connected, false otherwise
 */
bool ble_hid_client_is_connected(void);

#ifdef __cplusplus
}
#endif