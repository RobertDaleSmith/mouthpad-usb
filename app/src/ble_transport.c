/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/bluetooth/addr.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>

#include "ble_transport.h"
#include "ble_central.h"
#include "ble_nus_client.h"
#include "ble_hid.h"
#include "ble_bas.h"
#include "usb_cdc.h"
#include "usb_hid.h"

#define LOG_MODULE_NAME ble_transport
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

/* USB CDC callback */
static usb_cdc_send_cb_t usb_cdc_send_callback = NULL;

/* NUS Bridge state */
static bool nus_client_ready = false;
static bool mtu_exchange_complete = false;
static bool bridging_started = false;

/* HID Bridge state */
static bool hid_client_ready = false;
static bool hid_discovery_complete = false;

/* Connection state tracking for sound effects */
static bool fully_connected = false;

/* Data activity tracking for LED indication */
static bool data_activity = false;
static int64_t last_data_time = 0;

/* RSSI tracking - stored from advertising during scan */
static int8_t last_known_rssi = 0;

/* Connected device name tracking */
static char connected_device_name[32] = "MouthPad USB";  /* Shortened to fit 12 char limit */

/* RSSI reading infrastructure */
static struct k_work_delayable rssi_read_work;
static bool rssi_reading_active = false;

/* HID Bridge callbacks */
static ble_data_callback_t hid_data_callback = NULL;
static ble_ready_callback_t hid_ready_callback = NULL;

/* Internal callback functions */
static void ble_nus_data_received_cb(const uint8_t *data, uint16_t len);
static void rssi_read_work_handler(struct k_work *work);
static void ble_nus_discovery_complete_cb(void);
static void ble_nus_mtu_exchange_cb(uint16_t mtu);
static void ble_hid_data_received_cb(const uint8_t *data, uint16_t len);
static void ble_hid_discovery_complete_cb(void);
static void ble_central_connected_cb(struct bt_conn *conn);
static void ble_central_disconnected_cb(struct bt_conn *conn, uint8_t reason);
static void gatt_discover(struct bt_conn *conn);

static bool nus_discovery_complete = false;

static void nus_discovery_completed_cb(void)
{
	LOG_INF("=== NUS DISCOVERY COMPLETED ===");
	nus_discovery_complete = true;
	
	/* Now start HID discovery after NUS is complete */
	LOG_INF("Starting HID service discovery after NUS completion...");
	int hid_discover_ret = ble_hid_discover(ble_central_get_default_conn());
	if (hid_discover_ret != 0) {
		LOG_ERR("BLE HID discovery failed (err %d)", hid_discover_ret);
		LOG_ERR("This might mean the connected device doesn't have HID services");
		LOG_ERR("Or there might be a GATT discovery conflict");
	} else {
		LOG_INF("BLE HID discovery started successfully");
	}
}

/* BLE Transport initialization */
int ble_transport_init(void)
{
	int err;

	/* Register BLE Central callbacks */
	ble_central_register_connected_cb(ble_central_connected_cb);
	ble_central_register_disconnected_cb(ble_central_disconnected_cb);

	/* Initialize BLE Central */
	err = ble_central_init();
	if (err != 0) {
		LOG_ERR("ble_central_init failed (err %d)", err);
		return err;
	}

	/* Register NUS Client callbacks */
	ble_nus_client_register_data_received_cb(ble_nus_data_received_cb);
	ble_nus_client_register_discovery_complete_cb(ble_nus_discovery_complete_cb);
	ble_nus_client_register_mtu_exchange_cb(ble_nus_mtu_exchange_cb);
	
	/* Register HID Client callbacks */
	LOG_INF("Registering BLE HID callbacks...");
	ble_hid_register_data_received_cb(ble_hid_data_received_cb);
	ble_hid_register_ready_cb(ble_hid_discovery_complete_cb);
	LOG_INF("BLE HID callbacks registered successfully");
	
	/* Register USB CDC callback */
	ble_transport_register_usb_cdc_callback((usb_cdc_send_cb_t)usb_cdc_send_data);

	/* Initialize NUS client */
	err = ble_nus_client_init();
	if (err != 0) {
		LOG_ERR("ble_nus_client_init failed (err %d)", err);
		return err;
	}
	LOG_INF("BLE NUS client initialized successfully");

	/* Initialize Battery Service client */
	err = ble_bas_init();
	if (err != 0) {
		LOG_ERR("ble_bas_init failed (err %d)", err);
		return err;
	}

	/* Initialize HID client */
	err = ble_hid_init();
	if (err != 0) {
		LOG_ERR("ble_hid_init failed (err %d)", err);
		return err;
	}
	LOG_INF("BLE HID client initialized successfully");

	/* Initialize RSSI reading work */
	k_work_init_delayable(&rssi_read_work, rssi_read_work_handler);

	/* Start scanning */
	err = ble_central_start_scan();
	if (err) {
		LOG_ERR("Scan start failed (err %d)", err);
		return err;
	}

	return 0;
}

