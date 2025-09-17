#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_event.h"
#include "esp_gap_ble_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "esp_hid_common.h"
#include "esp_hidh.h"
#include "esp_hidh_gattc.h"
#include "ble_transport.h"
#include "usb_hid.h"
#include "bootloader_trigger.h"
#include "ble_bas.h"
#include "leds.h"

static const char *TAG = "BLE_HID_CENTRAL";

static esp_hidh_dev_t *s_active_dev;
static esp_bd_addr_t s_active_addr;
static esp_timer_handle_t s_rssi_timer;
static bool s_rssi_timer_running;
static bool s_has_active_addr;

static void schedule_rssi_poll(void)
{
    if (!s_active_dev || !s_has_active_addr) {
        return;
    }
    esp_err_t err = esp_ble_gap_read_rssi(s_active_addr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to request RSSI: %s", esp_err_to_name(err));
    }
}

static void rssi_timer_callback(void *arg)
{
    (void)arg;
    schedule_rssi_poll();
}

static void start_rssi_timer(void)
{
    if (s_rssi_timer_running) {
        return;
    }
    if (!s_rssi_timer) {
        const esp_timer_create_args_t args = {
            .callback = rssi_timer_callback,
            .name = "rssi_poll",
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &s_rssi_timer));
    }
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_rssi_timer, 2 * 1000 * 1000));
    s_rssi_timer_running = true;
}

static void stop_rssi_timer(void)
{
    if (!s_rssi_timer_running || !s_rssi_timer) {
        return;
    }
    ESP_ERROR_CHECK(esp_timer_stop(s_rssi_timer));
    s_rssi_timer_running = false;
}

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

static void log_addr(const uint8_t *addr)
{
    if (!addr) {
        ESP_LOGI(TAG, "(unknown addr)");
        return;
    }
    ESP_LOGI(TAG, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

static bool addr_matches_active(const uint8_t *addr)
{
    return s_has_active_addr && addr && memcmp(addr, s_active_addr, sizeof(s_active_addr)) == 0;
}

static void gap_callback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT:
        if (param->read_rssi_cmpl.status == ESP_BT_STATUS_SUCCESS &&
            addr_matches_active(param->read_rssi_cmpl.remote_addr)) {
            ESP_LOGI(TAG, "RSSI update: addr=%02X:%02X:%02X:%02X:%02X:%02X, rssi=%d dBm",
                     param->read_rssi_cmpl.remote_addr[0], param->read_rssi_cmpl.remote_addr[1],
                     param->read_rssi_cmpl.remote_addr[2], param->read_rssi_cmpl.remote_addr[3],
                     param->read_rssi_cmpl.remote_addr[4], param->read_rssi_cmpl.remote_addr[5],
                     param->read_rssi_cmpl.rssi);
        } else {
            ESP_LOGW(TAG, "RSSI read failed: 0x%x", param->read_rssi_cmpl.status);
        }
        break;
    default:
        break;
    }
}

static void start_scan_task(void);

