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

/* LED for status indication */
static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

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
    0x05, 0x01,             /* UsagePage (Generic Desktop)           */
    0x09, 0x02,             /* Usage     (Mouse)                     */
    0xA1, 0x01,             /* Collection (Application)              */
      0x85, 0x01,           /*   Report ID (1)                       */
      0x09, 0x01,           /*   Usage     (Pointer)                 */
      0xA1, 0x00,           /*   Collection (Physical)               */

      /* --- 5 button bits + 3 padding bits + 8 wheel bits --------------- */
      0x95, 0x05,           /*   ReportCount 5                       */
      0x75, 0x01,           /*   ReportSize 1                        */
      0x05, 0x09,           /*   UsagePage (Button)                  */
      0x19, 0x01,           /*   Usage Minimum (Button 1)            */
      0x29, 0x05,           /*   Usage Maximum (Button 5)            */
      0x15, 0x00,           /*   Logical Minimum (0)                 */
      0x25, 0x01,           /*   Logical Maximum (1)                 */
      0x81, 0x02,           /*   Input (Data,Var,Abs)                */

      0x95, 0x01,           /*   ReportCount 1                       */
      0x75, 0x03,           /*   ReportSize 3                        */
      0x81, 0x01,           /*   Input (Constant)                    */

      0x75, 0x08,           /*   ReportSize 8                        */
      0x95, 0x01,           /*   ReportCount 1                       */
      0x05, 0x01,           /*   UsagePage (Generic Desktop)         */
      0x09, 0x38,           /*   Usage     (Wheel)                   */
      0x15, 0x81,           /*   Logical Minimum (-127)              */
      0x25, 0x7F,           /*   Logical Maximum (127)               */
      0x81, 0x06,           /*   Input (Data,Var,Rel,NoPref)         */

      0x05, 0x0C,           /*   UsagePage (Consumer)                */
      0x0A, 0x38, 0x02,     /*   Usage     (Volume)                  */
      0x95, 0x01,           /*   ReportCount 1                       */
      0x81, 0x06,           /*   Input (Data,Var,Rel,NoPref)         */

      0xC0,                  /*   End Collection                      */
      0x85, 0x02,           /*   Report ID (2)                       */

      /* --- X and Y movement : 2 × 12-bit signed ------------------------ */
      0x09, 0x01,           /*   Usage     (Pointer)                 */
      0xA1, 0x00,           /*   Collection (Physical)               */
      0x75, 0x0C,           /*   ReportSize 12                       */
      0x95, 0x02,           /*   ReportCount 2                       */
      0x05, 0x01,           /*   UsagePage (Generic Desktop)         */
      0x09, 0x30,           /*   Usage     (X)                       */
      0x09, 0x31,           /*   Usage     (Y)                       */
      0x16, 0x01, 0xF8,     /*   Logical Minimum (-2047)             */
      0x26, 0xFF, 0x07,     /*   Logical Maximum (2047)              */
      0x81, 0x06,           /*   Input (Data,Var,Rel,NoPref)         */

      0xC0,                  /*   End Collection                      */
      0xC0,                  /*   End Collection                      */

      /* --- Consumer controls : Media keys and volume ------------------- */
      0x05, 0x0C,           /*   UsagePage (Consumer)                */
      0x09, 0x01,           /*   Usage     (Consumer Control)        */
      0xA1, 0x01,           /*   Collection (Application)            */
      0x85, 0x03,           /*   Report ID (3)                       */
      0x15, 0x00,           /*   Logical Minimum (0)                 */
      0x25, 0x01,           /*   Logical Maximum (1)                 */
      0x75, 0x01,           /*   ReportSize 1                        */
      0x95, 0x01,           /*   ReportCount 1                       */

      0x09, 0xCD,           /*   Usage     (Play/Pause)              */
      0x81, 0x06,           /*   Input (Data,Var,Rel,NoPref)         */
      0x09, 0xB5,           /*   Usage     (Scan Next Track)         */
      0x81, 0x06,           /*   Input (Data,Var,Rel,NoPref)         */
      0x09, 0xB6,           /*   Usage     (Scan Previous Track)     */
      0x81, 0x06,           /*   Input (Data,Var,Rel,NoPref)         */
      0x09, 0xEA,           /*   Usage     (Volume Up)               */
      0x81, 0x06,           /*   Input (Data,Var,Rel,NoPref)         */
      0x09, 0xE9,           /*   Usage     (Volume Down)             */
      0x81, 0x06,           /*   Input (Data,Var,Rel,NoPref)         */
      0x0A, 0x25, 0x02,     /*   Usage     (Mute)                    */
      0x81, 0x06,           /*   Input (Data,Var,Rel,NoPref)         */
      0x0A, 0x24, 0x02,     /*   Usage     (Power)                   */
      0x81, 0x06,           /*   Input (Data,Var,Rel,NoPref)         */

      0xC0                   /* End Collection                        */
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

/**
 * @brief USB HID endpoint ready callback
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

/* USB HID operations structure */
static const struct hid_ops ops = {
	.int_in_ready = int_in_ready_cb,
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
 * The function runs indefinitely, toggling the LED to indicate the
 * system is alive. HID reports are sent directly from BLE callbacks
 * for zero-latency operation.
 * 
 * @return 0 on success, negative error code on failure
 */
int usb_init(void)
{
	int ret;
	
	/* Initialize USB endpoint semaphore */
	k_sem_init(&ep_write_sem, 0, 1);

	/* Configure status LED */
	if (!gpio_is_ready_dt(&led0)) {
		LOG_ERR("LED device %s is not ready", led0.port->name);
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&led0, GPIO_OUTPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure the LED pin, error: %d", ret);
		return ret;
	}

	/* Get USB HID device */
#if defined(CONFIG_USB_DEVICE_STACK_NEXT)
	hid_dev = DEVICE_DT_GET_ONE(zephyr_hid_device);
#else
	hid_dev = device_get_binding("HID_0");
#endif
	if (hid_dev == NULL) {
		LOG_ERR("Cannot get USB HID Device");
		return -ENODEV;
	}

	/* Register HID device with descriptor and callbacks */
	usb_hid_register_device(hid_dev,
				hid_report_desc, sizeof(hid_report_desc),
				&ops);

	usb_hid_init(hid_dev);

	/* Enable USB device stack */
#if defined(CONFIG_USB_DEVICE_STACK_NEXT)
	ret = enable_usb_device_next();
#else
	ret = usb_enable(status_cb);
#endif
	if (ret != 0) {
		LOG_ERR("Failed to enable USB");
		return ret;
	}

	LOG_INF("USB HID device initialized successfully");

	/* Main loop - keep thread alive and indicate system status */
	while (true) {
		k_sleep(K_MSEC(1000));
		/* Toggle LED periodically to show system is alive */
		(void)gpio_pin_toggle(led0.port, led0.pin);
	}
	
	return 0;
}
