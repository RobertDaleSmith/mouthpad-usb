/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OLED_DISPLAY_H_
#define OLED_DISPLAY_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the OLED display
 * 
 * @return 0 on success, negative error code on failure
 */
int oled_display_init(void);

/**
 * @brief Clear the entire display
 * 
 * @return 0 on success, negative error code on failure
 */
int oled_display_clear(void);

/**
 * @brief Update the display with current battery level and connection status
 * 
 * @param battery_level Battery percentage (0-100), or 0xFF if unknown
 * @param is_connected Whether BLE device is connected
 * @return 0 on success, negative error code on failure
 */
int oled_display_update_status(uint8_t battery_level, bool is_connected);

/**
 * @brief Display a simple message on the OLED
 * 
 * @param message Message to display (null-terminated string)
 * @return 0 on success, negative error code on failure
 */
int oled_display_message(const char *message);

/**
 * @brief Display device information screen
 * 
 * @param device_name Connected device name (or "Not Connected")
 * @param connection_count Number of successful connections since boot
 * @return 0 on success, negative error code on failure
 */
int oled_display_device_info(const char *device_name, uint32_t connection_count);

/**
 * @brief Check if OLED display is available and ready for use
 * 
 * @return true if display is available, false otherwise
 */
bool oled_display_is_available(void);

#ifdef __cplusplus
}
#endif

#endif /* OLED_DISPLAY_H_ */