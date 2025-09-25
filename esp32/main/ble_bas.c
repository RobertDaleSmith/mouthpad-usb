#include "ble_bas.h"

#include "esp_log.h"

#define TAG "BLE_BAS"

static bool s_ready;
static uint8_t s_battery_level;

static inline uint8_t clamp_u8(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (uint8_t)value;
}

esp_err_t ble_bas_init(void)
{
    s_ready = false;
    s_battery_level = 0xFF;
    ESP_LOGI(TAG, "Battery Service helper initialised");
    return ESP_OK;
}

void ble_bas_reset(void)
{
    s_ready = false;
    s_battery_level = 0xFF;
    ESP_LOGI(TAG, "Battery Service reset");
}

void ble_bas_handle_level(uint8_t level)
{
    if (level == 0xFF) {
        ESP_LOGW(TAG, "Invalid battery level received");
        s_ready = false;
        s_battery_level = 0xFF;
        return;
    }

    s_ready = true;
    s_battery_level = level;
    ESP_LOGI(TAG, "Battery level: %u%%", level);
}

bool ble_bas_is_ready(void)
{
    return s_ready;
}

uint8_t ble_bas_get_battery_level(void)
{
    return s_battery_level;
}

ble_bas_rgb_color_t ble_bas_get_battery_color(ble_bas_color_mode_t mode)
{
    ble_bas_rgb_color_t color = {0, 0, 0};

    if (!s_ready || s_battery_level == 0xFF || s_battery_level > 100) {
        color.green = 255;
        return color;
    }

    if (mode == BLE_BAS_COLOR_MODE_DISCRETE) {
        if (s_battery_level >= 50) {
            color.green = 255;
        } else if (s_battery_level >= 10) {
            color.red = 255;
            color.green = 255;
        } else {
            color.red = 255;
        }
    } else {
        if (s_battery_level >= 50) {
            color.green = 255;
            int red = (255 * (100 - s_battery_level)) / 50;
            color.red = clamp_u8(red);
        } else {
            color.red = 255;
            int green = (255 * s_battery_level) / 50;
            color.green = clamp_u8(green);
        }
    }

    return color;
}

