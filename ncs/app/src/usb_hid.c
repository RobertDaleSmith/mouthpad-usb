/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* ============================================================================
 * INCLUDES
 * ============================================================================ */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <zephyr/logging/log.h>
#include <nrf.h>
#include "sample_usbd.h"

LOG_MODULE_REGISTER(usb_mouse, LOG_LEVEL_INF);

/* ============================================================================
 * GLOBAL VARIABLES
 * ============================================================================ */

/* LED for status indication (blue LED) - only if available */
#if DT_NODE_EXISTS(DT_ALIAS(led2)) && DT_NODE_HAS_PROP(DT_ALIAS(led2), gpios)
static const struct gpio_dt_spec led2 = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);
#endif

/* USB HID device and semaphore - accessible to BLE module for direct sending */
const struct device *hid_dev;
struct k_sem ep_write_sem;

/* USB device context for new stack */
static struct usbd_context *usbd_ctx;

/* USB enumeration watchdog */
static struct k_work_delayable usb_enum_check_work;
static bool usb_enumerated = false;

#define USB_ENUM_TIMEOUT_MS 3000  /* 3 seconds - balanced: fast recovery with safety margin */
#define USB_ENUM_RETRY_MAGIC 0xE1  /* Magic value to track retry */

/* ============================================================================
 * HID DESCRIPTOR
 * ============================================================================ */

/* USB HID Report Descriptor for MouthPad device
 * 
 * Report Structure:
 * - Report ID 1: Buttons (5 bits) + Padding (3 bits) + Wheel (8 bits) = 16 bits
 * - Report ID 2: X movement (12 bits) + Y movement (12 bits) = 24 bits  
 * - Report ID 3: Consumer controls (volume, media keys, etc.)
 * 
 * This descriptor matches the MouthPad BLE device exactly to eliminate scaling
 */
static const uint8_t hid_report_desc[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x01, 0x09, 0x01, 0xA1, 0x00,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00,
    0x25, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x03, 0x81, 0x01, 0x75, 0x08,
    0x95, 0x01, 0x05, 0x01, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x81, 0x06,
    0x05, 0x0C, 0x0A, 0x38, 0x02, 0x95, 0x01, 0x81, 0x06, 0xC0, 0x85, 0x02,
    0x09, 0x01, 0xA1, 0x00, 0x75, 0x0C, 0x95, 0x02, 0x05, 0x01, 0x09, 0x30,
    0x09, 0x31, 0x16, 0x01, 0xF8, 0x26, 0xFF, 0x07, 0x81, 0x06, 0xC0, 0xC0,
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x03, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x01, 0x09, 0xCD, 0x81, 0x06, 0x0A, 0x83, 0x01, 0x81,
    0x06, 0x09, 0xB5, 0x81, 0x06, 0x09, 0xB6, 0x81, 0x06, 0x09, 0xEA, 0x81,
    0x06, 0x09, 0xE9, 0x81, 0x06, 0x0A, 0x25, 0x02, 0x81, 0x06, 0x0A, 0x24,
    0x02, 0x81, 0x06, 0xC0
};

/* ============================================================================
 * CONSTANTS AND ENUMERATIONS
 * ============================================================================ */

/* Maximum size for HID reports */
#define MAX_REPORT_SIZE 8

/* Report structure indices for parsing */
enum mouse_report1_idx {
	MOUSE_BTN_REPORT_IDX = 0,    /* Buttons byte */
	MOUSE_WHEEL_REPORT_IDX = 1,  /* Wheel byte */
	MOUSE_REPORT1_COUNT = 2,     /* Total bytes in Report 1 */
};

enum mouse_report2_idx {
	MOUSE_X_LOW_REPORT_IDX = 0,           /* X low 8 bits */
	MOUSE_X_HIGH_Y_LOW_REPORT_IDX = 1,    /* X high 4 bits + Y low 4 bits */
	MOUSE_Y_HIGH_REPORT_IDX = 2,          /* Y high 8 bits */
	MOUSE_REPORT2_COUNT = 3,              /* Total bytes in Report 2 */
};

/* ============================================================================
 * USB CALLBACK FUNCTIONS
 * ============================================================================ */

