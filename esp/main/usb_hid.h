#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void usb_hid_init(void);
bool usb_hid_ready(void);
void usb_hid_send_report(uint8_t report_id, const uint8_t *data, size_t len);

/**
 * @brief Send neutral/resting state for all HID reports
 *
 * This releases any stuck input states (buttons, movement, scroll) by sending
 * neutral reports for all report IDs. Call this after BLE device disconnect.
 */
void usb_hid_release_all(void);

#ifdef __cplusplus
}
#endif
