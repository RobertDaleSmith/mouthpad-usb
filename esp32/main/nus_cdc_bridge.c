#include "nus_cdc_bridge.h"
#include "ble_nus.h"
#include "usb_cdc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "string.h"

static const char *TAG = "NUS_CDC_BRIDGE";

// Bridge state
static bool s_bridge_active = false;
static bool s_nus_connected = false;
static bool s_cdc_ready = false;

// Callback functions for NUS client events
static void nus_client_data_received_cb(const uint8_t *data, uint16_t len)
{
    if (!s_bridge_active) {
        ESP_LOGW(TAG, "Bridge not active, dropping %d bytes", len);
        return;
    }

    // Check CDC readiness dynamically
    bool cdc_ready = usb_cdc_is_ready();
    if (!cdc_ready) {
        ESP_LOGW(TAG, "CDC not ready, dropping %d bytes", len);
        return;
    }

    // Forward to CDC without verbose logging during streaming
    esp_err_t ret = usb_cdc_send_data(data, len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send data to CDC: %s", esp_err_to_name(ret));
    }
}

static void nus_client_data_sent_cb(esp_err_t status)
{
    if (status != ESP_OK) {
        ESP_LOGW(TAG, "NUS client data send failed: %s", esp_err_to_name(status));
    }
}

static void nus_client_connected_cb(void)
{
    ESP_LOGI(TAG, "NUS client connected");
    s_nus_connected = true;
}

static void nus_client_disconnected_cb(void)
{
    ESP_LOGI(TAG, "NUS client disconnected");
    s_nus_connected = false;
}

// Callback functions for CDC events
static void cdc_data_received_cb(const uint8_t *data, uint16_t len)
{
    // Fast path: forward data without verbose logging during streaming
    if (!s_bridge_active) {
        ESP_LOGW(TAG, "Bridge not active, dropping %d bytes", len);
        return;
    }

    if (!s_nus_connected) {
        ESP_LOGW(TAG, "NUS not connected, dropping %d bytes", len);
        return;
    }

    // Forward to NUS silently
    esp_err_t ret = ble_nus_client_send_data(data, len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send data to NUS client: %s", esp_err_to_name(ret));
    }
}

static void cdc_data_sent_cb(esp_err_t status)
{
    if (status != ESP_OK) {
        ESP_LOGW(TAG, "CDC data send failed: %s", esp_err_to_name(status));
    }
}

esp_err_t nus_cdc_bridge_init(void)
{
    ESP_LOGI(TAG, "Initializing NUS client to CDC bridge");

    // Initialize NUS client
    ble_nus_client_config_t nus_config = {
        .data_received_cb = nus_client_data_received_cb,
        .data_sent_cb = nus_client_data_sent_cb,
        .connected_cb = nus_client_connected_cb,
        .disconnected_cb = nus_client_disconnected_cb,
    };

    esp_err_t ret = ble_nus_client_init(&nus_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NUS client: %s", esp_err_to_name(ret));
        return ret;
    }

    // CDC is already initialized by usb_hid_init()
    // Now set up the bridge callbacks
    usb_cdc_config_t cdc_config = {
        .data_received_cb = cdc_data_received_cb,
        .data_sent_cb = cdc_data_sent_cb,
    };

    ret = usb_cdc_update_callbacks(&cdc_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update CDC callbacks: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "NUS client to CDC bridge initialized successfully");
    return ESP_OK;
}

esp_err_t nus_cdc_bridge_start(void)
{
    if (s_bridge_active) {
        ESP_LOGW(TAG, "Bridge already active");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "=== STARTING NUS CDC BRIDGE ===");
    ESP_LOGI(TAG, "CDC ready: %d", usb_cdc_is_ready());

    s_bridge_active = true;
    s_cdc_ready = usb_cdc_is_ready();

    ESP_LOGI(TAG, "NUS client to CDC bridge started successfully");
    ESP_LOGI(TAG, "Bridge active: %d, CDC ready: %d", s_bridge_active, s_cdc_ready);
    ESP_LOGI(TAG, "Waiting for BLE connection to device with NUS service...");
    
    return ESP_OK;
}

esp_err_t nus_cdc_bridge_stop(void)
{
    if (!s_bridge_active) {
        ESP_LOGW(TAG, "Bridge not active");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping NUS client to CDC bridge");

    s_bridge_active = false;
    s_nus_connected = false;
    s_cdc_ready = false;

    ESP_LOGI(TAG, "NUS client to CDC bridge stopped");
    return ESP_OK;
}

bool nus_cdc_bridge_is_active(void)
{
    return s_bridge_active;
}

esp_err_t nus_cdc_bridge_discover_services(esp_gatt_if_t gattc_if, uint16_t conn_id)
{
    if (!s_bridge_active) {
        ESP_LOGW(TAG, "Bridge not active");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGD(TAG, "Starting NUS service discovery on conn_id: %d, gattc_if: %d", conn_id, gattc_if);
    return ble_nus_client_discover_services(gattc_if, conn_id);
}

void nus_cdc_bridge_handle_gattc_event(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    if (s_bridge_active) {
        ble_nus_client_handle_gattc_event(event, gattc_if, param);
    }
}
