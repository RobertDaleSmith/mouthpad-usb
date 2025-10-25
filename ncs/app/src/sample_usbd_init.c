/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/bos.h>
#include <zephyr/usb/class/usb_cdc.h>
#include <zephyr/usb/usbd.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(usbd_sample_config);

#define ZEPHYR_PROJECT_USB_VID 0x1915 /* Augmental Tech VID */

/* By default, do not register the USB DFU class DFU mode instance. */
static const char *const blocklist[] = {
    "dfu_dfu",
    NULL,
};

/* doc device instantiation start */
/*
 * Instantiate a context named sample_usbd using the default USB device
 * controller, the Zephyr project vendor ID, and the sample product ID.
 * Zephyr project vendor ID must not be used outside of Zephyr samples.
 */
USBD_DEVICE_DEFINE(sample_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
                   ZEPHYR_PROJECT_USB_VID, CONFIG_SAMPLE_USBD_PID);
/* doc device instantiation end */

/* doc string instantiation start */
USBD_DESC_LANG_DEFINE(sample_lang);
USBD_DESC_MANUFACTURER_DEFINE(sample_mfr, CONFIG_SAMPLE_USBD_MANUFACTURER);
USBD_DESC_PRODUCT_DEFINE(sample_product, CONFIG_SAMPLE_USBD_PRODUCT);

static char serial_number_str[32];

/* macOS builds tty names from the first 12 hex chars of the serial; keep parity
 * with ESP32 */
#define SERIAL_ID_MAX_BYTES 6U

static const char cdc0_label[] = "MouthPad^NUS";
static const char cdc1_label[] = "MouthPad^CDC";

#if IS_ENABLED(CONFIG_USBD_CDC_ACM_CLASS)
struct usbd_cdc_acm_desc {
  struct usb_association_descriptor iad;
  struct usb_if_descriptor if0;
  struct cdc_header_descriptor if0_header;
  struct cdc_cm_descriptor if0_cm;
  struct cdc_acm_descriptor if0_acm;
  struct cdc_union_descriptor if0_union;
  struct usb_ep_descriptor if0_int_ep;
#if USBD_SUPPORTS_HIGH_SPEED
  struct usb_ep_descriptor if0_hs_int_ep;
#endif
  struct usb_if_descriptor if1;
  struct usb_ep_descriptor if1_in_ep;
  struct usb_ep_descriptor if1_out_ep;
#if USBD_SUPPORTS_HIGH_SPEED
  struct usb_ep_descriptor if1_hs_in_ep;
  struct usb_ep_descriptor if1_hs_out_ep;
#endif
  struct usb_desc_header nil_desc;
};

struct cdc_acm_uart_config {
  struct usbd_class_data *c_data;
  struct usbd_desc_node *const if_desc_data;
  struct usbd_cdc_acm_desc *const desc;
  const struct usb_desc_header **const fs_desc;
  const struct usb_desc_header **const hs_desc;
};
#endif /* CONFIG_USBD_CDC_ACM_CLASS */

static struct usbd_desc_node sample_sn = {
    .str =
        {
            .utype = USBD_DUT_STRING_SERIAL_NUMBER,
            .ascii7 = true,
        },
    .ptr = serial_number_str,
    .bLength = 0,
    .bDescriptorType = USB_DESC_STRING,
};

/* CDC interface string descriptors - will point to serial ID string to
 * align macOS port naming with ESP32 TinyUSB behaviour.
 */

static struct usbd_desc_node cdc0_if_desc = {
    .str =
        {
            .utype = USBD_DUT_STRING_INTERFACE,
            .ascii7 = true,
        },
    .ptr = cdc0_label,
    .bLength = USB_STRING_DESCRIPTOR_LENGTH(cdc0_label),
    .bDescriptorType = USB_DESC_STRING,
};

static struct usbd_desc_node cdc1_if_desc = {
    .str =
        {
            .utype = USBD_DUT_STRING_INTERFACE,
            .ascii7 = true,
        },
    .ptr = cdc1_label,
    .bLength = USB_STRING_DESCRIPTOR_LENGTH(cdc1_label),
    .bDescriptorType = USB_DESC_STRING,
};

/* doc string instantiation end */

USBD_DESC_CONFIG_DEFINE(fs_cfg_desc, "FS Configuration");
USBD_DESC_CONFIG_DEFINE(hs_cfg_desc, "HS Configuration");

/* doc configuration instantiation start */
static const uint8_t attributes =
    (IS_ENABLED(CONFIG_SAMPLE_USBD_SELF_POWERED) ? USB_SCD_SELF_POWERED : 0) |
    (IS_ENABLED(CONFIG_SAMPLE_USBD_REMOTE_WAKEUP) ? USB_SCD_REMOTE_WAKEUP : 0);

