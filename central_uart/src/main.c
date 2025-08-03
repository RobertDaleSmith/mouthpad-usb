/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** @file
 *  @brief Nordic UART Service Client sample
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

#include <bluetooth/services/nus.h>
#include <bluetooth/services/nus_client.h>
#include <bluetooth/gatt_dm.h>
#include <bluetooth/scan.h>

#include <zephyr/settings/settings.h>

#include <zephyr/drivers/uart.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#define LOG_MODULE_NAME central_uart
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

/* UART payload buffer element size. */
#define UART_BUF_SIZE 128

#define KEY_PASSKEY_ACCEPT DK_BTN1_MSK
#define KEY_PASSKEY_REJECT DK_BTN2_MSK

#define NUS_WRITE_TIMEOUT K_MSEC(150)
#define UART_WAIT_FOR_BUF_DELAY K_MSEC(50)
#define UART_RX_TIMEOUT 50000 /* Wait for RX complete event time in microseconds. */

static const struct device *uart = DEVICE_DT_GET(DT_CHOSEN(nordic_nus_uart));
static struct k_work_delayable uart_work;
static struct k_work scan_work;
static struct k_work_delayable send_command_work;
static struct k_work_delayable status_check_work;
static struct k_work_delayable data_check_work;

K_SEM_DEFINE(nus_write_sem, 0, 1);

struct uart_data_t {
	void *fifo_reserved;
	uint8_t  data[UART_BUF_SIZE];
	uint16_t len;
};

static K_FIFO_DEFINE(fifo_uart_tx_data);
static K_FIFO_DEFINE(fifo_uart_rx_data);

static struct bt_conn *default_conn;
static struct bt_nus_client nus_client;

// Calculate CRC-16 (CCITT) for data integrity checking
static uint16_t calculate_crc16(const uint8_t *data, uint16_t len)
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

static void ble_data_sent(struct bt_nus_client *nus, uint8_t err,
					const uint8_t *const data, uint16_t len)
{
	ARG_UNUSED(nus);
	ARG_UNUSED(data);
	ARG_UNUSED(len);

	k_sem_give(&nus_write_sem);

	if (err) {
		LOG_WRN("ATT error code: 0x%02X", err);
	}
}

// Binary data parser for MouthPad sensor packets
static void parse_mouthpad_sensor_data(const uint8_t *data, uint16_t len)
{
	if (len < 4) {
		printk("*** PACKET TOO SHORT TO PARSE ***\n");
		return;
	}
	
	uint8_t packet_type = data[0];
	uint8_t sequence = data[1];
	uint8_t flags = data[2];
	// uint8_t reserved = data[3]; // Unused for now
	
	printk("*** PARSED PACKET: Type=0x%02x, Seq=%d, Flags=0x%02x ***\n", 
		packet_type, sequence, flags);
	
	// Parse sensor data (starting from byte 4)
	if (len > 4) {
		printk("*** SENSOR DATA (%d bytes): ", len - 4);
		for (int i = 4; i < len && i < 20; i++) {  // Show first 16 sensor bytes
			printk("%02x ", data[i]);
		}
		if (len > 20) {
			printk("...");
		}
		printk("***\n");
		
		// Try to interpret as 16-bit sensor values
		if (len >= 6) {
			printk("*** FIRST SENSOR VALUES (16-bit): ");
			for (int i = 4; i < len - 1; i += 2) {
				uint16_t sensor_value = data[i] | (data[i+1] << 8);  // Little endian
				printk("%d ", sensor_value);
			}
			printk("***\n");
		}
	}
}

static uint8_t ble_data_received(struct bt_nus_client *nus,
						const uint8_t *data, uint16_t len)
{
	ARG_UNUSED(nus);

	// Bridge NUS data directly to USB CDC
	printk("*** NUS→CDC: Received %d bytes, sending to CDC ***\n", len);
	
	// Log the first few bytes to see what we're getting
	printk("*** NUS DATA: ");
	for (int i = 0; i < len && i < 16; i++) {
		printk("%02x ", data[i]);
	}
	printk("***\n");
	
	// Filter out 2-byte echo responses (73 XX format)
	if (len == 2 && data[0] == 0x73) {
		printk("*** SKIPPING 2-BYTE ECHO: 73 %02x ***\n", data[1]);
		return BT_GATT_ITER_CONTINUE;
	}
	
	// Don't echo back single characters (likely echo from our input)
	if (len == 1) {
		printk("*** SKIPPING SINGLE CHARACTER ECHO ***\n");
		return BT_GATT_ITER_CONTINUE;
	}
	
	// For larger packets, try to identify the structure
	if (len >= 4) {
		printk("*** PACKET STRUCTURE: Type=0x%02x, Length=%d ***\n", data[0], len);
	}
	
	// Send data directly to USB CDC with robust packet framing
	// New format: [0xAA][0x55][LEN_H][LEN_L][DATA...][CRC_H][CRC_L]
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
	
	return BT_GATT_ITER_CONTINUE;
}

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

