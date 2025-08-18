/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** @file
 *  @brief MouthPad USB Bridge - Main Application
 */

#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include "usb_cdc.h"
#include "usb_hid.h"
#include "ble_transport.h"
#include "ble_bas.h"
#include "oled_display.h"
#include "buzzer.h"

#define LOG_MODULE_NAME mouthpad_usb
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

/* Battery color indication mode - change this to test both modes */
#define BATTERY_COLOR_MODE BAS_COLOR_MODE_GRADIENT  /* or BAS_COLOR_MODE_DISCRETE */

/* RGB LED helper function */
static void set_rgb_led(const struct gpio_dt_spec *led_red, 
                       const struct gpio_dt_spec *led_green,
                       const struct gpio_dt_spec *led_blue,
                       ble_bas_rgb_color_t color,
                       bool red_available, bool green_available, bool blue_available)
{
	/* LEDs are active LOW, so invert the logic */
	if (red_available) {
		gpio_pin_set_dt(led_red, color.red == 0 ? 0 : 1);
	}
	if (green_available) {
		gpio_pin_set_dt(led_green, color.green == 0 ? 0 : 1);
	}
	if (blue_available) {
		gpio_pin_set_dt(led_blue, color.blue == 0 ? 0 : 1);
	}
}

/* NeoPixel RGB LED helper function */
#if DT_NODE_EXISTS(DT_NODELABEL(neopixel)) && \
    (DT_SAME_NODE(DT_ALIAS(led0), DT_NODELABEL(neopixel)) || \
     DT_SAME_NODE(DT_ALIAS(led1), DT_NODELABEL(neopixel)) || \
     DT_SAME_NODE(DT_ALIAS(led2), DT_NODELABEL(neopixel)))
#define HAS_NEOPIXEL 1
static const struct device *neopixel_dev = DEVICE_DT_GET(DT_NODELABEL(neopixel));
static struct led_rgb neopixel_color = {0, 0, 0};

static void set_neopixel_color(ble_bas_rgb_color_t color)
{
	if (!device_is_ready(neopixel_dev)) {
		return;
	}
	
	neopixel_color.r = color.red;
	neopixel_color.g = color.green; 
	neopixel_color.b = color.blue;
	
	led_strip_update_rgb(neopixel_dev, &neopixel_color, 1);
}
#else
#define HAS_NEOPIXEL 0
static void set_neopixel_color(ble_bas_rgb_color_t color) { /* No-op */ }
#endif

/* Unified RGB LED function that handles both GPIO and NeoPixel */
static void set_unified_rgb_led(const struct gpio_dt_spec *led_red, 
                               const struct gpio_dt_spec *led_green,
                               const struct gpio_dt_spec *led_blue,
                               ble_bas_rgb_color_t color,
                               bool red_available, bool green_available, bool blue_available)
{
#if HAS_NEOPIXEL
	/* Use NeoPixel if available */
	set_neopixel_color(color);
#else
	/* Fall back to individual GPIO LEDs */
	set_rgb_led(led_red, led_green, led_blue, color, red_available, green_available, blue_available);
#endif
}

/* USB HID callback function */
static void usb_hid_data_callback(const uint8_t *data, uint16_t len)
{
	// This callback is for USB HID data going TO USB (BLE->USB bridge)
	// Button detection is now handled in BLE HID layer where we have Report ID context
	
	// Bridge USB HID data to BLE HID
	if (ble_transport_is_hid_ready()) {
		LOG_DBG("BLE HID is ready, sending %d bytes", len);
		int err = ble_transport_send_hid_data(data, len);
		if (err) {
			LOG_ERR("USB HID→BLE HID FAILED (err %d)", err);
		} else {
			LOG_DBG("USB HID data sent to BLE HID successfully");
		}
	} else {
		LOG_DBG("HID client not ready - waiting for service discovery");
	}
}


