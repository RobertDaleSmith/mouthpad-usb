#include "usb_cdc.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_chip_info.h"
#include "tusb_cdc_acm.h"
// #include "tusb_console.h" // Not needed since we're not using CDC as console
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "usb_dfu.h"
#include "ble_bonds.h"
#include "ble_hid.h"
#include "ble_dis.h"
#include "leds.h"
#include "transport_hid.h"
#include "esp_gap_ble_api.h"
#include "relay_protocol.h"

static const char *TAG = "USB_CDC";

#define CDC_PACKET_MAX_LEN 64
#define CDC_PACKET_HEADER_LEN 4 // Start markers (2) + length (2)
#define CDC_PACKET_CRC_LEN 2    // CRC-16 (2 bytes)
#define CDC_PACKET_TOTAL_OVERHEAD (CDC_PACKET_HEADER_LEN + CDC_PACKET_CRC_LEN)

// Packet framing constants
#define CDC_PACKET_START_MARKER_1 0xAA
#define CDC_PACKET_START_MARKER_2 0x55

#define CDC_CMD_BUF_LEN 64

static char s_bridge_cmd_buf[CDC_CMD_BUF_LEN];
static size_t s_bridge_cmd_len;
#if CONFIG_TINYUSB_CDC_COUNT > 1
static char s_log_cmd_buf[CDC_CMD_BUF_LEN];
static size_t s_log_cmd_len;
#endif

// Configuration
static usb_cdc_config_t s_cdc_config = {0};

#if CONFIG_TINYUSB_CDC_COUNT < 1
#error "TinyUSB CDC support requires at least one CDC instance"
#endif

#define USB_CDC_PORT_BRIDGE TINYUSB_CDC_ACM_0
#if CONFIG_TINYUSB_CDC_COUNT > 1
#define USB_CDC_PORT_LOG TINYUSB_CDC_ACM_1
#endif
#define USB_CDC_PORT_COUNT CONFIG_TINYUSB_CDC_COUNT

// CDC connection state per interface
static bool s_cdc_connected[USB_CDC_PORT_COUNT];

#if CONFIG_TINYUSB_CDC_COUNT > 1
static SemaphoreHandle_t s_log_mutex;
static vprintf_like_t s_prev_vprintf;
static int usb_cdc_log_vprintf(const char *fmt, va_list args);
#endif

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

static void reset_bridge_cmd_buffer(void) {
  s_bridge_cmd_len = 0;
  s_bridge_cmd_buf[0] = '\0';
}

#if CONFIG_TINYUSB_CDC_COUNT > 1
static void reset_log_cmd_buffer(void) {
  s_log_cmd_len = 0;
  s_log_cmd_buf[0] = '\0';
}
#endif

static void reset_parser(void) {
  s_parser.state = CDC_PARSER_STATE_IDLE;
  s_parser.packet_length = 0;
  s_parser.packet_received = 0;
  s_parser.expected_crc = 0;
  s_parser.calculated_crc = 0;
  memset(s_parser.packet_buffer, 0, sizeof(s_parser.packet_buffer));
}

uint16_t usb_cdc_calculate_crc16(const uint8_t *data, uint16_t len) {
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

static void process_packet_data(const uint8_t *data, uint16_t len) {
  // Forward framed packet data to relay protocol for processing
  esp_err_t ret = relay_protocol_handle_usb_data(data, len);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to process USB data: %s", esp_err_to_name(ret));
  }
}

static void process_bridge_line(void) {
  size_t start = 0;
  size_t end = s_bridge_cmd_len;

  while (start < end && isspace((unsigned char)s_bridge_cmd_buf[start])) {
    ++start;
  }
  while (end > start && isspace((unsigned char)s_bridge_cmd_buf[end - 1])) {
    --end;
  }

  if (end <= start) {
    reset_bridge_cmd_buffer();
    return;
  }

  if (s_cdc_config.data_received_cb) {
    // Forward to NUS without verbose logging during streaming
    s_cdc_config.data_received_cb((uint8_t *)&s_bridge_cmd_buf[start],
                                  (uint16_t)(end - start));
  } else {
    ESP_LOGW(TAG, "Bridge line dropped; no callback registered");
  }

  reset_bridge_cmd_buffer();
}

