/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <bluetooth/gatt_dm.h>
#include <zephyr/sys/byteorder.h>
#include <bluetooth/scan.h>
#include <bluetooth/services/hogp.h>
#include <dk_buttons_and_leds.h>

#include <zephyr/settings/settings.h>

#include <zephyr/logging/log.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>

/* Forward declarations for direct USB access */
extern const struct device *hid_dev;
extern struct k_sem ep_write_sem;

LOG_MODULE_REGISTER(ble_mouthpad, LOG_LEVEL_INF);

/**
 * Switch between boot protocol and report protocol mode.
 */
#define KEY_BOOTMODE_MASK DK_BTN2_MSK
/**
 * Switch CAPSLOCK state.
 *
 * @note
 * For simplicity of the code it works only in boot mode.
 */
#define KEY_CAPSLOCK_MASK DK_BTN1_MSK
/**
 * Switch CAPSLOCK state with response
 *
 * Write CAPSLOCK with response.
 * Just for testing purposes.
 * The result should be the same like usine @ref KEY_CAPSLOCK_MASK
 */
#define KEY_CAPSLOCK_RSP_MASK DK_BTN3_MSK

/* Key used to accept or reject passkey value */
#define KEY_PAIRING_ACCEPT DK_BTN1_MSK
#define KEY_PAIRING_REJECT DK_BTN2_MSK

static struct bt_conn *default_conn;
static struct bt_hogp hogp;
static struct bt_conn *auth_conn;
static uint8_t capslock_state;

static void hids_on_ready(struct k_work *work);
static K_WORK_DEFINE(hids_ready_work, hids_on_ready);

// static void hogp_map_read_cb(struct bt_hogp *hogp, uint8_t err,
// 				const uint8_t *data, size_t size, size_t offset);

static void scan_filter_match(struct bt_scan_device_info *device_info,
			      struct bt_scan_filter_match *filter_match,
			      bool connectable)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (!filter_match->uuid.match ||
	    (filter_match->uuid.count != 1)) {

		printk("Invalid device connected\n");

		return;
	}

	const struct bt_uuid *uuid = filter_match->uuid.uuid[0];

	bt_addr_le_to_str(device_info->recv_info->addr, addr, sizeof(addr));

	printk("Filters matched on UUID 0x%04x.\nAddress: %s connectable: %s\n",
		BT_UUID_16(uuid)->val,
		addr, connectable ? "yes" : "no");
}

static void scan_connecting_error(struct bt_scan_device_info *device_info)
{
	printk("Connecting failed\n");
}

static void scan_connecting(struct bt_scan_device_info *device_info,
			    struct bt_conn *conn)
{
	default_conn = bt_conn_ref(conn);
}
/** .. include_startingpoint_scan_rst */
static void scan_filter_no_match(struct bt_scan_device_info *device_info,
				 bool connectable)
{
	int err;
	struct bt_conn *conn = NULL;
	char addr[BT_ADDR_LE_STR_LEN];

	if (device_info->recv_info->adv_type == BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
		bt_addr_le_to_str(device_info->recv_info->addr, addr,
				  sizeof(addr));
		printk("Direct advertising received from %s\n", addr);
		bt_scan_stop();

		err = bt_conn_le_create(device_info->recv_info->addr,
					BT_CONN_LE_CREATE_CONN,
					device_info->conn_param, &conn);

		if (!err) {
			default_conn = bt_conn_ref(conn);
			bt_conn_unref(conn);
		}
	}
}
/** .. include_endpoint_scan_rst */
BT_SCAN_CB_INIT(scan_cb, scan_filter_match, scan_filter_no_match,
		scan_connecting_error, scan_connecting);

static void discovery_completed_cb(struct bt_gatt_dm *dm,
				   void *context)
{
	int err;

	printk("The discovery procedure succeeded\n");

	bt_gatt_dm_data_print(dm);

	err = bt_hogp_handles_assign(dm, &hogp);
	if (err) {
		printk("Could not init HIDS client object, error: %d\n", err);
	}

	err = bt_gatt_dm_data_release(dm);
	if (err) {
		printk("Could not release the discovery data, error "
		       "code: %d\n", err);
	}
}

