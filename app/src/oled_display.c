/*
 * Copyright (c) 2025 Robert Dale Smith
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/display/cfb.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>

#include "oled_display.h"
#include "augmental_logo.h"

#define LOG_MODULE_NAME oled_display
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

/* Helper to get I2C device safely at runtime */
static const struct device *get_i2c_device(void)
{
    /* Try to get i2c1 first (XIAO), then i2c0 (Feather) */
    const struct device *dev;
    
    /* Try i2c1 first */
    dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(i2c1));
    if (dev && device_is_ready(dev)) {
        return dev;
    }
    
    /* Fallback to i2c0 */
    dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(i2c0));
    if (dev && device_is_ready(dev)) {
        return dev;
    }
    
    return NULL;
}

/* Display device and configuration */
static const struct device *display_dev;
static uint16_t display_rows;
static uint16_t display_cols;
static bool display_ready = false;
static bool display_available = false;

/* Display fonts and metrics */
static uint8_t font_width;
static uint8_t font_height;

/* Status tracking */
static uint8_t last_battery_level = 0xFF;
static bool last_connection_state = false;
static int8_t last_rssi_dbm = 0;

/* Private function declarations */
static int oled_display_setup_font(void);
static int oled_display_invert(void);
static int oled_display_set_contrast(uint8_t contrast);
static void oled_display_invert_bitmap(const uint8_t *src, uint8_t *dst, size_t size);
static int oled_display_invert_framebuffer(void);
static const char* rssi_to_signal_bars(int8_t rssi_dbm);

int oled_display_init(void)
{
    int ret;

    LOG_INF("Checking for OLED display...");

    /* Get display device */
    display_dev = DEVICE_DT_GET_OR_NULL(DT_ALIAS(oled_display));
    if (!display_dev || !device_is_ready(display_dev)) {
        LOG_INF("OLED display not detected - continuing without display");
        display_available = false;
        display_ready = false;
        return 0;  /* Return success to continue boot process */
    }

    display_available = true;
    LOG_INF("OLED display detected, initializing...");

    LOG_INF("Display device found: %s", display_dev->name);

    /* Initialize character framebuffer */
    ret = cfb_framebuffer_init(display_dev);
    if (ret != 0) {
        LOG_ERR("Failed to initialize framebuffer (err %d)", ret);
        return ret;
    }

    /* Get display dimensions */
    display_rows = cfb_get_display_parameter(display_dev, CFB_DISPLAY_ROWS);
    display_cols = cfb_get_display_parameter(display_dev, CFB_DISPLAY_COLS);
    
    LOG_INF("Display dimensions: %dx%d", display_cols, display_rows);

    /* Setup font */
    ret = oled_display_setup_font();
    if (ret != 0) {
        LOG_ERR("Failed to setup font (err %d)", ret);
        return ret;
    }

    /* Invert display for white text on black background */
    ret = oled_display_invert();
    if (ret != 0) {
        LOG_WRN("Failed to invert display (err %d) - continuing with normal display", ret);
        /* Continue anyway - inversion is not critical */
    } else {
        LOG_INF("Display inverted: white text on black background");
    }

    /* Clear display completely */
    ret = cfb_framebuffer_clear(display_dev, true);
    if (ret != 0) {
        LOG_ERR("Failed to clear framebuffer (err %d)", ret);
        return ret;
    }

    /* Small delay to ensure display clears */
    k_sleep(K_MSEC(100));

    display_ready = true;
    LOG_INF("OLED display initialized successfully");
    
    return 0;
}

int oled_display_clear(void)
{
    if (!display_available || !display_ready) {
        return 0;  /* Silently skip if no display */
    }

    int ret = cfb_framebuffer_clear(display_dev, true);
    if (ret != 0) {
        LOG_ERR("Failed to clear display (err %d)", ret);
        return ret;
    }

    return cfb_framebuffer_finalize(display_dev);
}

