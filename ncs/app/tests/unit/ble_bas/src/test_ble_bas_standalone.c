/*
 * Copyright (c) 2025 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Standalone unit tests for BLE BAS battery color calculations
 * This version doesn't depend on the actual ble_bas.c to avoid BLE dependencies
 */

#include <zephyr/ztest.h>
#include <stdint.h>
#include <stdbool.h>

/* Copy the enum and struct definitions from ble_bas.h */
typedef enum {
	BAS_COLOR_MODE_DISCRETE,  /* 4 discrete colors per quarter (green/yellow/orange/red) */
	BAS_COLOR_MODE_GRADIENT   /* Smooth gradient from green to red based on percentage */
} ble_bas_color_mode_t;

typedef struct {
	uint8_t red;    /* Red component (0-255) */
	uint8_t green;  /* Green component (0-255) */
	uint8_t blue;   /* Blue component (0-255) */
} ble_bas_rgb_color_t;

/* Inline copy of the battery color function for testing */
static ble_bas_rgb_color_t get_battery_color(uint8_t battery_level, ble_bas_color_mode_t mode)
{
	ble_bas_rgb_color_t color = {0, 0, 0}; /* Default to off */

	/* If battery level is invalid/unknown, default to green (assume full battery) */
	if (battery_level == 0xFF || battery_level > 100) {
		color.green = 255;
		return color;
	}

	if (mode == BAS_COLOR_MODE_DISCRETE) {
		/* Segmented colors optimized for GPIO LEDs */
		if (battery_level >= 50) {
			/* Green: 100-50% */
			color.green = 255;
		} else if (battery_level >= 10) {
			/* Yellow: 49-10% */
			color.red = 255;
			color.green = 255;
		} else {
			/* Red: 10-0% */
			color.red = 255;
		}
	} else {
		/* Gradient mode: smooth transition from green to red */
		if (battery_level >= 50) {
			/* Green to yellow gradient (100% to 50%) */
			/* Green stays at 255, red increases from 0 to 255 */
			color.green = 255;
			color.red = (uint8_t)(255 * (100 - battery_level) / 50);
		} else {
			/* Yellow to red gradient (50% to 0%) */
			/* Red stays at 255, green decreases from 255 to 0 */
			color.red = 255;
			color.green = (uint8_t)(255 * battery_level / 50);
		}
	}

	return color;
}

/* Test suite for Battery Service color calculations - Discrete Mode */
ZTEST_SUITE(ble_bas_color_discrete, NULL, NULL, NULL, NULL, NULL);

ZTEST(ble_bas_color_discrete, test_full_battery_100_percent)
{
	ble_bas_rgb_color_t color = get_battery_color(100, BAS_COLOR_MODE_DISCRETE);

	zassert_equal(color.red, 0, "Red should be 0 for full battery");
	zassert_equal(color.green, 255, "Green should be 255 for full battery");
	zassert_equal(color.blue, 0, "Blue should be 0");
}

ZTEST(ble_bas_color_discrete, test_high_battery_75_percent)
{
	ble_bas_rgb_color_t color = get_battery_color(75, BAS_COLOR_MODE_DISCRETE);

	zassert_equal(color.red, 0, "Red should be 0 for 75%% battery");
	zassert_equal(color.green, 255, "Green should be 255 for 75%% battery");
	zassert_equal(color.blue, 0, "Blue should be 0");
}

ZTEST(ble_bas_color_discrete, test_boundary_50_percent)
{
	ble_bas_rgb_color_t color = get_battery_color(50, BAS_COLOR_MODE_DISCRETE);

	zassert_equal(color.red, 0, "Red should be 0 for 50%% battery");
	zassert_equal(color.green, 255, "Green should be 255 for 50%% battery");
	zassert_equal(color.blue, 0, "Blue should be 0");
}

ZTEST(ble_bas_color_discrete, test_medium_battery_49_percent)
{
	ble_bas_rgb_color_t color = get_battery_color(49, BAS_COLOR_MODE_DISCRETE);

	zassert_equal(color.red, 255, "Red should be 255 for 49%% battery");
	zassert_equal(color.green, 255, "Green should be 255 for 49%% battery (yellow)");
	zassert_equal(color.blue, 0, "Blue should be 0");
}

ZTEST(ble_bas_color_discrete, test_medium_battery_25_percent)
{
	ble_bas_rgb_color_t color = get_battery_color(25, BAS_COLOR_MODE_DISCRETE);

	zassert_equal(color.red, 255, "Red should be 255 for 25%% battery");
	zassert_equal(color.green, 255, "Green should be 255 for 25%% battery (yellow)");
	zassert_equal(color.blue, 0, "Blue should be 0");
}

ZTEST(ble_bas_color_discrete, test_boundary_10_percent)
{
	ble_bas_rgb_color_t color = get_battery_color(10, BAS_COLOR_MODE_DISCRETE);

	zassert_equal(color.red, 255, "Red should be 255 for 10%% battery");
	zassert_equal(color.green, 255, "Green should be 255 for 10%% battery (yellow)");
	zassert_equal(color.blue, 0, "Blue should be 0");
}

ZTEST(ble_bas_color_discrete, test_low_battery_9_percent)
{
	ble_bas_rgb_color_t color = get_battery_color(9, BAS_COLOR_MODE_DISCRETE);

	zassert_equal(color.red, 255, "Red should be 255 for 9%% battery");
	zassert_equal(color.green, 0, "Green should be 0 for low battery");
	zassert_equal(color.blue, 0, "Blue should be 0");
}

