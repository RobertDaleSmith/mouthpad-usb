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
#include "usb_dfu.h"
#include "ble_bas.h"
#include "leds.h"
#include "transport_uart.h"
#include "ble_nus.h"
#include "app_config.h"
#include "button.h"
#include "ble_dis.h"
#include "transport_hid.h"
#include "ble_hid.h"
#include "ble_bonds.h"
#include "relay_protocol.h"

static const char *TAG = "MP_MAIN";

// s_active_dev now managed by transport_hid and ble_hid modules
static esp_bd_addr_t s_active_addr;
static esp_timer_handle_t s_rssi_timer;
static bool s_rssi_timer_running;
static esp_timer_handle_t s_service_discovery_timer;
static bool s_has_active_addr;
static uint16_t s_active_conn_id = 0xFFFF;
static esp_gatt_if_t s_active_gattc_if = ESP_GATT_IF_NONE;
static esp_gatt_if_t s_nus_gattc_if = ESP_GATT_IF_NONE;  // Separate interface for NUS
static esp_gatt_if_t s_dis_gattc_if = ESP_GATT_IF_NONE;  // Separate interface for DIS
static esp_gatt_if_t s_hid_gattc_if = ESP_GATT_IF_NONE;  // HID GATT interface (app_id 0)

// Advertisement data from the connected device
static uint16_t s_active_appearance = 0;
static uint8_t s_active_manufacturer_data[32] = {0};
static uint8_t s_active_manufacturer_len = 0;

// appearance_to_string moved to ble_hid.c

