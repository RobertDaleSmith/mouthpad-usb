#include "ble_nus.h"
#include "esp_log.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "string.h"

static const char *TAG = "BLE_NUS";

// NUS Service UUID (128-bit, little-endian)
static esp_bt_uuid_t nus_service_uuid = {
    .len = ESP_UUID_LEN_128,
    .uuid = {
        .uuid128 = {
            0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
            0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E
        }
    }
};

// NUS TX Characteristic UUID (128-bit, little-endian)
static esp_bt_uuid_t nus_char_tx_uuid = {
    .len = ESP_UUID_LEN_128,
    .uuid = {
        .uuid128 = {
            0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
            0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E
        }
    }
};

// NUS RX Characteristic UUID (128-bit, little-endian)
static esp_bt_uuid_t nus_char_rx_uuid = {
    .len = ESP_UUID_LEN_128,
    .uuid = {
        .uuid128 = {
            0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
            0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E
        }
    }
};

// GATT service and characteristic handles
static esp_gatt_if_t nus_gatts_if = ESP_GATT_IF_NONE;
static uint16_t nus_service_handle = 0;
static uint16_t nus_char_tx_handle = 0;
static uint16_t nus_char_rx_handle = 0;
static uint16_t nus_char_tx_ccc_handle = 0;

// Connection state
static uint16_t nus_conn_id = 0xFFFF;
static bool nus_connected = false;
static bool nus_tx_notify_enabled = false;

// Configuration callbacks
static ble_nus_config_t nus_config = {0};

// Queue for sending data
static QueueHandle_t nus_tx_queue = NULL;

// Data structure for TX queue
typedef struct {
    uint8_t data[NUS_MAX_DATA_LEN];
    uint16_t len;
} nus_tx_data_t;

// GATT server callback
static void nus_gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);

// GAP callback
static void nus_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);

// Task for handling TX data
static void nus_tx_task(void *pvParameters);

