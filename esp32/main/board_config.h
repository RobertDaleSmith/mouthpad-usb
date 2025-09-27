#pragma once

#include "sdkconfig.h"
#include "driver/gpio.h"

// Board configuration from Kconfig
#define BOARD_NAME              CONFIG_MOUTHPAD_BOARD_NAME
#define BOARD_BUTTON_GPIO       (gpio_num_t)CONFIG_MOUTHPAD_BUTTON_GPIO

// LED GPIO - only define if valid GPIO number
#if CONFIG_MOUTHPAD_LED_GPIO >= 0
    #define BOARD_LED_GPIO          CONFIG_MOUTHPAD_LED_GPIO
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