static void schedule_rssi_poll(void)
{
    if (!ble_hid_client_is_connected() || !s_has_active_addr) {
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

// Helper functions log_report and log_addr moved to ble_hid.c

static void service_discovery_timer_callback(void *arg)
{
    ESP_LOGI(TAG, "Starting delayed NUS and DIS service discovery");

    // Start NUS service discovery
#if ENABLE_NUS_CLIENT_MODE
    if (s_has_active_addr) {
        ble_nus_client_set_server_bda(s_active_addr);
        if (s_active_conn_id != 0xFFFF && s_nus_gattc_if != ESP_GATT_IF_NONE) {
            esp_err_t ret = transport_uart_discover_services(s_nus_gattc_if, s_active_conn_id);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to start NUS service discovery: %s", esp_err_to_name(ret));
            }
        }
    }
#endif

    // Start DIS service discovery
    if (s_active_conn_id != 0xFFFF && s_dis_gattc_if != ESP_GATT_IF_NONE && s_has_active_addr) {
        esp_err_t ret = ble_device_info_discover(s_dis_gattc_if, s_active_conn_id, s_active_addr, "RDSMouthPad");
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to start DIS service discovery: %s", esp_err_to_name(ret));
        }
    }
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
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
        if (param->update_conn_params.status == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Connection params updated: interval=%d, latency=%d, timeout=%d",
                     param->update_conn_params.conn_int,
                     param->update_conn_params.latency,
                     param->update_conn_params.timeout);

            // Notify NUS client that connection is now stable
            ble_nus_client_connection_ready();
        } else {
            ESP_LOGW(TAG, "Connection params update failed: 0x%x", param->update_conn_params.status);
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

        // Store the HID GATT interface when it registers
        if (param->reg.app_id == 0 && param->reg.status == ESP_GATT_OK) {
            s_hid_gattc_if = gattc_if;
            ESP_LOGI(TAG, "HID GATT client registered with interface: %d", s_hid_gattc_if);
        }
        // Store the NUS GATT interface when it registers
        else if (param->reg.app_id == 1 && param->reg.status == ESP_GATT_OK) {
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
    } else if (event == ESP_GATTC_SET_ASSOC_EVT) {
        ESP_LOGI(TAG, "GATT cache association event: status=%d", param->set_assoc_cmp.status);
        if (param->set_assoc_cmp.status == ESP_GATT_OK) {
            ESP_LOGI(TAG, "GATT cache association successful - service discovery should be faster");
        } else {
            ESP_LOGW(TAG, "GATT cache association failed: %d", param->set_assoc_cmp.status);
        }
    }
    
    // Route events based on separate GATT interfaces (fast path - no logging)
    if (gattc_if == s_nus_gattc_if) {
        // This is a NUS event on the dedicated NUS GATT interface
#if ENABLE_NUS_CLIENT_MODE
        transport_uart_handle_gattc_event(event, gattc_if, param);
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
            transport_uart_handle_gattc_event(event, gattc_if, param);
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

// BLE HID client callback functions for transport bridge
static void hid_connected_cb(esp_hidh_dev_t *dev, const uint8_t *bda)
{
    if (bda) {
        ESP_LOGI(TAG, "HID device connected: " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(bda));

        // Notify relay protocol that scanning has stopped
        relay_protocol_update_ble_scanning(false);

        memcpy(s_active_addr, bda, sizeof(s_active_addr));
        s_has_active_addr = true;
        schedule_rssi_poll();
        start_rssi_timer();

        // Handle bonding logic
        if (!ble_bonds_has_bonded_device()) {
            // No bonded device yet - bond with this one
            ESP_LOGI(TAG, "No existing bond, storing new bond with device");
            esp_err_t ret = ble_bonds_store_device(bda);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to store bond: %s", esp_err_to_name(ret));
            }
        } else if (!ble_bonds_is_bonded_device(bda)) {
            // This device is not our bonded device - disconnect
            ESP_LOGW(TAG, "Connected device is not bonded device, disconnecting");
            esp_hidh_dev_close(dev);
            return;
        } else {
            ESP_LOGI(TAG, "Connected to bonded device successfully");
        }


        // Set device in transport bridge
        transport_hid_set_device(dev, bda);

        // Request faster connection parameters for lower latency
        esp_ble_conn_update_params_t conn_params = {
            .bda = {0},
            .min_int = 0x06,  // 6 * 1.25ms = 7.5ms (minimum allowed)
            .max_int = 0x10,  // 16 * 1.25ms = 20ms
            .latency = 0x00,  // No slave latency for fastest response
            .timeout = 0xC8   // 200 * 10ms = 2 seconds
        };
        memcpy(conn_params.bda, bda, sizeof(esp_bd_addr_t));
        esp_err_t update_ret = esp_ble_gap_update_conn_params(&conn_params);
        if (update_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to request connection parameter update: %s", esp_err_to_name(update_ret));
        } else {
            ESP_LOGI(TAG, "Requested faster connection parameters (7.5-20ms interval)");
        }


        // ESP-IDF's built-in GATT cache handles service caching automatically
        // with CONFIG_BT_GATTC_CACHE_NVS_FLASH=y enabled

        // Start NUS and DIS service discovery after a short delay to prioritize HID readiness
        if (!s_service_discovery_timer) {
            esp_timer_create_args_t args = {
                .callback = &service_discovery_timer_callback,
                .arg = NULL,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "service_discovery"
            };
            ESP_ERROR_CHECK(esp_timer_create(&args, &s_service_discovery_timer));
        }
        ESP_ERROR_CHECK(esp_timer_start_once(s_service_discovery_timer, 50 * 1000)); // 50ms minimal delay
    }
    ble_bas_reset();
    leds_set_state(LED_STATE_CONNECTED);

    // Notify relay protocol of BLE connection
    relay_protocol_update_ble_connection(true);
}

static void hid_disconnected_cb(esp_hidh_dev_t *dev)
{
    (void)dev;
    ESP_LOGI(TAG, "Device disconnected");
    stop_rssi_timer();
    s_has_active_addr = false;

    // Handle disconnect and release any stuck HID inputs
    transport_hid_handle_disconnect();

    // Reset advertisement data
    s_active_appearance = 0;
    s_active_manufacturer_len = 0;
    memset(s_active_manufacturer_data, 0, sizeof(s_active_manufacturer_data));

    ble_bas_reset();
    leds_set_state(LED_STATE_SCANNING);

    // Notify relay protocol of BLE disconnection
    relay_protocol_update_ble_connection(false);

    start_scan_task();
}

static void hid_input_cb(uint8_t report_id, const uint8_t *data, uint16_t length)
{
    // Forward to transport bridge for USB forwarding
    transport_hid_handle_input(report_id, data, length);
}

static void hid_battery_cb(uint8_t level, esp_err_t status)
{
    if (status == ESP_OK) {
        ble_bas_handle_level(level);
    } else {
        ESP_LOGW(TAG, "Battery event error: %d", status);
    }
}

static void hidh_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    // Forward to BLE HID client for processing
    ble_hid_client_handle_event(handler_args, base, id, event_data);
}

