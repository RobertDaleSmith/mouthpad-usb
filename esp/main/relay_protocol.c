#include "relay_protocol.h"

#include "MouthpadRelay.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include "esp_log.h"
#include "usb_cdc.h"
#include "usb_dfu.h"
#include "ble_nus.h"
#include "ble_hid.h"
#include "ble_bas.h"
#include "ble_dis.h"
#include "ble_bonds.h"
#include "transport_hid.h"
#include "leds.h"
#include "main.h"

#include <string.h>

static const char *TAG = "RELAY_PROTO";

// Internal state
static bool s_ble_connected = false;
static bool s_ble_scanning = false;
static int32_t s_last_rssi = 0;

// Forward declarations
static esp_err_t handle_ble_connection_status_read(void);
static esp_err_t handle_device_info_read(void);
static esp_err_t handle_clear_bonds_write(void);
static esp_err_t handle_dfu_write(void);
static esp_err_t handle_pass_through_to_mouthpad(const uint8_t *data, size_t len);

// Nanopb callbacks for string encoding
static bool encode_string_callback(pb_ostream_t *stream, const pb_field_t *field, void * const *arg) {
    const char *str = (const char *)(*arg);

    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    return pb_encode_string(stream, (const uint8_t *)str, strlen(str));
}

esp_err_t relay_protocol_init(void) {
    ESP_LOGI(TAG, "Relay protocol initialized");
    return ESP_OK;
}

esp_err_t relay_protocol_handle_usb_data(const uint8_t *data, uint16_t len) {
    if (!data || len == 0) {
        ESP_LOGW(TAG, "Invalid USB data");
        return ESP_ERR_INVALID_ARG;
    }

    // Decode AppToRelayMessage
    mouthware_message_AppToRelayMessage app_msg = mouthware_message_AppToRelayMessage_init_zero;

    pb_istream_t stream = pb_istream_from_buffer(data, len);
    bool status = pb_decode(&stream, &mouthware_message_AppToRelayMessage_msg, &app_msg);

    if (!status) {
        ESP_LOGW(TAG, "Failed to decode AppToRelayMessage: %s", PB_GET_ERROR(&stream));
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Decoded AppToRelayMessage, destination=%d, which=%d",
             app_msg.destination, app_msg.which_message_body);

    // Route message based on which_message_body
    esp_err_t ret = ESP_OK;

    switch (app_msg.which_message_body) {
        case mouthware_message_AppToRelayMessage_ble_connection_status_read_tag:
            ESP_LOGD(TAG, "Handling BleConnectionStatusRead");
            ret = handle_ble_connection_status_read();
            break;

        case mouthware_message_AppToRelayMessage_device_info_read_tag:
            ESP_LOGD(TAG, "Handling DeviceInfoRead");
            ret = handle_device_info_read();
            break;

        case mouthware_message_AppToRelayMessage_clear_bonds_write_tag:
            ESP_LOGD(TAG, "Handling ClearBondsWrite");
            ret = handle_clear_bonds_write();
            break;

        case mouthware_message_AppToRelayMessage_dfu_write_tag:
            ESP_LOGD(TAG, "Handling DfuWrite");
            ret = handle_dfu_write();
            break;

        case mouthware_message_AppToRelayMessage_pass_through_to_mouthpad_tag:
            ESP_LOGD(TAG, "Handling PassThroughToMouthpad, len=%d",
                     app_msg.message_body.pass_through_to_mouthpad.data.size);
            ret = handle_pass_through_to_mouthpad(
                app_msg.message_body.pass_through_to_mouthpad.data.bytes,
                app_msg.message_body.pass_through_to_mouthpad.data.size
            );
            break;

        default:
            ESP_LOGW(TAG, "Unknown message type: %d", app_msg.which_message_body);
            ret = ESP_ERR_NOT_SUPPORTED;
            break;
    }

    return ret;
}

esp_err_t relay_protocol_handle_ble_data(const uint8_t *data, uint16_t len) {
    if (!data || len == 0) {
        ESP_LOGW(TAG, "Invalid BLE data");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "Forwarding BLE data to USB: %d bytes", len);

    // Create PassThroughToApp message
    mouthware_message_RelayToAppMessage relay_msg = mouthware_message_RelayToAppMessage_init_zero;
    relay_msg.which_message_body = mouthware_message_RelayToAppMessage_pass_through_to_app_tag;

    // Copy data to passthrough message
    if (len > sizeof(relay_msg.message_body.pass_through_to_app.data.bytes)) {
        ESP_LOGW(TAG, "BLE data too large: %d > %d", len,
                 sizeof(relay_msg.message_body.pass_through_to_app.data.bytes));
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(relay_msg.message_body.pass_through_to_app.data.bytes, data, len);
    relay_msg.message_body.pass_through_to_app.data.size = len;

    // Encode and send
    return relay_protocol_send_response(&relay_msg);
}

esp_err_t relay_protocol_send_response(const void *message) {
    const mouthware_message_RelayToAppMessage *relay_msg =
        (const mouthware_message_RelayToAppMessage *)message;

    // Encode the message
    uint8_t buffer[512];  // Should be large enough for any RelayToAppMessage
    pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));

    bool status = pb_encode(&stream, &mouthware_message_RelayToAppMessage_msg, relay_msg);

    if (!status) {
        ESP_LOGE(TAG, "Failed to encode RelayToAppMessage: %s", PB_GET_ERROR(&stream));
        return ESP_FAIL;
    }

    size_t message_length = stream.bytes_written;
    ESP_LOGD(TAG, "Encoded RelayToAppMessage: %d bytes", message_length);

    // Send via USB CDC with framing
    esp_err_t ret = usb_cdc_send_data(buffer, message_length);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send data via USB CDC: %s", esp_err_to_name(ret));
    }

    return ret;
}

