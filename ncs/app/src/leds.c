/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>

#include "leds.h"
#include "ble_bas.h"

#define LOG_MODULE_NAME leds
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

/* NeoPixel support detection */
#if DT_NODE_EXISTS(DT_NODELABEL(neopixel)) && \
    (DT_SAME_NODE(DT_ALIAS(led0), DT_NODELABEL(neopixel)) || \
     DT_SAME_NODE(DT_ALIAS(led1), DT_NODELABEL(neopixel)) || \
     DT_SAME_NODE(DT_ALIAS(led2), DT_NODELABEL(neopixel)))
#define HAS_NEOPIXEL 1
static const struct device *neopixel_dev = DEVICE_DT_GET(DT_NODELABEL(neopixel));
static struct led_rgb neopixel_color = {0, 0, 0};
#else
#define HAS_NEOPIXEL 0
#endif

/* GPIO LED support detection */
#if !HAS_NEOPIXEL
#if DT_NODE_EXISTS(DT_ALIAS(led0)) && DT_NODE_HAS_PROP(DT_ALIAS(led0), gpios)
static const struct gpio_dt_spec led_red = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static bool led_red_available = false;
#endif
#if DT_NODE_EXISTS(DT_ALIAS(led1)) && DT_NODE_HAS_PROP(DT_ALIAS(led1), gpios)
static const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static bool led_green_available = false;
#endif
#if DT_NODE_EXISTS(DT_ALIAS(led2)) && DT_NODE_HAS_PROP(DT_ALIAS(led2), gpios)
static const struct gpio_dt_spec led_blue = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);
static bool led_blue_available = false;
#endif
#endif

/* LED state tracking */
static bool leds_ready = false;
static led_state_t current_state = LED_STATE_OFF;
static uint32_t animation_counter = 0;
static bool animation_phase = false;
static uint8_t battery_color_mode = BAS_COLOR_MODE_GRADIENT;

/* NeoPixel brightness control (0-255, default 25 for comfortable viewing) */
#if HAS_NEOPIXEL
static uint8_t neopixel_brightness = 25;
#endif

/* Private function declarations */
static void set_rgb_color(ble_bas_rgb_color_t color);
static void set_gpio_leds(ble_bas_rgb_color_t color);
static int init_gpio_leds(void);
#if HAS_NEOPIXEL
static void set_neopixel_color(ble_bas_rgb_color_t color);
static int init_neopixel(void);
#endif

int leds_init(void)
{
    int ret = 0;

    LOG_INF("Initializing LED system...");

#if HAS_NEOPIXEL
    /* Enable NeoPixel power via P1.14 (required for Adafruit Feather nRF52840) */
    const struct gpio_dt_spec neopixel_power = {
        .port = DEVICE_DT_GET(DT_NODELABEL(gpio1)),
        .pin = 14,
        .dt_flags = GPIO_ACTIVE_HIGH
    };

    if (gpio_is_ready_dt(&neopixel_power)) {
        ret = gpio_pin_configure_dt(&neopixel_power, GPIO_OUTPUT_ACTIVE);
        if (ret == 0) {
            gpio_pin_set_dt(&neopixel_power, 1);  /* Enable NeoPixel power */
            k_sleep(K_MSEC(10));  /* Brief power stabilization */
            LOG_INF("NeoPixel power enabled on P1.14");
        }
    }

    ret = init_neopixel();
    if (ret == 0) {
        leds_ready = true;
    }
#else
    ret = init_gpio_leds();
    if (ret == 0) {
        leds_ready = true;
    }
#endif
    
    if (!leds_ready) {
        LOG_WRN("No LEDs available - continuing without LED status indication");
        return 0;  /* Not a critical error */
    }
    
    LOG_INF("LED system initialized successfully");
    return 0;
}

int leds_set_state(led_state_t state)
{
    if (!leds_ready) {
        return 0;  /* Silently ignore if no LEDs */
    }
    
    if (state != current_state) {
        LOG_DBG("LED state change: %d -> %d", current_state, state);
        current_state = state;
        animation_counter = 0;
        animation_phase = false;
        
        /* Immediately apply state for non-animated states */
        if (state == LED_STATE_OFF) {
            ble_bas_rgb_color_t off_color = {0, 0, 0};
            set_rgb_color(off_color);
        } else if (state == LED_STATE_CONNECTED) {
            ble_bas_rgb_color_t battery_color = ble_bas_get_battery_color(battery_color_mode);
            set_rgb_color(battery_color);
        }
    }
    
    return 0;
}

