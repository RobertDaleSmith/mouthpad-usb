#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_err.h"
#include "tusb.h"

static const char *TAG = "MouthPadBridge";
static esp_gatt_if_t gattc_if = 0;
static uint16_t conn_id = 0;
static uint16_t hid_char_handle = 0;

// HID service UUID (16-bit)
static const uint16_t HID_SERVICE_UUID = 0x1812;

// Nordic UART Service UUID
static const uint8_t NUS_UUID_128[16] = {0x9E,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,
                                         0x93,0xF3,0xA3,0xB5,0x01,0x00,0x40,0x6E};

static esp_ble_scan_params_t ble_scan_params = {
    .scan_type              = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type          = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy     = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval          = 0x50,
    .scan_window            = 0x30,
    .scan_duplicate         = BLE_SCAN_DUPLICATE_DISABLE
};

static void handle_hid_report(const uint8_t *data, uint16_t len)
{
    if (len < 1) {
        return;
    }
    if (tud_hid_ready()) {
        uint8_t report_id = data[0];
        tud_hid_report(report_id, data + 1, len - 1);
    }
}

static void gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if_evt,
                     esp_ble_gattc_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTC_REG_EVT:
        gattc_if = gattc_if_evt;
        ESP_LOGI(TAG, "GATTC registered, starting scan");
        esp_ble_gap_set_scan_params(&ble_scan_params);
        break;
    case ESP_GATTC_CONNECT_EVT:
        conn_id = param->connect.conn_id;
        ESP_LOGI(TAG, "Connected, searching services");
        esp_ble_gattc_search_service(gattc_if, conn_id, NULL);
        break;
    case ESP_GATTC_SEARCH_RES_EVT: {
        esp_gatt_srvc_id_t *srvc_id = &param->search_res.srvc_id;
        if (srvc_id->id.uuid.len == ESP_UUID_LEN_16 &&
            srvc_id->id.uuid.uuid.uuid16 == HID_SERVICE_UUID) {
            ESP_LOGI(TAG, "Found HID service");
        }
        break;
    }
    case ESP_GATTC_SEARCH_CMPL_EVT:
        ESP_LOGI(TAG, "Service discovery complete");
        break;
    case ESP_GATTC_NOTIFY_EVT:
        handle_hid_report(param->notify.value, param->notify.value_len);
        break;
    default:
        break;
    }
}

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        ESP_LOGI(TAG, "Starting scan");
        esp_ble_gap_start_scanning(0);
        break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        esp_ble_gap_cb_param_t *scan = param;
        if (scan->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            uint8_t len;
            uint8_t *uuid16 = esp_ble_resolve_adv_data(scan->scan_rst.ble_adv,
                                ESP_BLE_AD_TYPE_16SRV_CMPL, &len);
            bool has_hid = false;
            for (int i = 0; uuid16 && i < len; i += 2) {
                uint16_t u = uuid16[i] | (uuid16[i+1] << 8);
                if (u == HID_SERVICE_UUID) {
                    has_hid = true;
                }
            }
            uint8_t *uuid128 = esp_ble_resolve_adv_data(scan->scan_rst.ble_adv,
                                 ESP_BLE_AD_TYPE_128SRV_CMPL, &len);
            bool has_nus = false;
            if (uuid128 && len == 16 && memcmp(uuid128, NUS_UUID_128, 16) == 0) {
                has_nus = true;
            }
            if (has_hid && has_nus) {
                ESP_LOGI(TAG, "Found MouthPad^ device, connecting");
                esp_ble_gap_stop_scanning();
                esp_ble_gattc_open(gattc_if, scan->scan_rst.bda, true);
            }
        }
        break;
    }
    default:
        break;
    }
}

static void usb_init(void)
{
    tinyusb_config_t tusb_cfg = {
        .descriptor = NULL,
        .string_descriptor = NULL,
        .external_phy = false
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_cb));
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(gattc_cb));
    ESP_ERROR_CHECK(esp_ble_gattc_app_register(0));

    usb_init();

    while (1) {
        tud_task();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
