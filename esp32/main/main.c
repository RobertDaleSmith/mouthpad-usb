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
#include "ble_central.h"
#include "usb_hid.h"
#include "driver/uart.h"
#include "dev_dfu.h"
#include "ble_bas.h"
#include "leds.h"
#include "nus_cdc_bridge.h"
#include "ble_nus.h"
#include "app_config.h"
#include "button.h"
#include "ble_dis.h"

static const char *TAG = "BLE_HID_CENTRAL";

static esp_hidh_dev_t *s_active_dev;
static esp_bd_addr_t s_active_addr;
static esp_timer_handle_t s_rssi_timer;
static bool s_rssi_timer_running;
static bool s_has_active_addr;
static uint16_t s_active_conn_id = 0xFFFF;
static esp_gatt_if_t s_active_gattc_if = ESP_GATT_IF_NONE;
static esp_gatt_if_t s_nus_gattc_if = ESP_GATT_IF_NONE;  // Separate interface for NUS
static esp_gatt_if_t s_dis_gattc_if = ESP_GATT_IF_NONE;  // Separate interface for DIS

// Advertisement data from the connected device
static uint16_t s_active_appearance = 0;
static uint8_t s_active_manufacturer_data[32] = {0};
static uint8_t s_active_manufacturer_len = 0;

