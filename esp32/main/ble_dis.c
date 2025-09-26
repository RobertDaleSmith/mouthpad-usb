#include "ble_dis.h"
#include "esp_log.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "string.h"

static const char *TAG = "BLE_DIS";

// Device info client state
static esp_gatt_if_t dis_gattc_if = ESP_GATT_IF_NONE;
static uint16_t dis_conn_id = 0xFFFF;
static esp_bd_addr_t dis_server_bda = {0};
static bool dis_connected = false;
static bool dis_service_discovered = false;

// Service and characteristic handles
static uint16_t dis_service_start_handle = 0;
static uint16_t dis_service_end_handle = 0;
static uint16_t dis_char_handles[7] = {0}; // Handles for the 7 DIS characteristics

// Configuration callback
static ble_device_info_config_t dis_config = {0};

// Current device info
static ble_device_info_t current_device_info = {0};
static uint8_t chars_read_count = 0;
static uint8_t chars_found_count = 0;

// Characteristic UUIDs we're looking for
static const uint16_t dis_char_uuids[] = {
    DIS_CHAR_MANUFACTURER_NAME_UUID,
    DIS_CHAR_MODEL_NUMBER_UUID,
    DIS_CHAR_SERIAL_NUMBER_UUID,
    DIS_CHAR_HARDWARE_REV_UUID,
    DIS_CHAR_FIRMWARE_REV_UUID,
    DIS_CHAR_SOFTWARE_REV_UUID,
    DIS_CHAR_PNP_ID_UUID
};

// Pointers to device info strings for easier handling
static char* device_info_strings[] = {
    current_device_info.manufacturer_name,
    current_device_info.model_number,
    current_device_info.serial_number,
    current_device_info.hardware_revision,
    current_device_info.firmware_revision,
    current_device_info.software_revision
};

static const char* char_names[] = {
    "Manufacturer Name",
    "Model Number",
    "Serial Number",
    "Hardware Revision",
    "Firmware Revision",
    "Software Revision",
    "PnP ID"
};

