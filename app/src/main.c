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

#include "usb_cdc.h"
#include "usb_hid.h"
#include "ble_transport.h"

#define LOG_MODULE_NAME mouthpad_usb
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

/* USB HID callback function */
static void usb_hid_data_callback(const uint8_t *data, uint16_t len)
{
	LOG_DBG("USB HID data received: %d bytes", len);
	
	// Debug: Log the first few bytes
	if (len > 0) {
		LOG_DBG("USB HID First bytes: %02x %02x %02x %02x", 
			data[0], len > 1 ? data[1] : 0, len > 2 ? data[2] : 0, len > 3 ? data[3] : 0);
	}
	
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

	// LED status indication system
	const struct gpio_dt_spec led_blue = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);   // Blue LED
	const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);  // Green LED
	static int led_counter = 0;
	static bool data_activity = false;
	
	// Configure LEDs
	if (!gpio_is_ready_dt(&led_blue) || !gpio_is_ready_dt(&led_green)) {
		LOG_ERR("LED devices not ready");
	} else {
		gpio_pin_configure_dt(&led_blue, GPIO_OUTPUT);
		gpio_pin_configure_dt(&led_green, GPIO_OUTPUT);
		// Start with blue LED on (scanning state)
		gpio_pin_set_dt(&led_blue, 1);
		gpio_pin_set_dt(&led_green, 0);
	}

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
		
		// LED status system
		led_counter++;
		
		// Check BLE connection status and data activity
		bool is_connected = ble_transport_is_connected();
		bool ble_data_activity = ble_transport_has_data_activity();
		
		// Debug logging every second
		if (led_counter % 1000 == 0) {
			LOG_DBG("LED: connected=%d, usb_data=%d, ble_data=%d", is_connected, data_activity, ble_data_activity);
		}
		
		if (is_connected && (data_activity || ble_data_activity)) {
			// Connected with data activity: Green flicker
			if (led_counter % 20 == 0) {  // Very fast flicker every 20ms
				gpio_pin_toggle_dt(&led_green);
			}
			// Turn off blue LED
			gpio_pin_set_dt(&led_blue, 0);
			
		} else if (is_connected) {
			// Connected but no data: Solid green
			gpio_pin_set_dt(&led_green, 1);
			gpio_pin_set_dt(&led_blue, 0);
			
		} else {
			// Not connected: Blue blink (scanning)
			gpio_pin_set_dt(&led_green, 0);
			if (led_counter >= 500) {  // Slow blink every 500ms
				gpio_pin_toggle_dt(&led_blue);
				led_counter = 0;
			}
		}
		
		// Reset data activity flag periodically
		if (led_counter % 100 == 0) {
			data_activity = false;
		}
		
		// Small delay to prevent busy waiting
		k_sleep(K_MSEC(1));
	}
}
