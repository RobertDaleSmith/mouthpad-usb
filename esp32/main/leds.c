#include "leds.h"

#include "esp_err.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define TAG "leds"

#ifndef CONFIG_USER_LED_GPIO
#define CONFIG_USER_LED_GPIO 21
#endif

#define SCAN_BLINK_INTERVAL_US    800000  /* 0.8s */
#define ACTIVITY_OFF_US           10000   /* LED off duration during activity */
#define ACTIVITY_ON_US            30000   /* LED on duration after pulse */
#define LED_UPDATE_PERIOD_US      50000   /* 50ms */

static bool s_available;
static gpio_num_t s_gpio = CONFIG_USER_LED_GPIO;
static leds_state_t s_state = LED_STATE_OFF;
static bool s_output_level;
static const int s_hw_on_level = 0;
static const int s_hw_off_level = 1 - s_hw_on_level;

static bool s_scanning_phase;
static int64_t s_scanning_toggle_us;

static bool s_activity_pending;
static bool s_activity_phase_on;
static int64_t s_activity_toggle_us;

static esp_timer_handle_t s_timer;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void leds_apply(bool level)
{
    if (!s_available) {
        return;
    }
    if (s_output_level == level) {
        return;
    }
    gpio_set_level(s_gpio, level ? s_hw_on_level : s_hw_off_level);
    s_output_level = level;
}

static void leds_update_task(void *arg)
{
    (void)arg;
    if (!s_available) {
        return;
    }

    int64_t now = esp_timer_get_time();
    leds_state_t state;
    bool activity;

    portENTER_CRITICAL(&s_lock);
    state = s_state;
    activity = s_activity_pending;
    bool phase_on = s_activity_phase_on;
    portEXIT_CRITICAL(&s_lock);

    switch (state) {
    case LED_STATE_OFF:
        leds_apply(false);
        break;

    case LED_STATE_SCANNING:
        if (now - s_scanning_toggle_us >= SCAN_BLINK_INTERVAL_US) {
            s_scanning_phase = !s_scanning_phase;
            s_scanning_toggle_us = now;
        }
        leds_apply(s_scanning_phase);
        break;

    case LED_STATE_CONNECTED:
    default:
        if (activity) {
            if (!phase_on) {
                leds_apply(false);
                if (now - s_activity_toggle_us >= ACTIVITY_OFF_US) {
                    portENTER_CRITICAL(&s_lock);
                    s_activity_phase_on = true;
                    s_activity_toggle_us = now;
                    portEXIT_CRITICAL(&s_lock);
                }
            } else {
                leds_apply(true);
                if (now - s_activity_toggle_us >= ACTIVITY_ON_US) {
                    portENTER_CRITICAL(&s_lock);
                    s_activity_pending = false;
                    s_activity_phase_on = true;
                    portEXIT_CRITICAL(&s_lock);
                }
            }
        } else {
            leds_apply(true);
        }
        break;
    }
}

esp_err_t leds_init(void)
{
    if (s_available) {
        return ESP_OK;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << s_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LED GPIO %d: %s", s_gpio, esp_err_to_name(err));
        return err;
    }

    int64_t now = esp_timer_get_time();
    gpio_set_level(s_gpio, s_hw_off_level);
    s_output_level = false;
    s_scanning_phase = false;
    s_scanning_toggle_us = now;
    s_activity_pending = false;
    s_activity_phase_on = true;
    s_activity_toggle_us = now;

    esp_timer_create_args_t args = {
        .callback = leds_update_task,
        .name = "led_tick",
    };
    err = esp_timer_create(&args, &s_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LED timer: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_timer_start_periodic(s_timer, LED_UPDATE_PERIOD_US);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start LED timer: %s", esp_err_to_name(err));
        esp_timer_delete(s_timer);
        s_timer = NULL;
        return err;
    }

    s_available = true;
    ESP_LOGI(TAG, "Single-colour LED initialised on GPIO %d", s_gpio);
    return ESP_OK;
}

void leds_set_state(leds_state_t state)
{
    if (!s_available) {
        return;
    }
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    s_state = state;
    if (state == LED_STATE_SCANNING) {
        s_scanning_phase = true;
        s_scanning_toggle_us = now;
    }
    s_activity_pending = false;
    s_activity_phase_on = true;
    portEXIT_CRITICAL(&s_lock);
}

void leds_notify_activity(void)
{
    if (!s_available) {
        return;
    }
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    if (s_state == LED_STATE_CONNECTED && !s_activity_pending) {
        s_activity_pending = true;
        s_activity_phase_on = false;
        s_activity_toggle_us = now;
    }
    portEXIT_CRITICAL(&s_lock);
}

bool leds_is_available(void)
{
    return s_available;
}