int oled_display_update_status(uint8_t battery_level, bool is_connected, int8_t rssi_dbm)
{
    char battery_str[32];
    char status_str[40];
    int ret;

    if (!display_available || !display_ready) {
        return 0;  /* Silently skip if no display */
    }

    /* Only update if something changed */
    if (battery_level == last_battery_level && 
        is_connected == last_connection_state &&
        rssi_dbm == last_rssi_dbm) {
        return 0;
    }

    /* Clear display */
    ret = cfb_framebuffer_clear(display_dev, false);
    if (ret != 0) {
        LOG_ERR("Failed to clear framebuffer (err %d)", ret);
        return ret;
    }

    /* Ensure display stays inverted during status transitions */
    ret = oled_display_invert();
    if (ret != 0) {
        LOG_WRN("Failed to reapply inversion during status update (err %d)", ret);
        /* Continue anyway - display will still work */
    }

    /* Use much larger line spacing for clear separation */
    uint16_t line_spacing = 16;  /* 3 lines fit: 0, 16, 32 */
    uint16_t y_pos = 0;

    /* Line 1: Device name - use connected device name when connected, otherwise default */
    extern const char *ble_transport_get_device_name(void);
    const char *full_title = is_connected ? ble_transport_get_device_name() : "MouthPad^USB";
    
    /* Truncate title to 12 characters for display */
    char display_title[13];  /* 12 chars + null terminator */
    strncpy(display_title, full_title, 12);
    display_title[12] = '\0';
    
    cfb_print(display_dev, display_title, 0, y_pos);
    y_pos += line_spacing;

    /* Line 2: Connection status */
    if (is_connected) {
        strcpy(status_str, "Connected");
    } else {
        strcpy(status_str, "Scanning...");
    }
    cfb_print(display_dev, status_str, 0, y_pos);
    y_pos += line_spacing;

    /* Line 3: Battery status with icon */
    if (battery_level == 0xFF || battery_level > 100) {
        strcpy(battery_str, "");  /* Just "" for unknown */
    } else {
        /* Create battery icon based on charge level */
        char battery_icon[8];
        if (battery_level > 75) {
            strcpy(battery_icon, "[||||]");  /* Full battery */
        } else if (battery_level > 50) {
            strcpy(battery_icon, "[|||.]");  /* 3/4 battery */
        } else if (battery_level > 25) {
            strcpy(battery_icon, "[||..]");  /* 1/2 battery */
        } else if (battery_level > 10) {
            strcpy(battery_icon, "[|...]");  /* 1/4 battery */
        } else {
            strcpy(battery_icon, "[....]");  /* Low battery */
        }
        snprintf(battery_str, sizeof(battery_str), "%s %d%%", battery_icon, battery_level);
    }
    cfb_print(display_dev, battery_str, 0, y_pos);
    y_pos += line_spacing;

    /* Line 4: Signal strength (only when connected) */
    if (is_connected) {
        const char* signal_bars = rssi_to_signal_bars(rssi_dbm);
        char signal_str[32];
        snprintf(signal_str, sizeof(signal_str), "%s%ddBm", signal_bars, rssi_dbm);
        cfb_print(display_dev, signal_str, 0, y_pos);
    }

    /* Update display - hardware inversion is already set */
    ret = cfb_framebuffer_finalize(display_dev);
    if (ret != 0) {
        LOG_ERR("Failed to finalize framebuffer (err %d)", ret);
        return ret;
    }

    /* Update last known state */
    last_battery_level = battery_level;
    last_connection_state = is_connected;
    last_rssi_dbm = rssi_dbm;

    LOG_DBG("Display updated: battery=%d%%, connected=%s, rssi=%ddBm", 
            battery_level, is_connected ? "yes" : "no", rssi_dbm);

    return 0;
}

