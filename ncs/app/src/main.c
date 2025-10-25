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
#include "ble_central.h"
#include "ble_bas.h"
#include "ble_dis.h"
#include "oled_display.h"
#include "buzzer.h"
#include "leds.h"
#include "button.h"
#include "MouthpadRelay.pb.h"
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

/* Shell command: Restart firmware */
static int cmd_restart(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Restarting firmware...");
	k_sleep(K_MSEC(100));  // Give log time to flush

	/* Perform system reset */
	NVIC_SystemReset();

	return 0;
}

/* Shell command: Display firmware version information */
static int cmd_version(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "=== MouthPad^USB (nRF52) ===");
	shell_print(sh, "Built: %s %s", __DATE__, __TIME__);
#ifdef BUILD_VERSION
	shell_print(sh, "App Version: %s", STRINGIFY(BUILD_VERSION));
#endif

	return 0;
}

SHELL_CMD_REGISTER(dfu, NULL, "Enter DFU bootloader mode", cmd_dfu);
SHELL_CMD_REGISTER(reset, NULL, "Reset stored BLE bonds", cmd_reset);
SHELL_CMD_REGISTER(restart, NULL, "Restart firmware", cmd_restart);
SHELL_CMD_REGISTER(serial, NULL, "Display USB serial number", cmd_serial);
SHELL_CMD_REGISTER(version, NULL, "Display firmware version", cmd_version);

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

/* nanopb string encoding callback for device info strings */
static bool encode_string_callback(pb_ostream_t *stream, const pb_field_t *field, void * const *arg)
{
	const char *str = (const char *)(*arg);

	if (!str || str[0] == '\0') {
		return true;  /* Empty string, nothing to encode */
	}

	if (!pb_encode_tag_for_field(stream, field)) {
		return false;
	}

	return pb_encode_string(stream, (const uint8_t *)str, strlen(str));
}

int usb_cdc_send_proto_message(mouthware_message_RelayToAppMessage message)
{
	uint8_t dataPacket[1024];
	pb_ostream_t stream = pb_ostream_from_buffer(dataPacket, sizeof(dataPacket));
	if (!pb_encode(&stream, mouthware_message_RelayToAppMessage_fields, &message)) {
		LOG_ERR("Encoding failed: %s\n", PB_GET_ERROR(&stream));
		return -1;
	}
	return usb_cdc_send_data(dataPacket, stream.bytes_written);
}