void relay_protocol_update_ble_connection(bool connected) {
    s_ble_connected = connected;
    ESP_LOGD(TAG, "BLE connection state updated: %s", connected ? "connected" : "disconnected");
}

void relay_protocol_update_ble_scanning(bool scanning) {
    s_ble_scanning = scanning;
    ESP_LOGD(TAG, "BLE scanning state updated: %s", scanning ? "scanning" : "not scanning");
}

void relay_protocol_update_rssi(int32_t rssi) {
    s_last_rssi = rssi;
    ESP_LOGD(TAG, "RSSI updated: %d dBm", rssi);
}

// Command handlers

static esp_err_t handle_ble_connection_status_read(void) {
    mouthware_message_RelayToAppMessage relay_msg = mouthware_message_RelayToAppMessage_init_zero;
    relay_msg.which_message_body = mouthware_message_RelayToAppMessage_ble_connection_status_response_tag;

    // Determine connection status
    mouthware_message_RelayBleConnectionStatus status;
    const char *status_str;

    if (s_ble_connected && ble_hid_client_is_connected()) {
        status = mouthware_message_RelayBleConnectionStatus_RELAY_CONNECTION_STATUS_CONNECTED;
        status_str = "connected";
    } else if (s_ble_scanning) {
        status = mouthware_message_RelayBleConnectionStatus_RELAY_CONNECTION_STATUS_SEARCHING;
        status_str = "searching";
    } else {
        status = mouthware_message_RelayBleConnectionStatus_RELAY_CONNECTION_STATUS_DISCONNECTED;
        status_str = "disconnected";
    }

    relay_msg.message_body.ble_connection_status_response.connection_status = status;
    relay_msg.message_body.ble_connection_status_response.rssi = s_last_rssi;

    // Get battery level if available
    if (ble_bas_is_ready()) {
        relay_msg.message_body.ble_connection_status_response.battery_level = ble_bas_get_battery_level();
    } else {
        relay_msg.message_body.ble_connection_status_response.battery_level = 0;
    }

    ESP_LOGI(TAG, "BLE status: %s, RSSI: %d, Battery: %d%%",
             status_str, s_last_rssi,
             relay_msg.message_body.ble_connection_status_response.battery_level);

    return relay_protocol_send_response(&relay_msg);
}

