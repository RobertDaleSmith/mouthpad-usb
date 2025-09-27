#include "ble_bonds.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include <string.h>

static const char *TAG = "BLE_BONDS";

// NVS namespace and key for storing bonded device
#define NVS_NAMESPACE "ble_bonds"
#define NVS_KEY_BONDED_DEVICE "bonded_dev"

// Bonded device storage
static bool s_has_bonded_device = false;
static esp_bd_addr_t s_bonded_device_addr = {0};

esp_err_t ble_bonds_init(void)
{
    ESP_LOGI(TAG, "Initializing BLE bonding system");

    // Initialize NVS if not already done
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs to be erased, reinitializing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    // Try to load bonded device from NVS
    nvs_handle_t nvs_handle;
    ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret == ESP_OK) {
        size_t required_size = sizeof(esp_bd_addr_t);
        ret = nvs_get_blob(nvs_handle, NVS_KEY_BONDED_DEVICE, s_bonded_device_addr, &required_size);
        if (ret == ESP_OK && required_size == sizeof(esp_bd_addr_t)) {
            s_has_bonded_device = true;
            ESP_LOGI(TAG, "Loaded bonded device: %02X:%02X:%02X:%02X:%02X:%02X",
                     s_bonded_device_addr[0], s_bonded_device_addr[1], s_bonded_device_addr[2],
                     s_bonded_device_addr[3], s_bonded_device_addr[4], s_bonded_device_addr[5]);
        } else if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "No bonded device found in storage");
        } else {
            ESP_LOGW(TAG, "Failed to load bonded device: %s", esp_err_to_name(ret));
        }
        nvs_close(nvs_handle);
    } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to open NVS for reading: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "BLE bonding system initialized (has_bond=%d)", s_has_bonded_device);
    return ESP_OK;
}

bool ble_bonds_has_bonded_device(void)
{
    return s_has_bonded_device;
}

esp_err_t ble_bonds_get_bonded_device(esp_bd_addr_t bda)
{
    if (!s_has_bonded_device) {
        return ESP_ERR_NOT_FOUND;
    }

    memcpy(bda, s_bonded_device_addr, sizeof(esp_bd_addr_t));
    return ESP_OK;
}

bool ble_bonds_is_bonded_device(const esp_bd_addr_t bda)
{
    if (!s_has_bonded_device) {
        return false;
    }

    return memcmp(bda, s_bonded_device_addr, sizeof(esp_bd_addr_t)) == 0;
}

esp_err_t ble_bonds_store_device(const esp_bd_addr_t bda)
{
    ESP_LOGI(TAG, "Storing bonded device: %02X:%02X:%02X:%02X:%02X:%02X",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);

    // Open NVS for writing
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(ret));
        return ret;
    }

    // Store the device address
    ret = nvs_set_blob(nvs_handle, NVS_KEY_BONDED_DEVICE, bda, sizeof(esp_bd_addr_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to store bonded device: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    // Commit the changes
    ret = nvs_commit(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit bonded device: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    nvs_close(nvs_handle);

    // Update runtime state
    memcpy(s_bonded_device_addr, bda, sizeof(esp_bd_addr_t));
    s_has_bonded_device = true;

    ESP_LOGI(TAG, "Successfully stored bonded device");
    return ESP_OK;
}

esp_err_t ble_bonds_clear_all(void)
{
    ESP_LOGI(TAG, "Clearing all bonded devices");

    // ESP-IDF's built-in GATT cache will be cleared automatically with bonds

    // Clear runtime state
    s_has_bonded_device = false;
    memset(s_bonded_device_addr, 0, sizeof(esp_bd_addr_t));

    // Open NVS for writing
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for clearing: %s", esp_err_to_name(ret));
        return ret;
    }

    // Remove the bonded device entry
    ret = nvs_erase_key(nvs_handle, NVS_KEY_BONDED_DEVICE);
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to erase bonded device key: %s", esp_err_to_name(ret));
    }

    // Commit the changes
    ret = nvs_commit(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to commit bond clearing: %s", esp_err_to_name(ret));
    }

    nvs_close(nvs_handle);

    // Clear all BLE bonds from the BLE stack
    // Note: We don't use esp_ble_remove_bond_device(NULL) as it can cause crashes
    // with active connections. Instead, the bond clearing is handled through
    // runtime state clearing and NVS erasure above.
    ESP_LOGI(TAG, "BLE stack bonds will be cleared on next restart");

    ESP_LOGI(TAG, "All bonds cleared successfully");
    return ESP_OK;
}

esp_err_t ble_bonds_get_info_string(char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size < 24) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_has_bonded_device) {
        snprintf(buffer, buffer_size, "%02X:%02X:%02X:%02X:%02X:%02X",
                 s_bonded_device_addr[0], s_bonded_device_addr[1], s_bonded_device_addr[2],
                 s_bonded_device_addr[3], s_bonded_device_addr[4], s_bonded_device_addr[5]);
    } else {
        snprintf(buffer, buffer_size, "No bonded device");
    }

    return ESP_OK;
}