/* Transport registration functions */
int ble_transport_register_usb_cdc_callback(usb_cdc_send_cb_t cb)
{
	usb_cdc_send_callback = cb;
	return 0;
}

int ble_transport_register_usb_hid_callback(ble_data_callback_t cb)
{
	hid_data_callback = cb;
	return 0;
}

int ble_transport_start_bridging(void)
{
	bridging_started = true;
	LOG_INF("BLE Transport bridging started");
	return 0;
}

int ble_transport_send_nus_data(const uint8_t *data, uint16_t len)
{
	if (!nus_client_ready) {
		LOG_WRN("NUS client not ready");
		return -ENOTCONN;
	}

	LOG_INF("BLE Transport sending %d bytes to NUS", len);
	int err = ble_nus_client_send_data(data, len);
	if (err) {
		LOG_ERR("BLE Transport send failed: %d", err);
	} else {
		LOG_INF("BLE Transport send successful");
		data_activity = true;  // Mark data activity for LED indication
	}
	return err;
}

bool ble_transport_is_nus_ready(void)
{
	return nus_client_ready;
}

/* Future HID Transport functions */
int ble_transport_register_hid_data_callback(ble_data_callback_t cb)
{
	hid_data_callback = cb;
	return 0;
}

int ble_transport_register_hid_ready_callback(ble_ready_callback_t cb)
{
	hid_ready_callback = cb;
	return 0;
}

int ble_transport_send_hid_data(const uint8_t *data, uint16_t len)
{
	if (!hid_client_ready) {
		LOG_WRN("HID client not ready");
		return -ENOTCONN;
	}

	LOG_DBG("BLE Transport sending %d bytes to HID", len);
	int err = ble_hid_send_report(data, len);
	if (err) {
		LOG_ERR("BLE Transport HID send failed: %d", err);
	} else {
		LOG_DBG("BLE Transport HID send successful");
		data_activity = true;  // Mark data activity for LED indication
	}
	return err;
}

bool ble_transport_is_hid_ready(void)
{
	return hid_client_ready;
}

/* Internal callback functions */
static void ble_nus_data_received_cb(const uint8_t *data, uint16_t len)
{
	LOG_INF("NUS data received: %d bytes", len);
	
	// Only process data after MTU exchange is complete
	if (!mtu_exchange_complete) {
		LOG_DBG("Skipping data - MTU exchange not complete");
		return;
	}
	
	// Debug: Log the first few bytes to see what we're getting
	if (len > 0) {
		LOG_INF("First bytes: %02x %02x %02x %02x", 
			data[0], len > 1 ? data[1] : 0, len > 2 ? data[2] : 0, len > 3 ? data[3] : 0);
	}
	
	// Filter out 2-byte echo responses (73 XX format)
	if (len == 2 && data[0] == 0x73) {
		LOG_DBG("Skipping 2-byte echo: 73 %02x", data[1]);
		return;
	}
	
	// Don't echo back single characters (likely echo from our input)
	if (len == 1) {
		LOG_DBG("Skipping single character echo");
		return;
	}
	
	// For larger packets, try to identify the structure
	if (len >= 4) {
		LOG_INF("PACKET STRUCTURE: Type=0x%02x, Length=%d", data[0], len);
	}
	
	// Mark data activity for LED indication
	data_activity = true;
	last_data_time = k_uptime_get();
	LOG_DBG("=== DATA ACTIVITY MARKED ===");
	
	// Bridge NUS data directly to USB CDC
	if (usb_cdc_send_callback) {
		usb_cdc_send_callback(data, len);
	}
}

