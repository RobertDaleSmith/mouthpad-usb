#ifndef USB_CDC_H
#define USB_CDC_H

#include <stdint.h>
#include <stdbool.h>
#include "nrfx_usbd.h"

// USB CDC device states
typedef enum {
    USB_CDC_STATE_DISCONNECTED,
    USB_CDC_STATE_CONNECTED,
    USB_CDC_STATE_SUSPENDED,
    USB_CDC_STATE_ERROR
} usb_cdc_state_t;

// Function declarations
void usb_cdc_init(void);
void usb_cdc_start(void);
void usb_cdc_stop(void);
usb_cdc_state_t usb_cdc_get_state(void);
bool usb_cdc_is_connected(void);
bool usb_cdc_send_data(const uint8_t *data, uint16_t len);
bool usb_cdc_send_string(const char *str);
bool usb_cdc_send_line(const char *str);

// USB event handlers
void usb_cdc_on_usbd_evt(nrfx_usbd_evt_t const * p_event);

// Configuration
void usb_cdc_set_device_name(const char *name);
void usb_cdc_set_manufacturer_name(const char *name);
void usb_cdc_set_product_name(const char *name);

// Data reception callback
typedef void (*usb_cdc_rx_callback_t)(const uint8_t *data, uint16_t len);
void usb_cdc_set_rx_callback(usb_cdc_rx_callback_t callback);

#endif // USB_CDC_H