esp_err_t ble_nus_init(const ble_nus_config_t *config)
{
    if (config == NULL) {
        ESP_LOGE(TAG, "Configuration cannot be NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // Store configuration
    memcpy(&nus_config, config, sizeof(ble_nus_config_t));

    // Create TX queue
    nus_tx_queue = xQueueCreate(5, sizeof(nus_tx_data_t));
    if (nus_tx_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create TX queue");
        return ESP_ERR_NO_MEM;
    }

    // Register GATT server callback
    esp_err_t ret = esp_ble_gatts_register_callback(nus_gatts_event_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register GATT server callback: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register GAP callback
    ret = esp_ble_gap_register_callback(nus_gap_event_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register GAP callback: %s", esp_err_to_name(ret));
        return ret;
    }

    // Create TX task
    BaseType_t task_ret = xTaskCreate(nus_tx_task, "nus_tx", 4096, NULL, 5, NULL);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create TX task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "NUS service initialized");
    return ESP_OK;
}

esp_err_t ble_nus_start_advertising(const char *device_name)
{
    if (device_name == NULL) {
        ESP_LOGE(TAG, "Device name cannot be NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // Set device name
    esp_err_t ret = esp_ble_gap_set_device_name(device_name);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set device name: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configure advertising data
    esp_ble_adv_data_t adv_data = {
        .set_scan_rsp = false,
        .include_name = true,
        .include_txpower = true,
        .min_interval = 0x0006,
        .max_interval = 0x0010,
        .appearance = 0x00,
        .manufacturer_len = 0,
        .p_manufacturer_data = NULL,
        .service_data_len = 0,
        .p_service_data = NULL,
        .service_uuid_len = sizeof(nus_service_uuid.uuid.uuid128),
        .p_service_uuid = nus_service_uuid.uuid.uuid128,
        .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
    };

    ret = esp_ble_gap_config_adv_data(&adv_data);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure advertising data: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configure advertising parameters
    esp_ble_adv_params_t adv_params = {
        .adv_int_min = 0x20,
        .adv_int_max = 0x40,
        .adv_type = ADV_TYPE_IND,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .channel_map = ADV_CHNL_ALL,
        .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
    };

    ret = esp_ble_gap_start_advertising(&adv_params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start advertising: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Started advertising NUS service");
    return ESP_OK;
}

esp_err_t ble_nus_stop_advertising(void)
{
    esp_err_t ret = esp_ble_gap_stop_advertising();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop advertising: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Stopped advertising NUS service");
    return ESP_OK;
}

esp_err_t ble_nus_send_data(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0) {
        ESP_LOGE(TAG, "Invalid data or length");
        return ESP_ERR_INVALID_ARG;
    }

    if (!nus_connected || !nus_tx_notify_enabled) {
        ESP_LOGW(TAG, "NUS not connected or TX notifications not enabled");
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

bool ble_nus_is_connected(void)
{
    return nus_connected;
}

uint16_t ble_nus_get_conn_id(void)
{
    return nus_conn_id;
}

static void nus_gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT: {
        ESP_LOGI(TAG, "GATT server registered, app_id: %d", param->reg.app_id);

        nus_gatts_if = gatts_if;

        // Create NUS service
        esp_err_t ret = esp_ble_gap_set_device_name("MouthPad NUS");
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set device name: %s", esp_err_to_name(ret));
        }

        // Create service with enough handles (service + 2 characteristics + 1 CCCD)
        esp_gatt_srvc_id_t service_id = {
            .is_primary = true,
            .id.inst_id = 0x00,
            .id.uuid = nus_service_uuid,
        };
        ret = esp_ble_gatts_create_service(gatts_if, &service_id, 10);  // Increased handle count
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create NUS service: %s", esp_err_to_name(ret));
        }
        break;
    }
    
    case ESP_GATTS_CREATE_EVT: {
        ESP_LOGI(TAG, "Service created, status: %d, service_handle: %d",
                 param->create.status, param->create.service_handle);

        if (param->create.status == ESP_GATT_OK) {
            nus_service_handle = param->create.service_handle;

            // Start the service first
            esp_ble_gatts_start_service(nus_service_handle);

            // Create TX characteristic (notify) - Server to Client
            esp_gatt_char_prop_t tx_property = ESP_GATT_CHAR_PROP_BIT_NOTIFY;
            esp_attr_value_t tx_char_val = {
                .attr_max_len = NUS_MAX_DATA_LEN,
                .attr_len = 0,
                .attr_value = NULL,
            };

            esp_ble_gatts_add_char(nus_service_handle, &nus_char_tx_uuid,
                                  ESP_GATT_PERM_READ,
                                  tx_property,
                                  &tx_char_val, NULL);
        }
        break;
    }
    
    case ESP_GATTS_ADD_CHAR_EVT: {
        ESP_LOGI(TAG, "Characteristic added, status: %d, attr_handle: %d, service_handle: %d, char_uuid: %04x",
                 param->add_char.status, param->add_char.attr_handle, param->add_char.service_handle,
                 param->add_char.char_uuid.uuid.uuid16);

        if (param->add_char.status == ESP_GATT_OK) {
            // Check which characteristic was added by comparing the UUID
            if (nus_char_tx_handle == 0 &&
                memcmp(param->add_char.char_uuid.uuid.uuid128, nus_char_tx_uuid.uuid.uuid128, ESP_UUID_LEN_128) == 0) {
                nus_char_tx_handle = param->add_char.attr_handle;
                ESP_LOGI(TAG, "TX characteristic handle: %d", nus_char_tx_handle);

                // Add CCCD (Client Characteristic Configuration Descriptor) for TX characteristic
                esp_bt_uuid_t cccd_uuid = {
                    .len = ESP_UUID_LEN_16,
                    .uuid = {.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG}
                };

                esp_gatt_perm_t cccd_perm = ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE;
                esp_attr_value_t cccd_val = {
                    .attr_max_len = 2,
                    .attr_len = 2,
                    .attr_value = (uint8_t[]){0x00, 0x00},
                };

                esp_ble_gatts_add_char_descr(nus_service_handle, &cccd_uuid, cccd_perm, &cccd_val, NULL);

            } else if (nus_char_rx_handle == 0 &&
                       memcmp(param->add_char.char_uuid.uuid.uuid128, nus_char_rx_uuid.uuid.uuid128, ESP_UUID_LEN_128) == 0) {
                nus_char_rx_handle = param->add_char.attr_handle;
                ESP_LOGI(TAG, "RX characteristic handle: %d", nus_char_rx_handle);
            }
        }
        break;
    }
    
    case ESP_GATTS_ADD_CHAR_DESCR_EVT: {
        ESP_LOGI(TAG, "Characteristic descriptor added, status: %d, attr_handle: %d, service_handle: %d",
                 param->add_char_descr.status, param->add_char_descr.attr_handle, param->add_char_descr.service_handle);

        if (param->add_char_descr.status == ESP_GATT_OK) {
            // This should be the CCCD for TX characteristic
            nus_char_tx_ccc_handle = param->add_char_descr.attr_handle;
            ESP_LOGI(TAG, "TX CCCD handle: %d", nus_char_tx_ccc_handle);

            // Now create RX characteristic (write without response) - Client to Server
            esp_gatt_char_prop_t rx_property = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
            esp_attr_value_t rx_char_val = {
                .attr_max_len = NUS_MAX_DATA_LEN,
                .attr_len = 0,
                .attr_value = NULL,
            };

            esp_ble_gatts_add_char(nus_service_handle, &nus_char_rx_uuid,
                                  ESP_GATT_PERM_WRITE,
                                  rx_property,
                                  &rx_char_val, NULL);
        }
        break;
    }
    
    case ESP_GATTS_CONNECT_EVT: {
        ESP_LOGI(TAG, "Client connected, conn_id: %d, remote_bda: %02x:%02x:%02x:%02x:%02x:%02x",
                 param->connect.conn_id,
                 param->connect.remote_bda[0], param->connect.remote_bda[1], param->connect.remote_bda[2],
                 param->connect.remote_bda[3], param->connect.remote_bda[4], param->connect.remote_bda[5]);
        
        nus_conn_id = param->connect.conn_id;
        nus_connected = true;
        
        if (nus_config.connected_cb) {
            nus_config.connected_cb();
        }
        break;
    }
    
    case ESP_GATTS_DISCONNECT_EVT: {
        ESP_LOGI(TAG, "Client disconnected, conn_id: %d, reason: %d",
                 param->disconnect.conn_id, param->disconnect.reason);
        
        nus_connected = false;
        nus_tx_notify_enabled = false;
        nus_conn_id = 0xFFFF;
        
        if (nus_config.disconnected_cb) {
            nus_config.disconnected_cb();
        }
        
        // Restart advertising
        esp_ble_gap_start_advertising(NULL);
        break;
    }
    
    case ESP_GATTS_WRITE_EVT: {
        ESP_LOGI(TAG, "Write event, conn_id: %d, trans_id: %d, handle: %d, len: %d, is_prep: %d, need_rsp: %d",
                 param->write.conn_id, param->write.trans_id, param->write.handle, param->write.len,
                 param->write.is_prep, param->write.need_rsp);

        if (param->write.handle == nus_char_rx_handle) {
            // Data received on RX characteristic
            ESP_LOGI(TAG, "Received %d bytes on RX characteristic (handle: %d)", param->write.len, nus_char_rx_handle);
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, param->write.value, param->write.len, ESP_LOG_DEBUG);

            if (nus_config.data_received_cb) {
                nus_config.data_received_cb(param->write.value, param->write.len);
            }
        } else if (param->write.handle == nus_char_tx_ccc_handle) {
            // CCC descriptor written
            uint16_t ccc_value = param->write.value[0] | (param->write.value[1] << 8);
            nus_tx_notify_enabled = (ccc_value & 0x0001) != 0;

            ESP_LOGI(TAG, "TX CCCD written (handle: %d), notifications %s (value: 0x%04x)",
                     nus_char_tx_ccc_handle, nus_tx_notify_enabled ? "enabled" : "disabled", ccc_value);
        } else {
            ESP_LOGW(TAG, "Write to unknown handle: %d", param->write.handle);
        }

        // Send response if needed
        if (param->write.need_rsp) {
            esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK, NULL);
        }
        break;
    }
    
    case ESP_GATTS_READ_EVT: {
        ESP_LOGI(TAG, "Read event, conn_id: %d, trans_id: %d, handle: %d",
                 param->read.conn_id, param->read.trans_id, param->read.handle);
        
        // Send empty response for now
        esp_ble_gatts_send_response(gatts_if, param->read.conn_id, param->read.trans_id, ESP_GATT_OK, NULL);
        break;
    }
    
    default:
        ESP_LOGD(TAG, "Unhandled GATT server event: %d", event);
        break;
    }
}