int oled_display_message(const char *message)
{
    int ret;

    if (!display_available || !display_ready || !message) {
        return 0;  /* Silently skip if no display or invalid message */
    }

    /* Clear display */
    ret = cfb_framebuffer_clear(display_dev, false);
    if (ret != 0) {
        LOG_ERR("Failed to clear framebuffer (err %d)", ret);
        return ret;
    }

    /* Display message */
    cfb_print(display_dev, message, 0, 0);

    /* Update display */
    ret = cfb_framebuffer_finalize(display_dev);
    if (ret != 0) {
        LOG_ERR("Failed to finalize framebuffer (err %d)", ret);
        return ret;
    }

    return 0;
}

int oled_display_device_info(const char *device_name, uint32_t connection_count)
{
    char count_str[32];
    int ret;

    if (!display_available || !display_ready || !device_name) {
        return 0;  /* Silently skip if no display or invalid device name */
    }

    /* Clear display */
    ret = cfb_framebuffer_clear(display_dev, false);
    if (ret != 0) {
        LOG_ERR("Failed to clear framebuffer (err %d)", ret);
        return ret;
    }

    /* Line 1: Device info header */
    cfb_print(display_dev, "Device Info", 0, 0);

    /* Line 2: Device name (truncate if too long) */
    char truncated_name[21]; /* 20 chars + null */
    strncpy(truncated_name, device_name, 20);
    truncated_name[20] = '\0';
    cfb_print(display_dev, truncated_name, 0, font_height);

    /* Line 3: Connection count */
    snprintf(count_str, sizeof(count_str), "Connections: %u", connection_count);
    cfb_print(display_dev, count_str, 0, font_height * 2);

    /* Line 4: Current time (uptime) */
    uint32_t uptime_sec = k_uptime_get() / 1000;
    uint32_t hours = uptime_sec / 3600;
    uint32_t minutes = (uptime_sec % 3600) / 60;
    uint32_t seconds = uptime_sec % 60;
    
    char uptime_str[20];
    snprintf(uptime_str, sizeof(uptime_str), "Up: %02u:%02u:%02u", hours, minutes, seconds);
    cfb_print(display_dev, uptime_str, 0, font_height * 3);

    /* Update display */
    ret = cfb_framebuffer_finalize(display_dev);
    if (ret != 0) {
        LOG_ERR("Failed to finalize framebuffer (err %d)", ret);
        return ret;
    }

    return 0;
}

/* Private function implementations */
static int oled_display_setup_font(void)
{
    int ret;

    /* Get number of fonts available */
    int font_count = cfb_get_numof_fonts(display_dev);
    if (font_count < 1) {
        LOG_ERR("No fonts available");
        return -ENOENT;
    }

    LOG_INF("Available fonts: %d", font_count);

    /* Use the first available font */
    ret = cfb_framebuffer_set_font(display_dev, 0);
    if (ret != 0) {
        LOG_ERR("Failed to set font (err %d)", ret);
        return ret;
    }

    /* Get font information from CFB parameters */
    uint16_t width = cfb_get_display_parameter(display_dev, CFB_DISPLAY_WIDTH);
    uint16_t height = cfb_get_display_parameter(display_dev, CFB_DISPLAY_HEIGHT);
    
    /* For typical SSD1306 with 8x8 font, use standard metrics */
    font_width = 6;  /* Standard 6-pixel wide font */
    font_height = 8; /* Standard 8-pixel tall font */
    
    LOG_INF("Using standard font size: %dx%d (display: %dx%d, chars: %dx%d)", 
            font_width, font_height, width, height, display_cols, display_rows);

    LOG_INF("Font set: %dx%d pixels", font_width, font_height);

    return 0;
}