int mouthpad_nus_data_received_callback(const uint8_t *data, uint16_t len)
{
	/* Forward data from MouthPad (BLE NUS) to USB CDC0 - minimal logging to keep CDC0 clean */
	LOG_DBG("NUS→CDC: %d bytes", len);

	// take binary data received via BLE, wrap it in a RelayToAppMessage and send it to the USB CDC
	mouthware_message_RelayToAppMessage message = mouthware_message_RelayToAppMessage_init_zero;
	message.which_message_body = mouthware_message_RelayToAppMessage_pass_through_to_app_tag;
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
	err = oled_display_init();
	if (err != 0) {
		LOG_WRN("oled_display_init failed (err %d) - continuing without display", err);
		/* Continue without display - it's not critical for core functionality */
	} else {
		/* Show splash screen with Augmental logo */
		oled_display_splash_screen(2000);  /* Show logo for 2 seconds */
	}

	/* Initialize Passive Buzzer */
	err = buzzer_init();
	if (err != 0) {
		LOG_WRN("buzzer_init failed (err %d) - continuing without buzzer", err);
		/* Continue without buzzer - it's not critical for core functionality */
	} else if (buzzer_is_available()) {
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
	
	static int display_update_counter = 0;

	/* Reset display state after splash screen to ensure status updates work */
	if (oled_display_is_available()) {
		oled_display_reset_state();
	}

	/* Packet framing state machine for CDC RX */
	enum {
		FRAME_STATE_SEARCH_MAGIC1,  // Looking for 0xAA
		FRAME_STATE_SEARCH_MAGIC2,  // Looking for 0x55
		FRAME_STATE_LENGTH_HIGH,    // Reading length high byte
		FRAME_STATE_LENGTH_LOW,     // Reading length low byte
		FRAME_STATE_PAYLOAD,        // Reading payload
		FRAME_STATE_CRC_HIGH,       // Reading CRC high byte
		FRAME_STATE_CRC_LOW         // Reading CRC low byte
	} frame_state = FRAME_STATE_SEARCH_MAGIC1;

	static uint8_t frame_buffer[512];
	static uint16_t frame_length = 0;
	static uint16_t frame_pos = 0;
	static uint16_t expected_crc = 0;

	LOG_INF("Entering main loop...");

	for (;;) {
		/* USB CDC ↔ BLE NUS Bridge */

		/* Check BLE connection status and HID data activity */
		bool is_connected = ble_transport_is_connected();
		bool ble_hid_activity = ble_transport_has_hid_data_activity();
		uint8_t battery_level = ble_bas_get_battery_level();
		int8_t rssi_dbm = is_connected ? ble_transport_get_rssi() : 0;

		// Check for data from USB CDC - parse framed packets [0xAA 0x55][len][payload][CRC]
		uint8_t c;
		int bytes_read = usb_cdc_receive_data(&c, 1);

		if (bytes_read > 0) {
			switch (frame_state) {
				case FRAME_STATE_SEARCH_MAGIC1:
					if (c == 0xAA) {
						frame_state = FRAME_STATE_SEARCH_MAGIC2;
					}
					break;

				case FRAME_STATE_SEARCH_MAGIC2:
					if (c == 0x55) {
						frame_state = FRAME_STATE_LENGTH_HIGH;
						frame_pos = 0;
					} else if (c != 0xAA) {
						// Not magic byte sequence, restart search
						frame_state = FRAME_STATE_SEARCH_MAGIC1;
					}
					// If c == 0xAA, stay in SEARCH_MAGIC2 (could be start of new frame)
					break;

				case FRAME_STATE_LENGTH_HIGH:
					frame_length = (uint16_t)c << 8;
					frame_state = FRAME_STATE_LENGTH_LOW;
					break;

				case FRAME_STATE_LENGTH_LOW:
					frame_length |= c;
					if (frame_length > sizeof(frame_buffer)) {
						LOG_ERR("Frame too large: %d bytes", frame_length);
						frame_state = FRAME_STATE_SEARCH_MAGIC1;
					} else {
						frame_state = FRAME_STATE_PAYLOAD;
						frame_pos = 0;
					}
					break;

				case FRAME_STATE_PAYLOAD:
					frame_buffer[frame_pos++] = c;
					if (frame_pos >= frame_length) {
						frame_state = FRAME_STATE_CRC_HIGH;
					}
					break;

				case FRAME_STATE_CRC_HIGH:
					expected_crc = (uint16_t)c << 8;
					frame_state = FRAME_STATE_CRC_LOW;
					break;

				case FRAME_STATE_CRC_LOW:
					expected_crc |= c;

					// Validate CRC
					uint16_t calculated_crc = calculate_crc16(frame_buffer, frame_length);
					if (calculated_crc != expected_crc) {
						LOG_ERR("CRC mismatch: calc=0x%04X, expected=0x%04X", calculated_crc, expected_crc);
						frame_state = FRAME_STATE_SEARCH_MAGIC1;
						break;
					}

					// Valid framed packet received!
					LOG_DBG("Framed packet RX: %d bytes", frame_length);

					// Decode protobuf message
					mouthware_message_AppToRelayMessage message;
					pb_istream_t stream = pb_istream_from_buffer(frame_buffer, frame_length);
					if (!pb_decode(&stream, mouthware_message_AppToRelayMessage_fields, &message)) {
						LOG_ERR("Protobuf decode failed: %s", PB_GET_ERROR(&stream));
						frame_state = FRAME_STATE_SEARCH_MAGIC1;
						break;
					}

					// Handle message
					switch (message.destination) {
						case mouthware_message_AppToRelayMessageDestination_APP_RELAY_MESSAGE_DESTINATION_RELAY:
							if (message.which_message_body == mouthware_message_AppToRelayMessage_ble_connection_status_read_tag) {
								mouthware_message_RelayToAppMessage response = mouthware_message_RelayToAppMessage_init_zero;
								response.which_message_body = mouthware_message_RelayToAppMessage_ble_connection_status_response_tag;

								/* Use ble_central state machine as the ONLY source of truth for connection status */
								LOG_INF("=== BLE STATUS QUERY ===");
								bool central_connecting = ble_central_is_connecting();
								bool central_scanning = ble_central_is_scanning();
								bool central_connected = ble_central_is_connected();
								LOG_INF("Query state: connecting=%d, scanning=%d, connected=%d (transport is_connected=%d)",
								        central_connecting, central_scanning, central_connected, is_connected);

								if (central_connecting) {
									LOG_INF("Reporting: CONNECTING");
									response.message_body.ble_connection_status_response.connection_status = mouthware_message_RelayBleConnectionStatus_RELAY_CONNECTION_STATUS_CONNECTING;
								} else if (central_scanning) {
									LOG_INF("Reporting: SEARCHING");
									response.message_body.ble_connection_status_response.connection_status = mouthware_message_RelayBleConnectionStatus_RELAY_CONNECTION_STATUS_SEARCHING;
								} else if (central_connected) {
									LOG_INF("Reporting: CONNECTED (via ble_central state)");
									response.message_body.ble_connection_status_response.connection_status = mouthware_message_RelayBleConnectionStatus_RELAY_CONNECTION_STATUS_CONNECTED;
								} else {
									/* Default to DISCONNECTED if all other checks fail */
									LOG_INF("Reporting: DISCONNECTED");
									response.message_body.ble_connection_status_response.connection_status = mouthware_message_RelayBleConnectionStatus_RELAY_CONNECTION_STATUS_DISCONNECTED;
								}

								response.message_body.ble_connection_status_response.rssi = rssi_dbm;
								response.message_body.ble_connection_status_response.battery_level = battery_level;
								usb_cdc_send_proto_message_async(response);
							} else if (message.which_message_body == mouthware_message_AppToRelayMessage_device_info_read_tag) {
								/* Handle DeviceInfoRead request */
								mouthware_message_RelayToAppMessage response = mouthware_message_RelayToAppMessage_init_zero;
								response.which_message_body = mouthware_message_RelayToAppMessage_device_info_response_tag;

								/* Check if we have a bonded device (even if disconnected) */
								bt_addr_le_t bonded_addr;
								static char bonded_name[32];
								bool has_bonded = ble_central_get_bonded_device_addr(&bonded_addr, bonded_name, sizeof(bonded_name));

								/* Get BLE address - prefer active connection, fallback to bonded address */
								static char ble_addr_str[BT_ADDR_STR_LEN];
								struct bt_conn *conn = ble_central_get_default_conn();

								if (conn) {
									/* Device is currently connected - get address from connection */
									const bt_addr_le_t *addr_le = bt_conn_get_dst(conn);
									bt_addr_to_str(&addr_le->a, ble_addr_str, sizeof(ble_addr_str));
									response.message_body.device_info_response.address.funcs.encode = encode_string_callback;
									response.message_body.device_info_response.address.arg = (void *)ble_addr_str;
								} else if (has_bonded) {
									/* Device is bonded but not connected - return cached address */
									bt_addr_to_str(&bonded_addr.a, ble_addr_str, sizeof(ble_addr_str));
									response.message_body.device_info_response.address.funcs.encode = encode_string_callback;
									response.message_body.device_info_response.address.arg = (void *)ble_addr_str;
								}

								/* Get device name - prefer live name from active connection, fallback to cached name */
								const char *device_name = NULL;
								if (conn) {
									/* Get advertised device name from BLE transport (live connection) */
									device_name = ble_transport_get_device_name();
								}

								/* If no live name available but we have a cached bonded name, use that */
								if ((!device_name || device_name[0] == '\0') && has_bonded && bonded_name[0] != '\0') {
									device_name = bonded_name;
								}

								/* Encode device name if we have one */
								if (device_name && device_name[0] != '\0') {
									response.message_body.device_info_response.name.funcs.encode = encode_string_callback;
									response.message_body.device_info_response.name.arg = (void *)device_name;
								}

								/* Get device info from DIS client (only available when connected) */
								if (conn) {
									const ble_dis_info_t *info = ble_dis_get_info();
									if (info) {
										/* Firmware version */
										if (info->has_firmware_version) {
											response.message_body.device_info_response.firmware.funcs.encode = encode_string_callback;
											response.message_body.device_info_response.firmware.arg = (void *)info->firmware_version;
										}
										/* VID and PID from PnP ID */
										if (info->has_pnp_id) {
											response.message_body.device_info_response.vid = info->vendor_id;
											response.message_body.device_info_response.pid = info->product_id;
										}
									}
								}

								/* Device family and board (always available) */
								response.message_body.device_info_response.family.funcs.encode = encode_string_callback;
								response.message_body.device_info_response.family.arg = (void *)"nrf";
								response.message_body.device_info_response.board.funcs.encode = encode_string_callback;
								response.message_body.device_info_response.board.arg = (void *)CONFIG_BOARD;

								LOG_INF("Sending device info: bonded=%d, connected=%d, Addr=%s, Name=%s",
									has_bonded, conn != NULL,
									(conn || has_bonded) ? ble_addr_str : "(none)",
									(device_name && device_name[0] != '\0') ? device_name : "(none)");

								usb_cdc_send_proto_message_async(response);
							} else if (message.which_message_body == mouthware_message_AppToRelayMessage_clear_bonds_write_tag) {
								/* Handle ClearBondsWrite request */
								LOG_INF("=== CLEAR BONDS REQUEST (via protobuf) ===");

								/* Clear BLE bonds using existing logic */
								clear_ble_pairings();

								/* Send success response */
								mouthware_message_RelayToAppMessage response = mouthware_message_RelayToAppMessage_init_zero;
								response.which_message_body = mouthware_message_RelayToAppMessage_clear_bonds_response_tag;
								response.message_body.clear_bonds_response.success = true;

								LOG_INF("Bonds cleared successfully, sending response");
								usb_cdc_send_proto_message_async(response);
							} else if (message.which_message_body == mouthware_message_AppToRelayMessage_dfu_write_tag) {
								/* Handle DfuWrite request - enter bootloader mode */
								LOG_INF("=== DFU REQUEST (via protobuf) - entering bootloader ===");

								/* Send success response before reset */
								mouthware_message_RelayToAppMessage response = mouthware_message_RelayToAppMessage_init_zero;
								response.which_message_body = mouthware_message_RelayToAppMessage_dfu_response_tag;
								response.message_body.dfu_response.success = true;

								usb_cdc_send_proto_message_async(response);

								/* Give time for response to be sent */
								k_sleep(K_MSEC(100));

								/* Disable USB pullup to trigger disconnect before reset */
								NRF_USBD->USBPULLUP = 0;
								k_msleep(50);

								/* Set GPREGRET magic value for UF2 bootloader */
								NRF_POWER->GPREGRET = 0x57;

								/* Perform system reset */
								NVIC_SystemReset();
							}
							break;

						case mouthware_message_AppToRelayMessageDestination_APP_RELAY_MESSAGE_DESTINATION_MOUTHPAD:
							if (message.which_message_body == mouthware_message_AppToRelayMessage_pass_through_to_mouthpad_tag) {
								if (ble_transport_is_nus_ready()) {
									LOG_DBG("CDC→NUS: %d bytes", message.message_body.pass_through_to_mouthpad.data.size);
									err = ble_transport_send_nus_data(message.message_body.pass_through_to_mouthpad.data.bytes, message.message_body.pass_through_to_mouthpad.data.size);
									if (err) {
										LOG_WRN("CDC→NUS failed (err %d)", err);
									}
								} else {
									LOG_DBG("NUS not ready, dropping %d bytes", message.message_body.pass_through_to_mouthpad.data.size);
								}
							}
							break;

						default:
							LOG_WRN("Invalid destination: %d", message.destination);
							break;
					}

					// Reset for next frame
					frame_state = FRAME_STATE_SEARCH_MAGIC1;
					break;
			}
		}
		
		/* Update LED state based on connection and HID activity only */
		if (leds_is_available()) {
			if (is_connected && ble_hid_activity) {
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
