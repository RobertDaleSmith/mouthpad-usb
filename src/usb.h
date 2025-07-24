#ifndef USB_H
#define USB_H

#include <zephyr/kernel.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/hid.h>
#include <zephyr/usb/usb_common.h>

/* USB HID Configuration */
#define USB_VENDOR_ID 0x1234  /* Replace with your vendor ID */
#define USB_PRODUCT_ID 0x5678 /* Replace with your product ID */
#define USB_MANUFACTURER_STRING "MouthPad"
#define USB_PRODUCT_STRING "MouthPad Bridge"
#define USB_SERIAL_NUMBER "MP001"

/* HID Report Descriptor - Mouse */
#define HID_MOUSE_REPORT_DESC_SIZE 52
extern const uint8_t hid_mouse_report_desc[HID_MOUSE_REPORT_DESC_SIZE];

/* HID Report Descriptor - Keyboard */
#define HID_KEYBOARD_REPORT_DESC_SIZE 63
extern const uint8_t hid_keyboard_report_desc[HID_KEYBOARD_REPORT_DESC_SIZE];

/* HID Report Sizes */
#define HID_MOUSE_REPORT_SIZE 4
#define HID_KEYBOARD_REPORT_SIZE 8

/* HID Report Structures */
struct hid_mouse_report {
    uint8_t buttons;
    int8_t x;
    int8_t y;
    int8_t wheel;
} __packed;

struct hid_keyboard_report {
    uint8_t modifier;
    uint8_t reserved;
    uint8_t keycode[6];
} __packed;

/* Function declarations */
void usb_init(void);
int usb_send_mouse_report(const struct hid_mouse_report *report);
int usb_send_keyboard_report(const struct hid_keyboard_report *report);
int usb_send_raw_report(const uint8_t *data, size_t len, uint8_t report_id);
bool usb_is_ready(void);
void usb_set_connected_callback(void (*callback)(bool connected));

/* USB State Management */
enum usb_state {
    USB_STATE_DISCONNECTED,
    USB_STATE_CONNECTING,
    USB_STATE_CONNECTED,
    USB_STATE_ERROR
};

/* Global variables (extern declarations) */
extern enum usb_state usb_current_state;
extern struct k_sem usb_ready_sem;

/* USB HID Callbacks */
extern const struct hid_ops hid_ops;

/* Utility functions for HID reports */
void hid_mouse_report_init(struct hid_mouse_report *report);
void hid_keyboard_report_init(struct hid_keyboard_report *report);
int hid_mouse_move(struct hid_mouse_report *report, int8_t x, int8_t y);
int hid_mouse_click(struct hid_mouse_report *report, uint8_t button);
int hid_mouse_scroll(struct hid_mouse_report *report, int8_t wheel);
int hid_keyboard_press_key(struct hid_keyboard_report *report, uint8_t keycode);
int hid_keyboard_press_modifier(struct hid_keyboard_report *report, uint8_t modifier);
int hid_keyboard_release_all(struct hid_keyboard_report *report);

#endif /* USB_H */