#if CONFIG_TINYUSB_CDC_COUNT > 1
static void process_log_line(void) {
  size_t start = 0;
  size_t end = s_log_cmd_len;

  while (start < end && isspace((unsigned char)s_log_cmd_buf[start])) {
    ++start;
  }
  while (end > start && isspace((unsigned char)s_log_cmd_buf[end - 1])) {
    --end;
  }

  if (end <= start) {
    reset_log_cmd_buffer();
    return;
  }

  if ((end - start) == 3 && strncmp(&s_log_cmd_buf[start], "dfu", 3) == 0) {
    if (!usb_dfu_pending()) {
      ESP_LOGI(TAG, "DFU command received on CDC1");
      usb_dfu_enter_dfu();
    } else {
      ESP_LOGW(TAG, "DFU trigger already pending");
    }
  } else if ((end - start) == 10 && strncmp(&s_log_cmd_buf[start], "disconnect", 10) == 0) {
    ESP_LOGI(TAG, "DISCONNECT command received on CDC1 - disconnecting BLE device");

    // Disconnect the currently active device using BLE GAP disconnect
    esp_bd_addr_t active_addr;
    if (transport_hid_get_active_address(active_addr) == ESP_OK) {
        ESP_LOGI(TAG, "Disconnecting device: %02X:%02X:%02X:%02X:%02X:%02X",
                 active_addr[0], active_addr[1], active_addr[2],
                 active_addr[3], active_addr[4], active_addr[5]);

        esp_err_t disconnect_ret = esp_ble_gap_disconnect(active_addr);
        if (disconnect_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to disconnect device via GAP: %s", esp_err_to_name(disconnect_ret));
            // Fallback to HID close if GAP disconnect fails
            esp_hidh_dev_t *active_dev = ble_hid_client_get_active_device();
            if (active_dev != NULL) {
                ESP_LOGI(TAG, "Attempting disconnect via HID close");
                esp_hidh_dev_close(active_dev);
            }
        } else {
            ESP_LOGI(TAG, "Disconnect command sent successfully");
        }
    } else {
        ESP_LOGI(TAG, "No active device to disconnect");
    }
  } else if ((end - start) == 5 && strncmp(&s_log_cmd_buf[start], "reset", 5) == 0) {
    ESP_LOGI(TAG, "RESET command received on CDC1 - clearing all bonds");

    // Get bond info for logging
    char bond_info[32];
    ble_bonds_get_info_string(bond_info, sizeof(bond_info));
    ESP_LOGI(TAG, "Clearing bond with: %s", bond_info);

    // Disconnect the currently active device first using BLE GAP disconnect (same as button)
    esp_bd_addr_t active_addr;
    if (transport_hid_get_active_address(active_addr) == ESP_OK) {
        ESP_LOGI(TAG, "Disconnecting active device before clearing bonds");
        esp_err_t disconnect_ret = esp_ble_gap_disconnect(active_addr);
        if (disconnect_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to disconnect device: %s", esp_err_to_name(disconnect_ret));
            // Fallback to HID close if GAP disconnect fails
            esp_hidh_dev_t *active_dev = ble_hid_client_get_active_device();
            if (active_dev != NULL) {
                esp_hidh_dev_close(active_dev);
            }
        }
        // Give disconnect time to complete
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Clear all bonds
    esp_err_t ret = ble_bonds_clear_all();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "All bonds cleared successfully");
        leds_set_state(LED_STATE_SCANNING);  // Visual feedback that bonds were cleared
    } else {
        ESP_LOGW(TAG, "Failed to clear bonds: %s", esp_err_to_name(ret));
    }
  } else if ((end - start) == 7 && strncmp(&s_log_cmd_buf[start], "restart", 7) == 0) {
    ESP_LOGI(TAG, "RESTART command received on CDC1 - restarting firmware");
    vTaskDelay(pdMS_TO_TICKS(100));  // Give log time to flush
    esp_restart();
  } else if ((end - start) == 6 && strncmp(&s_log_cmd_buf[start], "serial", 6) == 0) {
    ESP_LOGI(TAG, "SERIAL command received on CDC1 - displaying USB serial number");

    uint8_t mac[6] = {0};
    esp_err_t ret = esp_efuse_mac_get_default(mac);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "USB Serial Number: %02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        ESP_LOGI(TAG, "Port should show as: usbmodem%02X%02X%02X%02X%02X%02X<N>",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        ESP_LOGW(TAG, "Failed to read MAC address: %s", esp_err_to_name(ret));
    }
  } else if ((end - start) == 7 && strncmp(&s_log_cmd_buf[start], "version", 7) == 0) {
    ESP_LOGI(TAG, "VERSION command received on CDC1 - displaying firmware version");

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "=== MouthPad^USB (ESP32) ===");
    ESP_LOGI(TAG, "Built: %s %s", __DATE__, __TIME__);
    ESP_LOGI(TAG, "ESP-IDF: %s", IDF_VER);
    ESP_LOGI(TAG, "Chip: %s rev%d, %d CPU core(s)",
             CONFIG_IDF_TARGET, chip_info.revision, chip_info.cores);
  } else if ((end - start) == 6 && strncmp(&s_log_cmd_buf[start], "device", 6) == 0) {
    const ble_device_info_t *device_info = ble_device_info_get_current();
    if (device_info && device_info->info_complete) {
        // Format device info and write directly to CDC to avoid log interleaving
        char info_buf[512];
        int len = 0;

        len += snprintf(info_buf + len, sizeof(info_buf) - len, "\r\n=== DEVICE INFORMATION ===\r\n");
        if (strlen(device_info->device_name) > 0) {
            len += snprintf(info_buf + len, sizeof(info_buf) - len, "Name: %s\r\n", device_info->device_name);
        }

        // Add BLE address
        esp_bd_addr_t active_addr;
        if (transport_hid_get_active_address(active_addr) == ESP_OK) {
            len += snprintf(info_buf + len, sizeof(info_buf) - len, "Address: %02X:%02X:%02X:%02X:%02X:%02X\r\n",
                          active_addr[0], active_addr[1], active_addr[2],
                          active_addr[3], active_addr[4], active_addr[5]);
        }
        if (strlen(device_info->manufacturer_name) > 0) {
            len += snprintf(info_buf + len, sizeof(info_buf) - len, "Manufacturer: %s\r\n", device_info->manufacturer_name);
        }
        if (strlen(device_info->model_number) > 0) {
            len += snprintf(info_buf + len, sizeof(info_buf) - len, "Model Number: %s\r\n", device_info->model_number);
        }
        if (strlen(device_info->serial_number) > 0) {
            len += snprintf(info_buf + len, sizeof(info_buf) - len, "Serial Number: %s\r\n", device_info->serial_number);
        }
        if (strlen(device_info->hardware_revision) > 0) {
            len += snprintf(info_buf + len, sizeof(info_buf) - len, "Hardware Revision: %s\r\n", device_info->hardware_revision);
        }
        if (strlen(device_info->firmware_revision) > 0) {
            len += snprintf(info_buf + len, sizeof(info_buf) - len, "Firmware Revision: %s\r\n", device_info->firmware_revision);
        }
        if (strlen(device_info->software_revision) > 0) {
            len += snprintf(info_buf + len, sizeof(info_buf) - len, "Software Revision: %s\r\n", device_info->software_revision);
        }
        if (device_info->has_pnp_id) {
            const char* vendor_source = (device_info->pnp_id.vendor_id_source == 0x02) ? "USB Forum" :
                                       (device_info->pnp_id.vendor_id_source == 0x01) ? "Bluetooth SIG" : "Unknown";
            len += snprintf(info_buf + len, sizeof(info_buf) - len,
                          "Vendor ID: 0x%04X (%s)\r\n", device_info->pnp_id.vendor_id, vendor_source);
            len += snprintf(info_buf + len, sizeof(info_buf) - len,
                          "Product ID: 0x%04X\r\n", device_info->pnp_id.product_id);
            len += snprintf(info_buf + len, sizeof(info_buf) - len,
                          "Product Version: 0x%04X\r\n", device_info->pnp_id.product_version);
        }
        len += snprintf(info_buf + len, sizeof(info_buf) - len, "==========================\r\n");

        // Write directly to CDC bypassing logging system to prevent interleaving
        if (s_cdc_connected[USB_CDC_PORT_LOG]) {
            tinyusb_cdcacm_write_queue(USB_CDC_PORT_LOG, (const uint8_t *)info_buf, len);
            tinyusb_cdcacm_write_flush(USB_CDC_PORT_LOG, 0);
        }
    } else {
        ESP_LOGI(TAG, "No device info available - device may not be connected or DIS not yet discovered");
    }
  } else {
    ESP_LOGW(TAG, "Ignoring command on CDC1: %.*s", (int)(end - start),
             &s_log_cmd_buf[start]);
  }

  reset_log_cmd_buffer();
}
#endif