int main(void)
{
	int err;

	LOG_INF("=== MouthPad^USB Starting ===");

	/* Initialize USB CDC */
	LOG_INF("Initializing USB CDC...");
	err = usb_cdc_init();
	if (err != 0) {
		LOG_ERR("usb_cdc_init failed (err %d)", err);
		return 0;
	}
	LOG_INF("USB CDC initialized successfully");

	/* Initialize USB HID */
	LOG_INF("Initializing USB HID...");
	err = usb_init();
	if (err != 0) {
		LOG_ERR("usb_init failed (err %d)", err);
		return 0;
	}
	LOG_INF("USB HID initialized successfully");

	/* Initialize OLED Display */
	LOG_INF("Initializing OLED Display...");
	err = oled_display_init();
	if (err != 0) {
		LOG_WRN("oled_display_init failed (err %d) - continuing without display", err);
		/* Continue without display - it's not critical for core functionality */
	} else {
		LOG_INF("OLED Display initialized successfully");
		
		/* Show splash screen with Augmental logo */
		oled_display_splash_screen(2000);  /* Show logo for 2 seconds */
	}

	/* Initialize Passive Buzzer */
	LOG_INF("Initializing Passive Buzzer...");
	err = buzzer_init();
	if (err != 0) {
		LOG_WRN("buzzer_init failed (err %d) - continuing without buzzer", err);
		/* Continue without buzzer - it's not critical for core functionality */
	} else {
		LOG_INF("Passive Buzzer initialized successfully");
	}

	/* Initialize BLE Transport */
	LOG_INF("Initializing BLE Transport...");
	err = ble_transport_init();
	if (err != 0) {
		LOG_ERR("ble_transport_init failed (err %d)", err);
		return 0;
	}
	LOG_INF("BLE Transport initialized successfully");

	/* Register USB callbacks with BLE Transport */
	ble_transport_register_usb_cdc_callback((usb_cdc_send_cb_t)usb_cdc_send_data);
	ble_transport_register_usb_hid_callback(usb_hid_data_callback);

	/* Start bridging */
	ble_transport_start_bridging();

	LOG_INF("Starting USB ↔ BLE bridge (NUS + HID)");
	LOG_INF("Battery LED color mode: %s", 
	        (BATTERY_COLOR_MODE == BAS_COLOR_MODE_DISCRETE) ? "DISCRETE" : "GRADIENT");

	LOG_INF("Initializing LED system...");
	// Enable NeoPixel power via P1.14 (required for Adafruit Feather nRF52840)
	const struct gpio_dt_spec neopixel_power = {
		.port = DEVICE_DT_GET(DT_NODELABEL(gpio1)),
		.pin = 14,
		.dt_flags = GPIO_ACTIVE_HIGH
	};
	
	if (gpio_is_ready_dt(&neopixel_power)) {
		int ret = gpio_pin_configure_dt(&neopixel_power, GPIO_OUTPUT_ACTIVE);
		if (ret == 0) {
			gpio_pin_set_dt(&neopixel_power, 1);  // Enable NeoPixel power
			k_sleep(K_MSEC(10));  // Brief power stabilization
		}
	}
	
#if HAS_NEOPIXEL
	if (device_is_ready(neopixel_dev)) {
		LOG_INF("NeoPixel initialized successfully");
		
		// Brief startup indication - short blue pulse  
		struct led_rgb startup_blue = {0, 0, 64};
		led_strip_update_rgb(neopixel_dev, &startup_blue, 1);
		k_sleep(K_MSEC(200));
		
		// Turn off after startup indication
		struct led_rgb off = {0, 0, 0};
		led_strip_update_rgb(neopixel_dev, &off, 1);
	} else {
		LOG_WRN("NeoPixel device not ready");
	}
#else
	LOG_INF("No NeoPixel detected, using GPIO LEDs");
#endif
	
	// RGB LED status indication system - only if GPIO LEDs are available
#if DT_NODE_EXISTS(DT_ALIAS(led0)) && DT_NODE_HAS_PROP(DT_ALIAS(led0), gpios)
	const struct gpio_dt_spec led_red = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);    // Red LED
#endif
#if DT_NODE_EXISTS(DT_ALIAS(led1)) && DT_NODE_HAS_PROP(DT_ALIAS(led1), gpios)
	const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);  // Green LED  
#endif
#if DT_NODE_EXISTS(DT_ALIAS(led2)) && DT_NODE_HAS_PROP(DT_ALIAS(led2), gpios)
	const struct gpio_dt_spec led_blue = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);   // Blue LED (or aliased to led1)
#endif
	static int led_counter = 0;
	static bool data_activity = false;
	static int display_update_counter = 0;
	
	// Configure RGB LEDs with error checking
	bool leds_ready = false;
	static bool led_red_available = false;
	static bool led_green_available = false; 
	static bool led_blue_available = false;
	
#if !HAS_NEOPIXEL
#if DT_NODE_EXISTS(DT_ALIAS(led0)) && DT_NODE_HAS_PROP(DT_ALIAS(led0), gpios)
	led_red_available = gpio_is_ready_dt(&led_red);
#endif
#if DT_NODE_EXISTS(DT_ALIAS(led1)) && DT_NODE_HAS_PROP(DT_ALIAS(led1), gpios)
	led_green_available = gpio_is_ready_dt(&led_green);
#endif
#if DT_NODE_EXISTS(DT_ALIAS(led2)) && DT_NODE_HAS_PROP(DT_ALIAS(led2), gpios)
	led_blue_available = gpio_is_ready_dt(&led_blue);
#endif
	
	LOG_INF("Checking GPIO devices...");
	LOG_INF("LED availability: red=%d, green=%d, blue=%d", 
	        led_red_available, led_green_available, led_blue_available);