int leds_update(void)
{
    if (!leds_ready) {
        return 0;  /* Silently ignore if no LEDs */
    }
    
    animation_counter++;
    
    switch (current_state) {
    case LED_STATE_OFF:
        /* Nothing to do */
        break;
        
    case LED_STATE_SCANNING:
        /* Blue blink every 500ms */
        if (animation_counter >= 500) {
            if (animation_phase) {
                ble_bas_rgb_color_t blue_color = {0, 0, 255};
                set_rgb_color(blue_color);
            } else {
                ble_bas_rgb_color_t off_color = {0, 0, 0};
                set_rgb_color(off_color);
            }
            animation_phase = !animation_phase;
            animation_counter = 0;
        }
        break;
        
    case LED_STATE_CONNECTED:
        /* Solid battery-aware color - already set in leds_set_state() */
        /* But update periodically in case battery level changed */
        if (animation_counter >= 1000) {
            ble_bas_rgb_color_t battery_color = ble_bas_get_battery_color(battery_color_mode);
            set_rgb_color(battery_color);
            animation_counter = 0;
        }
        break;
        
    case LED_STATE_DATA_ACTIVITY:
        /* Flicker every 50ms for better visibility */
        if (animation_counter % 50 == 0) {
            if (animation_phase) {
                ble_bas_rgb_color_t battery_color = ble_bas_get_battery_color(battery_color_mode);
                set_rgb_color(battery_color);
            } else {
                ble_bas_rgb_color_t off_color = {0, 0, 0};
                set_rgb_color(off_color);
            }
            animation_phase = !animation_phase;
        }
        break;
    }
    
    return 0;
}

bool leds_is_available(void)
{
    return leds_ready;
}

void leds_set_battery_color_mode(uint8_t mode)
{
    battery_color_mode = mode;
    LOG_INF("Battery LED color mode set to: %s", 
            (mode == BAS_COLOR_MODE_DISCRETE) ? "DISCRETE" : "GRADIENT");
}

bool leds_has_neopixel(void)
{
#if HAS_NEOPIXEL
    return true;
#else
    return false;
#endif
}

/* Private function implementations */

static void set_rgb_color(ble_bas_rgb_color_t color)
{
#if HAS_NEOPIXEL
    set_neopixel_color(color);
#else
    set_gpio_leds(color);
#endif
}

#if HAS_NEOPIXEL
static void set_neopixel_color(ble_bas_rgb_color_t color)
{
    if (!device_is_ready(neopixel_dev)) {
        return;
    }

    /* Scale colors by brightness to prevent blinding brightness */
    neopixel_color.r = (color.red * neopixel_brightness) / 255;
    neopixel_color.g = (color.green * neopixel_brightness) / 255;
    neopixel_color.b = (color.blue * neopixel_brightness) / 255;

    led_strip_update_rgb(neopixel_dev, &neopixel_color, 1);
}
#endif

__maybe_unused static void set_gpio_leds(ble_bas_rgb_color_t color)
{
#if !HAS_NEOPIXEL
    /* GPIO_ACTIVE_LOW in devicetree handles inversion automatically */

    /* Detect if multiple LEDs share the same GPIO pin (single LED board) */
    bool shared_led = false;
#if DT_NODE_EXISTS(DT_ALIAS(led0)) && DT_NODE_EXISTS(DT_ALIAS(led1)) && \
    DT_NODE_HAS_PROP(DT_ALIAS(led0), gpios) && DT_NODE_HAS_PROP(DT_ALIAS(led1), gpios)
    /* Check if red and green LEDs point to same port and pin at runtime */
    if (led_red_available && led_green_available &&
        led_red.port == led_green.port && led_red.pin == led_green.pin) {
        shared_led = true;
    }
#endif

    if (shared_led) {
        /* Single LED board - turn on if ANY color is active (OR logic) */
        bool led_on = (color.red > 0) || (color.green > 0) || (color.blue > 0);
#if DT_NODE_EXISTS(DT_ALIAS(led0)) && DT_NODE_HAS_PROP(DT_ALIAS(led0), gpios)
        if (led_red_available) {
            gpio_pin_set_dt(&led_red, led_on ? 1 : 0);
        }
#endif
    } else {
        /* Multi-LED board - control each LED independently */
#if DT_NODE_EXISTS(DT_ALIAS(led0)) && DT_NODE_HAS_PROP(DT_ALIAS(led0), gpios)
        if (led_red_available) {
            gpio_pin_set_dt(&led_red, color.red > 0 ? 1 : 0);
        }
#endif
#if DT_NODE_EXISTS(DT_ALIAS(led1)) && DT_NODE_HAS_PROP(DT_ALIAS(led1), gpios)
        if (led_green_available) {
            gpio_pin_set_dt(&led_green, color.green > 0 ? 1 : 0);
        }
#endif
#if DT_NODE_EXISTS(DT_ALIAS(led2)) && DT_NODE_HAS_PROP(DT_ALIAS(led2), gpios)
        if (led_blue_available) {
            gpio_pin_set_dt(&led_blue, color.blue > 0 ? 1 : 0);
        }
#endif
    }
#endif
}

