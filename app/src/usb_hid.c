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
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usb_hid.h>
#include <zephyr/logging/log.h>

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

/* USB device status tracking */

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
static enum usb_dc_status_code usb_status;


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
 * @brief USB device status change callback
 * 
 * @param status New USB device status
 * @param param Additional status parameters (unused)
 */
static inline void status_cb(enum usb_dc_status_code status, const uint8_t *param)
{
	usb_status = status;
}

/**
 * @brief Request USB wakeup if device is suspended
 * 
 * Called when input activity is detected while USB is suspended
 */
static ALWAYS_INLINE void rwup_if_suspended(void)
{
	if (IS_ENABLED(CONFIG_USB_DEVICE_REMOTE_WAKEUP)) {
		if (usb_status == USB_DC_SUSPEND) {
			usb_wakeup_request();
			return;
		}
	}
}

/* USB HID output report callback function pointer (for future expansion) */

/**
 * @brief USB HID interrupt IN endpoint ready callback
 * 
 * Called when the USB HID interrupt endpoint is ready to accept data.
 * Signals the semaphore to allow BLE module to send reports.
 * 
 * @param dev USB HID device (unused)
 */
static void int_in_ready_cb(const struct device *dev)
{
	ARG_UNUSED(dev);
	k_sem_give(&ep_write_sem);
}

/**
 * @brief USB HID interrupt OUT endpoint ready callback
 * 
 * Called when the USB HID receives output reports from the host.
 * This captures mouse button reports that we need for buzzer feedback.
 * 
 * @param dev USB HID device (unused)
 */
static void int_out_ready_cb(const struct device *dev)
{
	ARG_UNUSED(dev);
	LOG_INF("USB HID int_out_ready callback triggered");
	
	// TODO: Get the actual output report data
	// This callback indicates output data is available, but we need to read it
}

/* USB HID operations structure */
static const struct hid_ops ops = {
	.int_in_ready = int_in_ready_cb,
	.int_out_ready = int_out_ready_cb,
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

	/* Get USB HID device */
#if defined(CONFIG_USB_DEVICE_STACK_NEXT)
	hid_dev = DEVICE_DT_GET_ONE(zephyr_hid_device);
#else
	/* Try different possible HID device names */
	hid_dev = device_get_binding("HID_0");
	if (hid_dev == NULL) {
		hid_dev = device_get_binding("HID0");
	}
	if (hid_dev == NULL) {
		hid_dev = device_get_binding("HID_1");
	}
	if (hid_dev == NULL) {
		hid_dev = device_get_binding("HID");
	}
	if (hid_dev == NULL) {
		/* Try to get any HID device using device tree */
		hid_dev = DEVICE_DT_GET_ANY(zephyr_hid_device);
	}
	if (hid_dev == NULL) {
		LOG_ERR("Cannot get USB HID Device - no HID device found");
		LOG_WRN("Trying to initialize USB HID without specific device binding");
		/* Try to initialize USB HID without device binding */
		hid_dev = NULL;
	}

	LOG_INF("Found USB HID device: %s", hid_dev ? hid_dev->name : "NULL (using fallback)");
#endif
	if (hid_dev == NULL) {
		LOG_ERR("Cannot get USB HID Device - no HID device found");
		return -ENODEV;
	}

	LOG_INF("Found USB HID device: %s", hid_dev->name);

	LOG_INF("Registering HID device with descriptor...");
	/* Register HID device with descriptor and callbacks */
	usb_hid_register_device(hid_dev,
				hid_report_desc, sizeof(hid_report_desc),
				&ops);
	LOG_INF("HID device registered successfully");
	k_sleep(K_MSEC(10)); // Small delay to ensure registration completes

	LOG_INF("Initializing USB HID device...");
	int init_ret = usb_hid_init(hid_dev);
	if (init_ret != 0) {
		LOG_ERR("Failed to initialize USB HID device (err %d)", init_ret);
		return init_ret;
	}
	LOG_INF("USB HID device initialized");
	k_sleep(K_MSEC(10)); // Small delay to ensure initialization completes

	LOG_INF("Enabling USB device stack...");
	/* Enable USB device stack only if not already enabled by CDC */
#if defined(CONFIG_USB_DEVICE_STACK_NEXT)
	ret = enable_usb_device_next();
#else
	/* Check if USB is already enabled by CDC */
	if (usb_status == USB_DC_UNKNOWN) {
		ret = usb_enable(status_cb);
	} else {
		LOG_INF("USB stack already enabled by CDC, skipping HID enable");
		ret = 0;
	}
#endif
	if (ret != 0) {
		LOG_ERR("Failed to enable USB");
		return ret;
	}
	LOG_INF("USB device stack enabled successfully");

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
	
	/* Wait for endpoint to be ready */
	ret = k_sem_take(&ep_write_sem, K_MSEC(100));
	if (ret != 0) {
		LOG_ERR("USB HID endpoint not ready (err %d)", ret);
		return ret;
	}
	
	LOG_DBG("USB HID endpoint ready, sending report...");
	/* Send the HID report */
	ret = hid_int_ep_write(hid_dev, data, len, NULL);
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
	
	LOG_INF("=== SENDING HID RELEASE-ALL REPORTS ===");
	
	if (hid_dev == NULL) {
		LOG_ERR("USB HID device not initialized");
		return -ENODEV;
	}
	
	/* Report ID 1: Release all buttons and wheel */
	uint8_t report1[] = {
		0x01,  /* Report ID 1 */
		0x00,  /* No buttons pressed */
		0x00   /* No wheel movement */
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
	
	/* Send Report 1 - Release all buttons/wheel */
	ret = usb_hid_send_report(report1, sizeof(report1));
	if (ret != 0) {
		LOG_ERR("Failed to send release report 1 (err %d)", ret);
		return ret;
	}
	
	/* Send Report 2 - Stop all movement */
	ret = usb_hid_send_report(report2, sizeof(report2));
	if (ret != 0) {
		LOG_ERR("Failed to send release report 2 (err %d)", ret);
		return ret;
	}
	
	/* Send Report 3 - Release consumer controls */
	ret = usb_hid_send_report(report3, sizeof(report3));
	if (ret != 0) {
		LOG_ERR("Failed to send release report 3 (err %d)", ret);
		return ret;
	}
	
	LOG_INF("HID release-all reports sent successfully");
	return 0;
}
