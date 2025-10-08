#include "ble_nus.h"
#include "esp_log.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "string.h"

static const char *TAG = "BLE_NUS";

// NUS client state
static esp_gatt_if_t nus_gattc_if = ESP_GATT_IF_NONE;
static uint16_t nus_conn_id = 0xFFFF;
static esp_bd_addr_t nus_server_bda = {0};  // Server's BD address
static bool nus_connected = false;
static bool nus_service_discovered = false;
static bool nus_tx_notify_enabled = false;
static bool nus_connection_ready = false;  // Set when GAP conn params updated

// Service and characteristic handles
static esp_gatt_id_t nus_service_handle = {0};
static uint16_t nus_char_tx_handle = 0;   // Handle for TX characteristic (notifications from server)
static uint16_t nus_char_rx_handle = 0;   // Handle for RX characteristic (write to server)
static uint16_t nus_cccd_handle = 0;      // Handle for CCCD descriptor
static uint16_t nus_service_start_handle = 0;  // Start handle of NUS service
static uint16_t nus_service_end_handle = 0;    // End handle of NUS service


// Configuration callbacks
static ble_nus_client_config_t nus_config = {0};

// Queue for sending data
static QueueHandle_t nus_tx_queue = NULL;

// Flag to trigger CCCD write from dedicated task
static volatile bool cccd_write_pending = false;

// Data structure for TX queue
typedef struct {
    uint8_t data[NUS_MAX_DATA_LEN];
    uint16_t len;
} nus_tx_data_t;

// Task for handling TX data
static void nus_tx_task(void *pvParameters);

// Task for handling CCCD write
static void nus_cccd_task(void *pvParameters);

esp_err_t ble_nus_client_init(const ble_nus_client_config_t *config)
{
    if (config == NULL) {
        ESP_LOGE(TAG, "Configuration cannot be NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // Store configuration
    memcpy(&nus_config, config, sizeof(ble_nus_client_config_t));

    // Create TX queue
    nus_tx_queue = xQueueCreate(5, sizeof(nus_tx_data_t));
    if (nus_tx_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create TX queue");
        return ESP_ERR_NO_MEM;
    }

    // Create TX task
    BaseType_t task_ret = xTaskCreate(nus_tx_task, "nus_tx", 4096, NULL, 5, NULL);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create TX task");
        return ESP_ERR_NO_MEM;
    }

    // Create CCCD task for deferred CCCD write operations
    task_ret = xTaskCreate(nus_cccd_task, "nus_cccd", 4096, NULL, 5, NULL);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create CCCD task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "NUS client initialized");
    return ESP_OK;
}

