/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "ble.h"
#include "usb.h"
#include "virtual_mouse.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void) {
    LOG_INF("MouthPad^USB Bridge Started");

    // Initialize virtual mouse first
    if (virtual_mouse_init() != 0) {
        LOG_ERR("Failed to initialize virtual mouse");
        return -1;
    }

    ble_init();
    usb_init();

    return 0;
}
