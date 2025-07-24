#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/hid.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(usb_mouse, LOG_LEVEL_INF);

void usb_init(void)
{
    int ret = usb_enable(NULL);
    if (ret != 0) {
        LOG_ERR("Failed to enable USB");
    } else {
        LOG_INF("USB enabled");
    }
}