esp_err_t ble_nus_client_discover_services(esp_gatt_if_t gattc_if, uint16_t conn_id)
{
    ESP_LOGD(TAG, "Starting NUS service discovery on conn_id: %d, gattc_if: %d", conn_id, gattc_if);
    
    // Check if NUS service is already discovered
    if (nus_service_discovered && nus_connected && nus_conn_id == conn_id) {
        ESP_LOGI(TAG, "NUS service already discovered, skipping discovery");
        return ESP_OK;
    }
    
    nus_gattc_if = gattc_if;
    nus_conn_id = conn_id;
    nus_connected = false;  // Don't set to true until service is actually discovered
    nus_service_discovered = false;
    nus_tx_notify_enabled = false;

    // Start service discovery on the dedicated NUS GATT interface
    // First open connection on NUS interface using stored BD address, then search for services
    ESP_LOGD(TAG, "Starting NUS service discovery on dedicated NUS GATT interface...");

    // Use the stored server BD address from the HID connection
    if (nus_server_bda[0] == 0 && nus_server_bda[1] == 0 && nus_server_bda[2] == 0 &&
        nus_server_bda[3] == 0 && nus_server_bda[4] == 0 && nus_server_bda[5] == 0) {
        ESP_LOGE(TAG, "Server BD address not available, cannot open NUS GATT connection");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGD(TAG, "Opening GATT connection on NUS interface to %02x:%02x:%02x:%02x:%02x:%02x",
             nus_server_bda[0], nus_server_bda[1], nus_server_bda[2],
             nus_server_bda[3], nus_server_bda[4], nus_server_bda[5]);

    // Open connection on NUS GATT interface
    esp_err_t ret = esp_ble_gattc_open(nus_gattc_if, nus_server_bda, BLE_ADDR_TYPE_PUBLIC, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open GATT connection: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGD(TAG, "Opened GATT connection on NUS interface, service discovery will start in OPEN event");

    if (nus_config.connected_cb) {
        nus_config.connected_cb();
    }

    return ESP_OK;
}

esp_err_t ble_nus_client_send_data(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0) {
        ESP_LOGE(TAG, "Invalid data or length");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "Sending %d bytes to NUS RX handle %d", len, nus_char_rx_handle);
    
    if (!nus_connected || !nus_service_discovered || nus_char_rx_handle == 0) {
        ESP_LOGW(TAG, "NUS client not ready for sending data - connected: %d, discovered: %d, rx_handle: %d", 
                 nus_connected, nus_service_discovered, nus_char_rx_handle);
        return ESP_ERR_INVALID_STATE;
    }

    if (len > NUS_MAX_DATA_LEN) {
        ESP_LOGE(TAG, "Data length %d exceeds maximum %d", len, NUS_MAX_DATA_LEN);
        return ESP_ERR_INVALID_ARG;
    }

    nus_tx_data_t tx_data;
    memcpy(tx_data.data, data, len);
    tx_data.len = len;

    BaseType_t ret = xQueueSend(nus_tx_queue, &tx_data, pdMS_TO_TICKS(100));
    if (ret != pdTRUE) {
        ESP_LOGE(TAG, "Failed to queue TX data");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

bool ble_nus_client_is_ready(void)
{
    return nus_connected && nus_service_discovered && nus_tx_notify_enabled;
}

uint16_t ble_nus_client_get_conn_id(void)
{
    return nus_conn_id;
}

void ble_nus_client_set_server_bda(const esp_bd_addr_t bda)
{
    memcpy(nus_server_bda, bda, sizeof(esp_bd_addr_t));
    ESP_LOGD(TAG, "Set server BD address: %02x:%02x:%02x:%02x:%02x:%02x",
             nus_server_bda[0], nus_server_bda[1], nus_server_bda[2],
             nus_server_bda[3], nus_server_bda[4], nus_server_bda[5]);
}

void ble_nus_client_debug_status(void)
{
    ESP_LOGI(TAG, "=== NUS CLIENT STATUS ===");
    ESP_LOGI(TAG, "Connected: %s", nus_connected ? "YES" : "NO");
    ESP_LOGI(TAG, "Service discovered: %s", nus_service_discovered ? "YES" : "NO");
    ESP_LOGI(TAG, "TX notifications enabled: %s", nus_tx_notify_enabled ? "YES" : "NO");
    ESP_LOGI(TAG, "Connection ready: %s", nus_connection_ready ? "YES" : "NO");
    ESP_LOGI(TAG, "Connection ID: %d", nus_conn_id);
    ESP_LOGI(TAG, "GATT interface: %d", nus_gattc_if);
    ESP_LOGI(TAG, "Service range: %d-%d", nus_service_start_handle, nus_service_end_handle);
    ESP_LOGI(TAG, "TX handle (notifications FROM device): %d", nus_char_tx_handle);
    ESP_LOGI(TAG, "RX handle (write TO device): %d", nus_char_rx_handle);
    ESP_LOGI(TAG, "Server BD address: %02x:%02x:%02x:%02x:%02x:%02x",
             nus_server_bda[0], nus_server_bda[1], nus_server_bda[2],
             nus_server_bda[3], nus_server_bda[4], nus_server_bda[5]);
}

void ble_nus_client_connection_ready(void)
{
    ESP_LOGI(TAG, "Connection params updated - connection now stable");
    nus_connection_ready = true;
}

void ble_nus_client_handle_gattc_event(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    // Fast path: process events without verbose logging
    switch (event) {
    case ESP_GATTC_REG_EVT:
        ESP_LOGI(TAG, "GATT client registered, app_id: %d", param->reg.app_id);
        nus_gattc_if = gattc_if;
        break;

    case ESP_GATTC_CONNECT_EVT:
        // ESP_LOGI(TAG, "GATT client connected, conn_id: %d", param->connect.conn_id);
        // Store the server's BD address for later use
        memcpy(nus_server_bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        break;

    case ESP_GATTC_OPEN_EVT:
        ESP_LOGI(TAG, "GATT connection opened on NUS interface, conn_id: %d, status: %d",
                 param->open.conn_id, param->open.status);

        if (param->open.status == ESP_GATT_OK && param->open.conn_id == nus_conn_id) {
            ESP_LOGI(TAG, "Starting service discovery on opened NUS GATT connection");
            esp_err_t ret = esp_ble_gattc_search_service(nus_gattc_if, nus_conn_id, NULL);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start service discovery: %s", esp_err_to_name(ret));
            }
        } else {
            ESP_LOGE(TAG, "Failed to open GATT connection: status=%d", param->open.status);
        }
        break;

    case ESP_GATTC_DISCONNECT_EVT:
        ESP_LOGI(TAG, "GATT client disconnected, conn_id: %d, reason: %d",
                 param->disconnect.conn_id, param->disconnect.reason);

        if (param->disconnect.conn_id == nus_conn_id) {
            nus_connected = false;
            nus_service_discovered = false;
            nus_tx_notify_enabled = false;
            nus_connection_ready = false;
            nus_conn_id = 0xFFFF;

            if (nus_config.disconnected_cb) {
                nus_config.disconnected_cb();
            }
        }
        break;

    case ESP_GATTC_SEARCH_RES_EVT:
        ESP_LOGD(TAG, "Service found, conn_id: %d, service_id: %d, start_handle: %d, end_handle: %d",
                 param->search_res.conn_id, param->search_res.srvc_id,
                 param->search_res.start_handle, param->search_res.end_handle);
        
        if (param->search_res.conn_id == nus_conn_id) {
            // Log UUID details for debugging
            if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_128) {
                ESP_LOGD(TAG, "128-bit UUID: %02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                         param->search_res.srvc_id.uuid.uuid.uuid128[15], param->search_res.srvc_id.uuid.uuid.uuid128[14],
                         param->search_res.srvc_id.uuid.uuid.uuid128[13], param->search_res.srvc_id.uuid.uuid.uuid128[12],
                         param->search_res.srvc_id.uuid.uuid.uuid128[11], param->search_res.srvc_id.uuid.uuid.uuid128[10],
                         param->search_res.srvc_id.uuid.uuid.uuid128[9], param->search_res.srvc_id.uuid.uuid.uuid128[8],
                         param->search_res.srvc_id.uuid.uuid.uuid128[7], param->search_res.srvc_id.uuid.uuid.uuid128[6],
                         param->search_res.srvc_id.uuid.uuid.uuid128[5], param->search_res.srvc_id.uuid.uuid.uuid128[4],
                         param->search_res.srvc_id.uuid.uuid.uuid128[3], param->search_res.srvc_id.uuid.uuid.uuid128[2],
                         param->search_res.srvc_id.uuid.uuid.uuid128[1], param->search_res.srvc_id.uuid.uuid.uuid128[0]);
                
                // Check if this is the NUS service by comparing UUID
                // NUS service UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
                const uint8_t nus_uuid[] = {0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
                                           0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E};
                if (memcmp(param->search_res.srvc_id.uuid.uuid.uuid128, nus_uuid, 16) == 0) {
                    memcpy(&nus_service_handle, &param->search_res.srvc_id, sizeof(esp_gatt_id_t));
                    nus_service_discovered = true;
                    nus_service_start_handle = param->search_res.start_handle;
                    nus_service_end_handle = param->search_res.end_handle;
                    ESP_LOGI(TAG, "NUS service discovered! Start handle: %d, End handle: %d",
                             nus_service_start_handle, nus_service_end_handle);
                } else {
                    ESP_LOGD(TAG, "Not NUS service (UUID mismatch)");
                }
            } else {
                ESP_LOGD(TAG, "16-bit UUID: 0x%04x", param->search_res.srvc_id.uuid.uuid.uuid16);
            }
        }
        break;

    case ESP_GATTC_SEARCH_CMPL_EVT:
        ESP_LOGI(TAG, "Service discovery complete, conn_id: %d, status: %d",
                 param->search_cmpl.conn_id, param->search_cmpl.status);

        if (param->search_cmpl.conn_id == nus_conn_id && nus_service_discovered) {
            ESP_LOGI(TAG, "NUS service found! Now discovering characteristics...");

            // Get all characteristics of the NUS service
            esp_gattc_char_elem_t char_elem_result[10];
            uint16_t char_elem_count = 10;

            // Get characteristics using the service handle range
            esp_gatt_status_t status = esp_ble_gattc_get_all_char(
                nus_gattc_if, nus_conn_id,
                nus_service_start_handle,
                nus_service_end_handle,
                char_elem_result,
                &char_elem_count,
                0
            );

            if (status == ESP_GATT_OK) {
                ESP_LOGI(TAG, "Found %d characteristics in NUS service", char_elem_count);

                // Look for TX and RX characteristics by UUID
                for (int i = 0; i < char_elem_count; i++) {
                    ESP_LOGD(TAG, "Characteristic %d: handle=%d, properties=0x%02x",
                             i, char_elem_result[i].char_handle, char_elem_result[i].properties);

                    if (char_elem_result[i].uuid.len == ESP_UUID_LEN_128) {
                        // IMPORTANT: Some devices swap the TX/RX UUIDs or use different properties
                        // We'll identify by properties rather than UUID

                        // Check for NOTIFY property - this is where we receive data FROM the device
                        if (char_elem_result[i].properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY) {
                            nus_char_tx_handle = char_elem_result[i].char_handle;
                            ESP_LOGI(TAG, "Found NUS TX characteristic (NOTIFY) at handle %d", nus_char_tx_handle);
                        }
                        // Check for WRITE property - this is where we send data TO the device
                        if (char_elem_result[i].properties & (ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR)) {
                            nus_char_rx_handle = char_elem_result[i].char_handle;
                            ESP_LOGI(TAG, "Found NUS RX characteristic (WRITE) at handle %d", nus_char_rx_handle);
                        }
                    } else {
                        ESP_LOGI(TAG, "Characteristic has 16-bit UUID: 0x%04x", char_elem_result[i].uuid.uuid.uuid16);
                    }
                }

                if (nus_char_tx_handle != 0 && nus_char_rx_handle != 0) {
                    ESP_LOGI(TAG, "NUS service configured - TX: %d, RX: %d",
                             nus_char_tx_handle, nus_char_rx_handle);

                    // Signal CCCD task to write CCCD descriptor
                    ESP_LOGI(TAG, "Signaling CCCD task to enable notifications");
                    cccd_write_pending = true;

                    nus_connected = true;  // Service is fully discovered
                } else {
                    ESP_LOGW(TAG, "Failed to find all NUS characteristics");
                    nus_connected = false;
                }
            } else {
                ESP_LOGE(TAG, "Failed to get characteristics: %d", status);
            }
        } else if (!nus_service_discovered) {
            ESP_LOGW(TAG, "NUS service not found on this device");
        }
        break;

    case ESP_GATTC_NOTIFY_EVT:
        ESP_LOGD(TAG, "Notification: conn_id=%d, handle=%d, len=%d",
                 param->notify.conn_id, param->notify.handle, param->notify.value_len);

        if (param->notify.conn_id == nus_conn_id &&
            param->notify.handle == nus_char_tx_handle) {
            // NUS TX characteristic notification
            ESP_LOGD(TAG, "NUS data received: %.*s", param->notify.value_len, param->notify.value);

            if (nus_config.data_received_cb) {
                nus_config.data_received_cb(param->notify.value, param->notify.value_len);
            }
        } else {
            ESP_LOGD(TAG, "Notification from handle %d (not NUS TX %d)",
                     param->notify.handle, nus_char_tx_handle);
        }
        break;

    case ESP_GATTC_WRITE_CHAR_EVT:
        ESP_LOGD(TAG, "Write response: handle=%d, status=%d",
                 param->write.handle, param->write.status);

        if (param->write.conn_id == nus_conn_id && param->write.handle == nus_char_rx_handle) {
            if (param->write.status != ESP_GATT_OK) {
                ESP_LOGE(TAG, "NUS write failed: status=%d", param->write.status);
            }

            if (nus_config.data_sent_cb) {
                nus_config.data_sent_cb(param->write.status == ESP_GATT_OK ? ESP_OK : ESP_FAIL);
            }
        }
        break;

    case ESP_GATTC_CFG_MTU_EVT:
        ESP_LOGI(TAG, "=== MTU CONFIGURED ===");
        ESP_LOGI(TAG, "conn_id: %d, status: %d, mtu: %d",
                 param->cfg_mtu.conn_id, param->cfg_mtu.status, param->cfg_mtu.mtu);
        break;

    case ESP_GATTC_WRITE_DESCR_EVT:
        ESP_LOGI(TAG, "=== WRITE DESCRIPTOR RESPONSE ===");
        ESP_LOGI(TAG, "conn_id: %d, handle: %d, status: %d",
                 param->write.conn_id, param->write.handle, param->write.status);
        ESP_LOGI(TAG, "Expected CCCD handle: %d", nus_cccd_handle);

        // Check if this is the CCCD write response for NUS TX notifications
        if (param->write.handle == nus_cccd_handle) {
            if (param->write.status == ESP_GATT_OK) {
                ESP_LOGI(TAG, "✓ CCCD write successful - registering notification handler");

                // Now register client-side notification handler
                esp_err_t ret = esp_ble_gattc_register_for_notify(nus_gattc_if,
                                                                  nus_server_bda,
                                                                  nus_char_tx_handle);
                if (ret == ESP_OK) {
                    ESP_LOGI(TAG, "Notification handler registered");
                    nus_tx_notify_enabled = true;
                } else {
                    ESP_LOGE(TAG, "Failed to register notification handler: %s",
                            esp_err_to_name(ret));
                }
            } else {
                ESP_LOGE(TAG, "✗ CCCD write failed with status: %d", param->write.status);
            }
        } else {
            ESP_LOGD(TAG, "Write descriptor response for different handle %d (expected %d)",
                    param->write.handle, nus_cccd_handle);
        }
        break;

    default:
        ESP_LOGD(TAG, "Unhandled GATT client event: %d", event);
        break;
    }
}

static void nus_tx_task(void *pvParameters)
{
    nus_tx_data_t tx_data;

    while (1) {
        if (xQueueReceive(nus_tx_queue, &tx_data, portMAX_DELAY) == pdTRUE) {
            ESP_LOGD(TAG, "Sending %d bytes to NUS", tx_data.len);
            if (nus_connected && nus_service_discovered && nus_char_rx_handle != 0) {
                esp_err_t ret = esp_ble_gattc_write_char(nus_gattc_if, nus_conn_id, nus_char_rx_handle,
                                                        tx_data.len, tx_data.data, ESP_GATT_WRITE_TYPE_RSP,
                                                        ESP_GATT_AUTH_REQ_NONE);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to write NUS RX: %s", esp_err_to_name(ret));
                }
            } else {
                ESP_LOGW(TAG, "NUS client not ready for transmission");
            }
        }
    }
}

// Dedicated task for CCCD write operations
// Waits for connection params to be updated (via ble_nus_client_connection_ready)
// before writing CCCD to ensure BLE stack is stable
static void nus_cccd_task(void *pvParameters)
{
    while (1) {
        // Only write CCCD when:
        // 1. CCCD write is pending (service discovered)
        // 2. Connection params have been updated (connection is stable)
        // 3. TX handle is valid
        if (cccd_write_pending && nus_connection_ready && nus_char_tx_handle != 0) {
            // Add 100ms delay after connection ready to let stack fully stabilize
            // Connection params event fires, but stack may need a moment to settle
            vTaskDelay(pdMS_TO_TICKS(100));

            ESP_LOGI(TAG, "CCCD task: Connection stable, preparing CCCD write");
            ESP_LOGI(TAG, "State check - gattc_if: %d, conn_id: %d, tx_handle: %d, connected: %d",
                     nus_gattc_if, nus_conn_id, nus_char_tx_handle, nus_connected);

            if (nus_gattc_if == ESP_GATT_IF_NONE || nus_conn_id == 0xFFFF) {
                ESP_LOGE(TAG, "Invalid GATT state, skipping CCCD write");
                cccd_write_pending = false;
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }

            // CCCD is typically at characteristic handle + 1
            nus_cccd_handle = nus_char_tx_handle + 1;
            ESP_LOGI(TAG, "Using CCCD handle %d (TX handle %d + 1)", nus_cccd_handle, nus_char_tx_handle);

            // Write CCCD to enable notifications (0x01, 0x00 = notifications enabled)
            uint8_t notify_en[] = {0x01, 0x00};
            esp_err_t write_ret = esp_ble_gattc_write_char_descr(
                nus_gattc_if,
                nus_conn_id,
                nus_cccd_handle,
                sizeof(notify_en),
                notify_en,
                ESP_GATT_WRITE_TYPE_RSP,  // Back to RSP for proper response handling
                ESP_GATT_AUTH_REQ_NONE
            );

            if (write_ret == ESP_OK) {
                ESP_LOGI(TAG, "CCCD write initiated successfully");
            } else {
                ESP_LOGE(TAG, "CCCD write failed: %s", esp_err_to_name(write_ret));
            }

            cccd_write_pending = false;
        }

        vTaskDelay(pdMS_TO_TICKS(50));  // Check every 50ms
    }
}
