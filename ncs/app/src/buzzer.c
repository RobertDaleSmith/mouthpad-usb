/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>

#include "buzzer.h"

#define LOG_MODULE_NAME buzzer
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

/* PWM device and configuration */
static const struct device *pwm_dev;
static bool buzzer_ready = false;
static bool buzzer_available = false;

/* PWM channel for buzzer */
#define BUZZER_PWM_CHANNEL 0

/* Click sound parameters */
#define CLICK_FREQUENCY_HZ  2000    /* 2kHz click */
#define CLICK_DURATION_MS   5       /* 5ms short click */

/* Work queue for non-blocking buzzer operations */
static struct k_work_delayable buzzer_stop_work;

/* Function to stop buzzer after delay */
static void buzzer_stop_work_handler(struct k_work *work) __maybe_unused;
static void buzzer_stop_work_handler(struct k_work *work)
{
    if (buzzer_ready && pwm_dev) {
        pwm_set_dt(&(struct pwm_dt_spec){pwm_dev, BUZZER_PWM_CHANNEL, 0, PWM_POLARITY_NORMAL}, 0, 0);
        LOG_DBG("Buzzer stopped");
    }
}

int buzzer_init(void)
{
#if !defined(CONFIG_BOARD_XIAO_BLE)
    /* Buzzer only available on XIAO boards with expansion board */
    LOG_DBG("Buzzer not available on this board (XIAO only)");
    buzzer_available = false;
    buzzer_ready = false;
    return 0;
#else
    LOG_INF("Checking for passive buzzer...");

    /* Get PWM device for buzzer */
    pwm_dev = DEVICE_DT_GET(DT_ALIAS(buzzer_pwm));
    if (!device_is_ready(pwm_dev)) {
        LOG_INF("Buzzer PWM device not available - continuing without buzzer");
        buzzer_available = false;
        buzzer_ready = false;
        return 0;  /* Return success to continue boot process */
    }

    buzzer_available = true;
    LOG_INF("Buzzer PWM device detected, initializing...");

    /* Initialize work queue for delayed stop */
    k_work_init_delayable(&buzzer_stop_work, buzzer_stop_work_handler);

    /* Test buzzer with a short beep */
    buzzer_ready = true;
    buzzer_beep(1000, 100);  /* 1kHz for 100ms test */

    LOG_INF("Passive buzzer initialized successfully");
    return 0;
#endif
}

void buzzer_click(void)
{
    buzzer_click_left();  /* Default to left click sound */
}

void buzzer_click_left(void)
{
    if (!buzzer_available || !buzzer_ready) {
        return;  /* Silently skip if no buzzer */
    }
    
    /* Left click: Higher pitch, short duration */
    buzzer_beep(2500, 4);  /* 2.5kHz for 4ms */
    
    LOG_DBG("Left click buzz generated");
}

void buzzer_click_right(void)
{
    if (!buzzer_available || !buzzer_ready) {
        return;  /* Silently skip if no buzzer */
    }
    
    /* Right click: Lower pitch, slightly longer */
    buzzer_beep(1800, 6);  /* 1.8kHz for 6ms */
    
    LOG_DBG("Right click buzz generated");
}

void buzzer_click_double(void)
{
    if (!buzzer_available || !buzzer_ready) {
        return;  /* Silently skip if no buzzer */
    }
    
    /* Double click: Two short high-pitched beeps */
    buzzer_beep(3000, 3);     /* First beep: 3kHz for 3ms */
    k_sleep(K_MSEC(2));       /* 2ms gap */
    buzzer_beep(3000, 3);     /* Second beep: 3kHz for 3ms */
    
    LOG_DBG("Double click buzz generated");
}

void buzzer_click_mechanical(void)
{
    if (!buzzer_available || !buzzer_ready) {
        return;  /* Silently skip if no buzzer */
    }
    
    /* Mechanical click: Low pitched, very short */
    buzzer_beep(800, 2);   /* 800Hz for 2ms - mimics mechanical switch */
    
    LOG_DBG("Mechanical click buzz generated");
}