static void hidh_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_hidh_event_t event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t *param = (esp_hidh_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDH_OPEN_EVENT:
        if (param->open.dev) {
            const uint8_t *bda = esp_hidh_dev_bda_get(param->open.dev);
            ESP_LOGI(TAG, "Device connected: %s",
                     esp_hidh_dev_name_get(param->open.dev));
            log_addr(bda);
            if (bda) {
                memcpy(s_active_addr, bda, sizeof(s_active_addr));
                s_active_dev = param->open.dev;
                s_has_active_addr = true;
                schedule_rssi_poll();
                start_rssi_timer();
            }
            ble_bas_reset();
            leds_set_state(LED_STATE_CONNECTED);
        }
        break;
    case ESP_HIDH_BATTERY_EVENT:
        if (param->battery.status == ESP_OK) {
            ble_bas_handle_level(param->battery.level);
        } else {
            ESP_LOGW(TAG, "Battery event error: %d", param->battery.status);
        }
        break;
    case ESP_HIDH_INPUT_EVENT:
        if (param->input.dev) {
            ESP_LOGI(TAG, "Input report (usage=%s, id=%u)",
                     esp_hid_usage_str(param->input.usage), param->input.report_id);
            log_report(param->input.data, param->input.length);
            schedule_rssi_poll();
            if (usb_hid_ready()) {
                usb_hid_send_report(param->input.report_id,
                                         param->input.data,
                                         param->input.length);
            }
            leds_notify_activity();
        }
        break;
    case ESP_HIDH_FEATURE_EVENT:
        if (param->feature.dev) {
            ESP_LOGI(TAG, "Feature report (usage=%s, id=%u)",
                     esp_hid_usage_str(param->feature.usage), param->feature.report_id);
            log_report(param->feature.data, param->feature.length);
        }
        break;
    case ESP_HIDH_CLOSE_EVENT:
        ESP_LOGI(TAG, "Device disconnected");
        stop_rssi_timer();
        if (param->close.dev == s_active_dev) {
            s_active_dev = NULL;
            s_has_active_addr = false;
        }
        if (param->close.dev) {
            esp_hidh_dev_free(param->close.dev);
        }
        ble_bas_reset();
        leds_set_state(LED_STATE_SCANNING);
        start_scan_task();
        break;
    default:
        ESP_LOGD(TAG, "Unhandled HID event %d", event);
        break;
    }
}

static ble_transport_scan_result_t *choose_best_result(ble_transport_scan_result_t *results)
{
    ble_transport_scan_result_t *best = NULL;
    int best_rssi = -128;
    for (ble_transport_scan_result_t *r = results; r != NULL; r = r->next) {
        if (r->name) {
            ESP_LOGI(TAG, "Found %s device RSSI=%d name=%s",
                     r->transport == ESP_HID_TRANSPORT_BLE ? "BLE" : "BT",
                     r->rssi,
                     r->name);
        }
        if (r->transport == ESP_HID_TRANSPORT_BLE && r->rssi > best_rssi) {
            best = r;
            best_rssi = r->rssi;
        }
    }
    return best;
}

static void scan_task(void *args)
{
    (void)args;
    while (true) {
        size_t results_len = 0;
        ble_transport_scan_result_t *results = NULL;

        ESP_LOGI(TAG, "Scanning for BLE HID devices...");
        ble_transport_scan(5, &results_len, &results);
        ESP_LOGI(TAG, "Scan window complete, %u result(s)", (unsigned)results_len);

        ble_transport_scan_result_t *target = NULL;
        if (results) {
            target = choose_best_result(results);
        }

        if (target) {
            ESP_LOGI(TAG, "Connecting to best result RSSI=%d", target->rssi);
            esp_hidh_dev_t *dev = esp_hidh_dev_open(target->bda, target->transport, target->ble.addr_type);
            if (!dev) {
                ESP_LOGW(TAG, "Failed to initiate connection, continuing scan");
            } else {
                ESP_LOGI(TAG, "Connection requested");
                ble_transport_scan_results_free(results);
                break;
            }
        } else {
            ESP_LOGI(TAG, "No named BLE HID results found, continuing scan");
        }

        if (results) {
            ble_transport_scan_results_free(results);
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }

    vTaskDelete(NULL);
}

static void start_scan_task(void)
{
    leds_set_state(LED_STATE_SCANNING);
    xTaskCreate(scan_task, "hid_scan", 4096, NULL, 2, NULL);
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    esp_err_t led_err = leds_init();
    if (led_err != ESP_OK) {
        ESP_LOGW(TAG, "LED init failed: %s", esp_err_to_name(led_err));
    } else {
        leds_set_state(LED_STATE_SCANNING);
    }

    ESP_ERROR_CHECK(ble_bas_init());
    bootloader_trigger_init();

    usb_hid_init();

    ESP_ERROR_CHECK(ble_transport_init(HID_HOST_MODE));

    ble_transport_set_user_ble_callback(gap_callback);

    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler));

    esp_hidh_config_t config = {
        .callback = hidh_callback,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };

    ESP_ERROR_CHECK(esp_hidh_init(&config));

    start_scan_task();
    ESP_LOGI(TAG, "BLE HID central ready");
}
