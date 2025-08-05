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

#include "ble_hid.h"

/* Forward declarations for direct USB access */
extern const struct device *hid_dev;
extern struct k_sem ep_write_sem;

LOG_MODULE_REGISTER(ble_hid, LOG_LEVEL_INF);

/* Callback registration */
static ble_hid_data_received_cb_t data_received_callback = NULL;
static ble_hid_ready_cb_t ready_callback = NULL;

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
	printk("Notification, id: %u, size: %u, data:",
	       bt_hogp_rep_id(rep),
	       size);
	for (i = 0; i < size; ++i) {
		printk(" 0x%x", data[i]);
	}
	printk("\n");
	
	// Handle different report types based on Report ID
	uint8_t report_id = bt_hogp_rep_id(rep);
	
	// Parse and forward each report ID independently
	if (size >= 1) {
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

	ARG_UNUSED(work);

	printk("HIDS is ready\n");

	/* Subscribe to all reports */
	do {
		rep = bt_hogp_rep_next(&hogp, rep);
		if (rep) {
			err = bt_hogp_rep_subscribe(&hogp, rep, hogp_notify_cb);
			if (err) {
				printk("Subscribe failed, err: %d\n", err);
			}
		}
	} while (rep);

	/* Subscribe to boot reports if in boot mode */
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

	hid_ready = true;
}

static void hogp_prep_fail_cb(struct bt_hogp *hogp, int err)
{
	printk("HOGP prepare failed, err: %d\n", err);
}

static void hogp_pm_update_cb(struct bt_hogp *hogp)
{
	enum bt_hids_pm pm = bt_hogp_pm_get(hogp);

	printk("HOGP PM update: %d\n", pm);
}

/* Button handler functions */
static void button_bootmode(void)
{
	enum bt_hids_pm pm = bt_hogp_pm_get(&hogp);
	enum bt_hids_pm new_pm = ((pm == BT_HIDS_PM_BOOT) ? BT_HIDS_PM_REPORT : BT_HIDS_PM_BOOT);

	printk("Switching HIDS PM from %d to %d\n", pm, new_pm);
	bt_hogp_pm_update(&hogp, K_SECONDS(5));
}

static void hidc_write_cb(struct bt_hogp *hidc,
			  struct bt_hogp_rep_info *rep,
			  uint8_t err)
{
	printk("HOGP write completed\n");
}

static void button_capslock(void)
{
	if (!hogp.rep_boot.kbd_out) {
		printk("HID device does not have Keyboard OUT report\n");
		return;
	}

	capslock_state = capslock_state ? 0 : 1;
	uint8_t data = capslock_state ? 0x02 : 0;
	int err = bt_hogp_rep_write_wo_rsp(&hogp, hogp.rep_boot.kbd_out,
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
		printk("Could not start discovery (err %d)\n", err);
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

/* Discovery callback implementations */
static void discovery_completed_cb(struct bt_gatt_dm *dm, void *context)
{
	struct bt_hogp *hogp = context;
	int err;

	printk("The discovery procedure succeeded\n");

	bt_gatt_dm_data_print(dm);

	err = bt_hogp_handles_assign(dm, hogp);
	if (err) {
		printk("Could not assign HOGP handles (err %d)\n", err);
		return;
	}

	printk("HOGP ready\n");
}

static void discovery_service_not_found_cb(struct bt_conn *conn, void *context)
{
	printk("The service could not be found during the discovery\n");
}

static void discovery_error_found_cb(struct bt_conn *conn, int err, void *context)
{
	printk("The discovery procedure failed with %d\n", err);
} 