void buzzer_click_pop(void)
{
    if (!buzzer_available || !buzzer_ready) {
        return;  /* Silently skip if no buzzer */
    }
    
    /* Pop sound: Quick frequency sweep */
    buzzer_beep(2000, 1);   /* Start at 2kHz */
    k_sleep(K_USEC(500));   /* Tiny gap */
    buzzer_beep(1500, 1);   /* Drop to 1.5kHz */
    k_sleep(K_USEC(500));   /* Tiny gap */
    buzzer_beep(1000, 1);   /* Finish at 1kHz */
    
    LOG_DBG("Pop click buzz generated");
}

void buzzer_connected(void)
{
    if (!buzzer_available || !buzzer_ready) {
        return;  /* Silently skip if no buzzer */
    }
    
    /* Happy connection sound: Rising melody */
    buzzer_beep(800, 80);   /* Low C */
    k_sleep(K_MSEC(10));    /* Short gap */
    buzzer_beep(1000, 80);  /* E */
    k_sleep(K_MSEC(10));    /* Short gap */
    buzzer_beep(1200, 120); /* G - longer and higher */
    
    LOG_DBG("Connection success buzz generated");
}

void buzzer_disconnected(void)
{
    if (!buzzer_available || !buzzer_ready) {
        return;  /* Silently skip if no buzzer */
    }
    
    /* Sad disconnection sound: Falling melody (opposite of connected) */
    buzzer_beep(1200, 80);  /* High G */
    k_sleep(K_MSEC(10));    /* Short gap */
    buzzer_beep(1000, 80);  /* E */
    k_sleep(K_MSEC(10));    /* Short gap */
    buzzer_beep(800, 120);  /* Low C - longer and lower */
    
    LOG_DBG("Disconnection sad buzz generated");
}

void buzzer_beep(uint32_t frequency_hz, uint32_t duration_ms)
{
    if (!buzzer_available || !buzzer_ready || !pwm_dev) {
        return;  /* Silently skip if no buzzer */
    }
    
    /* Validate frequency range */
    if (frequency_hz < 100 || frequency_hz > 10000) {
        LOG_WRN("Frequency %u Hz out of range (100-10000 Hz)", frequency_hz);
        return;
    }
    
    /* Calculate PWM period from frequency */
    uint32_t period_ns = 1000000000U / frequency_hz;  /* Period in nanoseconds */
    uint32_t pulse_ns = period_ns / 2;                 /* 50% duty cycle */
    
    /* Set PWM to generate tone */
    struct pwm_dt_spec pwm_spec = {
        .dev = pwm_dev,
        .channel = BUZZER_PWM_CHANNEL,
        .period = period_ns,
        .flags = PWM_POLARITY_NORMAL
    };
    
    int ret = pwm_set_dt(&pwm_spec, period_ns, pulse_ns);
    if (ret != 0) {
        LOG_ERR("Failed to set PWM for buzzer (err %d)", ret);
        return;
    }
    
    /* Cancel any existing stop work */
    k_work_cancel_delayable(&buzzer_stop_work);
    
    /* Schedule stop after duration */
    if (duration_ms > 0) {
        k_work_schedule(&buzzer_stop_work, K_MSEC(duration_ms));
    }
    
    LOG_DBG("Buzzer beep: %u Hz for %u ms", frequency_hz, duration_ms);
}

void buzzer_stop(void)
{
    if (!buzzer_available || !buzzer_ready) {
        return;  /* Silently skip if no buzzer */
    }
    
    /* Cancel any pending stop work */
    k_work_cancel_delayable(&buzzer_stop_work);
    
    /* Stop PWM immediately */
    if (pwm_dev) {
        struct pwm_dt_spec pwm_spec = {
            .dev = pwm_dev,
            .channel = BUZZER_PWM_CHANNEL,
            .period = 0,
            .flags = PWM_POLARITY_NORMAL
        };
        pwm_set_dt(&pwm_spec, 0, 0);
    }
    
    LOG_DBG("Buzzer stopped manually");
}

bool buzzer_is_available(void)
{
    return buzzer_available && buzzer_ready;
}