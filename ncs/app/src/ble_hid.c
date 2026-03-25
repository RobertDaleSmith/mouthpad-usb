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
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_hid.h>

#include "ble_hid.h"

/* Forward declarations for direct USB access */
extern const struct device *hid_dev;
extern struct k_sem ep_write_sem;

/* Forward declaration for connection state check */
extern bool ble_transport_is_connected(void);

LOG_MODULE_REGISTER(ble_hid, LOG_LEVEL_INF);

/* Callback registration */
static ble_hid_data_received_cb_t data_received_callback = NULL;
static ble_hid_ready_cb_t ready_callback = NULL;

/**
 * Consumer Control usage codes for backwards compatibility.
 * Maps old 1-byte bitmap format to new 16-bit usage selector format.
 * Old firmware used 8 individual bit flags, new descriptor uses usage codes.
 */
static const uint16_t consumer_bitmap_to_usage[] = {
	0x00CD,  /* bit 0: Play/Pause */
	0x0183,  /* bit 1: AL Consumer Control Configuration */
	0x00B5,  /* bit 2: Scan Next Track */
	0x00B6,  /* bit 3: Scan Previous Track */
	0x00EA,  /* bit 4: Volume Decrement */
	0x00E9,  /* bit 5: Volume Increment */
	0x0225,  /* bit 6: AC Forward */
	0x0224,  /* bit 7: AC Back */
};

/**
 * Translate old consumer control bitmap to new 16-bit usage format.
 * Returns the usage code for the first set bit, or 0 if no bits set.
 */
static uint16_t translate_consumer_bitmap(uint8_t bitmap)
{
	if (bitmap == 0) {
		return 0;
	}
	/* Find first set bit and return corresponding usage */
	for (int i = 0; i < 8; i++) {
		if (bitmap & (1 << i)) {
			return consumer_bitmap_to_usage[i];
		}
	}
	return 0;
}

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

static struct bt_hogp hogp;
static uint8_t capslock_state;
static bool hid_ready = false;

static void hids_on_ready(struct k_work *work);
static K_WORK_DEFINE(hids_ready_work, hids_on_ready);

/* HOGP callback functions */
static uint8_t hogp_notify_cb(struct bt_hogp *hogp,
			     struct bt_hogp_rep_info *rep,
			     uint8_t err,
			     const uint8_t *data);
static uint8_t hogp_boot_mouse_report(struct bt_hogp *hogp,
				     struct bt_hogp_rep_info *rep,
				     uint8_t err,
				     const uint8_t *data);
static uint8_t hogp_boot_kbd_report(struct bt_hogp *hogp,
				   struct bt_hogp_rep_info *rep,
				   uint8_t err,
				   const uint8_t *data);
static void hogp_ready_cb(struct bt_hogp *hogp);
static void hogp_prep_fail_cb(struct bt_hogp *hogp, int err);
static void hogp_pm_update_cb(struct bt_hogp *hogp);

/* Button handler functions */
static void button_bootmode(void);
static void button_capslock(void);
static void button_capslock_rsp(void);
static void hidc_write_cb(struct bt_hogp *hidc,
			  struct bt_hogp_rep_info *rep,
			  uint8_t err);
static uint8_t capslock_read_cb(struct bt_hogp *hogp,
			     struct bt_hogp_rep_info *rep,
			     uint8_t err,
			     const uint8_t *data);
static void capslock_write_cb(struct bt_hogp *hogp,
			      struct bt_hogp_rep_info *rep,
			      uint8_t err);

/* Discovery callback functions */
static void discovery_completed_cb(struct bt_gatt_dm *dm, void *context);
static void discovery_service_not_found_cb(struct bt_conn *conn, void *context);
static void discovery_error_found_cb(struct bt_conn *conn, int err, void *context);

static const struct bt_gatt_dm_cb discovery_cb = {
	.completed = discovery_completed_cb,
	.service_not_found = discovery_service_not_found_cb,
	.error_found = discovery_error_found_cb,
};

/* HOGP initialization parameters */
static const struct bt_hogp_init_params hogp_init_params = {
	.ready_cb      = hogp_ready_cb,
	.prep_error_cb = hogp_prep_fail_cb,
	.pm_update_cb  = hogp_pm_update_cb
};

