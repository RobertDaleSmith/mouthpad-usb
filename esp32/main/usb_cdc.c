#include "usb_cdc.h"

#include "esp_log.h"
#include "tusb_cdc_acm.h"
// #include "tusb_console.h" // Not needed since we're not using CDC as console
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <ctype.h>
#include <string.h>

#include "bootloader_trigger.h"

static const char *TAG = "usb_cdc";

#define CDC_CMD_BUF_LEN 16
#define CDC_PACKET_MAX_LEN 64
#define CDC_PACKET_HEADER_LEN 4  // Start markers (2) + length (2)
#define CDC_PACKET_CRC_LEN 2     // CRC-16 (2 bytes)
#define CDC_PACKET_TOTAL_OVERHEAD (CDC_PACKET_HEADER_LEN + CDC_PACKET_CRC_LEN)

// Packet framing constants
#define CDC_PACKET_START_MARKER_1 0xAA
#define CDC_PACKET_START_MARKER_2 0x55

static char s_cmd_buf[CDC_CMD_BUF_LEN];
static size_t s_cmd_len;

// Configuration
static usb_cdc_config_t s_cdc_config = {0};

// CDC connection state
static bool s_cdc_connected = false;

// Packet parsing state
typedef enum {
    CDC_PARSER_STATE_IDLE,
    CDC_PARSER_STATE_WAIT_MARKER_1,
    CDC_PARSER_STATE_WAIT_MARKER_2,
    CDC_PARSER_STATE_WAIT_LENGTH_HIGH,
    CDC_PARSER_STATE_WAIT_LENGTH_LOW,
    CDC_PARSER_STATE_WAIT_DATA,
    CDC_PARSER_STATE_WAIT_CRC_HIGH,
    CDC_PARSER_STATE_WAIT_CRC_LOW
} cdc_parser_state_t;

typedef struct {
    cdc_parser_state_t state;
    uint8_t packet_buffer[CDC_PACKET_MAX_LEN];
    uint16_t packet_length;
    uint16_t packet_received;
    uint16_t expected_crc;
    uint16_t calculated_crc;
} cdc_parser_t;

static cdc_parser_t s_parser = {0};

static void reset_cmd_buffer(void)
{
    s_cmd_len = 0;
    s_cmd_buf[0] = '\0';
}

static void reset_parser(void)
{
    s_parser.state = CDC_PARSER_STATE_IDLE;
    s_parser.packet_length = 0;
    s_parser.packet_received = 0;
    s_parser.expected_crc = 0;
    s_parser.calculated_crc = 0;
    memset(s_parser.packet_buffer, 0, sizeof(s_parser.packet_buffer));
}

uint16_t usb_cdc_calculate_crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF; // Initial value
    
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021; // CCITT polynomial
            } else {
                crc = crc << 1;
            }
        }
    }
    
    return crc & 0xFFFF;
}

static void process_packet_data(const uint8_t *data, uint16_t len)
{
    ESP_LOGI(TAG, "=== CDC PACKET RECEIVED ===");
    ESP_LOGI(TAG, "Data: %.*s", len, data);
    ESP_LOGI(TAG, "Callback: %p", s_cdc_config.data_received_cb);
    
    // Call the data received callback
    if (s_cdc_config.data_received_cb) {
        ESP_LOGI(TAG, "Calling data received callback");
        s_cdc_config.data_received_cb(data, len);
    } else {
        ESP_LOGW(TAG, "No data received callback set!");
    }
}

