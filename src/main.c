#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
// #include "usb.h"
#include "ble.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void) {
    LOG_INF("MouthPad^USB Bridge Started");

    // usb_init();
    ble_init();

    // while (1) {
    //     // ble_poll(); // Optional depending on how BLE is set up
    //     k_sleep(K_MSEC(10));
    // }

    return 0;
}