static void ble_nus_mtu_exchange_cb(uint16_t mtu)
{
	LOG_INF("MTU exchange completed: %d bytes", mtu);
	mtu_exchange_complete = true;
}

static void ble_nus_discovery_complete_cb(void)
{
	LOG_INF("NUS client ready - service discovery complete");
	nus_client_ready = true;
	LOG_INF("NUS client ready - bridge operational");
	
	/* Trigger HID discovery after NUS discovery completes */
	nus_discovery_completed_cb();
}

static void ble_hid_data_received_cb(const uint8_t *data, uint16_t len)
{
	LOG_DBG("=== BLE HID DATA RECEIVED ===");
	LOG_DBG("HID data received: %d bytes", len);
	LOG_DBG("HID discovery status: ready=%d, complete=%d", hid_client_ready, hid_discovery_complete);
	
	// Only process data after HID discovery is complete
	if (!hid_discovery_complete) {
		LOG_DBG("Skipping HID data - HID discovery not complete");
		return;
	}
	
	// Debug: Log the first few bytes to see what we're getting
	if (len > 0) {
		LOG_DBG("HID First bytes: %02x %02x %02x %02x", 
			data[0], len > 1 ? data[1] : 0, len > 2 ? data[2] : 0, len > 3 ? data[3] : 0);
	}
	
	// Filter out 2-byte echo responses (73 XX format)
	if (len == 2 && data[0] == 0x73) {
		LOG_DBG("Skipping 2-byte echo: 73 %02x", data[1]);
		return;
	}
	
	// Don't echo back single characters (likely echo from our input)
	if (len == 1) {
		LOG_DBG("Skipping single character echo");
		return;
	}
	
	// For larger packets, try to identify the structure
	if (len >= 4) {
		LOG_INF("HID PACKET STRUCTURE: Type=0x%02x, Length=%d", data[0], len);
	}
	
	// Mark data activity for LED indication
	data_activity = true;
	last_data_time = k_uptime_get();
	LOG_DBG("=== DATA ACTIVITY MARKED ===");
	
	// Bridge HID data directly to USB HID
	// Note: HID data is already sent directly to USB in ble_hid.c for zero latency
	// No need to duplicate the USB sending here to avoid semaphore conflicts
	if (hid_data_callback) {
		LOG_DBG("Calling USB HID callback with %d bytes", len);
		hid_data_callback(data, len);
	} else {
		LOG_DBG("No USB HID callback registered (normal - direct USB sending used)");
	}
}

static void ble_hid_discovery_complete_cb(void)
{
	LOG_INF("=== BLE HID DISCOVERY COMPLETE ===");
	LOG_INF("HID client ready - service discovery complete");
	hid_client_ready = true;
	hid_discovery_complete = true;
	fully_connected = true;  /* Mark as fully connected - eligible for disconnect sound */
	LOG_INF("HID client ready - bridge operational");
	LOG_INF("BLE HID discovery status: ready=%d, complete=%d", hid_client_ready, hid_discovery_complete);
	
	/* Play happy connection sound - full bridge is now operational! */
	extern void buzzer_connected(void);
	buzzer_connected();
	
	/* Start Battery Service discovery after HID is complete */
	ble_bas_discover(ble_central_get_default_conn());
}

