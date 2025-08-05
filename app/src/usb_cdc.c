/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "usb_cdc.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(usb_cdc, LOG_LEVEL_INF);

/* UART device for USB CDC */
static const struct device *uart = DEVICE_DT_GET(DT_CHOSEN(nordic_nus_uart));

/* Work structures for UART handling */
static struct k_work_delayable uart_work;

/* FIFOs for UART data */
static K_FIFO_DEFINE(fifo_uart_tx_data);
static K_FIFO_DEFINE(fifo_uart_rx_data);

/* Calculate CRC-16 (CCITT) for data integrity checking */
uint16_t calculate_crc16(const uint8_t *data, uint16_t len)
{
	uint16_t crc = 0xFFFF; // Initial value
	
	for (uint16_t i = 0; i < len; i++) {
		crc ^= (uint16_t)data[i] << 8;
		for (int j = 0; j < 8; j++) {
			if (crc & 0x8000) {
				crc = (crc << 1) ^ 0x1021; // CCITT polynomial
			} else {
				crc = crc << 1;
			}
		}
	}
	
	return crc & 0xFFFF;
}

/* UART callback for handling UART events */
static void uart_cb(const struct device *dev, struct uart_event *evt, void *user_data)
{
	ARG_UNUSED(dev);

	static size_t aborted_len;
	struct uart_data_t *buf;
	static uint8_t *aborted_buf;
	static bool disable_req;

	switch (evt->type) {
	case UART_TX_DONE:
		LOG_DBG("UART_TX_DONE");
		if ((evt->data.tx.len == 0) ||
		    (!evt->data.tx.buf)) {
			return;
		}

		if (aborted_buf) {
			buf = CONTAINER_OF(aborted_buf, struct uart_data_t,
					   data[0]);
			aborted_buf = NULL;
			aborted_len = 0;
		} else {
			buf = CONTAINER_OF(evt->data.tx.buf,
					   struct uart_data_t,
					   data[0]);
		}

		k_free(buf);

		buf = k_fifo_get(&fifo_uart_tx_data, K_NO_WAIT);
		if (!buf) {
			return;
		}

		if (uart_tx(uart, buf->data, buf->len, SYS_FOREVER_MS)) {
			LOG_WRN("Failed to send data over UART");
		}

		break;

	case UART_RX_RDY:
		LOG_DBG("UART_RX_RDY");
		buf = CONTAINER_OF(evt->data.rx.buf, struct uart_data_t, data[0]);
		buf->len += evt->data.rx.len;

		// Debug: Print received UART data
		printk("*** UART RX: Received %d bytes ***\n", evt->data.rx.len);
		for (int i = 0; i < evt->data.rx.len; i++) {
			printk("%c", evt->data.rx.buf[i]);
		}
		printk("***\n");

		if (disable_req) {
			return;
		}

		if ((evt->data.rx.buf[buf->len - 1] == '\n') ||
		    (evt->data.rx.buf[buf->len - 1] == '\r')) {
			disable_req = true;
			uart_rx_disable(uart);
		}

		break;

	case UART_RX_DISABLED:
		LOG_DBG("UART_RX_DISABLED");
		disable_req = false;

		buf = k_malloc(sizeof(*buf));
		if (buf) {
			buf->len = 0;
		} else {
			LOG_WRN("Not able to allocate UART receive buffer");
			k_work_reschedule(&uart_work, UART_WAIT_FOR_BUF_DELAY);
			return;
		}

		uart_rx_enable(uart, buf->data, sizeof(buf->data),
			       UART_RX_TIMEOUT);

		break;

	case UART_RX_BUF_REQUEST:
		LOG_DBG("UART_RX_BUF_REQUEST");
		buf = k_malloc(sizeof(*buf));
		if (buf) {
			buf->len = 0;
			uart_rx_buf_rsp(uart, buf->data, sizeof(buf->data));
		} else {
			LOG_WRN("Not able to allocate UART receive buffer");
		}

		break;

	case UART_RX_BUF_RELEASED:
		LOG_DBG("UART_RX_BUF_RELEASED");
		buf = CONTAINER_OF(evt->data.rx_buf.buf, struct uart_data_t,
				   data[0]);

		if (buf->len > 0) {
			k_fifo_put(&fifo_uart_rx_data, buf);
		} else {
			k_free(buf);
		}

		break;

	case UART_TX_ABORTED:
		LOG_DBG("UART_TX_ABORTED");
		if (!aborted_buf) {
			aborted_buf = (uint8_t *)evt->data.tx.buf;
		}

		aborted_len += evt->data.tx.len;
		buf = CONTAINER_OF(aborted_buf, struct uart_data_t,
				   data[0]);

		uart_tx(uart, &buf->data[aborted_len],
			buf->len - aborted_len, SYS_FOREVER_MS);

		break;

	default:
		break;
	}
}

