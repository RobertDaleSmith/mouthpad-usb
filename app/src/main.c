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

#include "usb_cdc.h"
#include "usb_hid.h"
#include "ble_transport.h"
#include "ble_bas.h"
#include "oled_display.h"
#include "buzzer.h"
#include "leds.h"

#define LOG_MODULE_NAME mouthpad_usb
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

/* Battery color indication mode - change this to test both modes */
#define BATTERY_COLOR_MODE BAS_COLOR_MODE_GRADIENT  /* or BAS_COLOR_MODE_DISCRETE */

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

	/* Initialize LED subsystem */
	LOG_INF("Initializing LED subsystem...");
	err = leds_init();
	if (err != 0) {
		LOG_WRN("leds_init failed (err %d) - continuing without LEDs", err);
		/* Continue without LEDs - not critical for core functionality */
	} else {
		LOG_INF("LED subsystem initialized successfully");
		leds_set_battery_color_mode(BATTERY_COLOR_MODE);
		leds_set_state(LED_STATE_SCANNING);  /* Start in scanning state */
	}
	
	static bool data_activity = false;
	static int display_update_counter = 0;
	
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
		
		/* Check BLE connection status and data activity */
		bool is_connected = ble_transport_is_connected();
		bool ble_data_activity = ble_transport_has_data_activity();
		bool bas_ready = ble_bas_is_ready();
		uint8_t battery_level = ble_bas_get_battery_level();
		
		/* Update LED state based on connection and activity */
		if (leds_is_available()) {
			if (is_connected && (data_activity || ble_data_activity)) {
				leds_set_state(LED_STATE_DATA_ACTIVITY);
			} else if (is_connected) {
				leds_set_state(LED_STATE_CONNECTED);
			} else {
				leds_set_state(LED_STATE_SCANNING);
			}
			
			/* Update LED animations */
			leds_update();
		}
		
		/* Reset data activity flag periodically */
		static int activity_counter = 0;
		activity_counter++;
		if (activity_counter >= 100) {
			data_activity = false;
			activity_counter = 0;
		}
		
		// Update OLED display every 500ms for responsive connection status updates (if available)
		// Reduced frequency to avoid overwhelming RSSI reads
		if (oled_display_is_available()) {
			display_update_counter++;
			if (display_update_counter >= 500) {
				int8_t rssi_dbm = is_connected ? ble_transport_get_rssi() : 0;
				oled_display_update_status(battery_level, is_connected, rssi_dbm);
				display_update_counter = 0;
			}
		}
		
		// Small delay to prevent busy waiting
		k_sleep(K_MSEC(1));
	}
}