static void gatt_discover(struct bt_conn *conn)
{
	if (conn != ble_central_get_default_conn()) {
		return;
	}

	LOG_INF("Starting GATT discovery for both NUS and HID services");
	char addr_str[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr_str, sizeof(addr_str));
	LOG_INF("Connected device address: %s", addr_str);

	/* Reset discovery state */
	nus_discovery_complete = false;

	/* Discover NUS service first */
	LOG_INF("Starting NUS service discovery...");
	ble_nus_client_discover(conn);
	
	/* HID discovery will be started in nus_discovery_completed_cb */
}

static void ble_central_connected_cb(struct bt_conn *conn)
{
	int err;

	LOG_INF("BLE Central connected - starting setup");

	/* Update display to show pairing status */
	extern int oled_display_pairing(void);
	oled_display_pairing();

	/* Request optimal connection parameters for better signal and responsiveness */
	struct bt_le_conn_param conn_params = {
		.interval_min = 16,    /* 20ms (16 * 1.25ms) */
		.interval_max = 40,    /* 50ms (40 * 1.25ms) */
		.latency = 0,          /* No latency for responsiveness */
		.timeout = 400         /* 4 seconds (400 * 10ms) */
	};
	
	err = bt_conn_le_param_update(conn, &conn_params);
	if (err) {
		LOG_WRN("Failed to request connection parameter update (err %d)", err);
	} else {
		LOG_INF("Connection parameter update requested (20-50ms interval, 0 latency)");
	}

#if defined(CONFIG_BT_USER_PHY_UPDATE)
	/* Request PHY update for better throughput or range */
	struct bt_conn_le_phy_param phy_params = {
		.options = BT_CONN_LE_PHY_OPT_NONE,
		.pref_tx_phy = BT_GAP_LE_PHY_2M | BT_GAP_LE_PHY_CODED, /* Prefer 2M for speed or Coded for range */
		.pref_rx_phy = BT_GAP_LE_PHY_2M | BT_GAP_LE_PHY_CODED
	};
	
	err = bt_conn_le_phy_update(conn, &phy_params);
	if (err) {
		LOG_WRN("Failed to request PHY update (err %d)", err);
	} else {
		LOG_INF("PHY update requested (2M or Coded PHY for better signal)");
	}
#endif

	// Perform MTU exchange using the NUS client module
	err = ble_nus_client_exchange_mtu(conn);
	if (err) {
		LOG_WRN("MTU exchange failed (err %d)", err);
	} else {
		LOG_INF("MTU exchange initiated successfully");
	}

	err = bt_conn_set_security(conn, BT_SECURITY_L2);
	if (err) {
		LOG_WRN("Failed to set security: %d", err);
		gatt_discover(conn);
	} else {
		LOG_INF("Security setup successful");
		gatt_discover(conn);
	}

	/* Start periodic RSSI reading */
	rssi_reading_active = true;
	k_work_schedule(&rssi_read_work, K_SECONDS(2));  /* First read in 2 seconds */
	LOG_INF("Started periodic RSSI reading");
}

static void ble_central_disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	
	LOG_INF("BLE Central disconnected (reason: 0x%02x) - cleaning up and resetting states", reason);
	
	/* Only play disconnection sound if we were fully connected (not during connection failures) */
	if (fully_connected) {
		extern void buzzer_disconnected(void);
		buzzer_disconnected();
		LOG_INF("Played disconnection sound (was fully connected)");
	} else {
		LOG_INF("Skipped disconnection sound (was not fully connected)");
	}
	
	/* Send USB HID release-all report to prevent stuck inputs */
	LOG_INF("Sending USB HID release-all to clear any stuck inputs");
	int ret = usb_hid_send_release_all();
	if (ret != 0) {
		LOG_ERR("Failed to send USB HID release-all (err %d)", ret);
	}
	
	// Reset ready states for both NUS and HID
	nus_client_ready = false;
	hid_client_ready = false;
	hid_discovery_complete = false;

	/* Stop periodic RSSI reading */
	rssi_reading_active = false;
	k_work_cancel_delayable(&rssi_read_work);
	LOG_INF("Stopped periodic RSSI reading");
	mtu_exchange_complete = false;
	nus_discovery_complete = false;
	fully_connected = false;
	
	/* Reset device name to default */
	strncpy(connected_device_name, "MouthPad USB", sizeof(connected_device_name) - 1);
	connected_device_name[sizeof(connected_device_name) - 1] = '\0';
	
	// Reset battery service state
	ble_bas_reset();
	
	// Release HOGP if active - like Nordic sample does
	extern struct bt_hogp *ble_hid_get_hogp(void);
	extern bool bt_hogp_assign_check(const struct bt_hogp *hogp);
	extern void bt_hogp_release(struct bt_hogp *hogp);
	
	struct bt_hogp *hogp = ble_hid_get_hogp();
	if (bt_hogp_assign_check(hogp)) {
		LOG_INF("HIDS client active - releasing");
		bt_hogp_release(hogp);
	}
	
	LOG_INF("BLE Central disconnected - cleanup complete, ready for new connection");
}

