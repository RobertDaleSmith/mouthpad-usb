#pragma once

#include <stdbool.h>

void bootloader_trigger_init(void);
void bootloader_trigger_enter_dfu(void);
bool bootloader_trigger_pending(void);