static void discovery_service_not_found_cb(struct bt_conn *conn,
					   void *context)
{
	printk("The service could not be found during the discovery\n");
}

static void discovery_error_found_cb(struct bt_conn *conn,
				     int err,
				     void *context)
{
	printk("The discovery procedure failed with %d\n", err);
}

static const struct bt_gatt_dm_cb discovery_cb = {
	.completed = discovery_completed_cb,
	.service_not_found = discovery_service_not_found_cb,
	.error_found = discovery_error_found_cb,
};

static void gatt_discover(struct bt_conn *conn)
{
	int err;

	if (conn != default_conn) {
		return;
	}

	err = bt_gatt_dm_start(conn, BT_UUID_HIDS, &discovery_cb, NULL);
	if (err) {
		printk("could not start the discovery procedure, error "
			"code: %d\n", err);
	}
}

static void connected(struct bt_conn *conn, uint8_t conn_err)
{
	int err;
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (conn_err) {
		printk("Failed to connect to %s, 0x%02x %s\n", addr, conn_err,
		       bt_hci_err_to_str(conn_err));
		if (conn == default_conn) {
			bt_conn_unref(default_conn);
			default_conn = NULL;

			/* This demo doesn't require active scan */
			err = bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE);
			if (err) {
				printk("Scanning failed to start (err %d)\n",
				       err);
			}
		}

		return;
	}

	printk("Connected: %s\n", addr);

	err = bt_conn_set_security(conn, BT_SECURITY_L2);
	if (err) {
		printk("Failed to set security: %d\n", err);

		gatt_discover(conn);
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];
	int err;

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (auth_conn) {
		bt_conn_unref(auth_conn);
		auth_conn = NULL;
	}

	printk("Disconnected: %s, reason 0x%02x %s\n", addr, reason, bt_hci_err_to_str(reason));

	if (bt_hogp_assign_check(&hogp)) {
		printk("HIDS client active - releasing");
		bt_hogp_release(&hogp);
	}

	if (default_conn != conn) {
		return;
	}

	bt_conn_unref(default_conn);
	default_conn = NULL;

	/* This demo doesn't require active scan */
	err = bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE);
	if (err) {
		printk("Scanning failed to start (err %d)\n", err);
	}
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (!err) {
		printk("Security changed: %s level %u\n", addr, level);
	} else {
		printk("Security failed: %s level %u err %d %s\n", addr, level, err,
		       bt_security_err_to_str(err));
	}

	gatt_discover(conn);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected        = connected,
	.disconnected     = disconnected,
	.security_changed = security_changed
};

static void scan_init(void)
{
	int err;

	struct bt_scan_init_param scan_init = {
		.connect_if_match = 1,
		.scan_param = NULL,
		.conn_param = BT_LE_CONN_PARAM_DEFAULT
	};

	bt_scan_init(&scan_init);
	bt_scan_cb_register(&scan_cb);

	err = bt_scan_filter_add(BT_SCAN_FILTER_TYPE_UUID, BT_UUID_HIDS);
	if (err) {
		printk("Scanning filters cannot be set (err %d)\n", err);

		return;
	}

	err = bt_scan_filter_enable(BT_SCAN_UUID_FILTER, false);
	if (err) {
		printk("Filters cannot be turned on (err %d)\n", err);
	}
}