/* Private function to set normal (non-inverted) SSD1306 display via I2C command */
__maybe_unused static int oled_display_normal(void)
{
    const struct device *i2c_dev;
    uint8_t cmd_buffer[2] = {0x00, 0xA6}; /* Command prefix + SSD1306 normal display command */
    int ret;

    if (!display_available || !display_ready) {
        return 0;  /* Silently skip if no display */
    }

    /* Get I2C device */
    i2c_dev = get_i2c_device();
    if (!i2c_dev || !device_is_ready(i2c_dev)) {
        LOG_WRN("I2C device not available for display normal mode");
        return 0;  /* Silently continue */
    }

    /* Send normal display command to SSD1306 with proper command format */
    /* 0x00 = Command byte, 0xA6 = Normal display command */
    ret = i2c_write(i2c_dev, cmd_buffer, 2, 0x3c);
    if (ret != 0) {
        LOG_ERR("Failed to send normal display command via I2C (err %d)", ret);
        /* Try again once after a brief delay */
        k_sleep(K_MSEC(1));
        ret = i2c_write(i2c_dev, cmd_buffer, 2, 0x3c);
        if (ret != 0) {
            LOG_ERR("Normal display command failed again (err %d)", ret);
            return ret;
        }
    }

    LOG_DBG("SSD1306 normal display command sent successfully (0x00 0xA6)");
    return 0;
}

/* Private function to invert the SSD1306 display via I2C command */
static int oled_display_invert(void)
{
    const struct device *i2c_dev;
    uint8_t cmd_buffer[2] = {0x00, 0xA7}; /* Command prefix + SSD1306 invert display command */
    int ret;

    /* Get I2C device */
    i2c_dev = get_i2c_device();
    if (!i2c_dev || !device_is_ready(i2c_dev)) {
        LOG_ERR("I2C device not ready for display inversion");
        return -ENODEV;
    }

    /* Send invert command to SSD1306 with proper command format */
    /* 0x00 = Command byte, 0xA7 = Invert display command */
    ret = i2c_write(i2c_dev, cmd_buffer, 2, 0x3c);
    if (ret != 0) {
        LOG_ERR("Failed to send invert command via I2C (err %d)", ret);
        return ret;
    }

    LOG_DBG("SSD1306 invert command sent successfully (0x00 0xA7)");
    return 0;
}

/* Check if display is available for use */
bool oled_display_is_available(void)
{
    return display_available && display_ready;
}

/* Set display contrast level (0-255) */
static int oled_display_set_contrast(uint8_t contrast)
{
    const struct device *i2c_dev;
    uint8_t cmd_buffer[2] = {0x00, 0x81}; /* Command prefix + Set contrast command */
    uint8_t contrast_buffer[2] = {0x00, contrast}; /* Command prefix + contrast value */
    int ret;

    /* Get I2C device */
    i2c_dev = get_i2c_device();
    if (!i2c_dev || !device_is_ready(i2c_dev)) {
        LOG_WRN("I2C device not available for contrast adjustment");
        return 0;  /* Silently continue */
    }

    /* Send contrast command */
    ret = i2c_write(i2c_dev, cmd_buffer, 2, 0x3c);
    if (ret != 0) {
        LOG_ERR("Failed to send contrast command via I2C (err %d)", ret);
        return ret;
    }

    /* Send contrast value */
    ret = i2c_write(i2c_dev, contrast_buffer, 2, 0x3c);
    if (ret != 0) {
        LOG_ERR("Failed to send contrast value via I2C (err %d)", ret);
        return ret;
    }

    LOG_DBG("Display contrast set to %d", contrast);
    return 0;
}

/* Invert bitmap data (flip all bits) */
static void oled_display_invert_bitmap(const uint8_t *src, uint8_t *dst, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        dst[i] = ~src[i];  /* Invert all bits */
    }
}