#if HAS_NEOPIXEL
static int init_neopixel(void)
{
    if (!device_is_ready(neopixel_dev)) {
        LOG_WRN("NeoPixel device not ready");
        return -ENODEV;
    }

    LOG_INF("NeoPixel initialized successfully");

    /* Brief startup indication - short blue pulse */
    struct led_rgb startup_blue = {0, 0, (64 * neopixel_brightness) / 255};
    led_strip_update_rgb(neopixel_dev, &startup_blue, 1);
    k_sleep(K_MSEC(200));

    /* Turn off after startup indication */
    struct led_rgb off = {0, 0, 0};
    led_strip_update_rgb(neopixel_dev, &off, 1);

    return 0;
}
#endif

__maybe_unused static int init_gpio_leds(void)
{
#if !HAS_NEOPIXEL
    int ret = 0;
    bool any_led_available = false;

    LOG_INF("Checking GPIO LED devices...");

#if DT_NODE_EXISTS(DT_ALIAS(led0)) && DT_NODE_HAS_PROP(DT_ALIAS(led0), gpios)
    led_red_available = gpio_is_ready_dt(&led_red);
    if (led_red_available) {
        ret = gpio_pin_configure_dt(&led_red, GPIO_OUTPUT);
        if (ret != 0) {
            LOG_ERR("Failed to configure red LED (err %d)", ret);
            led_red_available = false;
        } else {
            gpio_pin_set_dt(&led_red, 0);  /* Start with LED off */
            any_led_available = true;
            LOG_INF("Red LED: port=%p, pin=%d", led_red.port, led_red.pin);
        }
    }
#endif

#if DT_NODE_EXISTS(DT_ALIAS(led1)) && DT_NODE_HAS_PROP(DT_ALIAS(led1), gpios)
    led_green_available = gpio_is_ready_dt(&led_green);
    if (led_green_available) {
        ret = gpio_pin_configure_dt(&led_green, GPIO_OUTPUT);
        if (ret != 0) {
            LOG_ERR("Failed to configure green LED (err %d)", ret);
            led_green_available = false;
        } else {
            gpio_pin_set_dt(&led_green, 0);  /* Start with LED off */
            any_led_available = true;
            LOG_INF("Green LED: port=%p, pin=%d", led_green.port, led_green.pin);
        }
    }
#endif

#if DT_NODE_EXISTS(DT_ALIAS(led2)) && DT_NODE_HAS_PROP(DT_ALIAS(led2), gpios)
    led_blue_available = gpio_is_ready_dt(&led_blue);
    if (led_blue_available) {
        ret = gpio_pin_configure_dt(&led_blue, GPIO_OUTPUT);
        if (ret != 0) {
            LOG_ERR("Failed to configure blue LED (err %d)", ret);
            led_blue_available = false;
        } else {
            gpio_pin_set_dt(&led_blue, 0);  /* Start with LED off */
            any_led_available = true;
            LOG_INF("Blue LED: port=%p, pin=%d", led_blue.port, led_blue.pin);
        }
    }
#endif

    LOG_INF("LED availability: red=%d, green=%d, blue=%d",
            led_red_available, led_green_available, led_blue_available);

    /* Check for shared LED configuration */
#if DT_NODE_EXISTS(DT_ALIAS(led0)) && DT_NODE_EXISTS(DT_ALIAS(led1)) && \
    DT_NODE_HAS_PROP(DT_ALIAS(led0), gpios) && DT_NODE_HAS_PROP(DT_ALIAS(led1), gpios)
    if (led_red_available && led_green_available &&
        led_red.port == led_green.port && led_red.pin == led_green.pin) {
        LOG_INF("Shared LED detected - using OR logic for single LED board");
    } else {
        LOG_INF("Multiple LEDs detected - using independent control");
    }
#endif

    if (any_led_available) {
        LOG_INF("GPIO LEDs configured successfully");
        return 0;
    } else {
        LOG_WRN("No GPIO LEDs available");
        return -ENODEV;
    }
#else
    return -ENOTSUP;
#endif
}