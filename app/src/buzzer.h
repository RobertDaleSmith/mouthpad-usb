/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BUZZER_H_
#define BUZZER_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the passive buzzer
 * 
 * @return 0 on success, negative error code on failure
 */
int buzzer_init(void);

/**
 * @brief Generate a click sound for mouse clicks (default)
 * 
 * Short, sharp click sound for tactile feedback
 */
void buzzer_click(void);

/**
 * @brief Generate a left click sound
 * 
 * Higher pitched, short duration sound for left clicks
 */
void buzzer_click_left(void);

/**
 * @brief Generate a right click sound
 * 
 * Lower pitched, slightly longer sound for right clicks
 */
void buzzer_click_right(void);

/**
 * @brief Generate a double click sound
 * 
 * Two quick high-pitched beeps
 */
void buzzer_click_double(void);

/**
 * @brief Generate a mechanical click sound
 * 
 * Low pitched, very short sound mimicking mechanical switches
 */
void buzzer_click_mechanical(void);

/**
 * @brief Generate a pop click sound
 * 
 * Quick frequency sweep from high to low
 */
void buzzer_click_pop(void);

/**
 * @brief Generate a happy connection success sound
 * 
 * Rising melody to indicate successful connection
 */
void buzzer_connected(void);

/**
 * @brief Generate a sad disconnection sound
 * 
 * Falling melody to indicate device disconnection (opposite of connected sound)
 */
void buzzer_disconnected(void);

/**
 * @brief Generate a beep tone
 * 
 * @param frequency_hz Frequency in Hz (100-5000 Hz recommended)
 * @param duration_ms Duration in milliseconds
 */
void buzzer_beep(uint32_t frequency_hz, uint32_t duration_ms);

/**
 * @brief Stop any current buzzer sound
 */
void buzzer_stop(void);

/**
 * @brief Check if buzzer is available
 * 
 * @return true if buzzer is initialized and ready, false otherwise
 */
bool buzzer_is_available(void);

#ifdef __cplusplus
}
#endif

#endif /* BUZZER_H_ */