/* HOGP callback implementations */
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

	/* Check if still connected - prevent forwarding stale HID data during disconnect */
	if (!ble_transport_is_connected()) {
		LOG_DBG("Ignoring HID report - BLE disconnected");
		return BT_GATT_ITER_STOP;
	}

	LOG_DBG("Notification, id: %u, size: %u, data:",
	       bt_hogp_rep_id(rep),
	       size);
	for (i = 0; i < size; ++i) {
		LOG_DBG(" 0x%x", data[i]);
	}
	LOG_DBG("\n");

	// Handle different report types based on Report ID
	uint8_t report_id = bt_hogp_rep_id(rep);
	
	// Button detection for buzzer feedback - check Report ID 1 (buttons)
	if (report_id == 1 && size >= 1) {
		static uint8_t last_buttons = 0;
		uint8_t current_buttons = data[0];  // Raw BLE data, no Report ID prepended yet
		
		// Detect left click (bit 0) - press only (0->1 transition)
		if ((current_buttons & 0x01) && !(last_buttons & 0x01)) {
			LOG_DBG("LEFT CLICK DETECTED - buzzing");
			extern void buzzer_click_left(void);
			buzzer_click_left();
		}
		
		// Detect right click (bit 1) - press only (0->1 transition)  
		if ((current_buttons & 0x02) && !(last_buttons & 0x02)) {
			LOG_DBG("RIGHT CLICK DETECTED - buzzing");
			extern void buzzer_click_right(void);
			buzzer_click_right();
		}
		
		last_buttons = current_buttons;
	}
	
	// Parse and forward each report ID independently
	if (size >= 1) {
		int ret;

		/* Handle Report ID 3 (Consumer Control) backwards compatibility */
		if (report_id == 3 && size == 1) {
			/* Old firmware sends 1-byte bitmap, translate to 16-bit usage */
			uint16_t usage = translate_consumer_bitmap(data[0]);
			uint8_t consumer_report[3] = {
				0x03,              /* Report ID 3 */
				usage & 0xFF,     /* Usage low byte */
				(usage >> 8) & 0xFF  /* Usage high byte */
			};
			LOG_DBG("Consumer control: bitmap 0x%02x -> usage 0x%04x", data[0], usage);
			ret = hid_device_submit_report(hid_dev, sizeof(consumer_report), consumer_report);
		} else {
			/* Send directly to USB for zero latency */
			uint8_t report_with_id[size + 1];
			report_with_id[0] = report_id;
			for (uint8_t i = 0; i < size; i++) {
				report_with_id[i + 1] = data[i];
			}
			ret = hid_device_submit_report(hid_dev, size + 1, report_with_id);
		}

		if (ret) {
			LOG_ERR("HID write error, %d", ret);
		} else {
			// LOG_DBG("Report %u sent directly to USB", report_id);

			/* Trigger data activity callback for LED indication */
			if (data_received_callback) {
				data_received_callback(data, size);
			}

			/* Mark HID-specific data activity for LED indication */
			extern void ble_transport_mark_hid_data_activity(void);
			ble_transport_mark_hid_data_activity();
		}
	}
	
	return BT_GATT_ITER_CONTINUE;
}

