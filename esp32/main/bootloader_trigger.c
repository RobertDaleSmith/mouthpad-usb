#include "bootloader_trigger.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"

#include "esp32s3/rom/usb/chip_usb_dw_wrapper.h"
#include "esp32s3/rom/usb/usb_persist.h"

static const char *TAG = "bootloader_trigger";

static bool s_pending;

void bootloader_trigger_init(void)
{
    s_pending = false;
}

void bootloader_trigger_enter_dfu(void)
{
    if (s_pending) {
        return;
    }

    s_pending = true;
    ESP_LOGI(TAG, "USB host requested DFU reboot");

    /* Allow the host to observe the response before we reboot. */
    vTaskDelay(pdMS_TO_TICKS(20));

    chip_usb_set_persist_flags(USBDC_BOOT_DFU);
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);

    esp_restart();

    while (true) {
        vTaskDelay(portMAX_DELAY);
    }
}

bool bootloader_trigger_pending(void)
{
    return s_pending;
}