/* Display splash screen with Augmental logo */
int oled_display_splash_screen(uint32_t duration_ms)
{
    int ret;

    if (!display_available || !display_ready) {
        return 0;  /* Silently skip if no display */
    }

    LOG_INF("Displaying Augmental logo splash screen...");

    /* Clear display first using CFB */
    ret = cfb_framebuffer_clear(display_dev, false);
    if (ret != 0) {
        LOG_ERR("Failed to clear framebuffer for splash (err %d)", ret);
        return ret;
    }
    
    /* Finalize CFB to ensure display is cleared */
    ret = cfb_framebuffer_finalize(display_dev);
    if (ret != 0) {
        LOG_ERR("Failed to finalize clear framebuffer (err %d)", ret);
        return ret;
    }

    /* Apply inversion BEFORE displaying the logo */
    ret = oled_display_invert();
    if (ret != 0) {
        LOG_WRN("Failed to apply inversion before splash screen (err %d)", ret);
        /* Continue anyway */
    }

    /* Invert the bitmap data for proper display with hardware inversion */
    static uint8_t inverted_logo[128 * 64 / 8];  /* Buffer for inverted logo */
    oled_display_invert_bitmap(augmental_logo_bitmap, inverted_logo, 
                               AUGMENTAL_LOGO_WIDTH * AUGMENTAL_LOGO_HEIGHT / 8);
    
    /* Prepare buffer descriptor */
    struct display_buffer_descriptor desc = {
        .buf_size = AUGMENTAL_LOGO_WIDTH * AUGMENTAL_LOGO_HEIGHT / 8,
        .width = AUGMENTAL_LOGO_WIDTH,
        .height = AUGMENTAL_LOGO_HEIGHT,
        .pitch = AUGMENTAL_LOGO_WIDTH,
        .frame_incomplete = false
    };

    /* Write the inverted bitmap to the display */
    ret = display_write(display_dev, 0, 0, &desc, inverted_logo);
    if (ret != 0) {
        LOG_ERR("Failed to write logo bitmap (err %d)", ret);
        /* Fall back to text display on error */
        uint16_t center_x = 64 - (9 * font_width / 2);  /* Center "AUGMENTAL" */
        uint16_t center_y = 32 - font_height;
        
        cfb_print(display_dev, "AUGMENTAL", center_x, center_y);
        cfb_print(display_dev, "TECH", center_x + 20, center_y + font_height + 4);
        
        ret = cfb_framebuffer_finalize(display_dev);
        if (ret != 0) {
            LOG_ERR("Failed to finalize text fallback (err %d)", ret);
            return ret;
        }
    }

    /* Set full brightness immediately - no fade in */
    oled_display_set_contrast(255);
    
    /* Hold splash screen at full brightness */
    k_sleep(K_MSEC(duration_ms));

    /* Fast fade out effect */
    LOG_INF("Starting fast fade out...");
    for (int contrast = 255; contrast >= 0; contrast -= 51) {  /* Faster steps: 255, 204, 153, 102, 51, 0 */
        oled_display_set_contrast(contrast);
        k_sleep(K_MSEC(25));  /* Slightly longer per step but fewer steps */
    }
    
    /* Turn display OFF completely to avoid any flashing */
    const struct device *i2c_dev = get_i2c_device();
    if (i2c_dev && device_is_ready(i2c_dev)) {
        uint8_t display_off_cmd[2] = {0x00, 0xAE}; /* Display OFF command */
        i2c_write(i2c_dev, display_off_cmd, 2, 0x3c);
    }
    
    k_sleep(K_MSEC(50));  /* Brief pause while off */

    /* Clear the bitmap completely and prepare status display while display is OFF */
    ret = cfb_framebuffer_clear(display_dev, false);
    if (ret != 0) {
        LOG_ERR("Failed to clear framebuffer (err %d)", ret);
        return ret;
    }

    /* Render the status display content while display is OFF */
    uint16_t line_spacing = 16;
    uint16_t y_pos = 0;
    
    /* Line 1: MouthPad^USB title */
    cfb_print(display_dev, "MouthPad^USB", 0, y_pos);
    y_pos += line_spacing;
    
    /* Line 2: Scanning status */
    cfb_print(display_dev, "Scanning...", 0, y_pos);
    
    /* Finalize the content while display is still OFF */
    ret = cfb_framebuffer_finalize(display_dev);
    if (ret != 0) {
        LOG_ERR("Failed to finalize status display (err %d)", ret);
        return ret;
    }

    /* Restore full contrast first */
    oled_display_set_contrast(255);
    
    /* Small delay to ensure all settings are applied */
    k_sleep(K_MSEC(10));

    /* Now turn display back ON - content is already rendered and inverted */
    if (i2c_dev && device_is_ready(i2c_dev)) {
        uint8_t display_on_cmd[2] = {0x00, 0xAF}; /* Display ON command */
        i2c_write(i2c_dev, display_on_cmd, 2, 0x3c);
    }

    /* Ensure inversion is reapplied after display ON command */
    ret = oled_display_invert();
    if (ret != 0) {
        LOG_WRN("Failed to reapply inversion after splash screen (err %d)", ret);
        /* Continue anyway */
    }
    
    /* Reset the display state so the next update will work properly */
    oled_display_reset_state();

    LOG_INF("Splash screen complete - transitioned to status display");

    return 0;
}