static void parse_packet_byte(uint8_t byte) {
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
      ESP_LOGW(TAG, "Packet length %d exceeds maximum %d",
               s_parser.packet_length, CDC_PACKET_MAX_LEN);
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
    s_parser.calculated_crc =
        usb_cdc_calculate_crc16(s_parser.packet_buffer, s_parser.packet_length);

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

static void handle_cdc_line_state_changed(int itf, cdcacm_event_t *event) {
  if (!event || event->type != CDC_EVENT_LINE_STATE_CHANGED) {
    return;
  }

  if (itf >= USB_CDC_PORT_COUNT) {
    ESP_LOGW(TAG, "CDC line event for unknown interface %d", itf);
    return;
  }

  bool dtr = event->line_state_changed_data.dtr;
  bool rts = event->line_state_changed_data.rts;

  s_cdc_connected[itf] = dtr && rts;

  ESP_LOGI(TAG, "CDC%d line state changed: DTR=%d, RTS=%d, connected=%d", itf,
           dtr, rts, s_cdc_connected[itf]);
}

#if CONFIG_TINYUSB_CDC_COUNT > 1
static void handle_aux_cdc_rx(int itf) {
  uint8_t buf[64];
  size_t rx = 0;

  while (tinyusb_cdcacm_read(itf, buf, sizeof(buf), &rx) == ESP_OK && rx > 0) {
    ESP_LOGD(TAG, "CDC%d received %d bytes (aux)", itf, (int)rx);
    tinyusb_cdcacm_write_queue(itf, buf, rx);
    esp_err_t err = tinyusb_cdcacm_write_flush(itf, 0);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "CDC%d flush failed (%d)", itf, err);
      break;
    }

    if (rx < sizeof(buf)) {
      break;
    }
  }
}
#endif