/* Full speed configuration */
USBD_CONFIGURATION_DEFINE(sample_fs_config, attributes,
                          CONFIG_SAMPLE_USBD_MAX_POWER, &fs_cfg_desc);

/* High speed configuration */
USBD_CONFIGURATION_DEFINE(sample_hs_config, attributes,
                          CONFIG_SAMPLE_USBD_MAX_POWER, &hs_cfg_desc);
/* doc configuration instantiation end */

#if CONFIG_SAMPLE_USBD_20_EXTENSION_DESC
/*
 * This does not yet provide valuable information, but rather serves as an
 * example, and will be improved in the future.
 */
static const struct usb_bos_capability_lpm bos_cap_lpm = {
    .bLength = sizeof(struct usb_bos_capability_lpm),
    .bDescriptorType = USB_DESC_DEVICE_CAPABILITY,
    .bDevCapabilityType = USB_BOS_CAPABILITY_EXTENSION,
    .bmAttributes = 0UL,
};

USBD_DESC_BOS_DEFINE(sample_usbext, sizeof(bos_cap_lpm), &bos_cap_lpm);
#endif

static void sample_fix_code_triple(struct usbd_context *uds_ctx,
                                   const enum usbd_speed speed) {
  /* Always use class code information from Interface Descriptors */
  if (IS_ENABLED(CONFIG_USBD_CDC_ACM_CLASS) ||
      IS_ENABLED(CONFIG_USBD_CDC_ECM_CLASS) ||
      IS_ENABLED(CONFIG_USBD_CDC_NCM_CLASS) ||
      IS_ENABLED(CONFIG_USBD_MIDI2_CLASS) ||
      IS_ENABLED(CONFIG_USBD_AUDIO2_CLASS)) {
    /*
     * Class with multiple interfaces have an Interface
     * Association Descriptor available, use an appropriate triple
     * to indicate it.
     */
    usbd_device_set_code_triple(uds_ctx, speed, USB_BCC_MISCELLANEOUS, 0x02,
                                0x01);
  } else {
    usbd_device_set_code_triple(uds_ctx, speed, 0, 0, 0);
  }
}

#if IS_ENABLED(CONFIG_USBD_CDC_ACM_CLASS)
static void cdc_assign_interface_string(const struct device *dev,
                                        struct usbd_desc_node *desc_nd) {
  if ((dev == NULL) || (desc_nd == NULL)) {
    return;
  }

  const struct cdc_acm_uart_config *cfg = dev->config;
  if ((cfg == NULL) || (cfg->desc == NULL)) {
    return;
  }

  uint8_t idx = usbd_str_desc_get_idx(desc_nd);
  if (idx == 0U) {
    return;
  }

  struct usbd_cdc_acm_desc *desc = cfg->desc;
  desc->if0.iInterface = idx;
  desc->if1.iInterface = idx;
}
#endif /* CONFIG_USBD_CDC_ACM_CLASS */

struct usbd_context *sample_usbd_setup_device(usbd_msg_cb_t msg_cb) {
  int err;

  /* Populate serial descriptor string for device identification */
  memset(serial_number_str, 0, sizeof(serial_number_str));
  uint8_t hwid[8];
  ssize_t hwid_len = hwinfo_get_device_id(hwid, sizeof(hwid));
  if (hwid_len > 0) {
    size_t max_bytes = MIN((size_t)hwid_len, SERIAL_ID_MAX_BYTES);
    if (max_bytes == 0U) {
      LOG_WRN("Serial buffer too small, falling back to default string");
    } else {
      size_t offset = 0U;

      /* Copy first six HWID bytes so the serial matches ESP32 TinyUSB behaviour
       */
      for (size_t i = 0U;
           i < max_bytes && offset < (sizeof(serial_number_str) - 1U); i++) {
        int rc = snprintf(&serial_number_str[offset],
                          sizeof(serial_number_str) - offset, "%02X", hwid[i]);
        if (rc <= 0) {
          break;
        }
        offset += (size_t)rc;
      }
    }
  }

  if (serial_number_str[0] == '\0') {
    LOG_WRN("Failed to build serial string from HWID, using default");
    snprintf(serial_number_str, sizeof(serial_number_str), "000000000000");
  }

  sample_sn.bLength = (uint8_t)(2U * strlen(serial_number_str) + 2U);

  LOG_INF("Serial descriptor string: %s", serial_number_str);
  LOG_INF("CDC0 interface string: %s", cdc0_label);
  LOG_INF("CDC1 interface string: %s", cdc1_label);

