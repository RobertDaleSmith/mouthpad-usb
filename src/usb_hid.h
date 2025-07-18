#ifndef USB_HID_H
#define USB_HID_H

#include <stdint.h>
#include <stdbool.h>
#include "nrfx_usbd.h"

// USB HID device states
typedef enum {
    USB_HID_STATE_DISCONNECTED,
    USB_HID_STATE_CONNECTED,
    USB_HID_STATE_SUSPENDED,
    USB_HID_STATE_ERROR
} usb_hid_state_t;

// USB HID report types
typedef enum {
    USB_HID_REPORT_TYPE_INPUT = 1,
    USB_HID_REPORT_TYPE_OUTPUT = 2,
    USB_HID_REPORT_TYPE_FEATURE = 3
} usb_hid_report_type_t;

// Function declarations
void usb_hid_init(void);
void usb_hid_start(void);
void usb_hid_stop(void);
usb_hid_state_t usb_hid_get_state(void);
bool usb_hid_is_connected(void);
bool usb_hid_send_report(const uint8_t *data, uint16_t len);
bool usb_hid_send_mouse_report(int8_t x, int8_t y, uint8_t buttons);
bool usb_hid_send_keyboard_report(uint8_t modifier, uint8_t *keys, uint8_t key_count);

// USB event handlers
void usb_hid_on_usbd_evt(nrfx_usbd_evt_t const * p_event);

// Configuration
void usb_hid_set_report_descriptor(const uint8_t *desc, uint16_t len);
void usb_hid_set_device_name(const char *name);
void usb_hid_set_manufacturer_name(const char *name);
void usb_hid_set_product_name(const char *name);

#endif // USB_HID_H