#pragma once

#include <stdbool.h>

void usb_dfu_init(void);
void usb_dfu_enter_dfu(void);
bool usb_dfu_pending(void);