/**
 * @brief Trigger USB enumeration retry via system reset
 *
 * Increments retry counter and performs system reset if under retry limit.
 * Used for both timeout-based and error-based recovery.
 */
static void trigger_usb_recovery_reset(const char *reason)
{
	/* Check retry counter to prevent infinite reboot loop */
	uint8_t retry_count = NRF_POWER->GPREGRET2;

	if (retry_count >= 3) {
		LOG_ERR("USB enumeration failed after 3 retries - giving up (%s)", reason);
		NRF_POWER->GPREGRET2 = 0;  /* Clear for next power cycle */
		return;
	}

	LOG_WRN("USB enumeration failed (%s) - restarting device (attempt %d/3)",
		reason, retry_count + 1);

	/* Increment retry counter */
	NRF_POWER->GPREGRET2 = retry_count + 1;

	/* Force disconnect and reset */
	NRF_USBD->USBPULLUP = 0;
	k_msleep(50);

	NVIC_SystemReset();
}

/**
 * @brief USB enumeration check handler
 *
 * Called after timeout to check if USB enumerated. If not, restart device.
 */
static void usb_enum_check_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (usb_enumerated) {
		LOG_INF("USB enumeration successful - watchdog satisfied");
		return;
	}

	trigger_usb_recovery_reset("timeout");
}

/**
 * @brief USB device message callback
 *
 * Handles USB device state changes (VBUS, configuration, errors, etc.)
 */
static void usb_msg_cb(struct usbd_context *const ctx,
		       const struct usbd_msg *const msg)
{
	LOG_DBG("USBD message: %s", usbd_msg_type_string(msg->type));

	switch (msg->type) {
	case USBD_MSG_RESET:
		/* Bus reset detected - enumeration is starting */
		if (!usb_enumerated) {
			LOG_DBG("USB reset detected - starting enumeration watchdog");
			/* (Re)start watchdog on each reset attempt */
			k_work_reschedule(&usb_enum_check_work, K_MSEC(USB_ENUM_TIMEOUT_MS));
		}
		break;

	case USBD_MSG_CONFIGURATION:
		LOG_INF("USB Configuration value %d", msg->status);
		if (msg->status > 0) {
			/* USB successfully enumerated - cancel watchdog */
			usb_enumerated = true;
			k_work_cancel_delayable(&usb_enum_check_work);
			/* Clear retry counter on successful enumeration */
			NRF_POWER->GPREGRET2 = 0;
			LOG_INF("USB enumeration successful");
		}
		break;

	case USBD_MSG_UDC_ERROR:
		/* Hardware controller error - restart immediately */
		LOG_ERR("USB controller error detected");
		trigger_usb_recovery_reset("UDC error");
		break;

	case USBD_MSG_STACK_ERROR:
		/* Unrecoverable stack error - restart immediately */
		LOG_ERR("USB stack error detected");
		trigger_usb_recovery_reset("stack error");
		break;

	case USBD_MSG_VBUS_READY:
		if (usbd_can_detect_vbus(ctx)) {
			if (usbd_enable(ctx)) {
				LOG_ERR("Failed to enable device support");
			}
		}
		break;

	case USBD_MSG_VBUS_REMOVED:
		if (usbd_can_detect_vbus(ctx)) {
			if (usbd_disable(ctx)) {
				LOG_ERR("Failed to disable device support");
			}
		}
		/* Cable unplugged - cancel watchdog */
		k_work_cancel_delayable(&usb_enum_check_work);
		break;

	default:
		/* Other events (SUSPEND, RESUME, etc.) - no action needed */
		break;
	}
}

/**
 * @brief HID interface ready callback
 */
static void hid_iface_ready(const struct device *dev, const bool ready)
{
	LOG_INF("HID device %s interface is %s",
		dev->name, ready ? "ready" : "not ready");
	if (ready) {
		k_sem_give(&ep_write_sem);
	}
}

/**
 * @brief HID get report callback
 */
static int hid_get_report(const struct device *dev, const uint8_t type,
			  const uint8_t id, const uint16_t len, uint8_t *const buf)
{
	LOG_DBG("Get report: type %u id %u len %u", type, id, len);
	return 0;
}