static int uart_init(void)
{
	int err;
	struct uart_data_t *rx;

	printk("uart_init: Starting UART initialization\n");
	printk("uart_init: Using UART device: %s\n", uart->name);

	if (!device_is_ready(uart)) {
		printk("uart_init: UART device not ready\n");
		LOG_ERR("UART device not ready");
		return -ENODEV;
	}
	printk("uart_init: UART device is ready\n");

	rx = k_malloc(sizeof(*rx));
	if (rx) {
		rx->len = 0;
		printk("uart_init: Allocated RX buffer\n");
	} else {
		printk("uart_init: Failed to allocate RX buffer\n");
		return -ENOMEM;
	}

	printk("uart_init: Initializing UART work handler\n");
	k_work_init_delayable(&uart_work, uart_work_handler);

	printk("uart_init: Skipping UART callback setup - using polling instead\n");
	printk("uart_init: UART initialization successful\n");
	return 0;
}

static void status_check_work_handler(struct k_work *item)
{
	ARG_UNUSED(item);
	
	if (default_conn) {
		printk("*** CONNECTION STATUS: STILL CONNECTED ***\n");
	} else {
		printk("*** CONNECTION STATUS: DISCONNECTED ***\n");
	}
	
	// Check again in 10 seconds
	k_work_schedule(&status_check_work, K_SECONDS(10));
}

static void data_check_work_handler(struct k_work *item)
{
	ARG_UNUSED(item);
	
	printk("*** DATA CHECK: Still monitoring for MouthPad data ***\n");
	
	// Check again in 5 seconds
	k_work_schedule(&data_check_work, K_SECONDS(5));
}

static void send_command_to_mouthpad(const char *command)
{
	if (!default_conn) {
		printk("*** NOT CONNECTED - CANNOT SEND COMMAND ***\n");
		return;
	}
	
	printk("*** SENDING COMMAND TO MOUTHPAD: %s ***\n", command);
	
	int err = bt_nus_client_send(&nus_client, (const uint8_t*)command, strlen(command));
	if (err) {
		printk("*** FAILED TO SEND COMMAND (err %d) ***\n", err);
	} else {
		printk("*** COMMAND SENT SUCCESSFULLY ***\n");
	}
}

static void send_command_work_handler(struct k_work *item)
{
	ARG_UNUSED(item);
	
	printk("*** CONNECTED TO MOUTHPAD - READY FOR MANUAL COMMANDS ***\n");
	printk("*** USE SCREEN TO SEND COMMANDS LIKE 'StartStream jcp' ***\n");
	
	// Start periodic status checks
	k_work_init_delayable(&status_check_work, status_check_work_handler);
	k_work_schedule(&status_check_work, K_SECONDS(10));
	
	// Start periodic data checks
	k_work_init_delayable(&data_check_work, data_check_work_handler);
	k_work_schedule(&data_check_work, K_SECONDS(5));
}

static void discovery_complete(struct bt_gatt_dm *dm,
			       void *context)
{
	struct bt_nus_client *nus = context;
	LOG_INF("Service discovery completed");

	bt_gatt_dm_data_print(dm);

	bt_nus_handles_assign(dm, nus);
	bt_nus_subscribe_receive(nus);

	bt_gatt_dm_data_release(dm);
	
	/* Send initial command to MouthPad after 1 second */
	printk("*** SCHEDULING INITIAL COMMAND IN 1 SECOND ***\n");
	k_work_init_delayable(&send_command_work, send_command_work_handler);
	k_work_schedule(&send_command_work, K_SECONDS(1));
}

static void discovery_service_not_found(struct bt_conn *conn,
					void *context)
{
	LOG_INF("Service not found");
}

static void discovery_error(struct bt_conn *conn,
			    int err,
			    void *context)
{
	LOG_WRN("Error while discovering GATT database: (%d)", err);
}

struct bt_gatt_dm_cb discovery_cb = {
	.completed         = discovery_complete,
	.service_not_found = discovery_service_not_found,
	.error_found       = discovery_error,
};

static void gatt_discover(struct bt_conn *conn)
{
	int err;

	if (conn != default_conn) {
		return;
	}

	err = bt_gatt_dm_start(conn,
			       BT_UUID_NUS_SERVICE,
			       &discovery_cb,
			       &nus_client);
	if (err) {
		LOG_ERR("could not start the discovery procedure, error "
			"code: %d", err);
	}
}