static ble_central_scan_result_t *choose_best_result(ble_central_scan_result_t *results)
{
    ble_central_scan_result_t *best = NULL;
    ble_central_scan_result_t *bonded_device = NULL;
    ble_central_scan_result_t *unbonded_mouthpad = NULL;
    int bonded_rssi = -128;
    int unbonded_rssi = -128;

    bool has_bonded_device = ble_bonds_has_bonded_device();

    for (ble_central_scan_result_t *r = results; r != NULL; r = r->next) {
        if (r->name) {
            ESP_LOGI(TAG, "Found %s device RSSI=%d name=%s",
                     r->transport == ESP_HID_TRANSPORT_BLE ? "BLE" : "BT",
                     r->rssi,
                     r->name);
        }

        // Filter for devices with both HID and NUS services (MouthPad devices)
        if (r->transport == ESP_HID_TRANSPORT_BLE) {
            bool has_both_services = r->ble.has_nus_uuid;  // NUS UUID indicates MouthPad device

            if (has_both_services) {
                bool is_bonded_device = ble_bonds_is_bonded_device(r->bda);

                if (has_bonded_device) {
                    // We have a bonded device - only connect to it
                    if (is_bonded_device) {
                        if (r->rssi > bonded_rssi) {
                            bonded_device = r;
                            bonded_rssi = r->rssi;
                            ESP_LOGI(TAG, "Found BONDED MouthPad device: %s (RSSI=%d, appearance=0x%04X)",
                                    r->name ? r->name : "(no name)", r->rssi, r->ble.appearance);
                        }
                    } else {
                        ESP_LOGD(TAG, "Skipping unbonded MouthPad device: %s (RSSI=%d) - we have a bond",
                                r->name ? r->name : "(no name)", r->rssi);
                    }
                } else {
                    // No bonded device yet - can connect to any MouthPad device for pairing
                    if (r->rssi > unbonded_rssi) {
                        unbonded_mouthpad = r;
                        unbonded_rssi = r->rssi;
                        ESP_LOGI(TAG, "Found unbonded MouthPad device for pairing: %s (RSSI=%d, appearance=0x%04X)",
                                r->name ? r->name : "(no name)", r->rssi, r->ble.appearance);
                    }
                }
            } else {
                ESP_LOGD(TAG, "Skipping device: %s (appearance=0x%04X, no NUS UUID)",
                        r->name ? r->name : "(no name)", r->ble.appearance);
            }
        }
    }

    // Choose the best device based on bonding state
    if (has_bonded_device && bonded_device) {
        best = bonded_device;
        ESP_LOGI(TAG, "Selected bonded device for connection");
    } else if (!has_bonded_device && unbonded_mouthpad) {
        best = unbonded_mouthpad;
        ESP_LOGI(TAG, "Selected unbonded device for pairing");
    }

    if (best == NULL && results != NULL) {
        // Count the number of devices for logging
        int count = 0;
        for (ble_central_scan_result_t *r = results; r != NULL; r = r->next) {
            count++;
        }
        if (count > 0) {
            if (has_bonded_device) {
                ESP_LOGI(TAG, "Bonded device not found among %d HID device(s) (waiting for bonded device)", count);
            } else {
                ESP_LOGI(TAG, "No suitable devices found among %d HID device(s) (need both HID and NUS UUIDs)", count);
            }
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

        // Check bonding status for informative scanning logs
        bool has_bond = ble_bonds_has_bonded_device();

        // Print scanning message for each scan cycle
        if (has_bond) {
            char bond_info[32];
            ble_bonds_get_info_string(bond_info, sizeof(bond_info));
            ESP_LOGI(TAG, "Scanning for MouthPad (%s)...", bond_info);
        } else {
            ESP_LOGI(TAG, "Scanning for MouthPad...");
        }

        // Use minimum scan window (1 second) - API doesn't support sub-second scans
        ble_central_scan(1, &results_len, &results);

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

            // ESP-IDF now handles GATT caching automatically with CONFIG_BT_GATTC_CACHE_NVS_FLASH=y

            esp_hidh_dev_t *dev = esp_hidh_dev_open(target->bda, target->transport, target->ble.addr_type);
            if (!dev) {
                ESP_LOGW(TAG, "Failed to initiate connection, continuing scan");
            } else {
                ESP_LOGI(TAG, "Connection requested");
                ble_central_scan_results_free(results);
                break;
            }
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

    // Notify relay protocol that scanning has started
    relay_protocol_update_ble_scanning(true);

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
            ESP_LOGI(TAG, "Button long press detected - clearing all bonds");

            char bond_info[32];
            ble_bonds_get_info_string(bond_info, sizeof(bond_info));
            ESP_LOGI(TAG, "Clearing bond with: %s", bond_info);

            // Disconnect the currently active device first using BLE GAP disconnect
            if (s_has_active_addr) {
                ESP_LOGI(TAG, "Disconnecting active device before clearing bonds");
                esp_err_t disconnect_ret = esp_ble_gap_disconnect(s_active_addr);
                if (disconnect_ret != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to disconnect device: %s", esp_err_to_name(disconnect_ret));
                    // Fallback to HID close if GAP disconnect fails
                    esp_hidh_dev_t *active_dev = ble_hid_client_get_active_device();
                    if (active_dev != NULL) {
                        esp_hidh_dev_close(active_dev);
                    }
                }
            }

            esp_err_t ret = ble_bonds_clear_all();
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "All bonds cleared successfully");
                leds_set_state(LED_STATE_SCANNING);  // Visual feedback that bonds were cleared
            } else {
                ESP_LOGW(TAG, "Failed to clear bonds: %s", esp_err_to_name(ret));
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

    ESP_LOGI(TAG, "Initializing MouthPad^USB");

    // Initialize bonding system early (requires NVS)
    ESP_ERROR_CHECK(ble_bonds_init());

    // ESP-IDF's built-in GATT cache is enabled via CONFIG_BT_GATTC_CACHE_NVS_FLASH=y

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
    usb_dfu_init();

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

    // Initialize HID transport bridge and BLE HID client
    ESP_LOGI(TAG, "Initializing HID transport bridge");
    ESP_ERROR_CHECK(transport_hid_init());
    ESP_ERROR_CHECK(transport_hid_start());

    ble_hid_client_config_t hid_config = {
        .connected_cb = hid_connected_cb,
        .disconnected_cb = hid_disconnected_cb,
        .input_cb = hid_input_cb,
        .feature_cb = NULL,
        .battery_cb = hid_battery_cb,
    };
    ESP_LOGI(TAG, "Initializing BLE HID client");
    ESP_ERROR_CHECK(ble_hid_client_init(&hid_config));

#if ENABLE_NUS_CLIENT_MODE
    // Register a separate GATT app for NUS (app_id 1)
    // The ESP HID library has already registered app_id 0
    ESP_LOGI(TAG, "Registering separate NUS GATT client app (app_id=1)");
    ESP_ERROR_CHECK(esp_ble_gattc_app_register(1));

    // Initialize NUS client to CDC bridge
    ESP_LOGI(TAG, "Initializing NUS client to CDC bridge");
    ESP_ERROR_CHECK(transport_uart_init());
    ESP_ERROR_CHECK(transport_uart_start());
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
