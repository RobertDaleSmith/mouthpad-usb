/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>

#include "button.h"

#define LOG_MODULE_NAME button
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

/* Button timing constants */
#define DEBOUNCE_TIME_MS        50      /* Button debounce time */
#define DOUBLE_CLICK_TIMEOUT_MS 300     /* Max time between clicks for double-click */
#define HOLD_TIME_MS            2000    /* Time to trigger hold event */

/* Button detection - check for sw0 alias (standard user button) */
#if DT_NODE_EXISTS(DT_ALIAS(sw0)) && DT_NODE_HAS_PROP(DT_ALIAS(sw0), gpios)
#define HAS_USER_BUTTON 1
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
#else
#define HAS_USER_BUTTON 0
#endif

/* Button state tracking */
static bool button_ready = false;
static button_event_callback_t event_callback = NULL;
static bool pin_stuck_low = false;  /* Workaround for hardware issue */

/* Button state machine */
typedef enum {
    BUTTON_STATE_IDLE,
    BUTTON_STATE_PRESSED,
    BUTTON_STATE_RELEASED,
    BUTTON_STATE_WAIT_DOUBLE,
    BUTTON_STATE_HOLD_DETECTED,
} button_state_t;

static button_state_t button_state = BUTTON_STATE_IDLE;
static uint32_t button_timer = 0;
static uint32_t press_start_time = 0;
static bool button_pressed_raw = false;
static bool button_pressed_debounced = false;

/* Private function declarations */
static void process_button_state_machine(void);
static void trigger_button_event(button_event_t event);

int button_init(void)
{
    int ret;
    
    LOG_INF("Initializing user button...");
    
#if HAS_USER_BUTTON
    if (!gpio_is_ready_dt(&button)) {
        LOG_ERR("User button GPIO device not ready");
        return -ENODEV;
    }
    
    /* Configure GPIO with explicit pull-up */
    gpio_flags_t flags = GPIO_INPUT | GPIO_PULL_UP;
    ret = gpio_pin_configure_dt(&button, flags);
    if (ret != 0) {
        LOG_ERR("Failed to configure user button GPIO (err %d)", ret);
        return ret;
    }
    
    /* Give pull-up time to settle */
    k_sleep(K_MSEC(10));
    
    /* Test initial pin state and detect stuck-low condition */
    int initial_state = gpio_pin_get_dt(&button);
    LOG_INF("User button GPIO configured. Port: %p, Pin: %d, Initial state: %d", 
            button.port, button.pin, initial_state);
    
    /* Check if pin is stuck low (common hardware issue) */
    if (initial_state == 0) {
        LOG_WRN("Button pin appears stuck low - enabling workaround mode");
        pin_stuck_low = true;
    }
    
    button_ready = true;
    button_state = BUTTON_STATE_IDLE;
    button_timer = 0;
    
    LOG_INF("User button initialized successfully");
#else
    LOG_INF("No user button available on this board");
    return 0;  /* Not an error, just no button */
#endif
    
    return 0;
}

void button_register_callback(button_event_callback_t callback)
{
    event_callback = callback;
}

bool button_is_available(void)
{
    return button_ready;
}

int button_update(void)
{
    if (!button_ready) {
        return 0;  /* Silently ignore if no button */
    }
    
#if HAS_USER_BUTTON
    /* Read raw button state */
    int pin_state = gpio_pin_get_dt(&button);
    
    if (pin_stuck_low) {
        /* Workaround: if pin is stuck low, treat brief high pulses as button presses */
        button_pressed_raw = (pin_state == 1);  /* High pulse means pressed */
    } else {
        /* Normal operation: active low */
        button_pressed_raw = (pin_state == 0);  /* Active low - 0 means pressed */
    }
    
    /* Debug logging for troubleshooting */
    static uint32_t last_debug_time = 0;
    static int last_pin_state = -1;
    
    uint32_t current_time = k_uptime_get_32();
    
    /* Log when pin state changes or periodically */
    if (pin_state != last_pin_state || (current_time - last_debug_time) > 5000) {
        LOG_INF("BUTTON DEBUG: pin=%d, raw=%d, debounced=%d, state=%d", 
                pin_state, button_pressed_raw, button_pressed_debounced, button_state);
        last_pin_state = pin_state;
        last_debug_time = current_time;
    }
    
    /* Simple debouncing */
    static uint32_t last_debounce_time = 0;
    static bool last_raw_state = false;
    
    if (button_pressed_raw != last_raw_state) {
        last_debounce_time = k_uptime_get_32();
    }
    
    if ((k_uptime_get_32() - last_debounce_time) > DEBOUNCE_TIME_MS) {
        button_pressed_debounced = button_pressed_raw;
    }
    
    last_raw_state = button_pressed_raw;
    
    /* Process button state machine */
    process_button_state_machine();
#endif
    
    return 0;
}

