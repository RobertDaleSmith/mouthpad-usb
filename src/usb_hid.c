#include "usb_hid.h"
#include "utils.h"
#include "nrf_log.h"
#include "app_error.h"
#include "nrfx_usbd.h"
#include "usb_descriptors.h"
#include <string.h>

static usb_hid_state_t m_state = USB_HID_STATE_DISCONNECTED;
static bool m_initialized = false;
static const uint8_t *m_report_descriptor = NULL;
static uint16_t m_report_descriptor_len = 0;
static char m_device_name[64] = "BLE-USB Bridge";
static char m_manufacturer_name[64] = "Nordic";
static char m_product_name[64] = "BLE-USB Bridge";

// Generic HID Report Descriptor (supports mouse and keyboard)
static const uint8_t DEFAULT_HID_REPORT_DESC[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x05, 0x07,        // Usage Page (Key Codes)
    0x19, 0xE0,        // Usage Minimum (224)
    0x29, 0xE7,        // Usage Maximum (231)
    0x15, 0x00,        // Logical Minimum (0)
    0x25, 0x01,        // Logical Maximum (1)
    0x75, 0x01,        // Report Size (1)
    0x95, 0x08,        // Report Count (8)
    0x81, 0x02,        // Input (Data, Variable, Absolute)
    0x95, 0x01,        // Report Count (1)
    0x75, 0x08,        // Report Size (8)
    0x81, 0x01,        // Input (Constant)
    0x95, 0x06,        // Report Count (6)
    0x75, 0x08,        // Report Size (8)
    0x15, 0x00,        // Logical Minimum (0)
    0x25, 0x65,        // Logical Maximum (101)
    0x05, 0x07,        // Usage Page (Key Codes)
    0x19, 0x00,        // Usage Minimum (0)
    0x29, 0x65,        // Usage Maximum (101)
    0x81, 0x00,        // Input (Data, Array)
    0xC0,              // End Collection
    
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x09, 0x01,        // Usage (Pointer)
    0xA1, 0x00,        // Collection (Physical)
    0x05, 0x09,        // Usage Page (Button)
    0x19, 0x01,        // Usage Minimum (Button 1)
    0x29, 0x05,        // Usage Maximum (Button 5)
    0x15, 0x00,        // Logical Minimum (0)
    0x25, 0x01,        // Logical Maximum (1)
    0x95, 0x05,        // Report Count (5)
    0x75, 0x01,        // Report Size (1)
    0x81, 0x02,        // Input (Data, Variable, Absolute)
    0x95, 0x01,        // Report Count (1)
    0x75, 0x03,        // Report Size (3)
    0x81, 0x01,        // Input (Constant)
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x30,        // Usage (X)
    0x09, 0x31,        // Usage (Y)
    0x09, 0x38,        // Usage (Wheel)
    0x15, 0x81,        // Logical Minimum (-127)
    0x25, 0x7F,        // Logical Maximum (127)
    0x75, 0x08,        // Report Size (8)
    0x95, 0x03,        // Report Count (3)
    0x81, 0x06,        // Input (Data, Variable, Relative)
    0xC0,              // End Collection
    0xC0               // End Collection
};

// USB HID Report Buffer
static uint8_t m_hid_report_buffer[64];
static uint16_t m_hid_report_len = 0;

void usb_hid_init(void)
{
    if (m_initialized) {
        return;
    }
    
    // Set default report descriptor
    usb_hid_set_report_descriptor(DEFAULT_HID_REPORT_DESC, sizeof(DEFAULT_HID_REPORT_DESC));
    
    // Initialize USB device
    ret_code_t err_code = nrfx_usbd_init(usb_hid_on_usbd_evt);
    if (err_code != NRF_SUCCESS) {
        NRF_LOG_ERROR("Failed to initialize USB: %d", err_code);
        return;
    }
    
    m_initialized = true;
    m_state = USB_HID_STATE_DISCONNECTED;
    
    NRF_LOG_INFO("USB HID initialized");
}

void usb_hid_start(void)
{
    if (!m_initialized) {
        NRF_LOG_ERROR("USB HID not initialized");
        return;
    }
    
    ret_code_t err_code = nrfx_usbd_start();
    if (err_code != NRF_SUCCESS) {
        NRF_LOG_ERROR("Failed to start USB: %d", err_code);
        return;
    }
    
    NRF_LOG_INFO("USB HID started");
}

void usb_hid_stop(void)
{
    if (!m_initialized) {
        return;
    }
    
    nrfx_usbd_stop();
    m_state = USB_HID_STATE_DISCONNECTED;
    
    NRF_LOG_INFO("USB HID stopped");
}

usb_hid_state_t usb_hid_get_state(void)
{
    return m_state;
}

bool usb_hid_is_connected(void)
{
    return m_state == USB_HID_STATE_CONNECTED;
}

bool usb_hid_send_report(const uint8_t *data, uint16_t len)
{
    if (!usb_hid_is_connected()) {
        NRF_LOG_WARNING("Cannot send HID report: USB not connected");
        return false;
    }
    
    if (len > sizeof(m_hid_report_buffer)) {
        NRF_LOG_ERROR("HID report too large: %d > %d", len, sizeof(m_hid_report_buffer));
        return false;
    }
    
    // Copy data to buffer
    memcpy(m_hid_report_buffer, data, len);
    m_hid_report_len = len;
    
    // Send report via USB
    ret_code_t err_code = nrfx_usbd_ep_easy_dma_transfer(NRFX_USBD_EPIN1, 
                                                        m_hid_report_buffer, 
                                                        m_hid_report_len);
    if (err_code != NRF_SUCCESS) {
        NRF_LOG_ERROR("Failed to send HID report: %d", err_code);
        return false;
    }
    
    NRF_LOG_INFO("HID report sent, length: %d", len);
    utils_hex_dump(data, len);
    
    return true;
}