static esp_err_t handle_device_info_read(void) {
    mouthware_message_RelayToAppMessage relay_msg = mouthware_message_RelayToAppMessage_init_zero;
    relay_msg.which_message_body = mouthware_message_RelayToAppMessage_device_info_response_tag;

    // Device family and board (always available - dongle hardware info)
    // These describe the dongle hardware itself, not the bonded MouthPad
    relay_msg.message_body.device_info_response.family.funcs.encode = encode_string_callback;
    relay_msg.message_body.device_info_response.family.arg = (void *)"esp";
    relay_msg.message_body.device_info_response.board.funcs.encode = encode_string_callback;
    relay_msg.message_body.device_info_response.board.arg = (void *)CONFIG_MOUTHPAD_BOARD_NAME;

    const ble_device_info_t *device_info = ble_device_info_get_current();

    if (!device_info || !device_info->info_complete) {
        ESP_LOGW(TAG, "Device info not available - sending dongle hardware info only (family=esp, board=%s)",
                 CONFIG_MOUTHPAD_BOARD_NAME);
        // Send response with family/board only (no bonded MouthPad info)
        return relay_protocol_send_response(&relay_msg);
    }

    // Bonded MouthPad device info available - add it to the response
    // Set up callbacks for string fields
    if (strlen(device_info->device_name) > 0) {
        relay_msg.message_body.device_info_response.name.funcs.encode = encode_string_callback;
        relay_msg.message_body.device_info_response.name.arg = (void *)device_info->device_name;
    }

    if (strlen(device_info->firmware_revision) > 0) {
        relay_msg.message_body.device_info_response.firmware.funcs.encode = encode_string_callback;
        relay_msg.message_body.device_info_response.firmware.arg = (void *)device_info->firmware_revision;
    }

    // Format BLE address as string
    // Try to get active address first (if connected), otherwise use bonded device address
    char address_str[18];
    esp_bd_addr_t addr;
    bool have_address = false;

    if (transport_hid_get_active_address(addr) == ESP_OK) {
        // Connected - use active address
        have_address = true;
    } else if (ble_bonds_get_bonded_device(addr) == ESP_OK) {
        // Not connected but have bonded device - use bonded address
        have_address = true;
    }

    if (have_address) {
        snprintf(address_str, sizeof(address_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 addr[0], addr[1], addr[2],
                 addr[3], addr[4], addr[5]);
        relay_msg.message_body.device_info_response.address.funcs.encode = encode_string_callback;
        relay_msg.message_body.device_info_response.address.arg = address_str;
    }

    // Set VID/PID from PnP ID
    if (device_info->has_pnp_id) {
        relay_msg.message_body.device_info_response.vid = device_info->pnp_id.vendor_id;
        relay_msg.message_body.device_info_response.pid = device_info->pnp_id.product_id;
    }

    ESP_LOGI(TAG, "Sending device info: name=%s, vid=0x%04X, pid=0x%04X, family=esp, board=%s",
             device_info->device_name,
             relay_msg.message_body.device_info_response.vid,
             relay_msg.message_body.device_info_response.pid,
             CONFIG_MOUTHPAD_BOARD_NAME);

    return relay_protocol_send_response(&relay_msg);
}

static esp_err_t handle_clear_bonds_write(void) {
    ESP_LOGI(TAG, "ClearBondsWrite command - performing bond reset");

    mouthware_message_RelayToAppMessage relay_msg = mouthware_message_RelayToAppMessage_init_zero;
    relay_msg.which_message_body = mouthware_message_RelayToAppMessage_clear_bonds_response_tag;

    // Perform bond reset (disconnect device + clear all bonds)
    esp_err_t ret = perform_bond_reset();
    relay_msg.message_body.clear_bonds_response.success = (ret == ESP_OK);

    return relay_protocol_send_response(&relay_msg);
}

static esp_err_t handle_dfu_write(void) {
    ESP_LOGI(TAG, "=== DFU REQUEST (via protobuf) - entering bootloader ===");

    mouthware_message_RelayToAppMessage relay_msg = mouthware_message_RelayToAppMessage_init_zero;
    relay_msg.which_message_body = mouthware_message_RelayToAppMessage_dfu_response_tag;
    relay_msg.message_body.dfu_response.success = true;

    // Send success response before reset
    esp_err_t ret = relay_protocol_send_response(&relay_msg);

    // Give time for response to be sent
    vTaskDelay(pdMS_TO_TICKS(100));

    // Enter DFU/bootloader mode (will reset device)
    usb_dfu_enter_dfu();

    return ret;
}

static esp_err_t handle_pass_through_to_mouthpad(const uint8_t *data, size_t len) {
    mouthware_message_RelayToAppMessage relay_msg = mouthware_message_RelayToAppMessage_init_zero;
    relay_msg.which_message_body = mouthware_message_RelayToAppMessage_pass_through_to_mouthpad_response_tag;

    // Check if NUS is ready
    if (!ble_nus_client_is_ready()) {
        ESP_LOGW(TAG, "NUS not ready, cannot forward data");
        relay_msg.message_body.pass_through_to_mouthpad_response.error_code =
            mouthware_message_PassThroughToMouthpadErrorCode_PASS_THROUGH_TO_MOUTHPAD_ERROR_CODE_MESSAGE_NOT_CONNECTED;
        return relay_protocol_send_response(&relay_msg);
    }

    // Check message size
    if (len > 240) {  // Max size from protobuf definition
        ESP_LOGW(TAG, "Message too large: %d bytes", len);
        relay_msg.message_body.pass_through_to_mouthpad_response.error_code =
            mouthware_message_PassThroughToMouthpadErrorCode_PASS_THROUGH_TO_MOUTHPAD_ERROR_CODE_MESSAGE_TOO_LARGE;
        return relay_protocol_send_response(&relay_msg);
    }

    // Forward data to BLE NUS
    ESP_LOGD(TAG, "Forwarding %d bytes to MouthPad via NUS", len);
    esp_err_t ret = ble_nus_client_send_data(data, len);

    if (ret == ESP_OK) {
        relay_msg.message_body.pass_through_to_mouthpad_response.error_code =
            mouthware_message_PassThroughToMouthpadErrorCode_PASS_THROUGH_TO_MOUTHPAD_ERROR_CODE_UNSPECIFIED;
    } else if (ret == ESP_ERR_TIMEOUT) {
        relay_msg.message_body.pass_through_to_mouthpad_response.error_code =
            mouthware_message_PassThroughToMouthpadErrorCode_PASS_THROUGH_TO_MOUTHPAD_ERROR_CODE_TIMEOUT;
    } else {
        relay_msg.message_body.pass_through_to_mouthpad_response.error_code =
            mouthware_message_PassThroughToMouthpadErrorCode_PASS_THROUGH_TO_MOUTHPAD_ERROR_CODE_UNKNOWN_ERROR;
    }

    return relay_protocol_send_response(&relay_msg);
}