static void exchange_func(struct bt_conn *conn, uint8_t err, struct bt_gatt_exchange_params *params)
{
	if (!err) {
		uint16_t mtu = bt_gatt_get_mtu(conn);
		printk("*** MTU EXCHANGE SUCCESSFUL: MTU = %d bytes ***\n", mtu);
		LOG_INF("MTU exchange done, MTU: %d", mtu);
	} else {
		printk("*** MTU EXCHANGE FAILED: error = %d ***\n", err);
		LOG_WRN("MTU exchange failed (err %" PRIu8 ")", err);
	}
}

static void connected(struct bt_conn *conn, uint8_t conn_err)
{
	char addr[BT_ADDR_LE_STR_LEN];
	int err;

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (conn_err) {
		printk("*** CONNECTION FAILED: %s, error: 0x%02x ***\n", addr, conn_err);
		LOG_INF("Failed to connect to %s, 0x%02x %s", addr, conn_err,
			bt_hci_err_to_str(conn_err));

		if (default_conn == conn) {
			bt_conn_unref(default_conn);
			default_conn = NULL;

			(void)k_work_submit(&scan_work);
		}

		return;
	}

	printk("*** CONNECTED TO DEVICE: %s ***\n", addr);
	LOG_INF("Connected: %s", addr);

	static struct bt_gatt_exchange_params exchange_params;

	exchange_params.func = exchange_func;
	// Request maximum MTU (247 bytes) to handle bigger packets
	err = bt_gatt_exchange_mtu(conn, &exchange_params);
	if (err) {
		LOG_WRN("MTU exchange failed (err %d)", err);
	}

	err = bt_conn_set_security(conn, BT_SECURITY_L2);
	if (err) {
		LOG_WRN("Failed to set security: %d", err);

		gatt_discover(conn);
	}

	err = bt_scan_stop();
	if ((!err) && (err != -EALREADY)) {
		LOG_ERR("Stop LE scan failed (err %d)", err);
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("*** DISCONNECTED FROM DEVICE: %s, reason: 0x%02x ***\n", addr, reason);
	LOG_INF("Disconnected: %s, reason 0x%02x %s", addr, reason, bt_hci_err_to_str(reason));

	if (default_conn != conn) {
		return;
	}

	bt_conn_unref(default_conn);
	default_conn = NULL;

	printk("*** RESTARTING SCAN AFTER DISCONNECTION ***\n");
	(void)k_work_submit(&scan_work);
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (!err) {
		LOG_INF("Security changed: %s level %u", addr, level);
	} else {
		LOG_WRN("Security failed: %s level %u err %d %s", addr, level, err,
			bt_security_err_to_str(err));
	}

	gatt_discover(conn);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed
};

static void scan_filter_match(struct bt_scan_device_info *device_info,
			      struct bt_scan_filter_match *filter_match,
			      bool connectable)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(device_info->recv_info->addr, addr, sizeof(addr));

	printk("*** DEVICE FOUND WITH NUS: %s connectable: %d ***\n", addr, connectable);
	LOG_INF("Filters matched. Address: %s connectable: %d",
		addr, connectable);
}

static void scan_connecting_error(struct bt_scan_device_info *device_info)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(device_info->recv_info->addr, addr, sizeof(addr));
	printk("*** SCAN CONNECTING ERROR: %s ***\n", addr);
	LOG_WRN("Connecting failed");
}

static void scan_connecting(struct bt_scan_device_info *device_info,
			    struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(device_info->recv_info->addr, addr, sizeof(addr));
	printk("*** SCAN CONNECTING: %s ***\n", addr);
	default_conn = bt_conn_ref(conn);
}

static int nus_client_init(void)
{
	int err;
	struct bt_nus_client_init_param init = {
		.cb = {
			.received = ble_data_received,
			.sent = ble_data_sent,
		}
	};

	err = bt_nus_client_init(&nus_client, &init);
	if (err) {
		LOG_ERR("NUS Client initialization failed (err %d)", err);
		return err;
	}

	LOG_INF("NUS Client module initialized");
	return err;
}

BT_SCAN_CB_INIT(scan_cb, scan_filter_match, NULL,
		scan_connecting_error, scan_connecting);

static void try_add_address_filter(const struct bt_bond_info *info, void *user_data)
{
	int err;
	char addr[BT_ADDR_LE_STR_LEN];
	uint8_t *filter_mode = user_data;

	bt_addr_le_to_str(&info->addr, addr, sizeof(addr));

	struct bt_conn *conn = bt_conn_lookup_addr_le(BT_ID_DEFAULT, &info->addr);

	if (conn) {
		bt_conn_unref(conn);
		return;
	}

	err = bt_scan_filter_add(BT_SCAN_FILTER_TYPE_ADDR, &info->addr);
	if (err) {
		LOG_ERR("Address filter cannot be added (err %d): %s", err, addr);
		return;
	}

	LOG_INF("Address filter added: %s", addr);
	*filter_mode |= BT_SCAN_ADDR_FILTER;
}