static void handle_log_cdc_rx(int itf) {
#if CONFIG_TINYUSB_CDC_COUNT > 1
  uint8_t buf[32];
  size_t rx = 0;

  while (tinyusb_cdcacm_read(itf, buf, sizeof(buf), &rx) == ESP_OK && rx > 0) {
    for (size_t i = 0; i < rx; ++i) {
      char ch = (char)buf[i];

      if (ch == '\r') {
        continue;
      }

      if (ch == '\n') {
        process_log_line();
        continue;
      }

      if (s_log_cmd_len < (CDC_CMD_BUF_LEN - 1)) {
        s_log_cmd_buf[s_log_cmd_len++] = ch;
        s_log_cmd_buf[s_log_cmd_len] = '\0';
      } else {
        ESP_LOGW(TAG, "CDC1 command buffer overflow; resetting");
        reset_log_cmd_buffer();
      }
    }

    if (rx < sizeof(buf)) {
      break;
    }
  }
#else
  (void)itf;
#endif
}

static void handle_cdc_rx(int itf, cdcacm_event_t *event) {
  if (!event || event->type != CDC_EVENT_RX) {
    return;
  }

  if (itf == USB_CDC_PORT_BRIDGE && usb_dfu_pending()) {
    return;
  }

  if (itf >= USB_CDC_PORT_COUNT) {
    ESP_LOGW(TAG, "CDC RX for unknown interface %d", itf);
    return;
  }

  if (itf != USB_CDC_PORT_BRIDGE) {
#if CONFIG_TINYUSB_CDC_COUNT > 1
    if (itf == USB_CDC_PORT_LOG) {
      handle_log_cdc_rx(itf);
    } else {
      handle_aux_cdc_rx(itf);
    }
#endif
    return;
  }

  uint8_t buf[32];
  size_t rx = 0;

  while (tinyusb_cdcacm_read(itf, buf, sizeof(buf), &rx) == ESP_OK && rx > 0) {
    for (size_t i = 0; i < rx; ++i) {
      uint8_t byte = buf[i];
      char ch = (char)byte;

      parse_packet_byte(byte);

      if (ch == '\r') {
        continue;
      }

      if (ch == '\n') {
        process_bridge_line();
        continue;
      }

      if (isprint((unsigned char)ch) || isspace((unsigned char)ch)) {
        if (s_bridge_cmd_len < (CDC_CMD_BUF_LEN - 1)) {
          s_bridge_cmd_buf[s_bridge_cmd_len++] = ch;
          s_bridge_cmd_buf[s_bridge_cmd_len] = '\0';
        } else {
          ESP_LOGW(TAG, "CDC0 command buffer overflow; resetting");
          reset_bridge_cmd_buffer();
        }
      }
    }

    if (rx < sizeof(buf)) {
      break;
    }
  }
}

