#include "usb_hid.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "tinyusb.h"
#include "class/hid/hid_device.h"

#include "usb_cdc.h"

static const char *TAG = "usb_hid";

#define CDC_ITF_NUM_COMM      0
#define CDC_ITF_NUM_DATA      1
#define HID_INTERFACE_NUMBER  2
#define HID_INSTANCE          0
#define ITF_NUM_TOTAL         3

#define EPNUM_CDC_NOTIF       0x83
#define EPNUM_CDC_OUT         0x02
#define EPNUM_CDC_IN          0x82
#define HID_EP_IN             0x81

#define HID_EP_SIZE           16
#define HID_POLL_INTERVAL_MS  1

#define CONFIG_TOTAL_LEN      (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN)

static const uint8_t mouthpad_report_desc[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x01, 0x09, 0x01, 0xA1, 0x00,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00,
    0x25, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x03, 0x81, 0x01, 0x75, 0x08,
    0x95, 0x01, 0x05, 0x01, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x81, 0x06,
    0x05, 0x0C, 0x0A, 0x38, 0x02, 0x95, 0x01, 0x81, 0x06, 0xC0, 0x85, 0x02,
    0x09, 0x01, 0xA1, 0x00, 0x75, 0x0C, 0x95, 0x02, 0x05, 0x01, 0x09, 0x30,
    0x09, 0x31, 0x16, 0x01, 0xF8, 0x26, 0xFF, 0x07, 0x81, 0x06, 0xC0, 0xC0,
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x03, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x01, 0x09, 0xCD, 0x81, 0x06, 0x0A, 0x83, 0x01, 0x81,
    0x06, 0x09, 0xB5, 0x81, 0x06, 0x09, 0xB6, 0x81, 0x06, 0x09, 0xEA, 0x81,
    0x06, 0x09, 0xE9, 0x81, 0x06, 0x0A, 0x25, 0x02, 0x81, 0x06, 0x0A, 0x24,
    0x02, 0x81, 0x06, 0xC0
};

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CDC,
    STRID_HID,
};

static char serial_str[2 * 6 + 1];
static const char *string_desc[] = {
    (const char[]){0x09, 0x04},
    "Augmental Tech",
    "MouthPad^USB",
    serial_str,
    "MouthPad CDC",
    "MouthPad HID"
};

static const tusb_desc_device_t mouthpad_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x1915,
    .idProduct = 0xEEEE,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static const uint8_t mouthpad_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(CDC_ITF_NUM_COMM, STRID_CDC, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    TUD_HID_DESCRIPTOR(HID_INTERFACE_NUMBER, STRID_HID, false, sizeof(mouthpad_report_desc), HID_EP_IN, HID_EP_SIZE, HID_POLL_INTERVAL_MS),
};

static bool s_usb_ready;

void usb_hid_init(void)
{
    uint8_t mac[6] = {0};
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
    snprintf(serial_str, sizeof(serial_str), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = &mouthpad_device_descriptor,
        .external_phy = false,
#if (TUD_OPT_HIGH_SPEED)
        .fs_configuration_descriptor = mouthpad_configuration_descriptor,
        .hs_configuration_descriptor = mouthpad_configuration_descriptor,
        .qualifier_descriptor = NULL,
#else
        .configuration_descriptor = mouthpad_configuration_descriptor,
#endif
        .string_descriptor = string_desc,
        .string_descriptor_count = sizeof(string_desc) / sizeof(string_desc[0]),
    };

    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_ERROR_CHECK(usb_cdc_init());
}

bool usb_hid_ready(void)
{
    return s_usb_ready && tud_hid_ready();
}

void usb_hid_send_report(uint8_t report_id, const uint8_t *data, size_t len)
{
    if (!usb_hid_ready()) {
        return;
    }
    if (!tud_hid_n_report(HID_INSTANCE, report_id, data, (uint8_t)len)) {
        ESP_LOGW(TAG, "Failed to send HID report id %u len %u", report_id, (unsigned)len);
    }
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return mouthpad_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

void tud_mount_cb(void)
{
    s_usb_ready = true;
    ESP_LOGI(TAG, "USB mounted");
}

void tud_umount_cb(void)
{
    s_usb_ready = false;
    ESP_LOGI(TAG, "USB unmounted");
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    s_usb_ready = false;
}

void tud_resume_cb(void)
{
    s_usb_ready = true;
}
