#include "usb_cdc.h"

#include "esp_log.h"
#include "tusb_cdc_acm.h"
#include "tusb_console.h"
#include "esp_check.h"

static const char *TAG = "usb_cdc";

esp_err_t usb_cdc_init(void)
{
    const tinyusb_config_cdcacm_t acm_cfg = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = NULL,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };

    ESP_RETURN_ON_ERROR(tusb_cdc_acm_init(&acm_cfg), TAG, "CDC ACM init failed");
    ESP_RETURN_ON_ERROR(esp_tusb_init_console(TINYUSB_CDC_ACM_0), TAG, "USB console init failed");
    ESP_LOGI(TAG, "CDC ACM console ready");
    return ESP_OK;
}
