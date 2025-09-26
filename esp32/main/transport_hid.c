#include "transport_hid.h"
#include "usb_hid.h"
#include "leds.h"
#include "esp_log.h"
#include "esp_hidh.h"
#include "string.h"

static const char *TAG = "TRANSPORT_HID";

// Bridge state
static bool s_bridge_active = false;
static esp_hidh_dev_t *s_active_dev = NULL;
static uint8_t s_active_addr[6] = {0};
static bool s_has_active_addr = false;

esp_err_t transport_hid_init(void)
{
    ESP_LOGI(TAG, "Initializing BLE HID to USB HID bridge");

    // USB HID is already initialized by app_main
    // Just initialize our bridge state
    s_bridge_active = false;
    s_active_dev = NULL;
    s_has_active_addr = false;
    memset(s_active_addr, 0, sizeof(s_active_addr));

    ESP_LOGI(TAG, "HID transport bridge initialized successfully");
    return ESP_OK;
}

esp_err_t transport_hid_start(void)
{
    if (s_bridge_active) {
        ESP_LOGW(TAG, "HID bridge already active");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "=== STARTING HID BRIDGE ===");
    ESP_LOGI(TAG, "USB HID ready: %d", usb_hid_ready());

    s_bridge_active = true;

    ESP_LOGI(TAG, "HID transport bridge started successfully");
    ESP_LOGI(TAG, "Bridge active: %d", s_bridge_active);
    ESP_LOGI(TAG, "Waiting for BLE HID device connection...");

    return ESP_OK;
}

esp_err_t transport_hid_stop(void)
{
    if (!s_bridge_active) {
        ESP_LOGW(TAG, "HID bridge not active");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping HID transport bridge");

    s_bridge_active = false;
    s_active_dev = NULL;
    s_has_active_addr = false;
    memset(s_active_addr, 0, sizeof(s_active_addr));

    ESP_LOGI(TAG, "HID transport bridge stopped");
    return ESP_OK;
}

bool transport_hid_is_active(void)
{
    return s_bridge_active;
}

esp_err_t transport_hid_set_device(esp_hidh_dev_t *dev, const uint8_t *bda)
{
    if (!s_bridge_active) {
        ESP_LOGW(TAG, "Bridge not active");
        return ESP_ERR_INVALID_STATE;
    }

    if (!dev || !bda) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    s_active_dev = dev;
    memcpy(s_active_addr, bda, sizeof(s_active_addr));
    s_has_active_addr = true;

    ESP_LOGI(TAG, "Active HID device set: %02X:%02X:%02X:%02X:%02X:%02X",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);

    return ESP_OK;
}

void transport_hid_clear_device(void)
{
    ESP_LOGI(TAG, "Clearing active HID device");
    s_active_dev = NULL;
    s_has_active_addr = false;
    memset(s_active_addr, 0, sizeof(s_active_addr));
}

void transport_hid_handle_disconnect(void)
{
    ESP_LOGI(TAG, "Handling HID device disconnect - releasing stuck inputs");

    // Release any stuck HID inputs to prevent buttons/movement from being stuck
    usb_hid_release_all();

    // Clear the device state
    transport_hid_clear_device();
}

esp_err_t transport_hid_handle_input(uint8_t report_id, const uint8_t *data, uint16_t length)
{
    if (!s_bridge_active) {
        ESP_LOGD(TAG, "Bridge not active, dropping HID input");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_active_dev) {
        ESP_LOGD(TAG, "No active HID device, dropping input");
        return ESP_ERR_INVALID_STATE;
    }

    // Check USB HID readiness dynamically
    if (!usb_hid_ready()) {
        ESP_LOGD(TAG, "USB HID not ready, dropping input");
        return ESP_ERR_INVALID_STATE;
    }

    // Forward HID report to USB immediately (fast path)
    usb_hid_send_report(report_id, data, length);

    // Non-critical notification after report sending
    leds_notify_activity();

    return ESP_OK;
}