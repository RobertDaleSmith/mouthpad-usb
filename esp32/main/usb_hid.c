#include "usb_hid.h"

#include "class/hid/hid_device.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "tinyusb.h"
#include <stdio.h>
#include <string.h>

#include "usb_cdc.h"

static const char *TAG = "USB_HID";

#define CDC0_ITF_NUM_COMM 0
#define CDC0_ITF_NUM_DATA 1
#define CDC1_ITF_NUM_COMM 2
#define CDC1_ITF_NUM_DATA 3
#define HID_INTERFACE_NUMBER 4
#define HID_INSTANCE 0
#define ITF_NUM_TOTAL 5

#define EPNUM_CDC0_NOTIF 0x81
#define EPNUM_CDC0_OUT 0x02
#define EPNUM_CDC0_IN 0x82
#define EPNUM_CDC1_OUT 0x04
#define EPNUM_CDC1_IN 0x83
#define HID_EP_IN 0x84

#define HID_EP_SIZE 16
#define HID_POLL_INTERVAL_MS 1

#define CDC_DESC_LEN_NO_NOTIF (TUD_CDC_DESC_LEN - 7)
#define CONFIG_TOTAL_LEN                                                       \
  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + CDC_DESC_LEN_NO_NOTIF +            \
   TUD_HID_DESC_LEN)

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
    0x02, 0x81, 0x06, 0xC0};

enum {
  STRID_LANGID = 0,
  STRID_MANUFACTURER,
  STRID_PRODUCT,
  STRID_SERIAL,
  STRID_CDC0,
  STRID_CDC1,
  STRID_HID,
};

static char serial_str[2 * 6 + 1];
static const char *string_desc[] = {
  (const char[]){0x09, 0x04},
  "Augmental Tech",
  "MouthPad^USB",
  serial_str,
  "MouthPad^NUS",
  "MouthPad^CDC",
  "MouthPad^HID"
};

#define TUD_CDC_DESCRIPTOR_NO_NOTIF(_itfnum, _stridx, _epout, _epin, _epsize)  \
  /* Interface Association */                                                  \
  8, TUSB_DESC_INTERFACE_ASSOCIATION, _itfnum, 2, TUSB_CLASS_CDC,              \
      CDC_COMM_SUBCLASS_ABSTRACT_CONTROL_MODEL, CDC_COMM_PROTOCOL_NONE,        \
      0, /* CDC Control Interface */                                           \
      9, TUSB_DESC_INTERFACE, _itfnum, 0, 0, TUSB_CLASS_CDC,                   \
      CDC_COMM_SUBCLASS_ABSTRACT_CONTROL_MODEL, CDC_COMM_PROTOCOL_NONE,        \
      _stridx, /* CDC Header */                                                \
      5, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_HEADER,                         \
      U16_TO_U8S_LE(0x0120), /* CDC Call */                                    \
      5, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_CALL_MANAGEMENT, 0,             \
      (uint8_t)((_itfnum) + 1), /* CDC ACM */                                  \
      4, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_ABSTRACT_CONTROL_MANAGEMENT,    \
      6, /* CDC Union */                                                       \
      5, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_UNION, _itfnum,                 \
      (uint8_t)((_itfnum) + 1), /* CDC Data Interface */                       \
      9, TUSB_DESC_INTERFACE, (uint8_t)((_itfnum) + 1), 0, 2,                  \
      TUSB_CLASS_CDC_DATA, 0, 0, 0, /* Endpoint Out */                         \
      7, TUSB_DESC_ENDPOINT, _epout, TUSB_XFER_BULK, U16_TO_U8S_LE(_epsize),   \
      0, /* Endpoint In */                                                     \
      7, TUSB_DESC_ENDPOINT, _epin, TUSB_XFER_BULK, U16_TO_U8S_LE(_epsize), 0

static const tusb_desc_device_t mouthpad_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
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
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(CDC0_ITF_NUM_COMM, STRID_CDC0, EPNUM_CDC0_NOTIF, 8,
                       EPNUM_CDC0_OUT, EPNUM_CDC0_IN, 64),
    TUD_CDC_DESCRIPTOR_NO_NOTIF(CDC1_ITF_NUM_COMM, STRID_CDC1, EPNUM_CDC1_OUT,
                                EPNUM_CDC1_IN, 64),
    TUD_HID_DESCRIPTOR(HID_INTERFACE_NUMBER, STRID_HID, false,
                       sizeof(mouthpad_report_desc), HID_EP_IN, HID_EP_SIZE,
                       HID_POLL_INTERVAL_MS),
};

_Static_assert(sizeof(mouthpad_configuration_descriptor) == CONFIG_TOTAL_LEN,
               "Descriptor length mismatch");

static bool s_usb_ready;

void usb_hid_init(void) {
  uint8_t mac[6] = {0};
  ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
  snprintf(serial_str, sizeof(serial_str), "%02X%02X%02X%02X%02X%02X", mac[0],
           mac[1], mac[2], mac[3], mac[4], mac[5]);

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

  // Initialize CDC with minimal configuration for HID mode
  usb_cdc_config_t cdc_config = {
      .data_received_cb = NULL,
      .data_sent_cb = NULL,
  };
  ESP_ERROR_CHECK(usb_cdc_init(&cdc_config));
}

bool usb_hid_ready(void) { return s_usb_ready && tud_hid_ready(); }

void usb_hid_send_report(uint8_t report_id, const uint8_t *data, size_t len) {
  if (!usb_hid_ready()) {
    return;
  }
  if (!tud_hid_n_report(HID_INSTANCE, report_id, data, (uint8_t)len)) {
    ESP_LOGW(TAG, "Failed to send HID report id %u len %u", report_id,
             (unsigned)len);
  }
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
  (void)instance;
  return mouthpad_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen) {
  (void)instance;
  (void)report_id;
  (void)report_type;
  (void)buffer;
  (void)reqlen;
  return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize) {
  (void)instance;
  (void)report_id;
  (void)report_type;
  (void)buffer;
  (void)bufsize;
}

void tud_mount_cb(void) {
  s_usb_ready = true;
  ESP_LOGI(TAG, "USB mounted");
}

void tud_umount_cb(void) {
  s_usb_ready = false;
  ESP_LOGI(TAG, "USB unmounted");
}

void tud_suspend_cb(bool remote_wakeup_en) {
  (void)remote_wakeup_en;
  s_usb_ready = false;
}

void tud_resume_cb(void) { s_usb_ready = true; }
