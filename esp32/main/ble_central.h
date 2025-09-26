/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#define HIDH_IDLE_MODE 0x00
#define HIDH_BLE_MODE 0x01
#define HIDH_BT_MODE 0x02
#define HIDH_BTDM_MODE 0x03

#if CONFIG_BT_HID_HOST_ENABLED
#if CONFIG_BT_BLE_ENABLED
#define HID_HOST_MODE HIDH_BTDM_MODE
#else
#define HID_HOST_MODE HIDH_BT_MODE
#endif
#elif CONFIG_BT_BLE_ENABLED
#define HID_HOST_MODE HIDH_BLE_MODE
#elif CONFIG_BT_NIMBLE_ENABLED
#define HID_HOST_MODE HIDH_BLE_MODE
#else
#define HID_HOST_MODE HIDH_IDLE_MODE
#endif

#include "esp_err.h"
#include "esp_log.h"

#include "esp_bt.h"
#if !CONFIG_BT_NIMBLE_ENABLED
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#endif
#include "esp_hid_common.h"
#if CONFIG_BT_BLE_ENABLED
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_gap_ble_api.h"
#endif

#if CONFIG_BT_NIMBLE_ENABLED
#include "nimble/ble.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ble_central_scan_result_s {
    struct ble_central_scan_result_s *next;
#if CONFIG_BT_NIMBLE_ENABLED
    uint8_t bda[6];
#else
    esp_bd_addr_t bda;
#endif

    const char *name;
    int8_t rssi;
    esp_hid_usage_t usage;
    esp_hid_transport_t transport;
    union {
    #if !CONFIG_BT_NIMBLE_ENABLED
        struct {
            esp_bt_cod_t cod;
            esp_bt_uuid_t uuid;
        } bt;
        struct {
            esp_ble_addr_type_t addr_type;
            uint16_t appearance;
        } ble;
    #else
        struct {
            uint8_t addr_type;
            uint16_t appearance;
        } ble;
    #endif
    };
} ble_central_scan_result_t;

esp_err_t ble_central_init(uint8_t mode);
esp_err_t ble_central_scan(uint32_t seconds, size_t *num_results, ble_central_scan_result_t **results);
void ble_central_scan_results_free(ble_central_scan_result_t *results);

void ble_central_set_user_ble_callback(esp_gap_ble_cb_t cb);

esp_err_t ble_central_adv_init(uint16_t appearance, const char *device_name);
esp_err_t ble_central_adv_start(void);

#if !CONFIG_BT_NIMBLE_ENABLED
void ble_central_print_uuid(esp_bt_uuid_t *uuid);
const char *ble_central_addr_type_str(esp_ble_addr_type_t ble_addr_type);
#endif

#ifdef __cplusplus
}
#endif
