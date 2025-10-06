/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef USB_CDC_H
#define USB_CDC_H

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>

/* UART payload buffer element size. */
#define UART_BUF_SIZE 128

/* UART timing constants */
#define UART_WAIT_FOR_BUF_DELAY K_MSEC(50)
#define UART_RX_TIMEOUT 50000 /* Wait for RX complete event time in microseconds. */

/* USB CDC data structure */
struct uart_data_t {
	void *fifo_reserved;
	uint8_t  data[UART_BUF_SIZE];
	uint16_t len;
};

/* USB CDC initialization and control functions */
int usb_cdc_init(void);
int usb_cdc_send_data(const uint8_t *data, uint16_t len);
int usb_cdc_receive_data(uint8_t *buffer, uint16_t max_len);

/* CRC calculation for packet framing */
uint16_t calculate_crc16(const uint8_t *data, uint16_t len);

/* Get UART device for external use */
const struct device *usb_cdc_get_uart_device(void);

#endif /* USB_CDC_H */ 