esp_err_t usb_cdc_init(const usb_cdc_config_t *config) {
  if (config == NULL) {
    ESP_LOGE(TAG, "Configuration cannot be NULL");
    return ESP_ERR_INVALID_ARG;
  }

  // Store configuration
  memcpy(&s_cdc_config, config, sizeof(usb_cdc_config_t));

  // Initialize parser and connection bookkeeping
  reset_parser();
  reset_bridge_cmd_buffer();
#if CONFIG_TINYUSB_CDC_COUNT > 1
  reset_log_cmd_buffer();
#endif
  memset(s_cdc_connected, 0, sizeof(s_cdc_connected));

  ESP_LOGI(TAG, "Initializing %d CDC ports...", USB_CDC_PORT_COUNT);

  // Initialize CDC0
  ESP_LOGI(TAG, "CDC0: Registering callbacks (rx_cb=%p, line_cb=%p)",
           handle_cdc_rx, handle_cdc_line_state_changed);

  tinyusb_config_cdcacm_t acm_cfg0 = {
      .cdc_port = TINYUSB_CDC_ACM_0,
      .callback_rx = handle_cdc_rx,
      .callback_rx_wanted_char = NULL,
      .callback_line_state_changed = handle_cdc_line_state_changed,
      .callback_line_coding_changed = NULL,
  };

  esp_err_t err = tinyusb_cdcacm_init(&acm_cfg0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "CDC0 ACM init failed: %s (0x%x)", esp_err_to_name(err), err);
    return err;
  }
  ESP_LOGI(TAG, "CDC0 ACM initialized successfully");

#if CONFIG_TINYUSB_CDC_COUNT > 1
  // Initialize CDC1
  ESP_LOGI(TAG, "CDC1: Registering callbacks (rx_cb=%p, line_cb=%p)",
           handle_cdc_rx, handle_cdc_line_state_changed);

  tinyusb_config_cdcacm_t acm_cfg1 = {
      .cdc_port = TINYUSB_CDC_ACM_1,
      .callback_rx = handle_cdc_rx,
      .callback_rx_wanted_char = NULL,
      .callback_line_state_changed = handle_cdc_line_state_changed,
      .callback_line_coding_changed = NULL,
  };

  err = tinyusb_cdcacm_init(&acm_cfg1);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "CDC1 ACM init failed: %s (0x%x)", esp_err_to_name(err), err);
    return err;
  }
  ESP_LOGI(TAG, "CDC1 ACM initialized successfully");
#endif

#if CONFIG_TINYUSB_CDC_COUNT > 1
  if (s_log_mutex == NULL) {
    s_log_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_log_mutex != NULL, ESP_ERR_NO_MEM, TAG,
                        "Failed to create CDC log mutex");
    s_prev_vprintf = esp_log_set_vprintf(usb_cdc_log_vprintf);
    ESP_LOGI(TAG, "CDC%d configured for logging output", USB_CDC_PORT_LOG);
  }
#endif

  ESP_LOGI(TAG, "CDC bridge ready (CDC0: bridge, CDC1: logs when connected)");
  return ESP_OK;
}

