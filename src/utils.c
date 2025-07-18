#include "utils.h"
#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"
#include "app_error.h"
#include "app_error_weak.h"
#include <string.h>

static uint32_t g_timestamp = 0;

void utils_led_init(void)
{
    nrf_gpio_cfg_output(LED_BLE_CONNECTED);
    nrf_gpio_cfg_output(LED_USB_CONNECTED);
    nrf_gpio_cfg_output(LED_ERROR);
    
    // Initialize LEDs to off state
    nrf_gpio_pin_clear(LED_BLE_CONNECTED);
    nrf_gpio_pin_clear(LED_USB_CONNECTED);
    nrf_gpio_pin_clear(LED_ERROR);
}

void utils_led_set(uint32_t pin, bool state)
{
    if (state) {
        nrf_gpio_pin_set(pin);
    } else {
        nrf_gpio_pin_clear(pin);
    }
}

void utils_led_toggle(uint32_t pin)
{
    nrf_gpio_pin_toggle(pin);
}

void utils_led_ble_connected(bool connected)
{
    utils_led_set(LED_BLE_CONNECTED, connected);
}

void utils_led_usb_connected(bool connected)
{
    utils_led_set(LED_USB_CONNECTED, connected);
}

void utils_led_error(bool error)
{
    utils_led_set(LED_ERROR, error);
}

void utils_error_handler(uint32_t error_code, uint32_t line_num, const uint8_t * p_file_name)
{
    NRF_LOG_ERROR("Error %lu at line %lu in file %s", error_code, line_num, p_file_name);
    utils_led_error(true);
    
    // Infinite loop to halt execution
    while (1) {
        nrf_delay_ms(100);
    }
}

void utils_assert_handler(uint32_t error_code, uint32_t line_num, const uint8_t * p_file_name)
{
    NRF_LOG_ERROR("Assert failed: %lu at line %lu in file %s", error_code, line_num, p_file_name);
    utils_led_error(true);
    
    // Infinite loop to halt execution
    while (1) {
        nrf_delay_ms(100);
    }
}

void utils_hex_dump(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        NRF_LOG_RAW_INFO("%02X ", data[i]);
        if ((i + 1) % 16 == 0) {
            NRF_LOG_RAW_INFO("\r\n");
        }
    }
    if (len % 16 != 0) {
        NRF_LOG_RAW_INFO("\r\n");
    }
}

void utils_print_address(const uint8_t *addr)
{
    NRF_LOG_INFO("Address: %02X:%02X:%02X:%02X:%02X:%02X",
                 addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}

void utils_print_uuid(const uint8_t *uuid, uint8_t len)
{
    NRF_LOG_INFO("UUID: ");
    for (uint8_t i = 0; i < len; i++) {
        NRF_LOG_RAW_INFO("%02X", uuid[i]);
        if (i < len - 1) {
            NRF_LOG_RAW_INFO(":");
        }
    }
    NRF_LOG_RAW_INFO("\r\n");
}

uint16_t utils_strlen(const char *str)
{
    return strlen(str);
}

void utils_strcpy(char *dst, const char *src)
{
    strcpy(dst, src);
}

bool utils_strcmp(const char *str1, const char *str2)
{
    return strcmp(str1, str2) == 0;
}

uint32_t utils_get_timestamp(void)
{
    return g_timestamp;
}

void utils_delay_ms(uint32_t ms)
{
    nrf_delay_ms(ms);
}

uint16_t utils_crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc = crc >> 1;
            }
        }
    }
    
    return crc;
}