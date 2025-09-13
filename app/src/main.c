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
#include "ble_central.h"
#include "ble_bas.h"
#include "ble_hid.h"
#include "ble_multi_conn.h"
#include "even_g1.h"
#include "oled_display.h"
#include "buzzer.h"
#include "leds.h"
#include "button.h"

#define LOG_MODULE_NAME mouthpad_usb
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

/* Battery color indication mode - automatically set based on LED hardware */
/* GPIO LEDs use discrete mode, NeoPixel uses gradient mode */

/* Button event callback function */
static void button_event_callback(button_event_t event)
{
	LOG_INF("*** BUTTON EVENT RECEIVED: %d ***", event);
	switch (event) {
	case BUTTON_EVENT_CLICK:
		LOG_INF("=== BUTTON CLICK - TOGGLE DISPLAY MODE ===");
		/* Toggle between bitmap and text display modes */
		even_g1_toggle_display_mode();
		break;
		
	case BUTTON_EVENT_DOUBLE_CLICK:
		LOG_INF("=== BUTTON DOUBLE CLICK - STARTING SMART DEVICE SCAN ===");
		/* Start smart scan for missing devices (MouthPad or Even G1) */
		ble_central_start_scan_for_missing_devices();
		/* Update Even G1 display immediately to show "Scanning..." */
		even_g1_show_current_status();
		break;
		
	case BUTTON_EVENT_HOLD:
		LOG_INF("=== BUTTON HOLD - CLEARING BLE BONDS ===");
		/* Clear BLE bonds and reset for new pairing */
		if (ble_transport_is_connected()) {
			LOG_INF("Disconnecting current BLE connection...");
			ble_transport_disconnect();
		}
		/* Clear all BLE bonds */
		ble_transport_clear_bonds();
		LOG_INF("BLE bonds cleared - ready for new pairing");
		break;
		
	default:
		break;
	}
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

	/* Initialize LED subsystem */
	LOG_INF("Initializing LED subsystem...");
	err = leds_init();
	if (err != 0) {
		LOG_WRN("leds_init failed (err %d) - continuing without LEDs", err);
		/* Continue without LEDs - not critical for core functionality */
	} else {
		LOG_INF("LED subsystem initialized successfully");
		
		/* Choose color mode based on LED hardware */
		if (leds_has_neopixel()) {
			/* NeoPixel supports smooth gradients */
			leds_set_battery_color_mode(BAS_COLOR_MODE_GRADIENT);
			LOG_INF("Using gradient mode for NeoPixel LEDs");
		} else {
			/* GPIO LEDs work better with discrete colors */
			leds_set_battery_color_mode(BAS_COLOR_MODE_DISCRETE);
			LOG_INF("Using discrete mode for GPIO LEDs");
		}
		
		leds_set_state(LED_STATE_SCANNING);  /* Start in scanning state */
	}
	
	/* Initialize User Button */
	LOG_INF("Initializing user button...");
	err = button_init();
	if (err != 0) {
		LOG_WRN("button_init failed (err %d) - continuing without button", err);
		/* Continue without button - not critical for core functionality */
	} else {
		LOG_INF("User button initialized successfully");
		button_register_callback(button_event_callback);
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
		
		/* Check multi-device connection status and data activity */
		bool has_mouthpad = ble_multi_conn_has_type(DEVICE_TYPE_MOUTHPAD);
		bool has_even_g1_ready = even_g1_is_ready(); /* Both arms fully connected */
		bool any_connected = has_mouthpad || has_even_g1_ready;
		bool ble_data_activity = ble_transport_has_data_activity();
		uint8_t battery_level = ble_bas_get_battery_level();
		
		/* Update LED state based on connection and activity */
		if (leds_is_available()) {
			
			if (any_connected && (data_activity || ble_data_activity)) {
				leds_set_state(LED_STATE_DATA_ACTIVITY);
			} else if (any_connected) {
				/* Connected state - show battery level if MouthPad connected, otherwise green */
				if (has_mouthpad) {
					/* Use MouthPad battery level for color */
					ble_device_connection_t *mouthpad = ble_multi_conn_get_mouthpad();
					if (mouthpad) {
						/* Update battery level from connected MouthPad for LED color */
						battery_level = ble_bas_get_battery_level();
					}
				} else {
					/* Even G1 only - show solid green */
					battery_level = 100;
				}
				leds_set_state(LED_STATE_CONNECTED);
			} else {
				/* No devices connected - scanning state */
				leds_set_state(LED_STATE_SCANNING);
			}
			
			/* Update LED animations */
			leds_update();
		}
		
		/* Update button state */
		if (button_is_available()) {
			button_update();
		}
		
		/* Process Even G1 command queue */
		if (has_even_g1_ready) {
			even_g1_process_queue();
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
				int8_t rssi_dbm = any_connected ? ble_transport_get_rssi() : 0;
				oled_display_update_status(battery_level, any_connected, rssi_dbm);
				display_update_counter = 0;
			}
		}
		
		// Mirror OLED status to Even G1 glasses every 2000ms (2 seconds) to prevent queue overflow
		if (has_even_g1_ready) {
			static int even_g1_display_counter = 0;
			even_g1_display_counter++;
			if (even_g1_display_counter >= 2000) {
				int8_t rssi_dbm = has_mouthpad ? ble_transport_get_rssi() : 0;
				
				// Create the same status text that would be displayed on OLED
				// Get device name from connected MouthPad, otherwise use default
				const char *full_title;
				if (has_mouthpad) {
					ble_device_connection_t *mouthpad = ble_multi_conn_get_mouthpad();
					if (mouthpad && strlen(mouthpad->name) > 0) {
						full_title = mouthpad->name;
					} else {
						full_title = "MouthPad";
					}
				} else if (has_even_g1_ready) {
					full_title = "MouthPad^USB";  // Show default when only Even G1 connected
				} else {
					full_title = "MouthPad^USB";  // Show default when scanning
				}
				
				char display_title[13];
				strncpy(display_title, full_title, 12);
				display_title[12] = '\0';
				
				/* Determine status line based on MouthPad connection and scanning state */
				const char *status_line;
				if (has_mouthpad) {
					status_line = "Connected";
				} else if (ble_central_is_scanning()) {
					status_line = "Scanning...";  /* Actively scanning for devices */
				} else if (has_even_g1_ready) {
					status_line = "Ready";  /* Even G1 connected but no MouthPad */
				} else {
					status_line = "Scanning...";  /* Nothing connected, default state */
				}
				
				// Battery line - always create it (empty if no battery data)
				char battery_str[32] = "";
				if (has_mouthpad && battery_level != 0xFF && battery_level <= 100) {
					char battery_icon[8];
					if (battery_level > 75) {
						strcpy(battery_icon, "[||||]");
					} else if (battery_level > 50) {
						strcpy(battery_icon, "[|||.]");
					} else if (battery_level > 25) {
						strcpy(battery_icon, "[||..]");
					} else if (battery_level > 10) {
						strcpy(battery_icon, "[|...]");
					} else {
						strcpy(battery_icon, "[....]");
					}
					snprintf(battery_str, sizeof(battery_str), "%s %d%%", battery_icon, battery_level);
				}
				
				// Signal line - always create it (empty if not connected), always on 4th line
				char signal_str[32] = "";
				if (has_mouthpad) {
					// Simple signal bars based on RSSI
					const char* signal_bars;
					if (rssi_dbm >= -50) {
						signal_bars = "[||||]";
					} else if (rssi_dbm >= -60) {
						signal_bars = "[|||.]";
					} else if (rssi_dbm >= -70) {
						signal_bars = "[||..]";
					} else if (rssi_dbm >= -80) {
						signal_bars = "[|...]";
					} else {
						signal_bars = "[....]";
					}
					snprintf(signal_str, sizeof(signal_str), "%s %d dBm", signal_bars, rssi_dbm);
				}
				
				// Mouse data line - show latest X/Y deltas if available
				char mouse_str[32] = "";
				if (has_mouthpad) {
					int16_t mouse_x, mouse_y;
					uint8_t mouse_buttons;
					if (ble_hid_get_mouse_data(&mouse_x, &mouse_y, &mouse_buttons)) {
						// Show mouse deltas and button state
						char btn_str[8] = "";
						if (mouse_buttons & 0x01) strcat(btn_str, "L");
						if (mouse_buttons & 0x02) strcat(btn_str, "R");
						if (mouse_buttons & 0x04) strcat(btn_str, "M");
						if (strlen(btn_str) == 0) strcpy(btn_str, "-");
						
						snprintf(mouse_str, sizeof(mouse_str), "X:%d Y:%d %s", mouse_x, mouse_y, btn_str);
					}
				}
				
				// Send mirrored status to Even G1 (only if content changed and rate limited)
				static char last_even_g1_content[256] = "";
				static int64_t last_even_g1_update_time = 0;
				char current_content[256];
				snprintf(current_content, sizeof(current_content), "%s|%s|%s|%s|%s", 
				        display_title, status_line, battery_str, signal_str, mouse_str);
				
				int64_t current_time = k_uptime_get();
				bool content_changed = strcmp(current_content, last_even_g1_content) != 0;
				bool rate_limit_ok = (current_time - last_even_g1_update_time) >= 1000; // 1 second minimum between updates
				
				if (content_changed && rate_limit_ok) {
					even_g1_send_text_formatted_dual_arm(display_title, status_line, 
					                                     battery_str,  // Line 3: battery (empty if no data)
					                                     signal_str,   // Line 4: signal (empty if not connected)
					                                     mouse_str);   // Line 5: mouse data (empty if no movement)
					strcpy(last_even_g1_content, current_content);
					last_even_g1_update_time = current_time;
				}
				
				even_g1_display_counter = 0;
			}
		}
		
		// Small delay to prevent busy waiting
		k_sleep(K_MSEC(1));
	}
}