bool usb_hid_send_mouse_report(int8_t x, int8_t y, uint8_t buttons)
{
    uint8_t report[4] = {
        buttons,  // Button state
        0,        // Reserved
        x,        // X movement
        y         // Y movement
    };
    
    return usb_hid_send_report(report, sizeof(report));
}

bool usb_hid_send_keyboard_report(uint8_t modifier, uint8_t *keys, uint8_t key_count)
{
    if (key_count > 6) {
        key_count = 6;  // Maximum 6 keys
    }
    
    uint8_t report[8] = {0};
    report[0] = modifier;  // Modifier keys
    
    // Copy key codes
    for (uint8_t i = 0; i < key_count; i++) {
        report[2 + i] = keys[i];
    }
    
    return usb_hid_send_report(report, sizeof(report));
}

void usb_hid_set_report_descriptor(const uint8_t *desc, uint16_t len)
{
    m_report_descriptor = desc;
    m_report_descriptor_len = len;
}

void usb_hid_set_device_name(const char *name)
{
    strncpy(m_device_name, name, sizeof(m_device_name) - 1);
    m_device_name[sizeof(m_device_name) - 1] = '\0';
}

void usb_hid_set_manufacturer_name(const char *name)
{
    strncpy(m_manufacturer_name, name, sizeof(m_manufacturer_name) - 1);
    m_manufacturer_name[sizeof(m_manufacturer_name) - 1] = '\0';
}

void usb_hid_set_product_name(const char *name)
{
    strncpy(m_product_name, name, sizeof(m_product_name) - 1);
    m_product_name[sizeof(m_product_name) - 1] = '\0';
}

void usb_hid_on_usbd_evt(nrfx_usbd_evt_t const * p_event)
{
    switch (p_event->type) {
        case NRFX_USBD_EVT_READY:
            {
                NRF_LOG_INFO("USB device ready");
                m_state = USB_HID_STATE_CONNECTED;
                utils_led_usb_connected(true);
            }
            break;
            
        case NRFX_USBD_EVT_DETECTED:
            {
                NRF_LOG_INFO("USB host detected");
            }
            break;
            
        case NRFX_USBD_EVT_REMOVED:
            {
                NRF_LOG_INFO("USB host removed");
                m_state = USB_HID_STATE_DISCONNECTED;
                utils_led_usb_connected(false);
            }
            break;
            
        case NRFX_USBD_EVT_SUSPEND:
            {
                NRF_LOG_INFO("USB suspended");
                m_state = USB_HID_STATE_SUSPENDED;
            }
            break;
            
        case NRFX_USBD_EVT_RESUME:
            {
                NRF_LOG_INFO("USB resumed");
                m_state = USB_HID_STATE_CONNECTED;
            }
            break;
            
        case NRFX_USBD_EVT_WUREQ:
            {
                NRF_LOG_INFO("USB wake-up request");
            }
            break;
            
        case NRFX_USBD_EVT_SETUP:
            {
                // Handle USB setup requests
                const nrfx_usbd_setup_t *p_setup = &p_event->data.setup;
                
                switch (p_setup->bmRequestType) {
                    case 0x81:  // Device to Host, Standard, Device
                        {
                            switch (p_setup->bRequest) {
                                case 0x06:  // GET_DESCRIPTOR
                                    {
                                        switch (p_setup->wValue.high) {
                                            case 0x01:  // Device descriptor
                                                // Send device descriptor
                                                break;
                                                
                                            case 0x02:  // Configuration descriptor
                                                // Send configuration descriptor
                                                break;
                                                
                                            case 0x03:  // String descriptor
                                                // Send string descriptor
                                                break;
                                                
                                            case 0x22:  // HID Report descriptor
                                                if (m_report_descriptor != NULL) {
                                                    nrfx_usbd_ep_easy_dma_transfer(NRFX_USBD_EPIN0, 
                                                                                  (uint8_t*)m_report_descriptor, 
                                                                                  m_report_descriptor_len);
                                                }
                                                break;
                                                
                                            default:
                                                break;
                                        }
                                    }
                                    break;
                                    
                                default:
                                    break;
                            }
                        }
                        break;
                        
                    case 0x21:  // Host to Device, Class, Interface
                        {
                            switch (p_setup->bRequest) {
                                case 0x09:  // SET_REPORT
                                    {
                                        // Handle HID output report
                                        NRF_LOG_INFO("HID output report received");
                                    }
                                    break;
                                    
                                case 0x0A:  // SET_IDLE
                                    {
                                        // Handle SET_IDLE request
                                        NRF_LOG_INFO("SET_IDLE request");
                                    }
                                    break;
                                    
                                default:
                                    break;
                            }
                        }
                        break;
                        
                    default:
                        break;
                }
            }
            break;
            
        case NRFX_USBD_EVT_EPTRANSFER:
            {
                // Handle endpoint transfer completion
                const nrfx_usbd_ep_transfer_t *p_transfer = &p_event->data.eptransfer;
                
                if (p_transfer->ep == NRFX_USBD_EPIN1) {
                    NRF_LOG_INFO("HID report transfer completed");
                }
            }
            break;
            
        default:
            break;
    }
}