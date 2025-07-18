#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>
#include <stdbool.h>
#include "nrf_gpio.h"
#include "nrf_delay.h"

// LED control functions
void utils_led_init(void);
void utils_led_set(uint32_t pin, bool state);
void utils_led_toggle(uint32_t pin);
void utils_led_ble_connected(bool connected);
void utils_led_usb_connected(bool connected);
void utils_led_error(bool error);

// Error handling
void utils_error_handler(uint32_t error_code, uint32_t line_num, const uint8_t * p_file_name);
void utils_assert_handler(uint32_t error_code, uint32_t line_num, const uint8_t * p_file_name);

// Data conversion utilities
void utils_hex_dump(const uint8_t *data, uint16_t len);
void utils_print_address(const uint8_t *addr);
void utils_print_uuid(const uint8_t *uuid, uint8_t len);

// String utilities
uint16_t utils_strlen(const char *str);
void utils_strcpy(char *dst, const char *src);
bool utils_strcmp(const char *str1, const char *str2);

// Time utilities
uint32_t utils_get_timestamp(void);
void utils_delay_ms(uint32_t ms);

// CRC calculation
uint16_t utils_crc16(const uint8_t *data, uint16_t len);

#endif // UTILS_H