esp_err_t ble_device_info_init(const ble_device_info_config_t *config)
{
    if (config == NULL) {
        ESP_LOGE(TAG, "Configuration cannot be NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // Store configuration
    memcpy(&dis_config, config, sizeof(ble_device_info_config_t));

    ESP_LOGI(TAG, "Device info client initialized");
    return ESP_OK;
}

esp_err_t ble_device_info_discover(esp_gatt_if_t gattc_if, uint16_t conn_id, const esp_bd_addr_t server_bda, const char *device_name)
{
    ESP_LOGI(TAG, "Starting DIS service discovery on conn_id: %d, gattc_if: %d", conn_id, gattc_if);
    // ESP_LOGI(TAG, "Device address: %02x:%02x:%02x:%02x:%02x:%02x",
    //          server_bda[0], server_bda[1], server_bda[2],
    //          server_bda[3], server_bda[4], server_bda[5]);

    // Reset state
    dis_gattc_if = gattc_if;
    dis_conn_id = conn_id;
    dis_connected = false;
    dis_service_discovered = false;
    memcpy(dis_server_bda, server_bda, sizeof(esp_bd_addr_t));
    memset(&current_device_info, 0, sizeof(ble_device_info_t));
    memset(dis_char_handles, 0, sizeof(dis_char_handles));
    chars_read_count = 0;
    chars_found_count = 0;

    // Store device name if provided
    if (device_name && strlen(device_name) > 0) {
        size_t copy_len = (strlen(device_name) < (DIS_MAX_STRING_LEN - 1)) ?
                          strlen(device_name) : (DIS_MAX_STRING_LEN - 1);
        memcpy(current_device_info.device_name, device_name, copy_len);
        current_device_info.device_name[copy_len] = '\0';
    }

    ESP_LOGI(TAG, "Searching for DIS service (UUID: 0x%04X)...", DIS_SERVICE_UUID);

    // Open GATT connection on DIS interface to the server
    ESP_LOGI(TAG, "Opening GATT connection on DIS interface to %02x:%02x:%02x:%02x:%02x:%02x",
             server_bda[0], server_bda[1], server_bda[2],
             server_bda[3], server_bda[4], server_bda[5]);

    esp_err_t ret = esp_ble_gattc_open(dis_gattc_if, (uint8_t *)server_bda, BLE_ADDR_TYPE_PUBLIC, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open GATT connection: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "DIS discovery initiated - service discovery will start after connection opens");

    return ESP_OK;
}

const ble_device_info_t* ble_device_info_get_current(void)
{
    return current_device_info.info_complete ? &current_device_info : NULL;
}

void ble_device_info_print(const ble_device_info_t *device_info)
{
    if (!device_info) {
        ESP_LOGI(TAG, "No device info available");
        return;
    }

    ESP_LOGI(TAG, "=== DEVICE INFORMATION ===");
    if (strlen(device_info->device_name) > 0) {
        ESP_LOGI(TAG, "Name: %s", device_info->device_name);
    }

    // Print BD address for reference
    ESP_LOGI(TAG, "Address: %02x:%02x:%02x:%02x:%02x:%02x",
        dis_server_bda[0], dis_server_bda[1], dis_server_bda[2],
        dis_server_bda[3], dis_server_bda[4], dis_server_bda[5]);

    if (strlen(device_info->manufacturer_name) > 0) {
        ESP_LOGI(TAG, "Manufacturer: %s", device_info->manufacturer_name);
    }
    if (strlen(device_info->model_number) > 0) {
        ESP_LOGI(TAG, "Model Number: %s", device_info->model_number);
    }
    if (strlen(device_info->serial_number) > 0) {
        ESP_LOGI(TAG, "Serial Number: %s", device_info->serial_number);
    }
    if (strlen(device_info->hardware_revision) > 0) {
        ESP_LOGI(TAG, "Hardware Revision: %s", device_info->hardware_revision);
    }
    if (strlen(device_info->firmware_revision) > 0) {
        ESP_LOGI(TAG, "Firmware Revision: %s", device_info->firmware_revision);
    }
    if (strlen(device_info->software_revision) > 0) {
        ESP_LOGI(TAG, "Software Revision: %s", device_info->software_revision);
    }
    if (device_info->has_pnp_id) {
        const char* vendor_source = (device_info->pnp_id.vendor_id_source == 0x01) ? "Bluetooth SIG" :
                                   (device_info->pnp_id.vendor_id_source == 0x02) ? "USB Forum" : "Unknown";
        ESP_LOGI(TAG, "Vendor ID: 0x%04X (%s)", device_info->pnp_id.vendor_id, vendor_source);
        ESP_LOGI(TAG, "Product ID: 0x%04X", device_info->pnp_id.product_id);
        ESP_LOGI(TAG, "Product Version: 0x%04X", device_info->pnp_id.product_version);
    }
}

static void check_info_complete(void)
{
    if (chars_read_count >= chars_found_count && chars_found_count > 0) {
        current_device_info.info_complete = true;
        ESP_LOGI(TAG, "Device info discovery complete (%d characteristics read)", chars_read_count);

        // Call completion callback (which will print the device info)
        if (dis_config.info_complete_cb) {
            dis_config.info_complete_cb(&current_device_info);
        }
    }
}

void ble_device_info_handle_gattc_event(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    ESP_LOGD(TAG, "DIS GATT event: %d, gattc_if: %d", event, gattc_if);

    switch (event) {
    case ESP_GATTC_REG_EVT:
        if (param->reg.status == ESP_GATT_OK) {
            dis_gattc_if = gattc_if;
            ESP_LOGI(TAG, "DIS GATT client registered successfully, gattc_if: %d", dis_gattc_if);
        } else {
            ESP_LOGE(TAG, "DIS GATT client registration failed: %d", param->reg.status);
        }
        break;

    case ESP_GATTC_OPEN_EVT:
        ESP_LOGI(TAG, "DIS GATT connection opened, conn_id: %d, status: %d",
                 param->open.conn_id, param->open.status);
        if (param->open.status == ESP_GATT_OK) {
            dis_conn_id = param->open.conn_id;
            dis_connected = true;
            ESP_LOGI(TAG, "Starting DIS service discovery on conn_id: %d", dis_conn_id);
            // Start service discovery to find DIS service
            esp_err_t ret = esp_ble_gattc_search_service(dis_gattc_if, dis_conn_id, NULL);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start service discovery: %s", esp_err_to_name(ret));
            }
        } else {
            ESP_LOGE(TAG, "Failed to open DIS GATT connection: status=%d", param->open.status);
        }
        break;
    case ESP_GATTC_SEARCH_RES_EVT:
        ESP_LOGD(TAG, "DIS received SEARCH_RES_EVT - conn_id: %d, expected: %d",
                 param->search_res.conn_id, dis_conn_id);
        // Process service discovery results from our own connection
        if (param->search_res.conn_id == dis_conn_id) {
            ESP_LOGD(TAG, "Service found - conn_id: %d, start_handle: %d, end_handle: %d",
                     param->search_res.conn_id,
                     param->search_res.start_handle, param->search_res.end_handle);

            // Log UUID details for debugging
            if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16) {
                ESP_LOGD(TAG, "16-bit Service UUID: 0x%04x (looking for DIS 0x%04x)",
                         param->search_res.srvc_id.uuid.uuid.uuid16, DIS_SERVICE_UUID);

                // Check if this is the DIS service
                if (param->search_res.srvc_id.uuid.uuid.uuid16 == DIS_SERVICE_UUID) {
                    ESP_LOGI(TAG, "Found DIS service (UUID 0x%04x)!", DIS_SERVICE_UUID);

                    // Only update if we have valid handle values
                    if (param->search_res.start_handle != 0 && param->search_res.end_handle != 0) {
                        dis_service_discovered = true;
                        dis_service_start_handle = param->search_res.start_handle;
                        dis_service_end_handle = param->search_res.end_handle;
                        ESP_LOGI(TAG, "DIS service discovered with valid handles! Start: %d, End: %d",
                                 dis_service_start_handle, dis_service_end_handle);
                    } else {
                        ESP_LOGW(TAG, "DIS service found but handles are invalid (start=%d, end=%d)",
                                 param->search_res.start_handle, param->search_res.end_handle);
                        ESP_LOGI(TAG, "Will need to get service handles through get_service API");

                        // Try to get the service info using a different method
                        esp_gattc_service_elem_t service_result;
                        uint16_t count = 1;
                        esp_bt_uuid_t dis_uuid = {
                            .len = ESP_UUID_LEN_16,
                            .uuid = {.uuid16 = DIS_SERVICE_UUID}
                        };

                        esp_gatt_status_t status = esp_ble_gattc_get_service(dis_gattc_if,
                                                                             param->search_res.conn_id,
                                                                             &dis_uuid,
                                                                             &service_result,
                                                                             &count, 0);

                        if (status == ESP_GATT_OK && count > 0) {
                            ESP_LOGI(TAG, "Got DIS service info: start=%d, end=%d",
                                     service_result.start_handle, service_result.end_handle);
                            dis_service_discovered = true;
                            dis_service_start_handle = service_result.start_handle;
                            dis_service_end_handle = service_result.end_handle;
                        } else {
                            ESP_LOGE(TAG, "Failed to get DIS service info: status=%d, count=%d", status, count);
                        }
                    }
                } else {
                    ESP_LOGD(TAG, "Not DIS service (UUID mismatch: 0x%04x != 0x%04x)",
                             param->search_res.srvc_id.uuid.uuid.uuid16, DIS_SERVICE_UUID);
                }
            } else if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_128) {
                ESP_LOGD(TAG, "128-bit UUID service (not DIS - DIS uses 16-bit UUID)");
            } else {
                ESP_LOGI(TAG, "Unknown UUID length: %d", param->search_res.srvc_id.uuid.len);
            }
        }
        break;

    case ESP_GATTC_SEARCH_CMPL_EVT:
        ESP_LOGD(TAG, "DIS received SEARCH_CMPL_EVT - conn_id: %d, expected: %d",
                 param->search_cmpl.conn_id, dis_conn_id);
        ESP_LOGD(TAG, "Service discovery complete for conn_id: %d, status: %d, DIS discovered: %s",
                 param->search_cmpl.conn_id, param->search_cmpl.status, dis_service_discovered ? "YES" : "NO");

        // If we found DIS but didn't get valid handles, try one more time
        if (!dis_service_discovered && param->search_cmpl.status == ESP_GATT_OK) {
            ESP_LOGI(TAG, "Service discovery completed but DIS handles not found, trying direct query...");

            esp_gattc_service_elem_t service_result;
            uint16_t count = 1;
            esp_bt_uuid_t dis_uuid = {
                .len = ESP_UUID_LEN_16,
                .uuid = {.uuid16 = DIS_SERVICE_UUID}
            };

            esp_gatt_status_t status = esp_ble_gattc_get_service(dis_gattc_if,
                                                                 param->search_cmpl.conn_id,
                                                                 &dis_uuid,
                                                                 &service_result,
                                                                 &count, 0);

            if (status == ESP_GATT_OK && count > 0) {
                ESP_LOGI(TAG, "Successfully found DIS service via direct query: start=%d, end=%d",
                         service_result.start_handle, service_result.end_handle);
                dis_service_discovered = true;
                dis_service_start_handle = service_result.start_handle;
                dis_service_end_handle = service_result.end_handle;
            } else {
                ESP_LOGW(TAG, "DIS service not available on this device (status=%d, count=%d)", status, count);
            }
        }

        // Process service discovery completion
        if (dis_service_discovered && param->search_cmpl.conn_id == dis_conn_id) {
            ESP_LOGI(TAG, "DIS service ready, discovering characteristics...");

            // Get all characteristics of the DIS service
            esp_gattc_char_elem_t char_elem_result[10];
            uint16_t char_elem_count = 10;

            ESP_LOGD(TAG, "Getting DIS service characteristics - start: %d, end: %d",
                     dis_service_start_handle, dis_service_end_handle);

            // Use DIS's own GATT interface with its own connection
            ESP_LOGD(TAG, "Using DIS gattc_if: %d with conn_id: %d", dis_gattc_if, dis_conn_id);

            esp_gatt_status_t status = esp_ble_gattc_get_all_char(
                dis_gattc_if, dis_conn_id,
                dis_service_start_handle,
                dis_service_end_handle,
                char_elem_result,
                &char_elem_count,
                0
            );

            ESP_LOGI(TAG, "Get characteristics result: status=%d, count=%d", status, char_elem_count);

            if (status == ESP_GATT_OK && char_elem_count > 0) {
                // ESP_LOGI(TAG, "Found %d characteristics in DIS service", char_elem_count);

                // Look for DIS characteristics by UUID and store handles
                for (int i = 0; i < char_elem_count; i++) {
                    if (char_elem_result[i].uuid.len == ESP_UUID_LEN_16) {
                        uint16_t char_uuid = char_elem_result[i].uuid.uuid.uuid16;

                        // Find which DIS characteristic this is
                        for (int j = 0; j < 7; j++) {
                            if (char_uuid == dis_char_uuids[j]) {
                                dis_char_handles[j] = char_elem_result[i].char_handle;
                                chars_found_count++;
                                // ESP_LOGI(TAG, "Found %s characteristic at handle %d",
                                //          char_names[j], dis_char_handles[j]);
                                break;
                            }
                        }
                    }
                }

                // Start reading characteristics
                if (chars_found_count > 0) {
                    // ESP_LOGI(TAG, "Starting to read %d DIS characteristics", chars_found_count);
                    dis_connected = true;

                    // Read first available characteristic
                    for (int i = 0; i < 7; i++) {
                        if (dis_char_handles[i] != 0) {
                            // ESP_LOGI(TAG, "Reading %s...", char_names[i]);
                            esp_err_t ret = esp_ble_gattc_read_char(dis_gattc_if, dis_conn_id,
                                                                   dis_char_handles[i], ESP_GATT_AUTH_REQ_NONE);
                            if (ret != ESP_OK) {
                                ESP_LOGE(TAG, "Failed to read %s: %s", char_names[i], esp_err_to_name(ret));
                            }
                            break; // Only start one read at a time
                        }
                    }
                } else {
                    ESP_LOGW(TAG, "No DIS characteristics found");
                }
            } else {
                ESP_LOGE(TAG, "Failed to get DIS characteristics: status=%d", status);
            }
        } else if (param->search_cmpl.conn_id == dis_conn_id && !dis_service_discovered) {
            ESP_LOGW(TAG, "DIS service (UUID 0x%04X) not found on this device", DIS_SERVICE_UUID);
            ESP_LOGI(TAG, "This is normal - many BLE HID devices don't implement Device Information Service");
            ESP_LOGI(TAG, "Device info may be available through other means (device name, manufacturer data, etc.)");

            // Mark as complete but with no info
            current_device_info.info_complete = false;

            // We could still try to extract some info from the device name or advertisement data
            // For now, let's call the completion callback to indicate we're done trying
            if (dis_config.info_complete_cb) {
                dis_config.info_complete_cb(&current_device_info);
            }
        }
        break;

    case ESP_GATTC_READ_CHAR_EVT:
        // ESP_LOGI(TAG, "DIS received read response - conn_id: %d, handle: %d, status: %d",
        //          param->read.conn_id, param->read.handle, param->read.status);
        if (param->read.status == ESP_GATT_OK) {
            // Find which characteristic was read
            for (int i = 0; i < 7; i++) {
                if (dis_char_handles[i] == param->read.handle) {
                    chars_read_count++;

                    if (i == 6) { // PnP ID characteristic (binary data)
                        if (param->read.value_len >= 7) { // PnP ID should be 7 bytes
                            current_device_info.pnp_id.vendor_id_source = param->read.value[0];
                            current_device_info.pnp_id.vendor_id = (param->read.value[2] << 8) | param->read.value[1];
                            current_device_info.pnp_id.product_id = (param->read.value[4] << 8) | param->read.value[3];
                            current_device_info.pnp_id.product_version = (param->read.value[6] << 8) | param->read.value[5];
                            current_device_info.has_pnp_id = true;

                            // const char* vendor_source = (current_device_info.pnp_id.vendor_id_source == 0x01) ? "Bluetooth SIG" :
                            //                            (current_device_info.pnp_id.vendor_id_source == 0x02) ? "USB Forum" : "Unknown";
                            // ESP_LOGI(TAG, "PnP ID: Source=%s, VID=0x%04X, PID=0x%04X, Ver=0x%04X",
                            //          vendor_source,
                            //          current_device_info.pnp_id.vendor_id,
                            //          current_device_info.pnp_id.product_id,
                            //          current_device_info.pnp_id.product_version);
                        } else {
                            ESP_LOGW(TAG, "PnP ID data too short (%d bytes, expected 7)", param->read.value_len);
                        }
                    } else { // String characteristic
                        // Copy the string data, ensuring null termination
                        size_t copy_len = (param->read.value_len < (DIS_MAX_STRING_LEN - 1)) ?
                                          param->read.value_len : (DIS_MAX_STRING_LEN - 1);
                        memcpy(device_info_strings[i], param->read.value, copy_len);
                        device_info_strings[i][copy_len] = '\0';
                        // ESP_LOGI(TAG, "%s: %s", char_names[i], device_info_strings[i]);
                    }

                    // Start reading next characteristic
                    bool started_next = false;
                    for (int j = i + 1; j < 7; j++) {
                        if (dis_char_handles[j] != 0) {
                            ESP_LOGD(TAG, "Reading %s...", char_names[j]);
                            esp_err_t ret = esp_ble_gattc_read_char(dis_gattc_if, param->read.conn_id,
                                                                   dis_char_handles[j], ESP_GATT_AUTH_REQ_NONE);
                            if (ret != ESP_OK) {
                                ESP_LOGE(TAG, "Failed to read %s: %s", char_names[j], esp_err_to_name(ret));
                            }
                            started_next = true;
                            break;
                        }
                    }

                    // If no more characteristics to read, check if we're done
                    if (!started_next) {
                        check_info_complete();
                    }
                    break;
                }
            }
        } else {
            ESP_LOGE(TAG, "Failed to read DIS characteristic: handle=%d, status=%d",
                     param->read.handle, param->read.status);
            chars_read_count++; // Count failed reads too
            check_info_complete();
        }
        break;

    case ESP_GATTC_DISCONNECT_EVT:
        if (param->disconnect.conn_id == dis_conn_id) {
            ESP_LOGI(TAG, "DIS client disconnected, conn_id: %d", param->disconnect.conn_id);
            dis_connected = false;
            dis_service_discovered = false;
            dis_conn_id = 0xFFFF;
        }
        break;

    default:
        // We don't handle other events in this module
        break;
    }
}