ZTEST(ble_bas_color_discrete, test_critical_battery_5_percent)
{
	ble_bas_rgb_color_t color = get_battery_color(5, BAS_COLOR_MODE_DISCRETE);

	zassert_equal(color.red, 255, "Red should be 255 for 5%% battery");
	zassert_equal(color.green, 0, "Green should be 0 for critical battery");
	zassert_equal(color.blue, 0, "Blue should be 0");
}

ZTEST(ble_bas_color_discrete, test_empty_battery_0_percent)
{
	ble_bas_rgb_color_t color = get_battery_color(0, BAS_COLOR_MODE_DISCRETE);

	zassert_equal(color.red, 255, "Red should be 255 for empty battery");
	zassert_equal(color.green, 0, "Green should be 0 for empty battery");
	zassert_equal(color.blue, 0, "Blue should be 0");
}

ZTEST(ble_bas_color_discrete, test_invalid_battery_0xFF)
{
	ble_bas_rgb_color_t color = get_battery_color(0xFF, BAS_COLOR_MODE_DISCRETE);

	zassert_equal(color.red, 0, "Red should be 0 for invalid battery");
	zassert_equal(color.green, 255, "Green should be 255 for invalid battery (default)");
	zassert_equal(color.blue, 0, "Blue should be 0");
}

ZTEST(ble_bas_color_discrete, test_out_of_range_battery_150)
{
	ble_bas_rgb_color_t color = get_battery_color(150, BAS_COLOR_MODE_DISCRETE);

	zassert_equal(color.red, 0, "Red should be 0 for out-of-range battery");
	zassert_equal(color.green, 255, "Green should be 255 for out-of-range battery");
	zassert_equal(color.blue, 0, "Blue should be 0");
}

/* Test suite for Battery Service color calculations - Gradient Mode */
ZTEST_SUITE(ble_bas_color_gradient, NULL, NULL, NULL, NULL, NULL);

ZTEST(ble_bas_color_gradient, test_gradient_full_battery_100_percent)
{
	ble_bas_rgb_color_t color = get_battery_color(100, BAS_COLOR_MODE_GRADIENT);

	zassert_equal(color.red, 0, "Red should be 0 for 100%% battery");
	zassert_equal(color.green, 255, "Green should be 255 for 100%% battery");
	zassert_equal(color.blue, 0, "Blue should be 0");
}

ZTEST(ble_bas_color_gradient, test_gradient_75_percent)
{
	ble_bas_rgb_color_t color = get_battery_color(75, BAS_COLOR_MODE_GRADIENT);

	zassert_equal(color.red, 127, "Red should be 127 for 75%% battery");
	zassert_equal(color.green, 255, "Green should be 255 for 75%% battery");
	zassert_equal(color.blue, 0, "Blue should be 0");
}

ZTEST(ble_bas_color_gradient, test_gradient_boundary_50_percent)
{
	ble_bas_rgb_color_t color = get_battery_color(50, BAS_COLOR_MODE_GRADIENT);

	zassert_equal(color.red, 255, "Red should be 255 for 50%% battery");
	zassert_equal(color.green, 255, "Green should be 255 for 50%% battery");
	zassert_equal(color.blue, 0, "Blue should be 0");
}

ZTEST(ble_bas_color_gradient, test_gradient_25_percent)
{
	ble_bas_rgb_color_t color = get_battery_color(25, BAS_COLOR_MODE_GRADIENT);

	zassert_equal(color.red, 255, "Red should be 255 for 25%% battery");
	zassert_equal(color.green, 127, "Green should be 127 for 25%% battery");
	zassert_equal(color.blue, 0, "Blue should be 0");
}

ZTEST(ble_bas_color_gradient, test_gradient_empty_battery_0_percent)
{
	ble_bas_rgb_color_t color = get_battery_color(0, BAS_COLOR_MODE_GRADIENT);

	zassert_equal(color.red, 255, "Red should be 255 for 0%% battery");
	zassert_equal(color.green, 0, "Green should be 0 for 0%% battery");
	zassert_equal(color.blue, 0, "Blue should be 0");
}

ZTEST(ble_bas_color_gradient, test_gradient_invalid_battery_0xFF)
{
	ble_bas_rgb_color_t color = get_battery_color(0xFF, BAS_COLOR_MODE_GRADIENT);

	zassert_equal(color.red, 0, "Red should be 0 for invalid battery");
	zassert_equal(color.green, 255, "Green should be 255 for invalid battery");
	zassert_equal(color.blue, 0, "Blue should be 0");
}

ZTEST(ble_bas_color_gradient, test_gradient_smooth_transition)
{
	ble_bas_rgb_color_t color;

	/* 90% - mostly green with slight red */
	color = get_battery_color(90, BAS_COLOR_MODE_GRADIENT);
	zassert_equal(color.red, 51, "Red should be 51 for 90%%");
	zassert_equal(color.green, 255, "Green should be 255 for 90%%");

	/* 60% - more yellow-green */
	color = get_battery_color(60, BAS_COLOR_MODE_GRADIENT);
	zassert_equal(color.red, 204, "Red should be 204 for 60%%");
	zassert_equal(color.green, 255, "Green should be 255 for 60%%");

	/* 40% - orange (more red than green) */
	color = get_battery_color(40, BAS_COLOR_MODE_GRADIENT);
	zassert_equal(color.red, 255, "Red should be 255 for 40%%");
	zassert_equal(color.green, 204, "Green should be 204 for 40%%");

	/* 10% - mostly red with slight green */
	color = get_battery_color(10, BAS_COLOR_MODE_GRADIENT);
	zassert_equal(color.red, 255, "Red should be 255 for 10%%");
	zassert_equal(color.green, 51, "Green should be 51 for 10%%");
}
