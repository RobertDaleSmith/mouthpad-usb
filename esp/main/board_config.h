#pragma once

#include "sdkconfig.h"
#include "driver/gpio.h"

// Board configuration from Kconfig
#define BOARD_NAME              CONFIG_MOUTHPAD_BOARD_NAME
#define BOARD_BUTTON_GPIO       (gpio_num_t)CONFIG_MOUTHPAD_BUTTON_GPIO

// LED defaults per board, with optional override from Kconfig
#if CONFIG_MOUTHPAD_LED_GPIO >= 0
    #define BOARD_LED_GPIO          (gpio_num_t)CONFIG_MOUTHPAD_LED_GPIO
    #define BOARD_LED_ACTIVE_LOW    CONFIG_MOUTHPAD_LED_ACTIVE_LOW
#elif CONFIG_MOUTHPAD_BOARD_XIAO_ESP32S3
    #define BOARD_LED_GPIO          GPIO_NUM_21
    #define BOARD_LED_ACTIVE_LOW    1
#elif CONFIG_MOUTHPAD_BOARD_LILYGO_T_DISPLAY_S3
    /* No user LED available */
#endif

// Board-specific pin definitions for T-Display-S3
#ifdef CONFIG_MOUTHPAD_BOARD_LILYGO_T_DISPLAY_S3
    // Display pins (reserved for future display implementation)
    #define BOARD_LCD_MOSI          GPIO_NUM_13
    #define BOARD_LCD_SCLK          GPIO_NUM_12
    #define BOARD_LCD_CS            GPIO_NUM_10
    #define BOARD_LCD_DC            GPIO_NUM_11
    #define BOARD_LCD_RST           GPIO_NUM_9
    #define BOARD_LCD_BL            GPIO_NUM_15  // Display backlight control
    #define BOARD_PWR_EN            GPIO_NUM_15  // Power enable
#endif