/* Reset display state to force next status update */
void oled_display_reset_state(void)
{
    /* Reset state tracking so next update will definitely trigger */
    last_battery_level = 0xFE;  /* Different from any real value */
    last_connection_state = true;  /* Opposite of typical initial state */
    last_rssi_dbm = -100;  /* Different from any typical value */
}

int oled_display_scanning(void)
{
    int ret;
    
    if (!display_available || !display_ready) {
        return 0;  /* Silently skip if no display */
    }
    
    /* Clear display */
    ret = cfb_framebuffer_clear(display_dev, false);
    if (ret != 0) {
        LOG_ERR("Failed to clear framebuffer (err %d)", ret);
        return ret;
    }
    
    /* Ensure display stays inverted */
    ret = oled_display_invert();
    if (ret != 0) {
        LOG_WRN("Failed to reapply inversion during scanning (err %d)", ret);
        /* Continue anyway */
    }
    
    /* Use same line spacing as main status display */
    uint16_t line_spacing = 16;
    uint16_t y_pos = 0;
    
    /* Display scanning message */
    cfb_print(display_dev, "MouthPad USB", 0, y_pos);
    y_pos += line_spacing;
    cfb_print(display_dev, "Scanning...", 0, y_pos);
    
    /* Update display */
    ret = cfb_framebuffer_finalize(display_dev);
    if (ret != 0) {
        LOG_ERR("Failed to finalize framebuffer (err %d)", ret);
        return ret;
    }
    
    /* Extra inversion call after finalize to ensure it sticks */
    ret = oled_display_invert();
    if (ret != 0) {
        LOG_WRN("Failed to reapply inversion after scanning finalize (err %d)", ret);
        /* Continue anyway */
    }
    
    LOG_DBG("Scanning status displayed");
    return 0;
}

int oled_display_device_found(const char *device_name)
{
    int ret;
    
    if (!display_available || !display_ready) {
        return 0;  /* Silently skip if no display */
    }
    
    /* Clear display */
    ret = cfb_framebuffer_clear(display_dev, false);
    if (ret != 0) {
        LOG_ERR("Failed to clear framebuffer (err %d)", ret);
        return ret;
    }
    
    /* Ensure display stays inverted */
    ret = oled_display_invert();
    if (ret != 0) {
        LOG_WRN("Failed to reapply inversion during device found (err %d)", ret);
        /* Continue anyway */
    }
    
    /* Use same line spacing as main status display */
    uint16_t line_spacing = 16;
    uint16_t y_pos = 0;
    
    /* Display device found message */
    cfb_print(display_dev, "MouthPad USB", 0, y_pos);
    y_pos += line_spacing;
    cfb_print(display_dev, "Pairing...", 0, y_pos);
    
    /* Update display */
    ret = cfb_framebuffer_finalize(display_dev);
    if (ret != 0) {
        LOG_ERR("Failed to finalize framebuffer (err %d)", ret);
        return ret;
    }
    
    /* Extra inversion call after finalize to ensure it sticks */
    ret = oled_display_invert();
    if (ret != 0) {
        LOG_WRN("Failed to reapply inversion after device found finalize (err %d)", ret);
        /* Continue anyway */
    }
    
    LOG_DBG("Device found status displayed: %s", device_name ? device_name : "Unknown");
    return 0;
}

