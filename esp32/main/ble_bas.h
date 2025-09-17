#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ble_bas_init(void);
void ble_bas_reset(void);
void ble_bas_handle_level(uint8_t level);
bool ble_bas_is_ready(void);
uint8_t ble_bas_get_battery_level(void);

typedef enum {
    BLE_BAS_COLOR_MODE_DISCRETE,
    BLE_BAS_COLOR_MODE_GRADIENT,
} ble_bas_color_mode_t;

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} ble_bas_rgb_color_t;

ble_bas_rgb_color_t ble_bas_get_battery_color(ble_bas_color_mode_t mode);

#ifdef __cplusplus
}
#endif

