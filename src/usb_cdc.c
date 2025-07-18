// USB CDC (Serial) implementation
#include "usb_cdc.h"
#include "utils.h"
#include "nrf_log.h"
#include "app_error.h"
#include "nrfx_usbd.h"
#include "usb_descriptors.h"
#include <string.h>

static usb_cdc_state_t m_state = USB_CDC_STATE_DISCONNECTED;
static bool m_initialized = false;
static char m_device_name[64] = "BLE-USB Bridge CDC";
static char m_manufacturer_name[64] = "Nordic";
static char m_product_name[64] = "BLE-USB Bridge CDC";
static usb_cdc_rx_callback_t m_rx_callback = NULL;

// USB CDC Data Buffer
static uint8_t m_cdc_tx_buffer[64];
static uint8_t m_cdc_rx_buffer[64];
static uint16_t m_cdc_tx_len = 0;

void usb_cdc_init(void)
{
    if (m_initialized) {
        return;
    }
    
    // Initialize USB device (shared with HID)
    // Note: In a real implementation, this would be coordinated with USB HID
    // For now, we assume USB is already initialized by HID module
    
    m_initialized = true;
    m_state = USB_CDC_STATE_DISCONNECTED;
    
    NRF_LOG_INFO("USB CDC initialized");
}

void usb_cdc_start(void)
{
    if (!m_initialized) {
        NRF_LOG_ERROR("USB CDC not initialized");
        return;
    }
    
    // USB is started by HID module
    NRF_LOG_INFO("USB CDC started");
}

void usb_cdc_stop(void)
{
    if (!m_initialized) {
        return;
    }
    
    m_state = USB_CDC_STATE_DISCONNECTED;
    
    NRF_LOG_INFO("USB CDC stopped");
}

usb_cdc_state_t usb_cdc_get_state(void)
{
    return m_state;
}

bool usb_cdc_is_connected(void)
{
    return m_state == USB_CDC_STATE_CONNECTED;
}

bool usb_cdc_send_data(const uint8_t *data, uint16_t len)
{
    if (!usb_cdc_is_connected()) {
        NRF_LOG_WARNING("Cannot send CDC data: USB not connected");
        return false;
    }
    
    if (len > sizeof(m_cdc_tx_buffer)) {
        NRF_LOG_ERROR("CDC data too large: %d > %d", len, sizeof(m_cdc_tx_buffer));
        return false;
    }
    
    // Copy data to buffer
    memcpy(m_cdc_tx_buffer, data, len);
    m_cdc_tx_len = len;
    
    // Send data via USB CDC endpoint
    ret_code_t err_code = nrfx_usbd_ep_easy_dma_transfer(NRFX_USBD_EPIN2, 
                                                        m_cdc_tx_buffer, 
                                                        m_cdc_tx_len);
    if (err_code != NRF_SUCCESS) {
        NRF_LOG_ERROR("Failed to send CDC data: %d", err_code);
        return false;
    }
    
    NRF_LOG_INFO("CDC data sent, length: %d", len);
    utils_hex_dump(data, len);
    
    return true;
}

bool usb_cdc_send_string(const char *str)
{
    uint16_t len = strlen(str);
    return usb_cdc_send_data((const uint8_t*)str, len);
}

bool usb_cdc_send_line(const char *str)
{
    char line_buffer[128];
    snprintf(line_buffer, sizeof(line_buffer), "%s\r\n", str);
    return usb_cdc_send_string(line_buffer);
}

void usb_cdc_set_device_name(const char *name)
{
    strncpy(m_device_name, name, sizeof(m_device_name) - 1);
    m_device_name[sizeof(m_device_name) - 1] = '\0';
}

void usb_cdc_set_manufacturer_name(const char *name)
{
    strncpy(m_manufacturer_name, name, sizeof(m_manufacturer_name) - 1);
    m_manufacturer_name[sizeof(m_manufacturer_name) - 1] = '\0';
}