static int scan_start(void)
{
	int err;
	uint8_t filter_mode = 0;

	err = bt_scan_stop();
	if (err) {
		LOG_ERR("Failed to stop scanning (err %d)", err);
		return err;
	}

	bt_scan_filter_remove_all();

	err = bt_scan_filter_add(BT_SCAN_FILTER_TYPE_UUID, BT_UUID_NUS_SERVICE);
	if (err) {
		LOG_ERR("UUID filter cannot be added (err %d", err);
		return err;
	}
	filter_mode |= BT_SCAN_UUID_FILTER;

	bt_foreach_bond(BT_ID_DEFAULT, try_add_address_filter, &filter_mode);

	err = bt_scan_filter_enable(filter_mode, false);
	if (err) {
		LOG_ERR("Filters cannot be turned on (err %d)", err);
		return err;
	}

	err = bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE);
	if (err) {
		LOG_ERR("Scanning failed to start (err %d)", err);
		return err;
	}

	LOG_INF("Scan started");
	return 0;
}

static void scan_work_handler(struct k_work *item)
{
	ARG_UNUSED(item);

	(void)scan_start();
}

static void scan_init(void)
{
	struct bt_scan_init_param scan_init = {
		.connect_if_match = true,
	};

	bt_scan_init(&scan_init);
	bt_scan_cb_register(&scan_cb);

	k_work_init(&scan_work, scan_work_handler);
	LOG_INF("Scan module initialized");
}

static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Pairing cancelled: %s", addr);
}


static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Pairing completed: %s, bonded: %d", addr, bonded);
}


static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_WRN("Pairing failed conn: %s, reason %d %s", addr, reason,
		bt_security_err_to_str(reason));
}

static struct bt_conn_auth_cb conn_auth_callbacks = {
	.cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb conn_auth_info_callbacks = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed
};

int main(void)
{
	int err;

	printk("=== Central UART Sample Starting ===\n");

	err = bt_conn_auth_cb_register(&conn_auth_callbacks);
	if (err) {
		LOG_ERR("Failed to register authorization callbacks.");
		return 0;
	}
	printk("Authorization callbacks registered\n");

	err = bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
	if (err) {
		printk("Failed to register authorization info callbacks.\n");
		return 0;
	}
	printk("Authorization info callbacks registered\n");

	printk("Starting Bluetooth initialization...\n");
	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return 0;
	}
	LOG_INF("Bluetooth initialized");
	printk("Bluetooth initialized successfully\n");

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		printk("Loading settings...\n");
		settings_load();
		printk("Settings loaded\n");
	}

	printk("Initializing UART...\n");
	err = uart_init();
	if (err != 0) {
		LOG_ERR("uart_init failed (err %d)", err);
		return 0;
	}
	printk("UART initialized successfully\n");

	printk("Initializing NUS client...\n");
	err = nus_client_init();
	if (err != 0) {
		LOG_ERR("nus_client_init failed (err %d)", err);
		return 0;
	}
	printk("NUS client initialized successfully\n");

	printk("Initializing scan...\n");
	scan_init();
	printk("Scan initialized\n");
	
	printk("Starting scan...\n");
	err = scan_start();
	if (err) {
		printk("Scan start failed (err %d)\n", err);
		return 0;
	}
	printk("Scan started successfully\n");

	printk("Starting Bluetooth Central UART sample\n");

	struct uart_data_t nus_data = {
		.len = 0,
	};

	for (;;) {
		/* True UART bridge: NUS ↔ USB CDC */
		
		// Check for data from USB CDC (screen input) and send to NUS
		static uint8_t cdc_buffer[UART_BUF_SIZE];
		static int cdc_pos = 0;
		
		unsigned char c;
		int bytes_read = uart_poll_in(uart, &c);
		
		if (bytes_read == 0) { // Data received from CDC
			cdc_buffer[cdc_pos] = c;
			cdc_pos++;
			
			// Send complete command when we get newline
			if (c == '\n' || c == '\r' || cdc_pos >= UART_BUF_SIZE) {
				if (cdc_pos > 1) { // Don't send empty commands
					printk("*** SENDING COMPLETE COMMAND (%d bytes) ***\n", cdc_pos);
					err = bt_nus_client_send(&nus_client, cdc_buffer, cdc_pos);
					if (err) {
						printk("*** CDC→NUS FAILED (err %d) ***\n", err);
					} else {
						printk("*** COMMAND SENT SUCCESSFULLY ***\n");
					}
				}
				cdc_pos = 0; // Reset buffer
			}
		}
		
		// Small delay to prevent busy waiting
		k_sleep(K_MSEC(1));
	}
}