bool ble_transport_is_connected(void)
{
	return nus_client_ready || hid_client_ready;
}

bool ble_transport_has_data_activity(void)
{
	// Check if we've had data activity within the last 100ms
	int64_t current_time = k_uptime_get();
	if (data_activity && (current_time - last_data_time) < 100) {
		LOG_DBG("BLE data activity detected (time diff: %lld ms)", current_time - last_data_time);
		return true;
	}
	
	// Reset if it's been too long
	if ((current_time - last_data_time) >= 100) {
		data_activity = false;
	}
	
	return false;
}

void ble_transport_mark_data_activity(void)
{
	data_activity = true;
	last_data_time = k_uptime_get();
	LOG_DBG("=== DATA ACTIVITY MARKED (DIRECT) ===");
}

int8_t ble_transport_get_rssi(void)
{
	static int64_t last_rssi_read_time = 0;
	static uint32_t rssi_read_attempts = 0;
	
	if (!ble_transport_is_connected()) {
		return 0;  /* Return 0 for no connection */
	}
	
	/* Only try to read RSSI every 2 seconds to avoid overwhelming the system */
	int64_t current_time = k_uptime_get();
	if (current_time - last_rssi_read_time < 2000) {
		LOG_DBG("Using cached RSSI: %d dBm", last_known_rssi);
		return last_known_rssi;
	}
	
	last_rssi_read_time = current_time;
	rssi_read_attempts++;
	
	/* Get the connection and try to read real RSSI */
	int8_t current_rssi = last_known_rssi;
	struct bt_conn *conn = ble_central_get_default_conn();
	
	if (conn) {
		/* Log connection info for debugging */
		struct bt_conn_info info;
		int err = bt_conn_get_info(conn, &info);
		if (err == 0 && info.type == BT_CONN_TYPE_LE) {
			LOG_DBG("Connection info - interval: %d, latency: %d, timeout: %d",
			        info.le.interval, info.le.latency, info.le.timeout);
			
#if defined(CONFIG_BT_USER_PHY_UPDATE)
			if (info.le.phy) {
				LOG_DBG("PHY info - TX: %d, RX: %d", 
				        info.le.phy->tx_phy, info.le.phy->rx_phy);
			}
#endif
#if defined(CONFIG_BT_USER_DATA_LEN_UPDATE)
			if (info.le.data_len) {
				LOG_DBG("Data len - TX max: %d, RX max: %d",
				        info.le.data_len->tx_max_len, info.le.data_len->rx_max_len);
			}
#endif
		}
		
		/* Use the real RSSI value (updated by periodic nRF controller reads) */
		current_rssi = last_known_rssi;
	}
	
	/* Bound the RSSI to reasonable values */
	if (current_rssi > -20) current_rssi = -20;
	if (current_rssi < -100) current_rssi = -100;
	
	LOG_DBG("Current RSSI: %d dBm (real measurement)", current_rssi);
	
	return current_rssi;
}