static uint8_t hogp_notify_cb(struct bt_hogp *hogp,
			     struct bt_hogp_rep_info *rep,
			     uint8_t err,
			     const uint8_t *data)
{
	uint8_t size = bt_hogp_rep_size(rep);
	uint8_t i;

	if (!data) {
		return BT_GATT_ITER_STOP;
	}
	printk("Notification, id: %u, size: %u, data:",
	       bt_hogp_rep_id(rep),
	       size);
	for (i = 0; i < size; ++i) {
		printk(" 0x%x", data[i]);
	}
	printk("\n");
	
	// Handle different report types based on Report ID
	uint8_t report_id = bt_hogp_rep_id(rep);
	
	// Pass through the raw data directly - no translation needed
	// printk("Report %u raw data: size=%u, data:", report_id, size);
	// for (uint8_t i = 0; i < size; i++) {
	// 	printk(" 0x%02x", data[i]);
	// }
	// printk("\n");
	
	// Parse and forward each report ID independently
	if (size >= 1) {
		// printk("Processing Report %u: size=%u, data:", report_id, size);
		// for (uint8_t i = 0; i < size; i++) {
		// 	printk(" 0x%02x", data[i]);
		// }
		// printk("\n");

		/* Send directly to USB for zero latency */
		uint8_t report_with_id[size + 1];
		report_with_id[0] = report_id;
		for (uint8_t i = 0; i < size; i++) {
			report_with_id[i + 1] = data[i];
		}
		
		int ret = hid_int_ep_write(hid_dev, report_with_id, size + 1, NULL);
		if (ret) {
			printk("HID write error, %d", ret);
		} else {
			k_sem_take(&ep_write_sem, K_FOREVER);
			// printk("Report %u sent directly to USB", report_id);
		}
	}
	
	return BT_GATT_ITER_CONTINUE;
}

/**
 * @brief Handle boot mouse reports from BLE HID device
 * 
 * Boot mouse reports are simpler 3-4 byte reports that get forwarded
 * directly to USB as Report ID 1 (buttons + wheel format).
 * 
 * @param hogp HOGP instance
 * @param rep Report information
 * @param err Error code
 * @param data Report data
 * @return BT_GATT_ITER_CONTINUE
 */
static uint8_t hogp_boot_mouse_report(struct bt_hogp *hogp,
				     struct bt_hogp_rep_info *rep,
				     uint8_t err,
				     const uint8_t *data)
{
	uint8_t size = bt_hogp_rep_size(rep);

	if (!data) {
		return BT_GATT_ITER_STOP;
	}
	
	printk("Boot mouse report: size=%u, data:", size);
	for (uint8_t i = 0; i < size; i++) {
		printk(" 0x%02x", data[i]);
	}
	printk("\n");
	
	/* Forward boot mouse report directly to USB as Report ID 1 */
	/* Boot reports are always sent as Report ID 1 (buttons + wheel) */
	uint8_t report_with_id[size + 1];
	report_with_id[0] = 1;  // Report ID 1 for boot mouse
	
	/* Copy the raw boot mouse data */
	for (uint8_t i = 0; i < size; i++) {
		report_with_id[i + 1] = data[i];
	}
	
	/* Send directly to USB for zero latency */
	int ret = hid_int_ep_write(hid_dev, report_with_id, size + 1, NULL);
	if (ret) {
		printk("HID write error, %d", ret);
	} else {
		k_sem_take(&ep_write_sem, K_FOREVER);
		printk("Boot mouse report sent directly to USB");
	}
	
	return BT_GATT_ITER_CONTINUE;
}

static uint8_t hogp_boot_kbd_report(struct bt_hogp *hogp,
				   struct bt_hogp_rep_info *rep,
				   uint8_t err,
				   const uint8_t *data)
{
	uint8_t size = bt_hogp_rep_size(rep);
	uint8_t i;

	if (!data) {
		return BT_GATT_ITER_STOP;
	}
	printk("Notification, keyboard boot, size: %u, data:", size);
	for (i = 0; i < size; ++i) {
		printk(" 0x%x", data[i]);
	}
	printk("\n");
	return BT_GATT_ITER_CONTINUE;
}

static void hogp_ready_cb(struct bt_hogp *hogp)
{
	k_work_submit(&hids_ready_work);
}

