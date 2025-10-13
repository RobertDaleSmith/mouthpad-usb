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
#include <zephyr/logging/log.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include "MouthpadRelay.pb.h"
#include "pb_encode.h"

LOG_MODULE_REGISTER(usb_cdc, LOG_LEVEL_INF);

/* USB CDC ACM device reference */
static const struct device *cdc_acm_dev;

/* Ring buffer for CDC0 RX data */
#define CDC0_RX_RINGBUF_SIZE 1024
static uint8_t cdc0_rx_ringbuf_data[CDC0_RX_RINGBUF_SIZE];
static struct ring_buf cdc0_rx_ringbuf;

/* Work structures for async USB CDC message sending */
static struct k_work usb_cdc_async_work;

/* FIFOs for UART data */
static K_FIFO_DEFINE(fifo_uart_tx_data);
static K_FIFO_DEFINE(fifo_uart_rx_data);

/* FIFO for async USB CDC message sending */
static K_FIFO_DEFINE(fifo_usb_cdc_async_data);

/* Data structure for async USB CDC message sending */
struct usb_cdc_async_data_t {
	void *fifo_reserved;
	mouthware_message_RelayToAppMessage message;
};

/* Work handler for async USB CDC message sending */
static void usb_cdc_async_work_handler(struct k_work *work)
{
	struct usb_cdc_async_data_t *async_data;
	
	/* Get message from FIFO */
	async_data = k_fifo_get(&fifo_usb_cdc_async_data, K_NO_WAIT);
	if (!async_data) {
		return;
	}
	
	/* Send the message synchronously (this will be quick) */
	uint8_t dataPacket[1024];
	pb_ostream_t stream = pb_ostream_from_buffer(dataPacket, sizeof(dataPacket));
	if (!pb_encode(&stream, mouthware_message_RelayToAppMessage_fields, &async_data->message)) {
		LOG_ERR("Async encoding failed: %s\n", PB_GET_ERROR(&stream));
	} else {
		usb_cdc_send_data(dataPacket, stream.bytes_written);
	}
	
	/* Free the async data */
	k_free(async_data);
}

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

/* UART interrupt callback for CDC0 */
static void cdc0_uart_callback(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	uart_irq_update(dev);

	if (uart_irq_rx_ready(dev)) {
		uint8_t buffer[64];
		int recv_len;

		recv_len = uart_fifo_read(dev, buffer, sizeof(buffer));
		if (recv_len > 0) {
			LOG_INF("CDC0 IRQ RX: %d bytes", recv_len);
			ring_buf_put(&cdc0_rx_ringbuf, buffer, recv_len);
		}
	}
}

/* Initialize USB CDC functionality */
int usb_cdc_init(void)
{
	LOG_INF("USB CDC: Starting CDC initialization");

	/* Get CDC0 device (BLE NUS bridge port) from device tree */
	cdc_acm_dev = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));
	if (!device_is_ready(cdc_acm_dev)) {
		LOG_ERR("CDC ACM device not ready");
		return -ENODEV;
	}

	/* Initialize ring buffer for CDC0 RX */
	ring_buf_init(&cdc0_rx_ringbuf, sizeof(cdc0_rx_ringbuf_data), cdc0_rx_ringbuf_data);

	/* Set up interrupt-driven UART for CDC0 */
	uart_irq_callback_user_data_set(cdc_acm_dev, cdc0_uart_callback, NULL);
	uart_irq_rx_enable(cdc_acm_dev);

	/* Initialize work queue for async message sending */
	k_work_init(&usb_cdc_async_work, usb_cdc_async_work_handler);

	/* Note: USB subsystem will be initialized by USB HID later */
	/* CDC1 (cdc_acm_uart1) is used for console/logs */
	LOG_INF("USB CDC: CDC0 ready: %s (interrupt-driven)", cdc_acm_dev->name);
	LOG_INF("USB CDC: Initialization successful");
	return 0;
}

/* Send data to USB CDC with robust packet framing */
int usb_cdc_send_data(const uint8_t *data, uint16_t len)
{
	if (!data || len == 0) {
		return -EINVAL;
	}

	if (!cdc_acm_dev) {
		LOG_WRN("CDC ACM device not initialized");
		return -ENODEV;
	}

	LOG_DBG("Forwarding %d bytes to CDC with framing", len);
	
	// Calculate CRC-16 for the payload
	uint16_t crc = calculate_crc16(data, len);
	
	// Send dual start markers
	uart_poll_out(cdc_acm_dev, 0xAA); // First start marker
	uart_poll_out(cdc_acm_dev, 0x55); // Second start marker
	
	// Send packet length (2 bytes, big-endian)
	uart_poll_out(cdc_acm_dev, (len >> 8) & 0xFF); // High byte
	uart_poll_out(cdc_acm_dev, len & 0xFF);        // Low byte
	
	// Send packet data
	for (uint16_t i = 0; i < len; i++) {
		uart_poll_out(cdc_acm_dev, data[i]);
	}
	
	// Send CRC (2 bytes, big-endian)
	uart_poll_out(cdc_acm_dev, (crc >> 8) & 0xFF); // CRC high byte
	uart_poll_out(cdc_acm_dev, crc & 0xFF);        // CRC low byte
	
	return 0;
}

/* Receive data from USB CDC */
int usb_cdc_receive_data(uint8_t *buffer, uint16_t max_len)
{
	if (!buffer || max_len == 0) {
		return -EINVAL;
	}

	if (!cdc_acm_dev) {
		return 0; // No device, no data
	}

	/* Read from ring buffer (filled by interrupt callback) */
	return ring_buf_get(&cdc0_rx_ringbuf, buffer, max_len);
}

/* Get CDC ACM device for external use */
const struct device *usb_cdc_get_uart_device(void)
{
	return cdc_acm_dev;
}

/* Send USB CDC proto message asynchronously (non-blocking) */
int usb_cdc_send_proto_message_async(mouthware_message_RelayToAppMessage message)
{
	struct usb_cdc_async_data_t *async_data;
	
	/* Allocate memory for async data */
	async_data = k_malloc(sizeof(struct usb_cdc_async_data_t));
	if (!async_data) {
		LOG_ERR("Failed to allocate memory for async USB CDC message");
		return -ENOMEM;
	}
	
	/* Copy the message */
	async_data->message = message;
	
	/* Put message in FIFO */
	k_fifo_put(&fifo_usb_cdc_async_data, async_data);
	
	/* Submit work to work queue */
	k_work_submit(&usb_cdc_async_work);
	
	return 0;
}
