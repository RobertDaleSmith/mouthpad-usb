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

#ifdef __cplusplus
}
#endif