static void parse_packet_byte(uint8_t byte)
{
    switch (s_parser.state) {
    case CDC_PARSER_STATE_IDLE:
        if (byte == CDC_PACKET_START_MARKER_1) {
            s_parser.state = CDC_PARSER_STATE_WAIT_MARKER_2;
        }
        break;
        
    case CDC_PARSER_STATE_WAIT_MARKER_2:
        if (byte == CDC_PACKET_START_MARKER_2) {
            s_parser.state = CDC_PARSER_STATE_WAIT_LENGTH_HIGH;
        } else {
            reset_parser();
        }
        break;
        
    case CDC_PARSER_STATE_WAIT_LENGTH_HIGH:
        s_parser.packet_length = (uint16_t)byte << 8;
        s_parser.state = CDC_PARSER_STATE_WAIT_LENGTH_LOW;
        break;
        
    case CDC_PARSER_STATE_WAIT_LENGTH_LOW:
        s_parser.packet_length |= byte;
        if (s_parser.packet_length > CDC_PACKET_MAX_LEN) {
            ESP_LOGW(TAG, "Packet length %d exceeds maximum %d", s_parser.packet_length, CDC_PACKET_MAX_LEN);
            reset_parser();
        } else {
            s_parser.packet_received = 0;
            s_parser.state = CDC_PARSER_STATE_WAIT_DATA;
        }
        break;
        
    case CDC_PARSER_STATE_WAIT_DATA:
        if (s_parser.packet_received < s_parser.packet_length) {
            s_parser.packet_buffer[s_parser.packet_received] = byte;
            s_parser.packet_received++;
        }
        
        if (s_parser.packet_received >= s_parser.packet_length) {
            s_parser.state = CDC_PARSER_STATE_WAIT_CRC_HIGH;
        }
        break;
        
    case CDC_PARSER_STATE_WAIT_CRC_HIGH:
        s_parser.expected_crc = (uint16_t)byte << 8;
        s_parser.state = CDC_PARSER_STATE_WAIT_CRC_LOW;
        break;
        
    case CDC_PARSER_STATE_WAIT_CRC_LOW:
        s_parser.expected_crc |= byte;
        
        // Calculate CRC for received data
        s_parser.calculated_crc = usb_cdc_calculate_crc16(s_parser.packet_buffer, s_parser.packet_length);
        
        if (s_parser.calculated_crc == s_parser.expected_crc) {
            // Valid packet received
            process_packet_data(s_parser.packet_buffer, s_parser.packet_length);
        } else {
            ESP_LOGW(TAG, "CRC mismatch: expected 0x%04X, calculated 0x%04X", 
                     s_parser.expected_crc, s_parser.calculated_crc);
        }
        
        reset_parser();
        break;
        
    default:
        reset_parser();
        break;
    }
}

static void process_command_buffer(void)
{
    size_t len = s_cmd_len;
    if (len == 0) {
        return;
    }

    while (len > 0 && isspace((unsigned char)s_cmd_buf[len - 1])) {
        --len;
    }
    size_t start = 0;
    while (start < len && isspace((unsigned char)s_cmd_buf[start])) {
        ++start;
    }

    if (len - start == 3 && strncmp(&s_cmd_buf[start], "dfu", 3) == 0) {
        if (!bootloader_trigger_pending()) {
            bootloader_trigger_enter_dfu();
        }
    } else {
        // Forward non-DFU commands to the NUS bridge
                ESP_LOGI(TAG, "Forwarding command to NUS bridge: %.*s", (int)(len - start), &s_cmd_buf[start]);
                if (s_cdc_config.data_received_cb) {
                    // Pass command as-is to NUS bridge
                    s_cdc_config.data_received_cb((uint8_t*)&s_cmd_buf[start], len - start);
                }
    }

    reset_cmd_buffer();
}

static void handle_cdc_line_state_changed(int itf, cdcacm_event_t *event)
{
    (void)itf;
    
    if (!event || event->type != CDC_EVENT_LINE_STATE_CHANGED) {
        return;
    }
    
    bool dtr = event->line_state_changed_data.dtr;
    bool rts = event->line_state_changed_data.rts;
    
    s_cdc_connected = dtr && rts;
    
    ESP_LOGI(TAG, "CDC line state changed: DTR=%d, RTS=%d, connected=%d", 
             dtr, rts, s_cdc_connected);
}

