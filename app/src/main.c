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
#include "button.h"
#include "MouthpadUsb.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#define LOG_MODULE_NAME mouthpad_usb
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

/* Battery color indication mode - automatically set based on LED hardware */
/* GPIO LEDs use discrete mode, NeoPixel uses gradient mode */

/* Button event callback function */
static void button_event_callback(button_event_t event)
{
	switch (event) {
	case BUTTON_EVENT_CLICK:
		LOG_INF("=== BUTTON CLICK ===");
		/* TODO: Add click functionality */
		break;
		
	case BUTTON_EVENT_DOUBLE_CLICK:
		LOG_INF("=== BUTTON DOUBLE CLICK ===");
		/* TODO: Add double-click functionality */
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

int usb_cdc_send_proto_message(mouthware_message_UsbDongleToMouthpadAppMessage message)
{
	uint8_t dataPacket[1024];
	pb_ostream_t stream = pb_ostream_from_buffer(dataPacket, sizeof(dataPacket));
	if (!pb_encode(&stream, mouthware_message_UsbDongleToMouthpadAppMessage_fields, &message)) {
		LOG_ERR("Encoding failed: %s\n", PB_GET_ERROR(&stream));
		return -1;
	}
	return usb_cdc_send_data(dataPacket, stream.bytes_written);
}

int mouthpad_nus_data_received_callback(const uint8_t *data, uint16_t len)
{
	LOG_INF("Mouthpad NUS data received: %d bytes", len);
	// take binary data received via BLE, wrap it in a UsbDongleToMouthpadAppMessage and send it to the USB CDC
	mouthware_message_UsbDongleToMouthpadAppMessage message = mouthware_message_UsbDongleToMouthpadAppMessage_init_zero;
	message.which_message_body = mouthware_message_UsbDongleToMouthpadAppMessage_pass_through_to_app_tag;
	message.message_body.pass_through_to_app.data.size = len;
	memcpy(message.message_body.pass_through_to_app.data.bytes, data, len);

	return usb_cdc_send_proto_message_async(message);
}


int main(void)
{
	int err;

	LOG_INF("=== MouthPad^USB Starting ===");

	/* Initialize USB device stack (HID + CDC) */
	LOG_INF("Initializing USB device stack...");
	err = usb_init();
	if (err != 0) {
		LOG_ERR("usb_init failed (err %d)", err);
		return 0;
	}
	LOG_INF("USB device stack initialized successfully");

	/* Initialize USB CDC (get CDC0 device reference) */
	LOG_INF("Initializing USB CDC...");
	err = usb_cdc_init();
	if (err != 0) {
		LOG_ERR("usb_cdc_init failed (err %d)", err);
		return 0;
	}
	LOG_INF("USB CDC initialized successfully");

	/* Debug: Log CDC device info */
	const struct device *cdc0 = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));
	const struct device *cdc1 = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart1));

	/* Use printk directly to test console output */
	printk("\n\n=== MouthPad^USB CDC TEST ===\n");
	printk("CDC0: %s (ready=%d)\n", cdc0->name, device_is_ready(cdc0));
	printk("CDC1: %s (ready=%d)\n", cdc1->name, device_is_ready(cdc1));
	printk("Console output on CDC1: %s\n", cdc1->name);
	printk("If you see this, console is working!\n");
	printk("================================\n\n");

	LOG_INF("=== CDC DEVICE INITIALIZATION ===");
	LOG_INF("CDC0: %s (ready=%d)", cdc0->name, device_is_ready(cdc0));
	LOG_INF("CDC1: %s (ready=%d)", cdc1->name, device_is_ready(cdc1));
	LOG_INF("Console output should appear on CDC1: %s", cdc1->name);
	LOG_INF("================================");

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
	ble_transport_register_usb_cdc_callback((usb_cdc_send_cb_t)mouthpad_nus_data_received_callback);
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
		
		/* Check BLE connection status and data activity */
		bool is_connected = ble_transport_is_connected();
		bool ble_data_activity = ble_transport_has_data_activity();
		uint8_t battery_level = ble_bas_get_battery_level();		
		int8_t rssi_dbm = is_connected ? ble_transport_get_rssi() : 0;
		
		// Check for data from USB CDC and send to NUS
		static uint8_t cdc_buffer[UART_BUF_SIZE];
		static int cdc_pos = 0;
		
		uint8_t c;
		int bytes_read = usb_cdc_receive_data(&c, 1);
		
		if (bytes_read > 0) { // Data received from CDC
			cdc_buffer[cdc_pos] = c;
			cdc_pos++;
			data_activity = true;  // Mark data activity
			
			int expected_len = cdc_buffer[0];
			if (cdc_pos >= UART_BUF_SIZE) {
				LOG_ERR("CDC buffer overflow");
				cdc_pos = 0;
			}

			if (cdc_pos == expected_len + 1) {
				// decode the byte string as 
				mouthware_message_MouthpadAppToUsbMessage message;
				pb_istream_t stream = pb_istream_from_buffer(&cdc_buffer[1], cdc_pos - 1);
				pb_decode(&stream, mouthware_message_MouthpadAppToUsbMessage_fields, &message);

				switch (message.destination) {
					case mouthware_message_MouthpadAppToUsbMessageDestination_MOUTHPAD_USB_MESSAGE_DESTINATION_DONGLE:
						if (message.which_message_body == mouthware_message_MouthpadAppToUsbMessage_connection_status_request_tag) {
							mouthware_message_UsbDongleToMouthpadAppMessage message = mouthware_message_UsbDongleToMouthpadAppMessage_init_zero;
							message.which_message_body = mouthware_message_UsbDongleToMouthpadAppMessage_connection_status_response_tag;
							message.message_body.connection_status_response.connection_status = mouthware_message_UsbDongleConnectionStatus_USB_DONGLE_CONNECTION_STATUS_DISCONNECTED;
							if (is_connected) {
								message.message_body.connection_status_response.connection_status = mouthware_message_UsbDongleConnectionStatus_USB_DONGLE_CONNECTION_STATUS_CONNECTED;
							} else {
								message.message_body.connection_status_response.connection_status = mouthware_message_UsbDongleConnectionStatus_USB_DONGLE_CONNECTION_STATUS_DISCONNECTED;
								
							}
						
							usb_cdc_send_proto_message_async(message);							
						}
						else if (message.which_message_body == mouthware_message_MouthpadAppToUsbMessage_rssi_request_tag) {
							mouthware_message_UsbDongleToMouthpadAppMessage message = mouthware_message_UsbDongleToMouthpadAppMessage_init_zero;
							message.which_message_body = mouthware_message_UsbDongleToMouthpadAppMessage_rssi_status_response_tag;
							message.message_body.rssi_status_response.rssi = rssi_dbm;
							usb_cdc_send_proto_message_async(message);							
						}
						break;
					case mouthware_message_MouthpadAppToUsbMessageDestination_MOUTHPAD_USB_MESSAGE_DESTINATION_MOUTHPAD:
						if (message.which_message_body == mouthware_message_MouthpadAppToUsbMessage_pass_through_to_mouthpad_request_tag) {
							if (ble_transport_is_nus_ready()) {
								LOG_INF("Sending command (%d bytes): %.*s", cdc_pos, cdc_pos, cdc_buffer);
								err = ble_transport_send_nus_data(&message.message_body.pass_through_to_mouthpad_request.data.bytes, message.message_body.pass_through_to_mouthpad_request.data.size);
								if (err) {
									LOG_ERR("CDC→NUS FAILED (err %d)", err);
								} else {
									LOG_INF("Command sent successfully");
								}
							}
						} else {
							// Return error indicating invalid message
						}
						break;
					default:
						LOG_ERR("Invalid destination: %d", message.destination);
						break;
				}

				// if (cdc_pos > 1) { // Don't send empty commands
				// 	if (ble_transport_is_nus_ready()) {
				// 		LOG_INF("Sending command (%d bytes): %.*s", cdc_pos, cdc_pos, cdc_buffer);
				// 		err = ble_transport_send_nus_data(&cdc_buffer[1], cdc_pos - 1);
				// 		if (err) {
				// 			LOG_ERR("CDC→NUS FAILED (err %d)", err);
				// 		} else {
				// 			LOG_INF("Command sent successfully");
				// 		}
				// 	} else {
				// 		LOG_DBG("NUS client not ready - waiting for service discovery");
				// 	}
				// }
				cdc_pos = 0; // Reset buffer
			}
		}
		
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
		
		/* Update button state */
		if (button_is_available()) {
			button_update();
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
				oled_display_update_status(battery_level, is_connected, rssi_dbm);
				display_update_counter = 0;
			}
		}
		
		// Small delay to prevent busy waiting
		k_sleep(K_MSEC(1));
	}
}
