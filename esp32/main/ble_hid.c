#include "ble_hid.h"
#include "esp_log.h"
#include "esp_hidh.h"
#include "esp_hid_common.h"
#include "string.h"

static const char *TAG = "BLE_HID";

// Client state and callbacks
static ble_hid_client_config_t s_config = {0};
static esp_hidh_dev_t *s_active_dev = NULL;
static bool s_connected = false;

// Helper function to log HID reports
static void log_report(const uint8_t *data, size_t length)
{
    if (!data || !length) {
        ESP_LOGI(TAG, "Empty report");
        return;
    }

    char buffer[3 * 32 + 1];
    size_t to_print = length < 32 ? length : 32;
    for (size_t i = 0; i < to_print; ++i) {
        snprintf(buffer + i * 3, sizeof(buffer) - i * 3, "%02X ", data[i]);
    }
    buffer[to_print * 3] = '\0';
    ESP_LOGI(TAG, "Report (%d bytes): %s%s", (int)length, buffer,
             length > to_print ? "..." : "");
}

// Helper function to log device address
static void log_addr(const uint8_t *addr)
{
    if (!addr) {
        ESP_LOGI(TAG, "(unknown addr)");
        return;
    }
    ESP_LOGI(TAG, "Address: %02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

esp_err_t ble_hid_client_init(const ble_hid_client_config_t *config)
{
    if (!config) {
        ESP_LOGE(TAG, "Invalid config");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Initializing BLE HID client");

    // Store configuration
    memcpy(&s_config, config, sizeof(ble_hid_client_config_t));

    // Initialize state
    s_active_dev = NULL;
    s_connected = false;

    ESP_LOGI(TAG, "BLE HID client initialized successfully");
    return ESP_OK;
}

void ble_hid_client_handle_event(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_hidh_event_t event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t *param = (esp_hidh_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDH_OPEN_EVENT:
        if (param->open.dev) {
            const uint8_t *bda = esp_hidh_dev_bda_get(param->open.dev);
            ESP_LOGI(TAG, "=== HID DEVICE CONNECTED ===");
            ESP_LOGI(TAG, "Name: %s", esp_hidh_dev_name_get(param->open.dev));
            if (bda) {
                log_addr(bda);
            }

            // Update state
            s_active_dev = param->open.dev;
            s_connected = true;

            // Notify transport layer via callback
            if (s_config.connected_cb && bda) {
                s_config.connected_cb(param->open.dev, bda);
            }
        }
        break;

    case ESP_HIDH_BATTERY_EVENT:
        ESP_LOGI(TAG, "Battery event");
        if (s_config.battery_cb) {
            s_config.battery_cb(param->battery.level, param->battery.status);
        }
        if (param->battery.status != ESP_OK) {
            ESP_LOGW(TAG, "Battery event error: %d", param->battery.status);
        }
        break;

    case ESP_HIDH_INPUT_EVENT:
        if (param->input.dev) {
            // Forward to transport layer via callback (fast path)
            if (s_config.input_cb) {
                s_config.input_cb(param->input.report_id, param->input.data, param->input.length);
            }
            // Optional verbose logging can be enabled here if needed
        }
        break;

    case ESP_HIDH_FEATURE_EVENT:
        if (param->feature.dev) {
            ESP_LOGI(TAG, "Feature report (usage=%s, id=%u)",
                     esp_hid_usage_str(param->feature.usage), param->feature.report_id);
            log_report(param->feature.data, param->feature.length);

            // Notify callback if set
            if (s_config.feature_cb) {
                s_config.feature_cb(param->feature.dev, param->feature.report_id,
                                  param->feature.data, param->feature.length, param->feature.usage);
            }
        }
        break;

    case ESP_HIDH_CLOSE_EVENT:
        ESP_LOGI(TAG, "HID device disconnected");

        // Update state
        if (param->close.dev == s_active_dev) {
            s_active_dev = NULL;
            s_connected = false;
        }

        // Notify transport layer via callback
        if (s_config.disconnected_cb) {
            s_config.disconnected_cb(param->close.dev);
        }

        // Clean up device handle
        if (param->close.dev) {
            esp_hidh_dev_free(param->close.dev);
        }
        break;

    default:
        ESP_LOGD(TAG, "Unhandled HID event %d", event);
        break;
    }
}

esp_hidh_dev_t *ble_hid_client_get_active_device(void)
{
    return s_active_dev;
}

bool ble_hid_client_is_connected(void)
{
    return s_connected && s_active_dev != NULL;
}