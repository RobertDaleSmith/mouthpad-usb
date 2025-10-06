/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BUTTON_H_
#define BUTTON_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Button event types
 */
typedef enum {
    BUTTON_EVENT_NONE,         /**< No event */
    BUTTON_EVENT_CLICK,        /**< Single click */
    BUTTON_EVENT_DOUBLE_CLICK, /**< Double click */
    BUTTON_EVENT_HOLD,         /**< Long hold */
} button_event_t;

/**
 * @brief Button event callback function type
 * 
 * @param event The button event that occurred
 */
typedef void (*button_event_callback_t)(button_event_t event);

/**
 * @brief Initialize the button subsystem
 * 
 * Detects and configures available user button (XIAO expansion board or Feather)
 * 
 * @return 0 on success, negative error code on failure
 */
int button_init(void);

/**
 * @brief Register callback for button events
 * 
 * @param callback Function to call when button events occur
 */
void button_register_callback(button_event_callback_t callback);

/**
 * @brief Check if button is available
 * 
 * @return true if button is available and initialized, false otherwise
 */
bool button_is_available(void);

/**
 * @brief Update button state and process events
 * 
 * Should be called periodically from main loop to handle
 * button debouncing and event detection
 * 
 * @return 0 on success, negative error code on failure
 */
int button_update(void);

#ifdef __cplusplus
}
#endif

#endif /* BUTTON_H_ */