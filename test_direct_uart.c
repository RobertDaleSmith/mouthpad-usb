// Test: Write directly to CDC UART to verify it works
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <string.h>

void test_direct_cdc_write(void)
{
    const struct device *cdc1 = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart1));
    
    if (!device_is_ready(cdc1)) {
        return;
    }
    
    const char *msg = "*** DIRECT UART WRITE TEST ***\r\n";
    for (int i = 0; i < strlen(msg); i++) {
        uart_poll_out(cdc1, msg[i]);
    }
}