int oled_display_pairing(void)
{
    int ret;
    
    if (!display_available || !display_ready) {
        return 0;  /* Silently skip if no display */
    }
    
    /* Clear display */
    ret = cfb_framebuffer_clear(display_dev, false);
    if (ret != 0) {
        LOG_ERR("Failed to clear framebuffer (err %d)", ret);
        return ret;
    }
    
    /* Ensure display stays inverted */
    ret = oled_display_invert();
    if (ret != 0) {
        LOG_WRN("Failed to reapply inversion during pairing (err %d)", ret);
        /* Continue anyway */
    }
    
    /* Use same line spacing as main status display */
    uint16_t line_spacing = 16;
    uint16_t y_pos = 0;
    
    /* Display pairing message */
    cfb_print(display_dev, "MouthPad USB", 0, y_pos);
    y_pos += line_spacing;
    cfb_print(display_dev, "Pairing...", 0, y_pos);
    
    /* Update display */
    ret = cfb_framebuffer_finalize(display_dev);
    if (ret != 0) {
        LOG_ERR("Failed to finalize framebuffer (err %d)", ret);
        return ret;
    }
    
    /* Extra inversion call after finalize to ensure it sticks */
    ret = oled_display_invert();
    if (ret != 0) {
        LOG_WRN("Failed to reapply inversion after pairing finalize (err %d)", ret);
        /* Continue anyway */
    }
    
    LOG_DBG("Pairing status displayed");
    return 0;
}

/* Custom framebuffer inversion function that directly manipulates pixel data */
__maybe_unused static int oled_display_invert_framebuffer(void)
{
    if (!display_available || !display_ready) {
        return 0;  /* Silently skip if no display */
    }

    /* Get direct access to the display buffer */
    size_t buffer_size;
    
    /* For SSD1306 128x64 display = 128 * 64 / 8 = 1024 bytes */
    buffer_size = display_cols * display_rows / 8;
    
    /* Try to get the CFB buffer - this might not work directly */
    /* So we'll use display_write with an inverted pattern instead */
    
    /* Create a temporary buffer for the inverted data */
    static uint8_t inverted_buffer[1024]; /* Max size for 128x64 display */
    
    /* First, we need to read the current framebuffer content */
    /* Since we can't directly read from CFB, we'll use display_read if available */
    /* or work with a known pattern */
    
    /* For now, let's create a fully inverted pattern */
    for (size_t i = 0; i < buffer_size && i < sizeof(inverted_buffer); i++) {
        inverted_buffer[i] = 0xFF; /* All pixels on = dark background */
    }
    
    /* Write the inverted pattern to the display */
    struct display_buffer_descriptor desc = {
        .buf_size = buffer_size,
        .width = display_cols,
        .height = display_rows,
        .pitch = display_cols,
        .frame_incomplete = false
    };
    
    int ret = display_write(display_dev, 0, 0, &desc, inverted_buffer);
    if (ret != 0) {
        LOG_ERR("Failed to write inverted framebuffer (err %d)", ret);
        return ret;
    }
    
    LOG_DBG("Framebuffer manually inverted");
    return 0;
}

/* Convert RSSI value to signal strength bars */
static const char* rssi_to_signal_bars(int8_t rssi_dbm)
{
    /* RSSI to signal bar conversion based on typical BLE signal strength:
     * -30 dBm = Excellent (4 bars)  [||||]
     * -50 dBm = Good (3 bars)       [|||.]
     * -70 dBm = Fair (2 bars)       [||..]  
     * -80 dBm = Weak (1 bar)        [|...]
     * -90+ dBm = Very weak (0 bars) [....]
     */
    if (rssi_dbm >= -40) {
        return "[||||]";  /* Excellent */
    } else if (rssi_dbm >= -60) {
        return "[|||.]";  /* Good */
    } else if (rssi_dbm >= -75) {
        return "[||..]";  /* Fair */
    } else if (rssi_dbm >= -85) {
        return "[|...]";  /* Weak */
    } else {
        return "[....]";  /* Very weak */
    }
}