#else
	LOG_INF("Using NeoPixel instead of individual GPIO LEDs");
#endif
	
#if HAS_NEOPIXEL
	leds_ready = device_is_ready(neopixel_dev);
	LOG_INF("NeoPixel LEDs ready: %s", leds_ready ? "YES" : "NO");
#else
	if (led_red_available || led_green_available || led_blue_available) {
		LOG_INF("At least one LED available, configuring...");
		
		int ret = 0;
#if DT_NODE_EXISTS(DT_ALIAS(led0)) && DT_NODE_HAS_PROP(DT_ALIAS(led0), gpios)
		if (led_red_available) {
			ret = gpio_pin_configure_dt(&led_red, GPIO_OUTPUT);
			if (ret != 0) {
				LOG_ERR("Failed to configure red LED (err %d)", ret);
				led_red_available = false;
			} else {
				gpio_pin_set_dt(&led_red, 0);
			}
		}
#endif
		
#if DT_NODE_EXISTS(DT_ALIAS(led1)) && DT_NODE_HAS_PROP(DT_ALIAS(led1), gpios)
		if (led_green_available) {
			ret = gpio_pin_configure_dt(&led_green, GPIO_OUTPUT);
			if (ret != 0) {
				LOG_ERR("Failed to configure green LED (err %d)", ret);
				led_green_available = false;
			} else {
				gpio_pin_set_dt(&led_green, 0);
			}
		}
#endif
		
#if DT_NODE_EXISTS(DT_ALIAS(led2)) && DT_NODE_HAS_PROP(DT_ALIAS(led2), gpios)
		if (led_blue_available) {
			ret = gpio_pin_configure_dt(&led_blue, GPIO_OUTPUT);
			if (ret != 0) {
				LOG_ERR("Failed to configure blue LED (err %d)", ret);
				led_blue_available = false;
			} else {
				gpio_pin_set_dt(&led_blue, 1);  // Start with blue on (scanning state)
			}
		}
#endif
		
		if (led_red_available || led_green_available || led_blue_available) {
			leds_ready = true;
			LOG_INF("LEDs configured successfully");
		}
	} else {
		LOG_WRN("No LEDs available - continuing without LED status indication");
	}
