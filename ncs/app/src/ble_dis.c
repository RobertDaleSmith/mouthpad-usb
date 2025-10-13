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
#include <zephyr/bluetooth/uuid.h>
#include <bluetooth/gatt_dm.h>
#include <string.h>

#include "ble_dis.h"
#include "ble_central.h"

#define LOG_MODULE_NAME ble_dis
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

/* Device Information Service UUIDs (Bluetooth SIG assigned numbers) */
#define BT_UUID_DIS_VAL           0x180A  /* Device Information Service */
#define BT_UUID_DIS_FW_REV_VAL    0x2A26  /* Firmware Revision String */
#define BT_UUID_DIS_HW_REV_VAL    0x2A27  /* Hardware Revision String */
#define BT_UUID_DIS_MFR_NAME_VAL  0x2A29  /* Manufacturer Name String */
#define BT_UUID_DIS_PNP_ID_VAL    0x2A50  /* PnP ID */

#define BT_UUID_DIS         BT_UUID_DECLARE_16(BT_UUID_DIS_VAL)
#define BT_UUID_DIS_FW_REV  BT_UUID_DECLARE_16(BT_UUID_DIS_FW_REV_VAL)
#define BT_UUID_DIS_HW_REV  BT_UUID_DECLARE_16(BT_UUID_DIS_HW_REV_VAL)
#define BT_UUID_DIS_MFR_NAME BT_UUID_DECLARE_16(BT_UUID_DIS_MFR_NAME_VAL)
#define BT_UUID_DIS_PNP_ID  BT_UUID_DECLARE_16(BT_UUID_DIS_PNP_ID_VAL)

/* GAP Device Name UUID */
#define BT_UUID_GAP_DEVICE_NAME_VAL 0x2A00
#define BT_UUID_GAP_DEVICE_NAME BT_UUID_DECLARE_16(BT_UUID_GAP_DEVICE_NAME_VAL)

/* Device Information client state */
static ble_dis_info_t device_info;
static bool dis_ready = false;
static struct bt_conn *current_conn = NULL;
static struct bt_gatt_read_params read_params;

/* Characteristic handles */
static uint16_t fw_rev_handle = 0;
static uint16_t hw_rev_handle = 0;
static uint16_t mfr_name_handle = 0;
static uint16_t pnp_id_handle = 0;

/* Forward declarations */
static void dis_discovery_completed_cb(struct bt_gatt_dm *dm, void *context);
static void dis_discovery_service_not_found_cb(struct bt_conn *conn, void *context);
static void dis_discovery_error_found_cb(struct bt_conn *conn, int err, void *context);
static void read_device_name_cb(struct bt_conn *conn, uint8_t err,
				struct bt_gatt_read_params *params,
				const void *data, uint16_t length);
static uint8_t read_pnp_id_cb(struct bt_conn *conn, uint8_t err,
			      struct bt_gatt_read_params *params,
			      const void *data, uint16_t length);

/* GATT Discovery Manager callback structure */
static struct bt_gatt_dm_cb dis_discovery_cb = {
	.completed = dis_discovery_completed_cb,
	.service_not_found = dis_discovery_service_not_found_cb,
	.error_found = dis_discovery_error_found_cb,
};

/* Public API implementations */
int ble_dis_init(void)
{
	LOG_INF("Initializing Device Information Service client...");
	memset(&device_info, 0, sizeof(device_info));
	dis_ready = false;
	LOG_INF("Device Information Service client initialized successfully");
	return 0;
}

int ble_dis_discover(struct bt_conn *conn)
{
	int err;

	if (!conn || conn != ble_central_get_default_conn()) {
		LOG_WRN("Invalid connection for Device Information Service discovery");
		return -EINVAL;
	}

	LOG_INF("Starting Device Information Service discovery");
	current_conn = conn;

	/* Start GATT discovery for DIS service */
	err = bt_gatt_dm_start(conn, BT_UUID_DIS, &dis_discovery_cb, NULL);
	if (err) {
		LOG_ERR("Could not start DIS discovery: %d", err);
		return err;
	}

	return 0;
}

bool ble_dis_is_ready(void)
{
	return dis_ready;
}

void ble_dis_reset(void)
{
	dis_ready = false;
	current_conn = NULL;
	fw_rev_handle = 0;
	hw_rev_handle = 0;
	mfr_name_handle = 0;
	pnp_id_handle = 0;
	memset(&device_info, 0, sizeof(device_info));
	LOG_DBG("Device Information Service reset");
}

const ble_dis_info_t *ble_dis_get_info(void)
{
	if (!dis_ready) {
		return NULL;
	}
	return &device_info;
}

