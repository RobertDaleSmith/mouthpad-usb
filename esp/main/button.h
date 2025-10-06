#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Button event types
 */
typedef enum {
    BUTTON_EVENT_SINGLE_CLICK,
    BUTTON_EVENT_DOUBLE_CLICK,
    BUTTON_EVENT_LONG_PRESS
} button_event_t;

/**
 * @brief Button event callback function type
 *
 * @param event The button event that occurred
 */
typedef void (*button_event_callback_t)(button_event_t event);

/**
 * @brief Initialize the button module
 *
 * @param callback Callback function to handle button events
 * @return esp_err_t ESP_OK on success
 */
esp_err_t button_init(button_event_callback_t callback);

/**
 * @brief Deinitialize the button module
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t button_deinit(void);

#ifdef __cplusplus
}
#endif