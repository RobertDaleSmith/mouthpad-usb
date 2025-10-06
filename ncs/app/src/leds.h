/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LEDS_H_
#define LEDS_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LED states for visual feedback
 */
typedef enum {
    LED_STATE_OFF,           /**< LEDs completely off */
    LED_STATE_SCANNING,      /**< Blue blinking - scanning for devices */
    LED_STATE_CONNECTED,     /**< Solid battery-aware color - device connected */
    LED_STATE_DATA_ACTIVITY, /**< Fast flicker battery-aware color - data transfer */
} led_state_t;

/**
 * @brief Initialize the LED subsystem
 * 
 * Detects and configures available LEDs (NeoPixel or GPIO)
 * 
 * @return 0 on success, negative error code on failure
 */
int leds_init(void);

/**
 * @brief Set the current LED state
 * 
 * @param state The desired LED state
 * @return 0 on success, negative error code on failure
 */
int leds_set_state(led_state_t state);

/**
 * @brief Update LED animation/timing
 * 
 * Should be called periodically from main loop to handle
 * blinking and flicker animations
 * 
 * @return 0 on success, negative error code on failure
 */
int leds_update(void);

/**
 * @brief Check if LED subsystem is available
 * 
 * @return true if LEDs are available and initialized, false otherwise
 */
bool leds_is_available(void);

/**
 * @brief Set battery color mode for connected states
 * 
 * @param mode BAS_COLOR_MODE_DISCRETE or BAS_COLOR_MODE_GRADIENT
 */
void leds_set_battery_color_mode(uint8_t mode);

/**
 * @brief Check if NeoPixel LEDs are being used
 * 
 * @return true if NeoPixel LEDs are available, false if using GPIO LEDs
 */
bool leds_has_neopixel(void);

#ifdef __cplusplus
}
#endif

#endif /* LEDS_H_ */