/* GATT read callbacks for each characteristic */
static uint8_t read_fw_rev_cb(struct bt_conn *conn, uint8_t err,
			      struct bt_gatt_read_params *params,
			      const void *data, uint16_t length)
{
	if (err) {
		LOG_ERR("Firmware Revision read failed: %d", err);
		return BT_GATT_ITER_STOP;
	}

	if (!data) {
		LOG_DBG("Firmware Revision read complete");

		/* Read PnP ID if available */
		if (pnp_id_handle) {
			LOG_DBG("Reading PnP ID...");
			memset(&read_params, 0, sizeof(read_params));
			read_params.func = (bt_gatt_read_func_t)read_pnp_id_cb;
			read_params.handle_count = 1;
			read_params.single.handle = pnp_id_handle;
			read_params.single.offset = 0;

			int err = bt_gatt_read(current_conn, &read_params);
			if (err) {
				LOG_ERR("PnP ID read failed to start: %d", err);
				/* Continue to read device name anyway */
				goto read_device_name;
			}
		} else {
read_device_name:
			/* Read device name from GAP service */
			LOG_DBG("Reading device name from GAP...");
			memset(&read_params, 0, sizeof(read_params));
			read_params.func = read_device_name_cb;
			read_params.by_uuid.uuid = BT_UUID_GAP_DEVICE_NAME;
			read_params.by_uuid.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
			read_params.by_uuid.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;

			int err = bt_gatt_read(current_conn, &read_params);
			if (err) {
				LOG_ERR("Device name read failed to start: %d", err);
				dis_ready = true;  /* Mark ready even without device name */
			}
		}

		return BT_GATT_ITER_STOP;
	}

	/* Copy firmware revision string */
	size_t copy_len = MIN(length, BLE_DIS_FIRMWARE_VERSION_MAX_LEN - 1);
	memcpy(device_info.firmware_version, data, copy_len);
	device_info.firmware_version[copy_len] = '\0';
	device_info.has_firmware_version = true;

	LOG_INF("Firmware Revision: %s", device_info.firmware_version);

	return BT_GATT_ITER_CONTINUE;
}

static uint8_t read_pnp_id_cb(struct bt_conn *conn, uint8_t err,
			      struct bt_gatt_read_params *params,
			      const void *data, uint16_t length)
{
	if (err) {
		LOG_ERR("PnP ID read failed: %d", err);
		/* Continue to read device name */
		goto read_device_name;
	}

	if (!data) {
		LOG_DBG("PnP ID read complete");
		/* Continue to read device name */
		goto read_device_name;
	}

	/* Parse PnP ID: 7 bytes total
	 * Byte 0: Vendor ID Source (0x01=Bluetooth SIG, 0x02=USB Implementer's Forum)
	 * Bytes 1-2: Vendor ID (little-endian)
	 * Bytes 3-4: Product ID (little-endian)
	 * Bytes 5-6: Product Version (little-endian) */
	if (length >= 7) {
		const uint8_t *pnp_data = (const uint8_t *)data;
		device_info.vendor_id = pnp_data[1] | (pnp_data[2] << 8);
		device_info.product_id = pnp_data[3] | (pnp_data[4] << 8);
		device_info.has_pnp_id = true;
		LOG_INF("PnP ID: VID=0x%04X, PID=0x%04X", device_info.vendor_id, device_info.product_id);
	} else {
		LOG_WRN("PnP ID data too short: %d bytes", length);
	}

read_device_name:
	/* Read device name from GAP service */
	LOG_DBG("Reading device name from GAP...");
	memset(&read_params, 0, sizeof(read_params));
	read_params.func = read_device_name_cb;
	read_params.by_uuid.uuid = BT_UUID_GAP_DEVICE_NAME;
	read_params.by_uuid.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	read_params.by_uuid.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;

	int read_err = bt_gatt_read(current_conn, &read_params);
	if (read_err) {
		LOG_ERR("Device name read failed to start: %d", read_err);
		dis_ready = true;  /* Mark ready even without device name */
	}

	return BT_GATT_ITER_STOP;
}

static void read_device_name_cb(struct bt_conn *conn, uint8_t err,
				struct bt_gatt_read_params *params,
				const void *data, uint16_t length)
{
	if (err) {
		LOG_ERR("Device Name read failed: %d", err);
		dis_ready = true;  /* Mark ready even without device name */
		return;
	}

	if (!data) {
		LOG_DBG("Device Name read complete");
		dis_ready = true;
		LOG_INF("Device Information Service ready");
		return;
	}

	/* Copy device name string */
	size_t copy_len = MIN(length, BLE_DIS_DEVICE_NAME_MAX_LEN - 1);
	memcpy(device_info.device_name, data, copy_len);
	device_info.device_name[copy_len] = '\0';
	device_info.has_device_name = true;

	LOG_INF("Device Name: %s", device_info.device_name);
}