/* UART work handler for delayed operations */
static void uart_work_handler(struct k_work *item)
{
	struct uart_data_t *buf;

	buf = k_malloc(sizeof(*buf));
	if (buf) {
		buf->len = 0;
	} else {
		LOG_WRN("Not able to allocate UART receive buffer");
		k_work_reschedule(&uart_work, UART_WAIT_FOR_BUF_DELAY);
		return;
	}

	uart_rx_enable(uart, buf->data, sizeof(buf->data), UART_RX_TIMEOUT);
}

/* Initialize USB CDC (UART) functionality */
int usb_cdc_init(void)
{
	struct uart_data_t *rx;

	printk("usb_cdc_init: Starting UART initialization\n");
	printk("usb_cdc_init: Using UART device: %s\n", uart->name);

	if (!device_is_ready(uart)) {
		printk("usb_cdc_init: UART device not ready\n");
		LOG_ERR("UART device not ready");
		return -ENODEV;
	}
	printk("usb_cdc_init: UART device is ready\n");

	rx = k_malloc(sizeof(*rx));
	if (rx) {
		rx->len = 0;
		printk("usb_cdc_init: Allocated RX buffer\n");
	} else {
		printk("usb_cdc_init: Failed to allocate RX buffer\n");
		return -ENOMEM;
	}

	printk("usb_cdc_init: Initializing UART work handler\n");
	k_work_init_delayable(&uart_work, uart_work_handler);

	printk("usb_cdc_init: Skipping UART callback setup - using polling instead\n");
	printk("usb_cdc_init: UART initialization successful\n");
	return 0;
}

/* Send data to USB CDC with robust packet framing */
int usb_cdc_send_data(const uint8_t *data, uint16_t len)
{
	if (!data || len == 0) {
		return -EINVAL;
	}

	printk("*** FORWARDING %d BYTES TO CDC WITH NEW FRAMING ***\n", len);
	
	// Calculate CRC-16 for the payload
	uint16_t crc = calculate_crc16(data, len);
	
	// Send dual start markers
	uart_poll_out(uart, 0xAA); // First start marker
	uart_poll_out(uart, 0x55); // Second start marker
	
	// Send packet length (2 bytes, big-endian)
	uart_poll_out(uart, (len >> 8) & 0xFF); // High byte
	uart_poll_out(uart, len & 0xFF);        // Low byte
	
	// Send packet data
	for (uint16_t i = 0; i < len; i++) {
		uart_poll_out(uart, data[i]);
	}
	
	// Send CRC (2 bytes, big-endian)
	uart_poll_out(uart, (crc >> 8) & 0xFF); // CRC high byte
	uart_poll_out(uart, crc & 0xFF);        // CRC low byte
	
	return 0;
}

/* Receive data from USB CDC */
int usb_cdc_receive_data(uint8_t *buffer, uint16_t max_len)
{
	if (!buffer || max_len == 0) {
		return -EINVAL;
	}

	// Simple polling-based receive for now
	unsigned char c;
	int bytes_read = uart_poll_in(uart, &c);
	
	if (bytes_read == 0) { // Data received from CDC
		*buffer = c;
		return 1; // Return 1 byte received
	}
	
	return 0; // No data available
}

/* Get UART device for external use */
const struct device *usb_cdc_get_uart_device(void)
{
	return uart;
}