/**
 * @brief HID set report callback
 */
static int hid_set_report(const struct device *dev, const uint8_t type,
			  const uint8_t id, const uint16_t len, const uint8_t *const buf)
{
	LOG_DBG("Set report: type %u id %u len %u", type, id, len);
	return 0;
}

/**
 * @brief HID set idle callback
 */
static void hid_set_idle(const struct device *dev, const uint8_t id, const uint32_t duration)
{
	LOG_DBG("Set Idle %u to %u", id, duration);
}

/**
 * @brief HID get idle callback
 */
static uint32_t hid_get_idle(const struct device *dev, const uint8_t id)
{
	LOG_DBG("Get Idle %u", id);
	return 0;
}

/**
 * @brief HID set protocol callback
 */
static void hid_set_protocol(const struct device *dev, const uint8_t proto)
{
	LOG_INF("Protocol changed to %s",
		proto == 0U ? "Boot Protocol" : "Report Protocol");
}

/**
 * @brief HID output report callback
 */
static void hid_output_report(const struct device *dev, const uint16_t len,
			      const uint8_t *const buf)
{
	LOG_HEXDUMP_DBG(buf, len, "HID output report");
}

/* USB HID operations structure for new stack */
static struct hid_device_ops hid_ops = {
	.iface_ready = hid_iface_ready,
	.get_report = hid_get_report,
	.set_report = hid_set_report,
	.set_idle = hid_set_idle,
	.get_idle = hid_get_idle,
	.set_protocol = hid_set_protocol,
	.output_report = hid_output_report,
};

/* ============================================================================
 * PUBLIC FUNCTIONS
 * ============================================================================ */

/**
 * @brief Initialize USB HID device
 * 
 * Sets up the USB HID device with the MouthPad descriptor, configures
 * the LED for status indication, and starts the USB device stack.
 * 
 * @return 0 on success, negative error code on failure
 */
int usb_init(void)
{
	int ret;

	/* Initialize USB endpoint semaphore */
	k_sem_init(&ep_write_sem, 0, 1);

	/* Initialize USB enumeration watchdog */
	k_work_init_delayable(&usb_enum_check_work, usb_enum_check_handler);
	usb_enumerated = false;

	/* Configure status blue LED - only if available */
#if DT_NODE_EXISTS(DT_ALIAS(led2)) && DT_NODE_HAS_PROP(DT_ALIAS(led2), gpios)
	if (!gpio_is_ready_dt(&led2)) {
		LOG_ERR("LED device %s is not ready", led2.port->name);
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&led2, GPIO_OUTPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure the LED pin, error: %d", ret);
		return ret;
	}
#else
	LOG_INF("No blue LED available for status indication");
#endif

	/* Get USB HID device from device tree */
	hid_dev = DEVICE_DT_GET_ONE(zephyr_hid_device);
	if (!device_is_ready(hid_dev)) {
		LOG_ERR("HID Device is not ready");
		return -ENODEV;
	}

	LOG_INF("Found USB HID device: %s", hid_dev->name);

	/* Register HID device with descriptor and callbacks */
	ret = hid_device_register(hid_dev,
				  hid_report_desc, sizeof(hid_report_desc),
				  &hid_ops);
	if (ret != 0) {
		LOG_ERR("Failed to register HID Device, %d", ret);
		return ret;
	}
	LOG_INF("HID device registered successfully");

	/* Initialize USB device context */
	usbd_ctx = sample_usbd_init_device(usb_msg_cb);
	if (usbd_ctx == NULL) {
		LOG_ERR("Failed to initialize USB device");
		return -ENODEV;
	}

	/* Enable USB device stack if VBUS detection not available */
	if (!usbd_can_detect_vbus(usbd_ctx)) {
		ret = usbd_enable(usbd_ctx);
		if (ret) {
			LOG_ERR("Failed to enable USB device support");
			return ret;
		}
	}
	LOG_INF("USB device stack enabled successfully");

	/* Start USB enumeration watchdog as fallback (main watchdog starts on RESET) */
	k_work_schedule(&usb_enum_check_work, K_MSEC(USB_ENUM_TIMEOUT_MS));
	LOG_INF("USB enumeration watchdog armed (%d ms timeout, starts on RESET)", USB_ENUM_TIMEOUT_MS);

	LOG_INF("USB HID device initialized successfully");

	return 0;
}