/* RSSI work handler - reads actual connection RSSI using HCI command */
static void rssi_read_work_handler(struct k_work *work)
{
	if (!ble_transport_is_connected()) {
		/* Stop RSSI reading if not connected */
		rssi_reading_active = false;
		return;
	}
	
	struct bt_conn *conn = ble_central_get_default_conn();
	if (!conn) {
		LOG_WRN("No active connection for RSSI reading");
		return;
	}
	
	/* Read actual connection RSSI using HCI command (based on zephyr/samples/bluetooth/hci_pwr_ctrl) */
	struct net_buf *buf, *rsp = NULL;
	struct bt_hci_cp_read_rssi *cp;
	struct bt_hci_rp_read_rssi *rp;
	int err;
	
	/* Allocate HCI command buffer */
	buf = bt_hci_cmd_alloc(K_FOREVER);
	if (!buf) {
		LOG_ERR("Failed to allocate HCI buffer for RSSI read");
		goto schedule_next;
	}
	
	cp = net_buf_add(buf, sizeof(*cp));
	cp->handle = sys_cpu_to_le16(bt_conn_index(conn));
	
	/* Send synchronous HCI Read RSSI command */
	err = bt_hci_cmd_send_sync(BT_HCI_OP_READ_RSSI, buf, &rsp);
	if (err) {
		LOG_ERR("HCI Read RSSI failed (err %d)", err);
		goto schedule_next;
	}
	
	rp = (void *)rsp->data;
	if (rp->status) {
		LOG_ERR("HCI Read RSSI command failed (status 0x%02x)", rp->status);
		goto cleanup_and_schedule;
	}
	
	/* Update RSSI with real measurement */
	int8_t new_rssi = rp->rssi;
	
	/* Log RSSI updates */
	static uint32_t rssi_read_count = 0;
	rssi_read_count++;
	
	if (rssi_read_count == 1) {
		LOG_INF("=== REAL-TIME CONNECTION RSSI ===");
		LOG_INF("Successfully reading live connection RSSI via HCI commands!");
		LOG_INF("Initial connection RSSI: %d dBm", new_rssi);
	} else if (new_rssi != last_known_rssi) {
		LOG_INF("RSSI CHANGE: %d -> %d dBm (signal strength updated)", last_known_rssi, new_rssi);
	} else if (rssi_read_count % 15 == 0) {  /* Every 30 seconds */
		LOG_INF("Connection RSSI: %d dBm (stable)", new_rssi);
	}
	
	last_known_rssi = new_rssi;
	
cleanup_and_schedule:
	if (rsp) {
		net_buf_unref(rsp);
	}

schedule_next:
	/* Schedule next RSSI reading in 2 seconds */
	if (rssi_reading_active) {
		k_work_schedule(&rssi_read_work, K_SECONDS(2));
	}
}

void ble_transport_set_rssi(int8_t rssi)
{
	last_known_rssi = rssi;
	LOG_DBG("RSSI updated to %d dBm", rssi);
}

void ble_transport_set_device_name(const char *name)
{
	if (name) {
		strncpy(connected_device_name, name, sizeof(connected_device_name) - 1);
		connected_device_name[sizeof(connected_device_name) - 1] = '\0';
		LOG_INF("Connected device name set to: %s", connected_device_name);
	}
}

const char *ble_transport_get_device_name(void)
{
	return connected_device_name;
}

void ble_transport_disconnect(void)
{
	struct bt_conn *conn = ble_central_get_default_conn();
	if (conn) {
		LOG_INF("Disconnecting BLE connection...");
		int err = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		if (err) {
			LOG_ERR("Failed to disconnect BLE connection (err %d)", err);
		}
	} else {
		LOG_WRN("No active BLE connection to disconnect");
	}
}

void ble_transport_clear_bonds(void)
{
	LOG_INF("Clearing all BLE bonds...");

	/* Clear all bonds */
	int err = bt_unpair(BT_ID_DEFAULT, NULL);
	if (err) {
		LOG_ERR("Failed to clear bonds (err %d)", err);
		return;
	}

	/* Clear bonded device tracking in ble_central */
	extern void ble_central_clear_bonded_device(void);
	ble_central_clear_bonded_device();

	LOG_INF("All BLE bonds cleared successfully");
}