static void nus_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        ESP_LOGI(TAG, "Advertising data set complete");
        break;
        
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Advertising started successfully");
        } else {
            ESP_LOGE(TAG, "Failed to start advertising: %d", param->adv_start_cmpl.status);
        }
        break;
        
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        ESP_LOGI(TAG, "Advertising stopped");
        break;
        
    default:
        ESP_LOGD(TAG, "Unhandled GAP event: %d", event);
        break;
    }
}

static void nus_tx_task(void *pvParameters)
{
    nus_tx_data_t tx_data;
    
    while (1) {
        if (xQueueReceive(nus_tx_queue, &tx_data, portMAX_DELAY) == pdTRUE) {
            if (nus_connected && nus_tx_notify_enabled) {
                esp_err_t ret = esp_ble_gatts_send_indicate(nus_gatts_if, nus_conn_id, nus_char_tx_handle, 
                                                           tx_data.len, tx_data.data, false);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to send indication: %s", esp_err_to_name(ret));
                } else {
                    ESP_LOGD(TAG, "Sent %d bytes via TX characteristic", tx_data.len);
                    
                    if (nus_config.data_sent_cb) {
                        nus_config.data_sent_cb(ESP_OK);
                    }
                }
            } else {
                ESP_LOGW(TAG, "Cannot send data: not connected or notifications disabled");
                
                if (nus_config.data_sent_cb) {
                    nus_config.data_sent_cb(ESP_ERR_INVALID_STATE);
                }
            }
        }
    }
}