static uint8_t hogp_boot_mouse_report(struct bt_hogp *hogp,
				     struct bt_hogp_rep_info *rep,
				     uint8_t err,
				     const uint8_t *data)
{
	uint8_t size = bt_hogp_rep_size(rep);

	if (!data) {
		return BT_GATT_ITER_STOP;
	}

	/* Check if still connected - prevent forwarding stale HID data during disconnect */
	if (!ble_transport_is_connected()) {
		LOG_DBG("Ignoring HID report - BLE disconnected");
		return BT_GATT_ITER_STOP;
	}

	LOG_HEXDUMP_DBG(data, size, "Boot mouse report:");

	// DEBUG: Analyze the BLE boot mouse data structure
	if (size >= 1) {
		uint8_t buttons = data[0] & 0x07;  // BLE boot mouse uses 3 bits for buttons
		LOG_DBG("BLE BOOT MOUSE: buttons=0x%02X (L:%c R:%c M:%c)%s%d",
			buttons,
			(buttons & 0x01) ? '1' : '0',  // Left
			(buttons & 0x02) ? '1' : '0',  // Right
			(buttons & 0x04) ? '1' : '0',  // Middle
			(size >= 2) ? " X=" : "",
			(size >= 2) ? (int8_t)data[1] : 0);
	}

	/* Forward boot mouse report directly to USB as Report ID 1 */
	/* Convert BLE boot mouse format to USB HID Report ID 1 format */
	uint8_t report_with_id[3];  // Report ID + Buttons + Wheel
	report_with_id[0] = 1;  // Report ID 1 for boot mouse

	/* Convert BLE boot mouse buttons (3 bits) to USB HID buttons (5 bits) */
	uint8_t ble_buttons = data[0] & 0x07;  // Extract 3 button bits from BLE
	uint8_t usb_buttons = ble_buttons;      // Map directly (left=bit0, right=bit1, middle=bit2)
	report_with_id[1] = usb_buttons;        // Buttons byte (5 bits used, 3 bits padding)

	/* Set wheel to 0 for now (BLE boot mouse doesn't have wheel in standard format) */
	report_with_id[2] = 0x00;  // Wheel byte

	/* Send directly to USB for zero latency */
	int ret = hid_device_submit_report(hid_dev, 3, report_with_id);  // Always 3 bytes: Report ID + Buttons + Wheel
	if (ret) {
		LOG_ERR("HID write error, %d", ret);
	} else {
		LOG_DBG("Boot mouse report sent directly to USB");

		/* Mark HID-specific data activity for LED indication */
		extern void ble_transport_mark_hid_data_activity(void);
		ble_transport_mark_hid_data_activity();
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
	LOG_DBG("Notification, keyboard boot, size: %u, data:", size);
	for (i = 0; i < size; ++i) {
		LOG_DBG(" 0x%x", data[i]);
	}
	LOG_DBG("\n");
	return BT_GATT_ITER_CONTINUE;
}

static void hogp_ready_cb(struct bt_hogp *hogp)
{
	k_work_submit(&hids_ready_work);
}

/* Auto-detect and switch to optimal protocol mode */
/**
 * @brief Auto-detect and switch to optimal HID protocol mode
 * 
 * This function implements automatic protocol mode detection and switching,
 * similar to how operating systems handle HID protocol mode optimization.
 * 
 * Behavior:
 * 1. Detects current protocol mode (BOOT or REPORT)
 * 2. Analyzes device capabilities (BOOT support, REPORT support)
 * 3. Automatically switches to optimal mode:
 *    - If in BOOT mode and REPORT supported → switch to REPORT
 *    - If in REPORT mode but no REPORT support → fallback to BOOT
 *    - If only BOOT supported → stay in BOOT mode
 * 
 * This is called automatically when HID device is ready, and can also
 * be triggered manually via ble_hid_auto_detect_mode().
 */
static void auto_detect_and_switch_mode(void)
{
	enum bt_hids_pm current_mode = bt_hogp_pm_get(&hogp);
	
	LOG_INF("Auto-detecting optimal protocol mode...");
	LOG_INF("Current mode: %s",
	       (current_mode == BT_HIDS_PM_BOOT) ? "BOOT" : "REPORT");
	
	/* Check if device supports REPORT mode */
	bool has_report_support = false;
	struct bt_hogp_rep_info *rep = NULL;
	
	/* Check for any report protocol reports */
	do {
		rep = bt_hogp_rep_next(&hogp, rep);
		if (rep) {
			/* If we have any report protocol reports, device supports REPORT mode */
			has_report_support = true;
			break;
		}
	} while (rep);
	
	/* Also check for boot protocol reports as fallback */
	bool has_boot_support = (hogp.rep_boot.kbd_inp != NULL || 
	                        hogp.rep_boot.mouse_inp != NULL);
	
	LOG_INF("Device capabilities: BOOT=%s, REPORT=%s",
	       has_boot_support ? "YES" : "NO",
	       has_report_support ? "YES" : "NO");
	
	/* Auto-switch logic (similar to OS behavior) */
	if (current_mode == BT_HIDS_PM_BOOT) {
		if (has_report_support) {
			/* Device supports REPORT mode - switch for better features */
			LOG_INF("Switching to REPORT mode for enhanced functionality");
			bt_hogp_pm_update(&hogp, K_SECONDS(5));
		} else if (has_boot_support) {
			/* Device only supports BOOT mode */
			LOG_INF("Device only supports BOOT mode - staying in BOOT mode");
		} else {
			/* No HID reports found */
			LOG_WRN("No HID reports found on device");
		}
	} else if (current_mode == BT_HIDS_PM_REPORT) {
		if (has_report_support) {
			/* Already in optimal mode */
			LOG_INF("Already in optimal REPORT mode");
		} else {
			/* REPORT mode but no report support - fallback to BOOT */
			LOG_INF("REPORT mode but no report support - switching to BOOT mode");
			bt_hogp_pm_update(&hogp, K_SECONDS(5));
		}
	}
}

static void hids_on_ready(struct k_work *work)
{
	int err;
	struct bt_hogp_rep_info *rep = NULL;

	ARG_UNUSED(work);

	LOG_INF("HIDS is ready");

	/* Auto-detect and switch to optimal protocol mode */
	auto_detect_and_switch_mode();

	/* Subscribe to all reports */
	do {
		rep = bt_hogp_rep_next(&hogp, rep);
		if (rep) {
			err = bt_hogp_rep_subscribe(&hogp, rep, hogp_notify_cb);
			if (err) {
				LOG_ERR("Subscribe failed, err: %d", err);
			}
		}
	} while (rep);

	/* Subscribe to boot reports if in boot mode */
	if (bt_hogp_pm_get(&hogp) == BT_HIDS_PM_BOOT) {
		if (hogp.rep_boot.kbd_inp) {
			LOG_INF("Subscribe to boot keyboard report");
			err = bt_hogp_rep_subscribe(&hogp,
							   hogp.rep_boot.kbd_inp,
							   hogp_boot_kbd_report);
			if (err) {
				LOG_ERR("Subscribe error (%d)", err);
			}
		}
		if (hogp.rep_boot.mouse_inp) {
			LOG_INF("Subscribe to boot mouse report");
			err = bt_hogp_rep_subscribe(&hogp,
							   hogp.rep_boot.mouse_inp,
							   hogp_boot_mouse_report);
			if (err) {
				LOG_ERR("Subscribe error (%d)", err);
			}
		}
	} else {
		LOG_INF("In REPORT mode - using report protocol reports only");
	}

	hid_ready = true;
	
	/* Call the registered ready callback */
	if (ready_callback) {
		ready_callback();
	}
}

static void hogp_prep_fail_cb(struct bt_hogp *hogp, int err)
{
	LOG_ERR("HOGP prepare failed, err: %d", err);
}

static void hogp_pm_update_cb(struct bt_hogp *hogp)
{
	enum bt_hids_pm pm = bt_hogp_pm_get(hogp);

	LOG_DBG("HOGP PM update: %d", pm);
}

/* Button handler functions */
static void button_bootmode(void)
{
	enum bt_hids_pm pm = bt_hogp_pm_get(&hogp);
	enum bt_hids_pm new_pm = ((pm == BT_HIDS_PM_BOOT) ? BT_HIDS_PM_REPORT : BT_HIDS_PM_BOOT);

	LOG_INF("Switching HIDS PM from %d to %d", pm, new_pm);
	bt_hogp_pm_update(&hogp, K_SECONDS(5));
}

static void hidc_write_cb(struct bt_hogp *hidc,
			  struct bt_hogp_rep_info *rep,
			  uint8_t err)
{
	LOG_DBG("HOGP write completed");
}

static void button_capslock(void)
{
	if (!hogp.rep_boot.kbd_out) {
		LOG_WRN("HID device does not have Keyboard OUT report");
		return;
	}

	capslock_state = capslock_state ? 0 : 1;
	uint8_t data = capslock_state ? 0x02 : 0;
	int err = bt_hogp_rep_write_wo_rsp(&hogp, hogp.rep_boot.kbd_out,
				       &data, sizeof(data),
				       hidc_write_cb);

	if (err) {
		LOG_ERR("Keyboard data write error (err: %d)", err);
		return;
	}
	LOG_DBG("Caps lock send (val: 0x%x)", data);
}

static uint8_t capslock_read_cb(struct bt_hogp *hogp,
			     struct bt_hogp_rep_info *rep,
			     uint8_t err,
			     const uint8_t *data)
{
	if (err) {
		LOG_ERR("Capslock read error (err: %u)", err);
		return BT_GATT_ITER_STOP;
	}
	if (!data) {
		LOG_DBG("Capslock read - no data");
		return BT_GATT_ITER_STOP;
	}
	LOG_DBG("Received data (size: %u, data[0]: 0x%x)",
	       bt_hogp_rep_size(rep), data[0]);

	return BT_GATT_ITER_STOP;
}

static void capslock_write_cb(struct bt_hogp *hogp,
			      struct bt_hogp_rep_info *rep,
			      uint8_t err)
{
	int ret;

	LOG_DBG("Capslock write result: %u", err);

	ret = bt_hogp_rep_read(hogp, rep, capslock_read_cb);
	if (ret) {
		LOG_ERR("Cannot read capslock value (err: %d)", ret);
	}
}

static void button_capslock_rsp(void)
{
	if (!bt_hogp_ready_check(&hogp)) {
		LOG_WRN("HID device not ready");
		return;
	}
	if (!hogp.rep_boot.kbd_out) {
		LOG_WRN("HID device does not have Keyboard OUT report");
		return;
	}
	int err;
	uint8_t data;

	capslock_state = capslock_state ? 0 : 1;
	data = capslock_state ? 0x02 : 0;
	err = bt_hogp_rep_write(&hogp, hogp.rep_boot.kbd_out, capslock_write_cb,
				&data, sizeof(data));
	if (err) {
		LOG_ERR("Keyboard data write error (err: %d)", err);
		return;
	}
	LOG_DBG("Caps lock send using write with response (val: 0x%x)", data);
}

/* Public API implementations */
int ble_hid_init(void)
{
	LOG_INF("Initializing BLE HID...");
	
	bt_hogp_init(&hogp, &hogp_init_params);
	
	LOG_INF("✓ BLE HID initialized");
	return 0;
}

int ble_hid_discover(struct bt_conn *conn)
{
	int err;
	
	LOG_INF("Starting HID service discovery...");
	
	err = bt_gatt_dm_start(conn, BT_UUID_HIDS, &discovery_cb, &hogp);
	if (err) {
		LOG_ERR("Could not start discovery (err %d)", err);
		return err;
	}
	
	return 0;
}

bool ble_hid_is_ready(void)
{
	return hid_ready;
}

int ble_hid_send_report(const uint8_t *data, uint16_t len)
{
	if (!hid_ready) {
		return -EAGAIN;
	}
	
	// Implementation would go here if needed
	return 0;
}

struct bt_hogp *ble_hid_get_hogp(void)
{
	return &hogp;
}

uint8_t (*ble_hid_get_notify_cb(void))(struct bt_hogp *, struct bt_hogp_rep_info *, uint8_t, const uint8_t *)
{
	return hogp_notify_cb;
}

uint8_t (*ble_hid_get_boot_mouse_cb(void))(struct bt_hogp *, struct bt_hogp_rep_info *, uint8_t, const uint8_t *)
{
	return hogp_boot_mouse_report;
}

uint8_t (*ble_hid_get_boot_kbd_cb(void))(struct bt_hogp *, struct bt_hogp_rep_info *, uint8_t, const uint8_t *)
{
	return hogp_boot_kbd_report;
}

void ble_hid_handle_buttons(uint32_t button_state, uint32_t has_changed)
{
	uint32_t button = button_state & has_changed;

	if (button & KEY_BOOTMODE_MASK) {
		button_bootmode();
	}

	if (button & KEY_CAPSLOCK_MASK) {
		button_capslock();
	}

	if (button & KEY_CAPSLOCK_RSP_MASK) {
		button_capslock_rsp();
	}

	/* Auto-detection trigger (if we had a button) */
	/* This would be triggered by a physical button if available */
	/* For now, we can call this manually via ble_hid_auto_detect_mode() */
}

int ble_hid_register_data_received_cb(ble_hid_data_received_cb_t cb)
{
	data_received_callback = cb;
	return 0;
}

int ble_hid_register_ready_cb(ble_hid_ready_cb_t cb)
{
	ready_callback = cb;
	return 0;
}

/* Manual trigger for auto-detection (for testing) */
int ble_hid_auto_detect_mode(void)
{
	if (!hid_ready) {
		LOG_WRN("HID device not ready for mode detection");
		return -EAGAIN;
	}

	LOG_INF("Manual trigger: Auto-detecting protocol mode");
	auto_detect_and_switch_mode();
	return 0;
}

/* Discovery callback implementations */
static void discovery_completed_cb(struct bt_gatt_dm *dm, void *context)
{
	struct bt_hogp *hogp = context;
	int err;

	LOG_INF("The discovery procedure succeeded");

	bt_gatt_dm_data_print(dm);

	err = bt_hogp_handles_assign(dm, hogp);
	if (err) {
		LOG_ERR("Could not assign HOGP handles (err %d)", err);
	}

	/* CRITICAL: Must release GATT DM data to allow subsequent discoveries */
	err = bt_gatt_dm_data_release(dm);
	if (err) {
		LOG_ERR("Could not release discovery data (err %d)", err);
	}

	LOG_INF("HOGP ready");
}

static void discovery_service_not_found_cb(struct bt_conn *conn, void *context)
{
	LOG_WRN("The service could not be found during the discovery");
}

static void discovery_error_found_cb(struct bt_conn *conn, int err, void *context)
{
	LOG_ERR("The discovery procedure failed with %d", err);
}