/**
 * @brief Send HID report to USB HID device
 * 
 * @param data HID report data
 * @param len Length of HID report data
 * @return 0 on success, negative error code on failure
 */
int usb_hid_send_report(const uint8_t *data, uint16_t len)
{
	int ret;
	
	LOG_DBG("=== USB HID SEND REPORT CALLED ===");
	LOG_DBG("Data length: %d bytes", len);
	
	if (hid_dev == NULL) {
		LOG_ERR("USB HID device not initialized");
		return -ENODEV;
	}
	
	LOG_DBG("Sending HID report: %d bytes", len);
	
	/* Send the HID report using new stack API */
	ret = hid_device_submit_report(hid_dev, len, data);
	if (ret != 0) {
		LOG_ERR("Failed to send HID report (err %d)", ret);
		return ret;
	}
	
	LOG_INF("HID report sent successfully");
	return 0;
}

/**
 * @brief Send HID release-all report to clear any stuck inputs
 * 
 * Sends reports with all buttons released and no movement to prevent
 * stuck inputs when BLE device disconnects.
 * 
 * @return 0 on success, negative error code on failure
 */
int usb_hid_send_release_all(void)
{
	int ret;
	int failed_count = 0;

	LOG_INF("=== SENDING HID RELEASE-ALL REPORTS (MULTIPLE ROUNDS) ===");

	if (hid_dev == NULL) {
		LOG_ERR("USB HID device not initialized");
		return -ENODEV;
	}

	/* Report ID 1: Release all buttons and wheel */
	uint8_t report1[] = {
		0x01,  /* Report ID 1 */
		0x00,  /* No buttons pressed */
		0x00,  /* No wheel movement */
		0x00   /* No AC Pan */
	};

	/* Report ID 2: No X/Y movement */
	uint8_t report2[] = {
		0x02,  /* Report ID 2 */
		0x00,  /* X movement low byte = 0 */
		0x00,  /* X high/Y low = 0 */
		0x00   /* Y high byte = 0 */
	};

	/* Report ID 3: Release all consumer controls */
	uint8_t report3[] = {
		0x03,  /* Report ID 3 */
		0x00   /* No consumer controls pressed */
	};

	/* Send clear reports 3 times to ensure USB host processes them */
	for (int round = 0; round < 3; round++) {
		LOG_INF("Clear reports round %d/3", round + 1);

		/* Send Report 1 - Release all buttons/wheel */
		ret = usb_hid_send_report(report1, sizeof(report1));
		if (ret != 0) {
			LOG_ERR("Failed to send release report 1 round %d (err %d)", round + 1, ret);
			failed_count++;
		} else {
			LOG_INF("Release report 1 sent round %d (buttons/wheel cleared)", round + 1);
		}
		k_msleep(10);  /* 10ms delay between reports */

		/* Send Report 2 - Stop all movement */
		ret = usb_hid_send_report(report2, sizeof(report2));
		if (ret != 0) {
			LOG_ERR("Failed to send release report 2 round %d (err %d)", round + 1, ret);
			failed_count++;
		} else {
			LOG_INF("Release report 2 sent round %d (X/Y movement cleared)", round + 1);
		}
		k_msleep(10);  /* 10ms delay between reports */

		/* Send Report 3 - Release consumer controls */
		ret = usb_hid_send_report(report3, sizeof(report3));
		if (ret != 0) {
			LOG_ERR("Failed to send release report 3 round %d (err %d)", round + 1, ret);
			failed_count++;
		} else {
			LOG_INF("Release report 3 sent round %d (consumer controls cleared)", round + 1);
		}

		if (round < 2) {
			k_msleep(20);  /* Longer delay between rounds */
		}
	}

	if (failed_count > 0) {
		LOG_WRN("HID release-all completed with %d failed report(s)", failed_count);
		return -EIO;
	}

	LOG_INF("=== HID RELEASE-ALL COMPLETE - 3 ROUNDS SENT ===");
	return 0;
}