static void hids_on_ready(struct k_work *work)
{
	int err;
	struct bt_hogp_rep_info *rep = NULL;

	printk("HIDS is ready to work\n");

	// Print HID descriptor information
	printk("HID Descriptor Information:\n");
	printk("  Protocol Mode: %s\n", 
	       bt_hogp_pm_get(&hogp) == BT_HIDS_PM_BOOT ? "BOOT" : "REPORT");
	
	// List all available reports
	while (NULL != (rep = bt_hogp_rep_next(&hogp, rep))) {
		printk("  Report ID: %u, Type: %u, Size: %u\n",
		       bt_hogp_rep_id(rep),
		       bt_hogp_rep_type(rep),
		       bt_hogp_rep_size(rep));
		
		if (bt_hogp_rep_type(rep) == BT_HIDS_REPORT_TYPE_INPUT) {
			printk("    -> Subscribing to input report\n");
			err = bt_hogp_rep_subscribe(&hogp, rep, hogp_notify_cb);
			if (err) {
				printk("    -> Subscribe error (%d)\n", err);
			}
		}
	}
	
	// Try to read the full HID descriptor
	// printk("Attempting to read full HID descriptor...\n");
	
	// // First, try to read a larger chunk to see if we can get more data
	// printk("Reading large chunk (0-255 bytes)...\n");
	// err = bt_hogp_map_read(&hogp, hogp_map_read_cb, 0, K_SECONDS(5));
	// if (err) {
	// 	printk("Failed to read large HID descriptor chunk: %d\n", err);
	// }
	
	// k_sleep(K_MSEC(500));  // Wait for callback
	
	// // Then try reading from offset 64 to get the rest
	// printk("Reading remaining chunk (64+ bytes)...\n");
	// err = bt_hogp_map_read(&hogp, hogp_map_read_cb, 64, K_SECONDS(5));
	// if (err) {
	// 	printk("Failed to read remaining HID descriptor chunk: %d\n", err);
	// }
	
	// k_sleep(K_MSEC(500));  // Wait for callback
	
	// // Try a few more offsets in case the descriptor is larger
	// for (size_t offset = 128; offset <= 256; offset += 64) {
	// 	printk("Trying offset %zu...\n", offset);
	// 	err = bt_hogp_map_read(&hogp, hogp_map_read_cb, offset, K_SECONDS(2));
	// 	if (err) {
	// 		printk("No more data at offset %zu (error: %d)\n", offset, err);
	// 		break;
	// 	}
	// 	k_sleep(K_MSEC(200));
	// }
	
	// printk("Finished attempting to read full HID descriptor\n");
	
	// Only subscribe to boot reports if we're in boot mode
	if (bt_hogp_pm_get(&hogp) == BT_HIDS_PM_BOOT) {
		if (hogp.rep_boot.kbd_inp) {
			printk("Subscribe to boot keyboard report\n");
			err = bt_hogp_rep_subscribe(&hogp,
							   hogp.rep_boot.kbd_inp,
							   hogp_boot_kbd_report);
			if (err) {
				printk("Subscribe error (%d)\n", err);
			}
		}
		if (hogp.rep_boot.mouse_inp) {
			printk("Subscribe to boot mouse report\n");
			err = bt_hogp_rep_subscribe(&hogp,
							   hogp.rep_boot.mouse_inp,
							   hogp_boot_mouse_report);
			if (err) {
				printk("Subscribe error (%d)\n", err);
			}
		}
	} else {
		printk("In REPORT mode - using report protocol reports only\n");
	}
}

static void hogp_prep_fail_cb(struct bt_hogp *hogp, int err)
{
	printk("ERROR: HIDS client preparation failed!\n");
}

static void hogp_pm_update_cb(struct bt_hogp *hogp)
{
	printk("Protocol mode updated: %s\n",
	      bt_hogp_pm_get(hogp) == BT_HIDS_PM_BOOT ?
	      "BOOT" : "REPORT");
}

// static void hogp_map_read_cb(struct bt_hogp *hogp, uint8_t err,
// 				const uint8_t *data, size_t size, size_t offset)
// {
// 	static size_t total_size = 0;
	
// 	if (err) {
// 		printk("HID descriptor read error: %d\n", err);
// 		return;
// 	}
	