esp_err_t usb_cdc_send_data(const uint8_t *data, uint16_t len) {
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
  tinyusb_cdcacm_write_queue(USB_CDC_PORT_BRIDGE,
                             (uint8_t[]){CDC_PACKET_START_MARKER_1}, 1);
  tinyusb_cdcacm_write_queue(USB_CDC_PORT_BRIDGE,
                             (uint8_t[]){CDC_PACKET_START_MARKER_2}, 1);

  // Send packet length (2 bytes, big-endian)
  uint8_t length_bytes[2] = {(len >> 8) & 0xFF, len & 0xFF};
  tinyusb_cdcacm_write_queue(USB_CDC_PORT_BRIDGE, length_bytes, 2);

  // Send packet data
  tinyusb_cdcacm_write_queue(USB_CDC_PORT_BRIDGE, data, len);

  // Send CRC (2 bytes, big-endian)
  uint8_t crc_bytes[2] = {(crc >> 8) & 0xFF, crc & 0xFF};
  tinyusb_cdcacm_write_queue(USB_CDC_PORT_BRIDGE, crc_bytes, 2);

  // Flush the queue
  tinyusb_cdcacm_write_flush(USB_CDC_PORT_BRIDGE, 0);

  if (s_cdc_config.data_sent_cb) {
    s_cdc_config.data_sent_cb(ESP_OK);
  }

  return ESP_OK;
}

bool usb_cdc_is_ready(void) { return s_cdc_connected[USB_CDC_PORT_BRIDGE]; }

esp_err_t usb_cdc_update_callbacks(const usb_cdc_config_t *config) {
  if (!config) {
    return ESP_ERR_INVALID_ARG;
  }

  // Update the stored configuration
  s_cdc_config = *config;

  ESP_LOGD(TAG, "CDC callbacks updated successfully");
  return ESP_OK;
}

#if CONFIG_TINYUSB_CDC_COUNT > 1

#define USB_CDC_LOG_STACK_BUFFER 256

static esp_err_t usb_cdc_log_write(const char *data, size_t len) {
  if (!data || len == 0) {
    return ESP_OK;
  }

  if (!s_cdc_connected[USB_CDC_PORT_LOG]) {
    return ESP_ERR_INVALID_STATE;
  }

  size_t offset = 0;
  while (offset < len) {
    size_t chunk = len - offset;
    const esp_err_t err = tinyusb_cdcacm_write_queue(
        USB_CDC_PORT_LOG, (const uint8_t *)&data[offset], chunk);
    if (err != ESP_OK) {
      return err;
    }
    offset += chunk;
  }

  return tinyusb_cdcacm_write_flush(USB_CDC_PORT_LOG, 0);
}

static int usb_cdc_log_vprintf(const char *fmt, va_list args) {
  if (!s_log_mutex) {
    return s_prev_vprintf ? s_prev_vprintf(fmt, args) : vprintf(fmt, args);
  }

  int written_chars = 0;
  bool handled = false;

  if (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    if (s_cdc_connected[USB_CDC_PORT_LOG]) {
      va_list length_args;
      va_copy(length_args, args);
      int needed = vsnprintf(NULL, 0, fmt, length_args);
      va_end(length_args);

      if (needed >= 0) {
        size_t buf_size = (size_t)needed + 1;
        char stack_buf[USB_CDC_LOG_STACK_BUFFER];
        char *buffer = stack_buf;

        if (buf_size > sizeof(stack_buf)) {
          buffer = (char *)malloc(buf_size);
        }

        if (buffer) {
          va_list copy_args;
          va_copy(copy_args, args);
          vsnprintf(buffer, buf_size, fmt, copy_args);
          va_end(copy_args);

          if (usb_cdc_log_write(buffer, buf_size - 1) == ESP_OK) {
            written_chars = needed;
            handled = true;
          }

          if (buffer != stack_buf) {
            free(buffer);
          }
        }
      }
    }

    xSemaphoreGive(s_log_mutex);
  }

  if (handled) {
    return written_chars;
  }

  return s_prev_vprintf ? s_prev_vprintf(fmt, args) : vprintf(fmt, args);
}

#endif // CONFIG_TINYUSB_CDC_COUNT > 1