/* Private function implementations */

static void process_button_state_machine(void)
{
    uint32_t current_time = k_uptime_get_32();
    static button_state_t last_state = BUTTON_STATE_IDLE;
    
    /* Log state changes */
    if (button_state != last_state) {
        LOG_INF("STATE CHANGE: %d -> %d", last_state, button_state);
        last_state = button_state;
    }
    
    switch (button_state) {
    case BUTTON_STATE_IDLE:
        if (button_pressed_debounced) {
            button_state = BUTTON_STATE_PRESSED;
            press_start_time = current_time;
            LOG_INF("=== BUTTON PRESSED - STARTING TIMER ===");
        }
        break;
        
    case BUTTON_STATE_PRESSED:
        if (!button_pressed_debounced) {
            /* Button released */
            uint32_t press_duration = current_time - press_start_time;
            LOG_INF("=== BUTTON RELEASED - Duration: %u ms ===", press_duration);
            
            if (press_duration >= HOLD_TIME_MS) {
                /* Hold was already detected, go back to idle */
                button_state = BUTTON_STATE_IDLE;
                LOG_INF("Released after hold detection");
            } else {
                /* Normal press/release - wait for potential double-click */
                button_state = BUTTON_STATE_WAIT_DOUBLE;
                button_timer = current_time;
                LOG_INF("Short press - waiting for double-click (%u ms)", press_duration);
            }
        } else {
            /* Still pressed - check for hold */
            uint32_t press_duration = current_time - press_start_time;
            if (press_duration >= HOLD_TIME_MS) {
                button_state = BUTTON_STATE_HOLD_DETECTED;
                trigger_button_event(BUTTON_EVENT_HOLD);
                LOG_INF("=== HOLD EVENT TRIGGERED - Duration: %u ms ===", press_duration);
            } else {
                /* Log progress towards hold every 500ms */
                static uint32_t last_progress_log = 0;
                if ((current_time - last_progress_log) > 500) {
                    LOG_DBG("Hold progress: %u/%u ms", press_duration, HOLD_TIME_MS);
                    last_progress_log = current_time;
                }
            }
        }
        break;
        
    case BUTTON_STATE_WAIT_DOUBLE:
        if (button_pressed_debounced) {
            /* Second press detected */
            button_state = BUTTON_STATE_PRESSED;
            press_start_time = current_time;
            trigger_button_event(BUTTON_EVENT_DOUBLE_CLICK);
            LOG_DBG("Double-click event triggered");
        } else if ((current_time - button_timer) >= DOUBLE_CLICK_TIMEOUT_MS) {
            /* Timeout - single click */
            button_state = BUTTON_STATE_IDLE;
            trigger_button_event(BUTTON_EVENT_CLICK);
            LOG_DBG("Single click event triggered");
        }
        break;
        
    case BUTTON_STATE_HOLD_DETECTED:
        if (!button_pressed_debounced) {
            /* Button released after hold */
            button_state = BUTTON_STATE_IDLE;
            LOG_DBG("Button released after hold");
        }
        break;
        
    case BUTTON_STATE_RELEASED:
        /* This state is currently unused - go back to idle */
        button_state = BUTTON_STATE_IDLE;
        break;
    }
}

static void trigger_button_event(button_event_t event)
{
    if (event_callback) {
        event_callback(event);
    }
}