  /* doc add string descriptor start */
  err = usbd_add_descriptor(&sample_usbd, &sample_lang);
  if (err) {
    LOG_ERR("Failed to initialize language descriptor (%d)", err);
    return NULL;
  }

  err = usbd_add_descriptor(&sample_usbd, &sample_mfr);
  if (err) {
    LOG_ERR("Failed to initialize manufacturer descriptor (%d)", err);
    return NULL;
  }

  err = usbd_add_descriptor(&sample_usbd, &sample_product);
  if (err) {
    LOG_ERR("Failed to initialize product descriptor (%d)", err);
    return NULL;
  }

  err = usbd_add_descriptor(&sample_usbd, &sample_sn);
  if (err) {
    LOG_ERR("Failed to initialize SN descriptor (%d)", err);
    return NULL;
  }

  /* Add CDC interface string descriptors */
  err = usbd_add_descriptor(&sample_usbd, &cdc0_if_desc);
  if (err) {
    LOG_ERR("Failed to add CDC0 interface descriptor (%d)", err);
    return NULL;
  }
#if IS_ENABLED(CONFIG_USBD_CDC_ACM_CLASS)
#if DT_NODE_EXISTS(DT_NODELABEL(cdc_acm_uart0))
  cdc_assign_interface_string(DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0)),
                              &cdc0_if_desc);
#endif
#endif

  err = usbd_add_descriptor(&sample_usbd, &cdc1_if_desc);
  if (err) {
    LOG_ERR("Failed to add CDC1 interface descriptor (%d)", err);
    return NULL;
  }
#if IS_ENABLED(CONFIG_USBD_CDC_ACM_CLASS)
#if DT_NODE_EXISTS(DT_NODELABEL(cdc_acm_uart1))
  cdc_assign_interface_string(DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart1)),
                              &cdc1_if_desc);
#endif
#endif
  /* doc add string descriptor end */

  if (USBD_SUPPORTS_HIGH_SPEED &&
      usbd_caps_speed(&sample_usbd) == USBD_SPEED_HS) {
    err =
        usbd_add_configuration(&sample_usbd, USBD_SPEED_HS, &sample_hs_config);
    if (err) {
      LOG_ERR("Failed to add High-Speed configuration");
      return NULL;
    }

    err = usbd_register_all_classes(&sample_usbd, USBD_SPEED_HS, 1, blocklist);
    if (err) {
      LOG_ERR("Failed to add register classes");
      return NULL;
    }

    sample_fix_code_triple(&sample_usbd, USBD_SPEED_HS);
  }

  /* doc configuration register start */
  err = usbd_add_configuration(&sample_usbd, USBD_SPEED_FS, &sample_fs_config);
  if (err) {
    LOG_ERR("Failed to add Full-Speed configuration");
    return NULL;
  }
  /* doc configuration register end */

  /* doc functions register start */
  err = usbd_register_all_classes(&sample_usbd, USBD_SPEED_FS, 1, blocklist);
  if (err) {
    LOG_ERR("Failed to add register classes");
    return NULL;
  }
  /* doc functions register end */

  sample_fix_code_triple(&sample_usbd, USBD_SPEED_FS);
  usbd_self_powered(&sample_usbd, attributes & USB_SCD_SELF_POWERED);

  /* Align bcdDevice with ESP32 firmware (version 0.1.0) */
  err = usbd_device_set_bcd_device(&sample_usbd, 0x0010);
  if (err) {
    LOG_WRN("Failed to set bcdDevice (%d)", err);
  }

  if (msg_cb != NULL) {
    /* doc device init-and-msg start */
    err = usbd_msg_register_cb(&sample_usbd, msg_cb);
    if (err) {
      LOG_ERR("Failed to register message callback");
      return NULL;
    }
    /* doc device init-and-msg end */
  }

#if CONFIG_SAMPLE_USBD_20_EXTENSION_DESC
  (void)usbd_device_set_bcd_usb(&sample_usbd, USBD_SPEED_FS, 0x0201);
  (void)usbd_device_set_bcd_usb(&sample_usbd, USBD_SPEED_HS, 0x0201);

  err = usbd_add_descriptor(&sample_usbd, &sample_usbext);
  if (err) {
    LOG_ERR("Failed to add USB 2.0 Extension Descriptor");
    return NULL;
  }
#endif

  return &sample_usbd;
}

struct usbd_context *sample_usbd_init_device(usbd_msg_cb_t msg_cb) {
  int err;

  if (sample_usbd_setup_device(msg_cb) == NULL) {
    return NULL;
  }

  /* doc device init start */
  err = usbd_init(&sample_usbd);
  if (err) {
    LOG_ERR("Failed to initialize device support");
    return NULL;
  }
  /* doc device init end */

  return &sample_usbd;
}
