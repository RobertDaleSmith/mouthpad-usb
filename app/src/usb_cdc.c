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
#include <zephyr/drivers/uart.h>
#include "MouthpadUsb.pb.h"
#include "pb_encode.h"

LOG_MODULE_REGISTER(usb_cdc, LOG_LEVEL_INF);

/* USB CDC ACM device reference */
static const struct device *cdc_acm_dev;

/* Work structures for UART handling */
static struct k_work_delayable uart_work;

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
	mouthware_message_UsbDongleToMouthpadAppMessage message;
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
	if (!pb_encode(&stream, mouthware_message_UsbDongleToMouthpadAppMessage_fields, &async_data->message)) {
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

		if (uart_tx(cdc_acm_dev, buf->data, buf->len, SYS_FOREVER_MS)) {
			LOG_WRN("Failed to send data over CDC ACM");
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
			uart_rx_disable(cdc_acm_dev);
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

		uart_rx_enable(cdc_acm_dev, buf->data, sizeof(buf->data),
			       UART_RX_TIMEOUT);

		break;

	case UART_RX_BUF_REQUEST:
		LOG_DBG("UART_RX_BUF_REQUEST");
		buf = k_malloc(sizeof(*buf));
		if (buf) {
			buf->len = 0;
			uart_rx_buf_rsp(cdc_acm_dev, buf->data, sizeof(buf->data));
		} else {
			LOG_WRN("Not able to allocate CDC ACM receive buffer");
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

		uart_tx(cdc_acm_dev, &buf->data[aborted_len],
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

	uart_rx_enable(cdc_acm_dev, buf->data, sizeof(buf->data), UART_RX_TIMEOUT);
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

	/* Initialize work queue for async message sending */
	k_work_init(&usb_cdc_async_work, usb_cdc_async_work_handler);

	/* Note: USB subsystem will be initialized by USB HID later */
	/* CDC1 (cdc_acm_uart1) is used for console/logs */
	LOG_INF("USB CDC: CDC0 ready: %s (BLE NUS bridge)", cdc_acm_dev->name);
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

	/* Try to read data from CDC ACM */
	int bytes_read = 0;
	for (uint16_t i = 0; i < max_len; i++) {
		unsigned char c;
		int ret = uart_poll_in(cdc_acm_dev, &c);
		if (ret == 0) {
			buffer[bytes_read] = c;
			bytes_read++;
		} else {
			break; // No more data available
		}
	}

	return bytes_read;
}

/* Get CDC ACM device for external use */
const struct device *usb_cdc_get_uart_device(void)
{
	return cdc_acm_dev;
}

/* Send USB CDC proto message asynchronously (non-blocking) */
int usb_cdc_send_proto_message_async(mouthware_message_UsbDongleToMouthpadAppMessage message)
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
