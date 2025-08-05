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
	LOG_INF("USB HID data received: %d bytes", len);
	
	// Debug: Log the first few bytes
	if (len > 0) {
		LOG_INF("USB HID First bytes: %02x %02x %02x %02x", 
			data[0], len > 1 ? data[1] : 0, len > 2 ? data[2] : 0, len > 3 ? data[3] : 0);
	}
	
	// Bridge USB HID data to BLE HID
	if (ble_transport_is_hid_ready()) {
		LOG_INF("BLE HID is ready, sending %d bytes", len);
		int err = ble_transport_send_hid_data(data, len);
		if (err) {
			LOG_ERR("USB HID→BLE HID FAILED (err %d)", err);
		} else {
			LOG_INF("USB HID data sent to BLE HID successfully");
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

	// LED for status indication
	const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
	static int led_counter = 0;

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
		
		// Toggle LED every 1000 iterations (roughly every second)
		led_counter++;
		if (led_counter >= 1000) {
			(void)gpio_pin_toggle(led0.port, led0.pin);
			led_counter = 0;
		}
		
		// Small delay to prevent busy waiting
		k_sleep(K_MSEC(1));
	}
}