// 	if (!data || size == 0) {
// 		printk("HID descriptor read complete - total size: %zu bytes\n", total_size);
// 		total_size = 0;  // Reset for next read
// 		return;
// 	}
	
// 	total_size += size;
// 	printk("HID Descriptor chunk (offset=%zu, size=%zu bytes, total=%zu):\n", offset, size, total_size);
// 	for (size_t i = 0; i < size; i++) {
// 		if (i % 16 == 0) {
// 			printk("  %04zx:", offset + i);
// 		}
// 		printk(" %02x", data[i]);
// 		if (i % 16 == 15 || i == size - 1) {
// 			printk("\n");
// 		}
// 	}
	
// 	// Try to parse the descriptor to understand the report format
// 	printk("Parsing HID descriptor chunk...\n");
	
// 	// Track the current report structure
// 	static uint8_t current_report_id = 0;
// 	static uint8_t total_bits = 0;
// 	static uint8_t report_count = 0;
// 	static uint8_t current_usage = 0;
// 	static uint8_t current_logical_min = 0;
// 	static uint8_t current_logical_max = 0;
	
// 	for (size_t i = 0; i < size; i++) {
// 		uint8_t item = data[i];
		
// 		if (item == 0x85) {  // Report ID
// 			if (i + 1 < size) {
// 				current_report_id = data[i + 1];
// 				total_bits = 0;  // Reset for new report
// 				printk("  Report ID: %u (starting new report)\n", current_report_id);
// 			}
// 		} else if (item == 0x95) {  // Report Count
// 			if (i + 1 < size) {
// 				report_count = data[i + 1];
// 				printk("  Report Count: %u\n", report_count);
// 			}
// 		} else if (item == 0x75) {  // Report Size
// 			if (i + 1 < size) {
// 				uint8_t bits = data[i + 1];
// 				total_bits += bits * report_count;
// 				printk("  Report Size: %u bits (total bits so far: %u)\n", bits, total_bits);
// 			}
// 		} else if (item == 0x09) {  // Usage
// 			if (i + 1 < size) {
// 				current_usage = data[i + 1];
// 				if (current_usage == 0x01) {
// 					printk("  Usage: Pointer (0x%02x)\n", current_usage);
// 				} else if (current_usage == 0x30) {
// 					printk("  Usage: X (0x%02x)\n", current_usage);
// 				} else if (current_usage == 0x31) {
// 					printk("  Usage: Y (0x%02x)\n", current_usage);
// 				} else if (current_usage == 0x38) {
// 					printk("  Usage: Wheel (0x%02x)\n", current_usage);
// 				} else {
// 					printk("  Usage: 0x%02x\n", current_usage);
// 				}
// 			}
// 		} else if (item == 0x19) {  // Usage Minimum
// 			if (i + 1 < size) {
// 				printk("  Usage Minimum: 0x%02x\n", data[i + 1]);
// 			}
// 		} else if (item == 0x29) {  // Usage Maximum
// 			if (i + 1 < size) {
// 				printk("  Usage Maximum: 0x%02x\n", data[i + 1]);
// 			}
// 		} else if (item == 0x15) {  // Logical Minimum
// 			if (i + 1 < size) {
// 				current_logical_min = data[i + 1];
// 				printk("  Logical Minimum: %d\n", (int8_t)current_logical_min);
// 			}
// 		} else if (item == 0x25) {  // Logical Maximum
// 			if (i + 1 < size) {
// 				current_logical_max = data[i + 1];
// 				printk("  Logical Maximum: %d\n", (int8_t)current_logical_max);
// 			}
// 		} else if (item == 0x81) {  // Input
// 			if (i + 1 < size) {
// 				uint8_t flags = data[i + 1];
// 				printk("  Input flags: 0x%02x (", flags);
// 				if (flags & 0x01) printk("Constant ");
// 				if (flags & 0x02) printk("Variable ");
// 				if (flags & 0x04) printk("Relative ");
// 				if (flags & 0x08) printk("Wrap ");
// 				if (flags & 0x10) printk("NonLinear ");
// 				if (flags & 0x20) printk("NoPreferred ");
// 				if (flags & 0x40) printk("NullState ");
// 				if (flags & 0x80) printk("Volatile ");
// 				printk(")\n");
				