#endif
	
	if (!leds_ready) {
		LOG_WRN("RGB LEDs not available - continuing without LED status");
	}
	
	/* Reset display state after splash screen to ensure status updates work */
	if (oled_display_is_available()) {
		oled_display_reset_state();
	}
	
	LOG_INF("Entering main loop...");

	for (;;) {
		/* USB CDC ↔ BLE NUS Bridge */
		
		// Check for data from USB CDC and send to NUS
		static uint8_t cdc_buffer[UART_BUF_SIZE];
		static int cdc_pos = 0;
		
		uint8_t c;
		int bytes_read = usb_cdc_receive_data(&c, 1);
		
		if (bytes_read > 0) { // Data received from CDC
			cdc_buffer[cdc_pos] = c;
			cdc_pos++;
			data_activity = true;  // Mark data activity
			
			// Send complete command when we get newline
			if (c == '\n' || c == '\r' || cdc_pos >= UART_BUF_SIZE) {
				if (cdc_pos > 1) { // Don't send empty commands
					if (ble_transport_is_nus_ready()) {
						LOG_INF("Sending command (%d bytes): %.*s", cdc_pos, cdc_pos, cdc_buffer);
						err = ble_transport_send_nus_data(cdc_buffer, cdc_pos);
						if (err) {
							LOG_ERR("CDC→NUS FAILED (err %d)", err);
						} else {
							LOG_INF("Command sent successfully");
						}
					} else {
						LOG_DBG("NUS client not ready - waiting for service discovery");
					}
				}
				cdc_pos = 0; // Reset buffer
			}
		}
		
		// RGB LED status system with battery-aware colors
		led_counter++;
		
		// Check BLE connection status and data activity
		bool is_connected = ble_transport_is_connected();
		bool ble_data_activity = ble_transport_has_data_activity();
		bool bas_ready = ble_bas_is_ready();
		uint8_t battery_level = ble_bas_get_battery_level();
		
		// Reduced debug logging to focus on HID reports
		
		// RGB LED status updates (only if LEDs are ready)
		if (leds_ready) {
			if (is_connected && (data_activity || ble_data_activity)) {
				// Connected with data activity: Battery-aware color flicker
				if (led_counter % 20 == 0) {  // Very fast flicker every 20ms
					static bool flicker_state = false;
					if (flicker_state) {
						// Show battery color
						ble_bas_rgb_color_t battery_color = ble_bas_get_battery_color(BATTERY_COLOR_MODE);
#if !HAS_NEOPIXEL
#if DT_NODE_EXISTS(DT_ALIAS(led0)) && DT_NODE_HAS_PROP(DT_ALIAS(led0), gpios) && \
    DT_NODE_EXISTS(DT_ALIAS(led1)) && DT_NODE_HAS_PROP(DT_ALIAS(led1), gpios) && \
    DT_NODE_EXISTS(DT_ALIAS(led2)) && DT_NODE_HAS_PROP(DT_ALIAS(led2), gpios)
						set_unified_rgb_led(&led_red, &led_green, &led_blue, battery_color, led_red_available, led_green_available, led_blue_available);
#endif
#else
						set_neopixel_color(battery_color);
#endif
					} else {
						// Turn off for flicker effect
						ble_bas_rgb_color_t off_color = {0, 0, 0};
#if !HAS_NEOPIXEL
#if DT_NODE_EXISTS(DT_ALIAS(led0)) && DT_NODE_HAS_PROP(DT_ALIAS(led0), gpios) && \
    DT_NODE_EXISTS(DT_ALIAS(led1)) && DT_NODE_HAS_PROP(DT_ALIAS(led1), gpios) && \
    DT_NODE_EXISTS(DT_ALIAS(led2)) && DT_NODE_HAS_PROP(DT_ALIAS(led2), gpios)
						set_unified_rgb_led(&led_red, &led_green, &led_blue, off_color, led_red_available, led_green_available, led_blue_available);
#endif
#else
						set_neopixel_color(off_color);
#endif
					}
					flicker_state = !flicker_state;
				}
				
			} else if (is_connected) {
				// Connected but no data: Solid battery-aware color
				ble_bas_rgb_color_t battery_color = ble_bas_get_battery_color(BATTERY_COLOR_MODE);
#if !HAS_NEOPIXEL
#if DT_NODE_EXISTS(DT_ALIAS(led0)) && DT_NODE_HAS_PROP(DT_ALIAS(led0), gpios) && \
    DT_NODE_EXISTS(DT_ALIAS(led1)) && DT_NODE_HAS_PROP(DT_ALIAS(led1), gpios) && \
    DT_NODE_EXISTS(DT_ALIAS(led2)) && DT_NODE_HAS_PROP(DT_ALIAS(led2), gpios)
				set_unified_rgb_led(&led_red, &led_green, &led_blue, battery_color, led_red_available, led_green_available, led_blue_available);
#endif
#else
				set_neopixel_color(battery_color);
#endif
				
			} else {
				// Not connected: Blue blink (scanning)
				if (led_counter >= 500) {  // Slow blink every 500ms
					static bool blue_state = false;
					if (blue_state) {
						ble_bas_rgb_color_t blue_color = {0, 0, 255};
#if !HAS_NEOPIXEL
#if DT_NODE_EXISTS(DT_ALIAS(led0)) && DT_NODE_HAS_PROP(DT_ALIAS(led0), gpios) && \
    DT_NODE_EXISTS(DT_ALIAS(led1)) && DT_NODE_HAS_PROP(DT_ALIAS(led1), gpios) && \
    DT_NODE_EXISTS(DT_ALIAS(led2)) && DT_NODE_HAS_PROP(DT_ALIAS(led2), gpios)
						set_unified_rgb_led(&led_red, &led_green, &led_blue, blue_color, led_red_available, led_green_available, led_blue_available);
#endif
#else
						set_neopixel_color(blue_color);
#endif
					} else {
						ble_bas_rgb_color_t off_color = {0, 0, 0};
#if !HAS_NEOPIXEL
#if DT_NODE_EXISTS(DT_ALIAS(led0)) && DT_NODE_HAS_PROP(DT_ALIAS(led0), gpios) && \
    DT_NODE_EXISTS(DT_ALIAS(led1)) && DT_NODE_HAS_PROP(DT_ALIAS(led1), gpios) && \
    DT_NODE_EXISTS(DT_ALIAS(led2)) && DT_NODE_HAS_PROP(DT_ALIAS(led2), gpios)
						set_unified_rgb_led(&led_red, &led_green, &led_blue, off_color, led_red_available, led_green_available, led_blue_available);
#endif
#else
						set_neopixel_color(off_color);
#endif
					}
					blue_state = !blue_state;
					led_counter = 0;
				}
			}
		}
		
		// Reset data activity flag periodically
		if (led_counter % 100 == 0) {
			data_activity = false;
		}
		
		// Update OLED display every 100ms for responsive connection status updates (if available)
		if (oled_display_is_available()) {
			display_update_counter++;
			if (display_update_counter >= 100) {
				oled_display_update_status(battery_level, is_connected);
				display_update_counter = 0;
			}
		}
		
		// Small delay to prevent busy waiting
		k_sleep(K_MSEC(1));
	}
}