/* DIS Discovery callbacks */
static void dis_discovery_completed_cb(struct bt_gatt_dm *dm, void *context)
{
	int err;
	const struct bt_gatt_dm_attr *gatt_chrc;
	const struct bt_gatt_dm_attr *gatt_desc;

	LOG_INF("Device Information Service discovery completed");
	bt_gatt_dm_data_print(dm);

	/* Find Firmware Revision characteristic */
	gatt_chrc = bt_gatt_dm_char_by_uuid(dm, BT_UUID_DIS_FW_REV);
	if (gatt_chrc) {
		gatt_desc = bt_gatt_dm_desc_by_uuid(dm, gatt_chrc, BT_UUID_DIS_FW_REV);
		if (gatt_desc) {
			fw_rev_handle = gatt_desc->handle;
			LOG_INF("Found Firmware Revision characteristic (handle: %d)", fw_rev_handle);
		}
	}

	/* Find Hardware Revision characteristic */
	gatt_chrc = bt_gatt_dm_char_by_uuid(dm, BT_UUID_DIS_HW_REV);
	if (gatt_chrc) {
		gatt_desc = bt_gatt_dm_desc_by_uuid(dm, gatt_chrc, BT_UUID_DIS_HW_REV);
		if (gatt_desc) {
			hw_rev_handle = gatt_desc->handle;
			LOG_INF("Found Hardware Revision characteristic (handle: %d)", hw_rev_handle);
		}
	}

	/* Find Manufacturer Name characteristic */
	gatt_chrc = bt_gatt_dm_char_by_uuid(dm, BT_UUID_DIS_MFR_NAME);
	if (gatt_chrc) {
		gatt_desc = bt_gatt_dm_desc_by_uuid(dm, gatt_chrc, BT_UUID_DIS_MFR_NAME);
		if (gatt_desc) {
			mfr_name_handle = gatt_desc->handle;
			LOG_INF("Found Manufacturer Name characteristic (handle: %d)", mfr_name_handle);
		}
	}

	/* Find PnP ID characteristic */
	gatt_chrc = bt_gatt_dm_char_by_uuid(dm, BT_UUID_DIS_PNP_ID);
	if (gatt_chrc) {
		gatt_desc = bt_gatt_dm_desc_by_uuid(dm, gatt_chrc, BT_UUID_DIS_PNP_ID);
		if (gatt_desc) {
			pnp_id_handle = gatt_desc->handle;
			LOG_INF("Found PnP ID characteristic (handle: %d)", pnp_id_handle);
		}
	}

	/* Release discovery data */
	err = bt_gatt_dm_data_release(dm);
	if (err) {
		LOG_ERR("Could not release DIS discovery data: %d", err);
	}

	/* Start reading characteristics - begin with Firmware Revision */
	if (fw_rev_handle) {
		LOG_DBG("Reading Firmware Revision...");
		memset(&read_params, 0, sizeof(read_params));
		read_params.func = (bt_gatt_read_func_t)read_fw_rev_cb;
		read_params.handle_count = 1;
		read_params.single.handle = fw_rev_handle;
		read_params.single.offset = 0;

		err = bt_gatt_read(current_conn, &read_params);
		if (err) {
			LOG_ERR("Firmware Revision read failed to start: %d", err);
			dis_ready = true;  /* Mark ready even if reads fail */
		}
	} else {
		LOG_WRN("No DIS characteristics found");
		dis_ready = true;
	}
}

static void dis_discovery_service_not_found_cb(struct bt_conn *conn, void *context)
{
	LOG_INF("Device Information Service not found during discovery");

	/* Still try to read device name from GAP */
	LOG_DBG("Reading device name from GAP...");
	current_conn = conn;
	memset(&read_params, 0, sizeof(read_params));
	read_params.func = read_device_name_cb;
	read_params.by_uuid.uuid = BT_UUID_GAP_DEVICE_NAME;
	read_params.by_uuid.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	read_params.by_uuid.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;

	int err = bt_gatt_read(conn, &read_params);
	if (err) {
		LOG_ERR("Device name read failed to start: %d", err);
		dis_ready = true;  /* Mark ready even without device name */
	}
}

static void dis_discovery_error_found_cb(struct bt_conn *conn, int err, void *context)
{
	LOG_ERR("Device Information Service discovery failed: %d", err);
	dis_ready = true;  /* Mark ready to unblock, but with no data */
}
