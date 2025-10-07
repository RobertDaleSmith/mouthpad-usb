/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** @file
 *  @brief MouthPad USB Bridge - Main Application
 */

#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/drivers/hwinfo.h>
#include <string.h>
#include <nrf.h>

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

#define LOG_MODULE_NAME main
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

static void clear_ble_pairings(void)
{
	if (ble_transport_is_connected()) {
		LOG_INF("Disconnecting current BLE connection before clearing bonds...");
		ble_transport_disconnect();
	}

	ble_transport_clear_bonds();
	LOG_INF("BLE bonds cleared - ready for new pairing");
}

/* Shell command: Enter DFU bootloader mode */
static int cmd_dfu(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Entering DFU bootloader mode...");
	k_sleep(K_MSEC(100)); /* Brief delay for message to be sent */

	/* Set GPREGRET magic value for UF2 bootloader */
	NRF_POWER->GPREGRET = 0x57;

	/* Perform system reset */
	NVIC_SystemReset();

	return 0;
}

/* Shell command: Display USB serial number (device ID) */
static int cmd_serial(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	uint8_t hwid[8];
	ssize_t hwid_len = hwinfo_get_device_id(hwid, sizeof(hwid));

	if (hwid_len > 0) {
		shell_print(sh, "USB Serial Number: %02X%02X%02X%02X%02X%02X%02X%02X",
			    hwid[0], hwid[1], hwid[2], hwid[3],
			    hwid[4], hwid[5], hwid[6], hwid[7]);
		shell_print(sh, "Port should show as: usbmodem%02X%02X%02X%02X%02X%02X%02X%02X<N>",
			    hwid[0], hwid[1], hwid[2], hwid[3],
			    hwid[4], hwid[5], hwid[6], hwid[7]);
	} else {
		shell_error(sh, "Failed to read device ID: %d", hwid_len);
	}

	return 0;
}

/* Shell command: Reset BLE bonds and pairing state */
static int cmd_reset(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Resetting stored BLE bonds and pairing state...");
	clear_ble_pairings();
	shell_print(sh, "Bonds cleared. Put MouthPad into pairing mode to reconnect.");

	return 0;
}

SHELL_CMD_REGISTER(dfu, NULL, "Enter DFU bootloader mode", cmd_dfu);
SHELL_CMD_REGISTER(reset, NULL, "Reset stored BLE bonds", cmd_reset);
SHELL_CMD_REGISTER(serial, NULL, "Display USB serial number", cmd_serial);

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
		clear_ble_pairings();
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
	/* Forward data from MouthPad (BLE NUS) to USB CDC0 - minimal logging to keep CDC0 clean */
	LOG_DBG("NUS→CDC: %d bytes", len);

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

	LOG_INF("=== MouthPad^USB Starting === Built: %s %s", __DATE__, __TIME__);

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

	/* Debug: Test HWINFO device ID reading */
	uint8_t hwid[8];
	ssize_t hwid_len = hwinfo_get_device_id(hwid, sizeof(hwid));
	if (hwid_len > 0) {
		LOG_INF("USB Serial Number: %02X%02X%02X%02X%02X%02X%02X%02X",
			hwid[0], hwid[1], hwid[2], hwid[3],
			hwid[4], hwid[5], hwid[6], hwid[7]);
	} else {
		LOG_ERR("HWINFO get_device_id failed: %d", hwid_len);
	}

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
		// Supports two modes with explicit detection:
		// 1. Text mode: Plain text commands ending with \n (for testing/debugging)
		// 2. Protobuf mode: Length-prefixed protobuf messages (for production app)
		//
		// Detection strategy:
		// - If message starts with printable ASCII letter (A-Z, a-z), assume text mode
		// - Otherwise assume protobuf mode (length prefix is a small number < 32)
		static uint8_t cdc_buffer[UART_BUF_SIZE];
		static int cdc_pos = 0;
		static bool text_mode = false;

		uint8_t c;
		int bytes_read = usb_cdc_receive_data(&c, 1);

		if (bytes_read > 0) { // Data received from CDC
			LOG_DBG("CDC0 RX byte: 0x%02X ('%c')", c, (c >= 32 && c < 127) ? c : '.');

			// Detect mode on first byte
			if (cdc_pos == 0) {
				// Text mode: starts with A-Z or a-z (typical commands like "StartStream")
				// Protobuf mode: starts with length byte
				//
				// Since protobuf messages are length-prefixed, the first byte indicates
				// the message length. For typical protobuf messages (<64 bytes), the first
				// byte will be 0-63, which doesn't overlap with A-Z/a-z (65-90, 97-122).
				//
				// Edge case: Protobuf messages of length 65-90 or 97-122 bytes will be
				// misdetected as text, but this is unlikely for typical control messages.
				text_mode = ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
				LOG_INF("Mode detected: %s (first byte: 0x%02X)", text_mode ? "TEXT" : "PROTOBUF", c);
			}

			cdc_buffer[cdc_pos] = c;
			cdc_pos++;
			data_activity = true;  // Mark data activity

			if (cdc_pos >= UART_BUF_SIZE) {
				LOG_ERR("CDC buffer overflow");
				cdc_pos = 0;
				continue;
			}

			// Text mode: wait for newline
			if (text_mode && (c == '\n' || c == '\r')) {
				cdc_buffer[cdc_pos - 1] = '\0'; // Null terminate, remove newline
				LOG_INF("Text command: '%s' (%d bytes)", cdc_buffer, cdc_pos - 1);

				if (ble_transport_is_nus_ready()) {
					LOG_INF("Sending to MouthPad via NUS...");
					err = ble_transport_send_nus_data(cdc_buffer, cdc_pos - 1);
					if (err) {
						LOG_ERR("CDC→NUS failed (err %d)", err);
					} else {
						LOG_INF("Sent successfully");
					}
				} else {
					LOG_WRN("NUS not ready - is MouthPad connected?");
				}

				cdc_pos = 0;
				text_mode = false;
				continue;
			}

			// Protobuf mode: wait for length-prefixed packet
			if (!text_mode) {
				int expected_len = cdc_buffer[0];

				if (cdc_pos == expected_len + 1) {
					LOG_INF("Protobuf packet: len=%d, total=%d bytes", expected_len, cdc_pos);
					LOG_HEXDUMP_INF(cdc_buffer, cdc_pos, "CDC RX");

					// decode the byte string as
					mouthware_message_MouthpadAppToUsbMessage message;
					pb_istream_t stream = pb_istream_from_buffer(&cdc_buffer[1], cdc_pos - 1);
					if (!pb_decode(&stream, mouthware_message_MouthpadAppToUsbMessage_fields, &message)) {
						LOG_ERR("Protobuf decode failed: %s", PB_GET_ERROR(&stream));
						cdc_pos = 0;
						continue;
					}

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
									/* Forward data to MouthPad via BLE NUS - log to console (CDC1) only */
									LOG_DBG("CDC→NUS: %d bytes", message.message_body.pass_through_to_mouthpad_request.data.size);
									err = ble_transport_send_nus_data(message.message_body.pass_through_to_mouthpad_request.data.bytes, message.message_body.pass_through_to_mouthpad_request.data.size);
									if (err) {
										LOG_WRN("CDC→NUS failed (err %d)", err);
									}
								} else {
									LOG_DBG("NUS not ready, dropping %d bytes", message.message_body.pass_through_to_mouthpad_request.data.size);
								}
							}
							break;
						default:
							LOG_WRN("Invalid destination: %d", message.destination);
							break;
					}

					cdc_pos = 0;
				}
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