// 				// Show detailed field information for Report 2
// 				if (current_report_id == 2) {
// 					printk("    -> Report 2 field: Usage=0x%02x, Size=%u bits, Count=%u, Range=[%d,%d]\n",
// 					       current_usage, total_bits, report_count, 
// 					       (int8_t)current_logical_min, (int8_t)current_logical_max);
// 				}
// 			}
// 		} else if (item == 0xC0) {  // End Collection
// 			printk("  End Collection\n");
// 			printk("  *** Report %u total size: %u bits (%u bytes)\n", 
// 			       current_report_id, total_bits, (total_bits + 7) / 8);
// 			total_bits = 0;  // Reset for next report
// 		}
// 	}
// }

/* HIDS client initialization parameters */
static const struct bt_hogp_init_params hogp_init_params = {
	.ready_cb      = hogp_ready_cb,
	.prep_error_cb = hogp_prep_fail_cb,
	.pm_update_cb  = hogp_pm_update_cb
};


static void button_bootmode(void)
{
	if (!bt_hogp_ready_check(&hogp)) {
		printk("HID device not ready\n");
		return;
	}
	int err;
	enum bt_hids_pm pm = bt_hogp_pm_get(&hogp);
	enum bt_hids_pm new_pm = ((pm == BT_HIDS_PM_BOOT) ? BT_HIDS_PM_REPORT : BT_HIDS_PM_BOOT);

	printk("Setting protocol mode: %s\n", (new_pm == BT_HIDS_PM_BOOT) ? "BOOT" : "REPORT");
	err = bt_hogp_pm_write(&hogp, new_pm);
	if (err) {
		printk("Cannot change protocol mode (err %d)\n", err);
	}
}

static void hidc_write_cb(struct bt_hogp *hidc,
			  struct bt_hogp_rep_info *rep,
			  uint8_t err)
{
	printk("Caps lock sent\n");
}

static void button_capslock(void)
{
	int err;
	uint8_t data;

	if (!bt_hogp_ready_check(&hogp)) {
		printk("HID device not ready\n");
		return;
	}
	if (!hogp.rep_boot.kbd_out) {
		printk("HID device does not have Keyboard OUT report\n");
		return;
	}
	if (bt_hogp_pm_get(&hogp) != BT_HIDS_PM_BOOT) {
		printk("This function works only in BOOT Report mode\n");
		return;
	}
	capslock_state = capslock_state ? 0 : 1;
	data = capslock_state ? 0x02 : 0;
	err = bt_hogp_rep_write_wo_rsp(&hogp, hogp.rep_boot.kbd_out,
				       &data, sizeof(data),
				       hidc_write_cb);

	if (err) {
		printk("Keyboard data write error (err: %d)\n", err);
		return;
	}
	printk("Caps lock send (val: 0x%x)\n", data);
}


static uint8_t capslock_read_cb(struct bt_hogp *hogp,
			     struct bt_hogp_rep_info *rep,
			     uint8_t err,
			     const uint8_t *data)
{
	if (err) {
		printk("Capslock read error (err: %u)\n", err);
		return BT_GATT_ITER_STOP;
	}
	if (!data) {
		printk("Capslock read - no data\n");
		return BT_GATT_ITER_STOP;
	}
	printk("Received data (size: %u, data[0]: 0x%x)\n",
	       bt_hogp_rep_size(rep), data[0]);

	return BT_GATT_ITER_STOP;
}


static void capslock_write_cb(struct bt_hogp *hogp,
			      struct bt_hogp_rep_info *rep,
			      uint8_t err)
{
	int ret;

	printk("Capslock write result: %u\n", err);

	ret = bt_hogp_rep_read(hogp, rep, capslock_read_cb);
	if (ret) {
		printk("Cannot read capslock value (err: %d)\n", ret);
	}
}