static void handle_cdc_rx(int itf, cdcacm_event_t *event)
{
    (void)itf;

    if (!event || event->type != CDC_EVENT_RX || bootloader_trigger_pending()) {
        return;
    }

    uint8_t buf[32];
    size_t rx = 0;

    while (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, buf, sizeof(buf), &rx) == ESP_OK && rx > 0) {
        for (size_t i = 0; i < rx; ++i) {
            uint8_t byte = buf[i];
            char ch = (char)byte;

            // First try to parse as packet data
            parse_packet_byte(byte);

            // Also handle as command line interface
            if (ch == '\r') {
                continue;
            }

            if (ch == '\n') {
                process_command_buffer();
                continue;
            }

            if (s_cmd_len < (CDC_CMD_BUF_LEN - 1)) {
                s_cmd_buf[s_cmd_len++] = (char)ch;
                s_cmd_buf[s_cmd_len] = '\0';
            } else {
                reset_cmd_buffer();
            }
        }

        if (rx < sizeof(buf)) {
            break;
        }
    }
}

esp_err_t usb_cdc_init(const usb_cdc_config_t *config)
{
    if (config == NULL) {
        ESP_LOGE(TAG, "Configuration cannot be NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // Store configuration
    memcpy(&s_cdc_config, config, sizeof(usb_cdc_config_t));

    // Initialize parser
    reset_parser();

    const tinyusb_config_cdcacm_t acm_cfg = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = handle_cdc_rx,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = handle_cdc_line_state_changed,
        .callback_line_coding_changed = NULL,
    };

    ESP_RETURN_ON_ERROR(tusb_cdc_acm_init(&acm_cfg), TAG, "CDC ACM init failed");
    // Note: NOT initializing CDC as console - keeping it for data bridge only
    // Logs will go to UART0 (115200 baud) instead
    ESP_LOGI(TAG, "CDC ACM ready for data bridge (logs on UART0)");
    return ESP_OK;
}

esp_err_t usb_cdc_send_data(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0) {
        ESP_LOGE(TAG, "Invalid data or length");
        return ESP_ERR_INVALID_ARG;
    }

    // No length limit needed with proper packet framing
    // The framing protocol handles variable-length packets

    ESP_LOGD(TAG, "Sending %d bytes with packet framing", len);

    // Calculate CRC-16 for the payload
    uint16_t crc = usb_cdc_calculate_crc16(data, len);

    // Send dual start markers
    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (uint8_t[]){CDC_PACKET_START_MARKER_1}, 1);
    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (uint8_t[]){CDC_PACKET_START_MARKER_2}, 1);

    // Send packet length (2 bytes, big-endian)
    uint8_t length_bytes[2] = {(len >> 8) & 0xFF, len & 0xFF};
    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, length_bytes, 2);

    // Send packet data
    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, data, len);

    // Send CRC (2 bytes, big-endian)
    uint8_t crc_bytes[2] = {(crc >> 8) & 0xFF, crc & 0xFF};
    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, crc_bytes, 2);

    // Flush the queue
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);

    if (s_cdc_config.data_sent_cb) {
        s_cdc_config.data_sent_cb(ESP_OK);
    }

    return ESP_OK;
}

bool usb_cdc_is_ready(void)
{
    // Return the tracked connection state
    return s_cdc_connected;
}

esp_err_t usb_cdc_update_callbacks(const usb_cdc_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "=== UPDATING CDC CALLBACKS ===");
    ESP_LOGI(TAG, "New data_received_cb: %p", config->data_received_cb);
    ESP_LOGI(TAG, "New data_sent_cb: %p", config->data_sent_cb);
    
    // Update the stored configuration
    s_cdc_config = *config;
    
    ESP_LOGI(TAG, "CDC callbacks updated successfully");
    return ESP_OK;
}
