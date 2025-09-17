#include "usb_cdc.h"

#include "esp_log.h"
#include "tusb_cdc_acm.h"
#include "tusb_console.h"
#include "esp_check.h"

#include <ctype.h>
#include <string.h>

#include "bootloader_trigger.h"

static const char *TAG = "usb_cdc";

#define CDC_CMD_BUF_LEN 16

static char s_cmd_buf[CDC_CMD_BUF_LEN];
static size_t s_cmd_len;

static void reset_cmd_buffer(void)
{
    s_cmd_len = 0;
    s_cmd_buf[0] = '\0';
}

static void process_command_buffer(void)
{
    size_t len = s_cmd_len;
    if (len == 0) {
        return;
    }

    while (len > 0 && isspace((unsigned char)s_cmd_buf[len - 1])) {
        --len;
    }
    size_t start = 0;
    while (start < len && isspace((unsigned char)s_cmd_buf[start])) {
        ++start;
    }

    if (len - start == 3 && strncmp(&s_cmd_buf[start], "dfu", 3) == 0) {
        if (!bootloader_trigger_pending()) {
            bootloader_trigger_enter_dfu();
        }
    }

    reset_cmd_buffer();
}

static void handle_cdc_rx(int itf, cdcacm_event_t *event)
{
    (void)itf;

    if (!event || event->type != CDC_EVENT_RX || bootloader_trigger_pending()) {
        return;
    }

    uint8_t buf[32];
    size_t rx = 0;

    while (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, buf, sizeof(buf), &rx) == ESP_OK && rx > 0) {
        for (size_t i = 0; i < rx; ++i) {
            char ch = (char)buf[i];

            if (ch == '\r') {
                continue;
            }

            if (ch == '\n') {
                process_command_buffer();
                continue;
            }

            if (s_cmd_len < (CDC_CMD_BUF_LEN - 1)) {
                s_cmd_buf[s_cmd_len++] = (char)tolower((unsigned char)ch);
                s_cmd_buf[s_cmd_len] = '\0';
            } else {
                reset_cmd_buffer();
            }
        }

        if (rx < sizeof(buf)) {
            break;
        }
    }
}

esp_err_t usb_cdc_init(void)
{
    const tinyusb_config_cdcacm_t acm_cfg = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = handle_cdc_rx,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };

    ESP_RETURN_ON_ERROR(tusb_cdc_acm_init(&acm_cfg), TAG, "CDC ACM init failed");
    ESP_RETURN_ON_ERROR(esp_tusb_init_console(TINYUSB_CDC_ACM_0), TAG, "USB console init failed");
    ESP_LOGI(TAG, "CDC ACM console ready");
    return ESP_OK;
}