void usb_cdc_set_product_name(const char *name)
{
    strncpy(m_product_name, name, sizeof(m_product_name) - 1);
    m_product_name[sizeof(m_product_name) - 1] = '\0';
}

void usb_cdc_set_rx_callback(usb_cdc_rx_callback_t callback)
{
    m_rx_callback = callback;
}

void usb_cdc_on_usbd_evt(nrfx_usbd_evt_t const * p_event)
{
    switch (p_event->type) {
        case NRFX_USBD_EVT_READY:
            {
                NRF_LOG_INFO("USB CDC device ready");
                m_state = USB_CDC_STATE_CONNECTED;
            }
            break;
            
        case NRFX_USBD_EVT_DETECTED:
            {
                NRF_LOG_INFO("USB CDC host detected");
            }
            break;
            
        case NRFX_USBD_EVT_REMOVED:
            {
                NRF_LOG_INFO("USB CDC host removed");
                m_state = USB_CDC_STATE_DISCONNECTED;
            }
            break;
            
        case NRFX_USBD_EVT_SUSPEND:
            {
                NRF_LOG_INFO("USB CDC suspended");
                m_state = USB_CDC_STATE_SUSPENDED;
            }
            break;
            
        case NRFX_USBD_EVT_RESUME:
            {
                NRF_LOG_INFO("USB CDC resumed");
                m_state = USB_CDC_STATE_CONNECTED;
            }
            break;
            
        case NRFX_USBD_EVT_WUREQ:
            {
                NRF_LOG_INFO("USB CDC wake-up request");
            }
            break;
            
        case NRFX_USBD_EVT_SETUP:
            {
                // Handle USB setup requests for CDC
                const nrfx_usbd_setup_t *p_setup = &p_event->data.setup;
                
                switch (p_setup->bmRequestType) {
                    case 0x21:  // Host to Device, Class, Interface
                        {
                            switch (p_setup->bRequest) {
                                case 0x20:  // SET_LINE_CODING
                                    {
                                        // Handle line coding setup
                                        NRF_LOG_INFO("SET_LINE_CODING request");
                                    }
                                    break;
                                    
                                case 0x22:  // SET_CONTROL_LINE_STATE
                                    {
                                        // Handle control line state
                                        NRF_LOG_INFO("SET_CONTROL_LINE_STATE request");
                                    }
                                    break;
                                    
                                default:
                                    break;
                            }
                        }
                        break;
                        
                    case 0xA1:  // Device to Host, Class, Interface
                        {
                            switch (p_setup->bRequest) {
                                case 0x21:  // GET_LINE_CODING
                                    {
                                        // Send line coding
                                        uint8_t line_coding[7] = {
                                            0x80, 0x25, 0x00, 0x00,  // 9600 baud
                                            0x00, 0x00, 0x00         // 1 stop bit, no parity, 8 data bits
                                        };
                                        nrfx_usbd_ep_easy_dma_transfer(NRFX_USBD_EPIN0, 
                                                                      line_coding, 
                                                                      sizeof(line_coding));
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
                
                if (p_transfer->ep == NRFX_USBD_EPIN2) {
                    NRF_LOG_INFO("CDC data transfer completed");
                } else if (p_transfer->ep == NRFX_USBD_EPOUT2) {
                    // Data received from host
                    NRF_LOG_INFO("CDC data received, length: %d", p_transfer->size);
                    utils_hex_dump(p_transfer->p_data, p_transfer->size);
                    
                    // Forward to callback
                    if (m_rx_callback != NULL) {
                        m_rx_callback(p_transfer->p_data, p_transfer->size);
                    }
                    
                    // Prepare for next reception
                    nrfx_usbd_ep_easy_dma_transfer(NRFX_USBD_EPOUT2, 
                                                  m_cdc_rx_buffer, 
                                                  sizeof(m_cdc_rx_buffer));
                }
            }
            break;
            
        default:
            break;
    }
}