static const char* appearance_to_string(uint16_t appearance)
{
    switch (appearance) {
        case 0x03C0: return "Generic HID";
        case 0x03C1: return "Keyboard";
        case 0x03C2: return "Mouse";
        case 0x03C3: return "Joystick";
        case 0x03C4: return "Gamepad";
        case 0x03C5: return "Digitizer Tablet";
        case 0x03C6: return "Card Reader";
        case 0x03C7: return "Digital Pen";
        case 0x03C8: return "Barcode Scanner";
        case 0x0000: return "Unknown";
        default: return "Other";
    }
}

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
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_rssi_timer, 10 * 1000 * 1000));
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
    ESP_LOGI(TAG, "Address: %02X:%02X:%02X:%02X:%02X:%02X",
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

// Device info completion callback
static void device_info_complete_callback(const ble_device_info_t *device_info)
{
    // ESP_LOGI(TAG, "Device info discovery completed for connected device");
    ble_device_info_print(device_info);
}

// Wrapper GATT client callback that handles both HID and NUS events
static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    // Fast path: process GATT events without per-event logging

    // Log registration events in detail
    if (event == ESP_GATTC_REG_EVT) {
        ESP_LOGI(TAG, "GATTC_REG_EVT: app_id=%d, status=%d, gattc_if=%d",
                 param->reg.app_id, param->reg.status, gattc_if);

        // Store the NUS GATT interface when it registers
        if (param->reg.app_id == 1 && param->reg.status == ESP_GATT_OK) {
            s_nus_gattc_if = gattc_if;
            ESP_LOGI(TAG, "NUS GATT client registered with interface: %d", s_nus_gattc_if);
        }
        // Store the DIS GATT interface when it registers
        else if (param->reg.app_id == 2 && param->reg.status == ESP_GATT_OK) {
            s_dis_gattc_if = gattc_if;
            ESP_LOGI(TAG, "DIS GATT client registered with interface: %d", s_dis_gattc_if);
        }
    }
    
    // Store connection ID and GATT client interface when device connects
    if (event == ESP_GATTC_CONNECT_EVT) {
        s_active_conn_id = param->connect.conn_id;
        s_active_gattc_if = gattc_if;
        ESP_LOGI(TAG, "GATT client connected, conn_id: %d, gattc_if: %d", s_active_conn_id, s_active_gattc_if);
        
#if ENABLE_NUS_CLIENT_MODE
        // NUS service discovery will be triggered from HID open event
        // to ensure HID service discovery completes first
#endif
    } else if (event == ESP_GATTC_DISCONNECT_EVT) {
        if (param->disconnect.conn_id == s_active_conn_id) {
            s_active_conn_id = 0xFFFF;
            s_active_gattc_if = ESP_GATT_IF_NONE;
            ESP_LOGI(TAG, "GATT client disconnected, conn_id: %d", param->disconnect.conn_id);
        }
    }
    
    // Route events based on separate GATT interfaces (fast path - no logging)
    if (gattc_if == s_nus_gattc_if) {
        // This is a NUS event on the dedicated NUS GATT interface
#if ENABLE_NUS_CLIENT_MODE
        nus_cdc_bridge_handle_gattc_event(event, gattc_if, param);
#endif
    } else if (gattc_if == s_dis_gattc_if) {
        // This is a DIS event on the dedicated DIS GATT interface
        ble_device_info_handle_gattc_event(event, gattc_if, param);
    } else {
        // This is a HID event (or registration event)
        esp_hidh_gattc_event_handler(event, gattc_if, param);

        // For registration events, also forward to NUS if it's app_id 1
        if (event == ESP_GATTC_REG_EVT && param->reg.app_id == 1) {
#if ENABLE_NUS_CLIENT_MODE
            ESP_LOGI(TAG, "Also routing NUS registration event to NUS handler");
            nus_cdc_bridge_handle_gattc_event(event, gattc_if, param);
#endif
        }
        // For registration events, also forward to DIS if it's app_id 2
        else if (event == ESP_GATTC_REG_EVT && param->reg.app_id == 2) {
            ESP_LOGI(TAG, "Also routing DIS registration event to DIS handler");
            ble_device_info_handle_gattc_event(event, gattc_if, param);
        }

        // NUS and DIS now handle their own service discovery on their own GATT interfaces
        // No forwarding needed from HID's discovery events
    }
}

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
            ESP_LOGI(TAG, "=== DEVICE CONNECTED ===");
            ESP_LOGI(TAG, "Name: %s",
                     esp_hidh_dev_name_get(param->open.dev));
            if (bda) {
                log_addr(bda);
            }
            if (s_active_appearance != 0) {
                ESP_LOGI(TAG, "Appearance: 0x%04X (%s)", s_active_appearance, appearance_to_string(s_active_appearance));
            }
            if (s_active_manufacturer_len > 0) {
                ESP_LOG_BUFFER_HEX_LEVEL(TAG, s_active_manufacturer_data, s_active_manufacturer_len, ESP_LOG_INFO);
            }
            if (bda) {
                memcpy(s_active_addr, bda, sizeof(s_active_addr));
                s_active_dev = param->open.dev;
                s_has_active_addr = true;
                schedule_rssi_poll();
                start_rssi_timer();

#if ENABLE_NUS_CLIENT_MODE
                // Set the server BD address for NUS client so it can open its own connection
                ble_nus_client_set_server_bda(bda);
                // Trigger NUS service discovery immediately - no delay needed with separate GATT instances
                ESP_LOGI(TAG, "s_active_conn_id=%d, s_nus_gattc_if=%d", s_active_conn_id, s_nus_gattc_if);

                if (s_active_conn_id != 0xFFFF && s_nus_gattc_if != ESP_GATT_IF_NONE) {
                    ESP_LOGI(TAG, "Starting NUS service discovery");
                    esp_err_t ret = nus_cdc_bridge_discover_services(s_nus_gattc_if, s_active_conn_id);
                    if (ret != ESP_OK) {
                        ESP_LOGW(TAG, "Failed to start NUS service discovery: %s", esp_err_to_name(ret));
                    } else {
                        ESP_LOGI(TAG, "NUS discovery started successfully on separate GATT interface");
                    }
                } else {
                    ESP_LOGW(TAG, "Cannot start NUS discovery - conn_id: %d, nus_gattc_if: %d",
                             s_active_conn_id, s_nus_gattc_if);
                }
#endif

                // Start Device Information Service discovery
                if (s_active_conn_id != 0xFFFF && s_dis_gattc_if != ESP_GATT_IF_NONE && s_has_active_addr) {
                    // ESP_LOGI(TAG, "Starting DIS service discovery to get device info");
                    esp_err_t ret = ble_device_info_discover(s_dis_gattc_if, s_active_conn_id, s_active_addr, esp_hidh_dev_name_get(param->open.dev));
                    if (ret != ESP_OK) {
                        ESP_LOGW(TAG, "Failed to start DIS service discovery: %s", esp_err_to_name(ret));
                    } else {
                        ESP_LOGI(TAG, "DIS discovery started successfully on separate GATT interface");
                    }
                } else {
                    ESP_LOGW(TAG, "Cannot start DIS discovery - conn_id: %d, dis_gattc_if: %d, has_addr: %d",
                             s_active_conn_id, s_dis_gattc_if, s_has_active_addr);
                }
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
            // Fast path: Send HID report immediately with minimal processing
            if (usb_hid_ready()) {
                usb_hid_send_report(param->input.report_id,
                                         param->input.data,
                                         param->input.length);
            }
            // Non-critical operations moved after the report sending
            leds_notify_activity();
            // RSSI is now only polled via timer (every 10s) to reduce log spam
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
            // Reset advertisement data
            s_active_appearance = 0;
            s_active_manufacturer_len = 0;
            memset(s_active_manufacturer_data, 0, sizeof(s_active_manufacturer_data));
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

static ble_central_scan_result_t *choose_best_result(ble_central_scan_result_t *results)
{
    ble_central_scan_result_t *best = NULL;
    int best_rssi = -128;
    for (ble_central_scan_result_t *r = results; r != NULL; r = r->next) {
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
        ble_central_scan_result_t *results = NULL;

        ESP_LOGI(TAG, "Scanning for BLE HID devices...");
        // Use minimum scan window (1 second) - API doesn't support sub-second scans
        ble_central_scan(1, &results_len, &results);
        ESP_LOGI(TAG, "Scan window complete, %u result(s)", (unsigned)results_len);

        ble_central_scan_result_t *target = NULL;
        if (results) {
            target = choose_best_result(results);
        }

        if (target) {
            ESP_LOGI(TAG, "Found BLE device RSSI=%d name=%s", target->rssi, target->name);
            ESP_LOGI(TAG, "Connecting to best result RSSI=%d", target->rssi);

            // Store advertisement data for later display
            if (target->transport == ESP_HID_TRANSPORT_BLE) {
                s_active_appearance = target->ble.appearance;
                s_active_manufacturer_len = 0; // Will be filled if manufacturer data was captured
            }

            esp_hidh_dev_t *dev = esp_hidh_dev_open(target->bda, target->transport, target->ble.addr_type);
            if (!dev) {
                ESP_LOGW(TAG, "Failed to initiate connection, continuing scan");
            } else {
                ESP_LOGI(TAG, "Connection requested");
                ble_central_scan_results_free(results);
                break;
            }
        } else {
            ESP_LOGI(TAG, "No named BLE HID results found, continuing scan");
        }

        if (results) {
            ble_central_scan_results_free(results);
        }

        // Minimize delay between scans for fastest discovery
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    vTaskDelete(NULL);
}

static void start_scan_task(void)
{
    leds_set_state(LED_STATE_SCANNING);
    xTaskCreate(scan_task, "hid_scan", 4096, NULL, 2, NULL);
}

// Button event handler
static void button_event_handler(button_event_t event)
{
    switch (event) {
        case BUTTON_EVENT_SINGLE_CLICK:
            ESP_LOGI(TAG, "Button single click detected");
            break;

        case BUTTON_EVENT_DOUBLE_CLICK:
            ESP_LOGI(TAG, "Button double click detected");
            break;

        case BUTTON_EVENT_LONG_PRESS:
            ESP_LOGI(TAG, "Button long press detected - clearing BLE bonds");

            // Get number of bonded devices
            int bond_dev_num = esp_ble_get_bond_device_num();
            ESP_LOGI(TAG, "Number of bonded devices: %d", bond_dev_num);

            if (bond_dev_num > 0) {
                // Get list of bonded devices
                esp_ble_bond_dev_t *bond_dev_list = malloc(sizeof(esp_ble_bond_dev_t) * bond_dev_num);
                if (bond_dev_list) {
                    esp_ble_get_bond_device_list(&bond_dev_num, bond_dev_list);

                    // Remove each bonded device
                    for (int i = 0; i < bond_dev_num; i++) {
                        ESP_LOGI(TAG, "Removing bond for device " ESP_BD_ADDR_STR,
                                ESP_BD_ADDR_HEX(bond_dev_list[i].bd_addr));
                        esp_err_t ret = esp_ble_remove_bond_device(bond_dev_list[i].bd_addr);
                        if (ret != ESP_OK) {
                            ESP_LOGW(TAG, "Failed to remove bond: %s", esp_err_to_name(ret));
                        }
                    }

                    free(bond_dev_list);
                    ESP_LOGI(TAG, "All BLE bonds cleared");
                } else {
                    ESP_LOGE(TAG, "Failed to allocate memory for bond device list");
                }
            } else {
                ESP_LOGI(TAG, "No bonded devices to clear");
            }
            break;

        default:
            ESP_LOGW(TAG, "Unknown button event: %d", event);
            break;
    }
}

// Remap UART0 for external logging (J-Link connection)
static void setup_uart_logging(void)
{
    // Remap UART0 to XIAO Expansion Board servo connector pins
    // Expansion board labels: TX|6 and RX|7
    // TX|6 = D6 pin for transmitting logs to J-Link
    // Connect J-Link UART RX to the TX|6 pin on servo connector
    uart_set_pin(UART_NUM_0, 6, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    ESP_LOGI(TAG, "UART0 remapped to TX|6 pin on expansion board servo connector");
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // Setup UART logging on accessible pin for J-Link
    setup_uart_logging();

    esp_err_t led_err = leds_init();
    if (led_err != ESP_OK) {
        ESP_LOGW(TAG, "LED init failed: %s", esp_err_to_name(led_err));
    } else {
        leds_set_state(LED_STATE_SCANNING);
    }

    // Initialize button module
    esp_err_t button_err = button_init(button_event_handler);
    if (button_err != ESP_OK) {
        ESP_LOGW(TAG, "Button init failed: %s", esp_err_to_name(button_err));
    }

    ESP_ERROR_CHECK(ble_bas_init());
    bootloader_trigger_init();

    usb_hid_init();

    // Initialize BLE transport for HID central mode
    ESP_ERROR_CHECK(ble_central_init(HID_HOST_MODE));

    ble_central_set_user_ble_callback(gap_callback);

    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(gattc_event_handler));

    esp_hidh_config_t config = {
        .callback = hidh_callback,
        .event_stack_size = 8192,  // Increased stack size for better performance
        .callback_arg = NULL,
    };

    ESP_ERROR_CHECK(esp_hidh_init(&config));

#if ENABLE_NUS_CLIENT_MODE
    // Register a separate GATT app for NUS (app_id 1)
    // The ESP HID library has already registered app_id 0
    ESP_LOGI(TAG, "Registering separate NUS GATT client app (app_id=1)");
    ESP_ERROR_CHECK(esp_ble_gattc_app_register(1));

    // Initialize NUS client to CDC bridge
    ESP_LOGI(TAG, "Initializing NUS client to CDC bridge");
    ESP_ERROR_CHECK(nus_cdc_bridge_init());
    ESP_ERROR_CHECK(nus_cdc_bridge_start());
#endif

    // Register a separate GATT app for Device Info Service (app_id 2)
    ESP_LOGI(TAG, "Registering separate DIS GATT client app (app_id=2)");
    ESP_ERROR_CHECK(esp_ble_gattc_app_register(2));

    // Initialize Device Info Service client
    ble_device_info_config_t dis_config = {
        .info_complete_cb = device_info_complete_callback
    };
    ESP_LOGI(TAG, "Initializing Device Info Service client");
    ESP_ERROR_CHECK(ble_device_info_init(&dis_config));

    start_scan_task();
    ESP_LOGI(TAG, "BLE HID central ready");
}