static void button_capslock_rsp(void)
{
	if (!bt_hogp_ready_check(&hogp)) {
		printk("HID device not ready\n");
		return;
	}
	if (!hogp.rep_boot.kbd_out) {
		printk("HID device does not have Keyboard OUT report\n");
		return;
	}
	int err;
	uint8_t data;

	capslock_state = capslock_state ? 0 : 1;
	data = capslock_state ? 0x02 : 0;
	err = bt_hogp_rep_write(&hogp, hogp.rep_boot.kbd_out, capslock_write_cb,
				&data, sizeof(data));
	if (err) {
		printk("Keyboard data write error (err: %d)\n", err);
		return;
	}
	printk("Caps lock send using write with response (val: 0x%x)\n", data);
}


static void num_comp_reply(bool accept)
{
	if (accept) {
		bt_conn_auth_passkey_confirm(auth_conn);
		printk("Numeric Match, conn %p\n", auth_conn);
	} else {
		bt_conn_auth_cancel(auth_conn);
		printk("Numeric Reject, conn %p\n", auth_conn);
	}

	bt_conn_unref(auth_conn);
	auth_conn = NULL;
}


static void button_handler(uint32_t button_state, uint32_t has_changed)
{
	uint32_t button = button_state & has_changed;

	if (auth_conn) {
		if (button & KEY_PAIRING_ACCEPT) {
			num_comp_reply(true);
		}

		if (button & KEY_PAIRING_REJECT) {
			num_comp_reply(false);
		}

		return;
	}

	if (button & KEY_BOOTMODE_MASK) {
		button_bootmode();
	}
	if (button & KEY_CAPSLOCK_MASK) {
		button_capslock();
	}
	if (button & KEY_CAPSLOCK_RSP_MASK) {
		button_capslock_rsp();
	}
}


static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Passkey for %s: %06u\n", addr, passkey);
}


static void auth_passkey_confirm(struct bt_conn *conn, unsigned int passkey)
{
	char addr[BT_ADDR_LE_STR_LEN];

	auth_conn = bt_conn_ref(conn);

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Passkey for %s: %06u\n", addr, passkey);

	if (IS_ENABLED(CONFIG_SOC_SERIES_NRF54HX) || IS_ENABLED(CONFIG_SOC_SERIES_NRF54LX)) {
		printk("Press Button 0 to confirm, Button 1 to reject.\n");
	} else {
		printk("Press Button 1 to confirm, Button 2 to reject.\n");
	}
}


static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Pairing cancelled: %s\n", addr);
}


static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Pairing completed: %s, bonded: %d\n", addr, bonded);
}


static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Pairing failed conn: %s, reason %d %s\n", addr, reason,
	       bt_security_err_to_str(reason));
}

static struct bt_conn_auth_cb conn_auth_callbacks = {
	.passkey_display = auth_passkey_display,
	.passkey_confirm = auth_passkey_confirm,
	.cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb conn_auth_info_callbacks = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed
};


int ble_init(void)
{
	LOG_INF("Initializing BLE stack...");
	int err;

	printk("Starting Bluetooth Central HIDS\n");

	bt_hogp_init(&hogp, &hogp_init_params);

	err = bt_conn_auth_cb_register(&conn_auth_callbacks);
	if (err) {
		printk("failed to register authorization callbacks.\n");
		return 0;
	}

	err = bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
	if (err) {
		printk("Failed to register authorization info callbacks.\n");
		return 0;
	}

	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return 0;
	}

	printk("Bluetooth initialized\n");

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}

	scan_init();

	err = dk_buttons_init(button_handler);
	if (err) {
		printk("Failed to initialize buttons (err %d)\n", err);
		return 0;
	}

	err = bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE);
	if (err) {
		printk("Scanning failed to start (err %d)\n", err);
		return 0;
	}

	printk("Scanning successfully started\n");
	return 0;
}
