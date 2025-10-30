/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>
#include <stdbool.h>
#include <inttypes.h>

#define LOG_LEVEL LOG_LEVEL_DBG

LOG_MODULE_REGISTER(main);

#define SW0_NODE DT_ALIAS(sw0)
#if !DT_NODE_HAS_STATUS_OKAY(SW0_NODE)
#endif
static volatile bool is_wdt_on = false;

void button0_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	if (!is_wdt_on) {
		LOG_INF("Enabling watchdog...\n");
		is_wdt_on = true;
	}
}

int main(void)
{
	int err;
	int ret;
	const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
	const struct gpio_dt_spec button0 = GPIO_DT_SPEC_GET_OR(SW0_NODE, gpios, {0});
	struct gpio_callback button0_cb_data;

	printk("Watchdog sample application\n");

	if (!device_is_ready(wdt)) {
		LOG_ERR("%s: device not ready.\n", wdt->name);
		return -ENODEV;
	}

	/* PM Suspend/Resume Tests */
	LOG_INF("=== Starting PM Suspend/Resume Tests ===");

	struct wdt_timeout_cfg wdt_config = {
		/* Reset SoC when timer expires. */
		.flags = WDT_FLAG_RESET_SOC,
		/* Expire watchdog after max window */
		.window.min = 0,
		.window.max = CONFIG_QCC730_WDT_TIMEOUT_MS,
	};

	/* Test 1: Operations should work when device is active */
	LOG_INF("Test 1: Install timeout while device is active");
	ret = wdt_install_timeout(wdt, &wdt_config);
	__ASSERT(ret == 0, "Failed to install timeout while active: %d", ret);
	LOG_INF("Test 1: PASS - Timeout installed successfully");

	/* Test 2: Suspend the device */
	LOG_INF("Test 2: Suspending watchdog device");
	ret = pm_device_action_run(wdt, PM_DEVICE_ACTION_SUSPEND);
	__ASSERT(ret == 0, "Failed to suspend device: %d", ret);
	LOG_INF("Test 2: PASS - Device suspended");

	/* Test 3: Operations should fail when device is suspended */
	LOG_INF("Test 3: Attempting operations while suspended (should fail)");
	ret = wdt_install_timeout(wdt, &wdt_config);
	__ASSERT(ret == -EBUSY, "Expected -EBUSY while suspended, got: %d", ret);
	LOG_INF("Test 3a: PASS - install_timeout returned -EBUSY as expected");

	ret = wdt_setup(wdt, 0);
	__ASSERT(ret == -EBUSY, "Expected -EBUSY for setup while suspended, got: %d", ret);
	LOG_INF("Test 3b: PASS - setup returned -EBUSY as expected");

	ret = wdt_feed(wdt, 0);
	__ASSERT(ret == -EBUSY, "Expected -EBUSY for feed while suspended, got: %d", ret);
	LOG_INF("Test 3c: PASS - feed returned -EBUSY as expected");

	/* Test 4: Resume the device */
	LOG_INF("Test 4: Resuming watchdog device");
	ret = pm_device_action_run(wdt, PM_DEVICE_ACTION_RESUME);
	__ASSERT(ret == 0, "Failed to resume device: %d", ret);
	LOG_INF("Test 4: PASS - Device resumed");

	/* Test 5: Operations should work again after resume */
	LOG_INF("Test 5: Reinstalling timeout after resume");
	ret = wdt_install_timeout(wdt, &wdt_config);
	__ASSERT(ret == 0, "Failed to install timeout after resume: %d", ret);
	LOG_INF("Test 5: PASS - Timeout reinstalled after resume");

	LOG_INF("=== PM Suspend/Resume Tests Complete ===\n");

	/* GPIO Button Setup */
	if (!gpio_is_ready_dt(&button0)) {
		LOG_ERR("Error: button0 device %s is not ready\n", button0.port->name);
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&button0, GPIO_INPUT);
	if (ret != 0) {
		LOG_ERR("Error %d: failed to configure %s pin %d\n", ret, button0.port->name,
			button0.pin);
		return -ENODEV;
	}

	ret = gpio_pin_interrupt_configure_dt(&button0, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret != 0) {
		LOG_ERR("Error %d: failed to configure interrupt on %s pin %d\n", ret,
			button0.port->name, button0.pin);
		return -ENODEV;
	}

	gpio_init_callback(&button0_cb_data, button0_pressed, BIT(button0.pin));
	gpio_add_callback(button0.port, &button0_cb_data);

	/* Watchdog Error Test */
	struct wdt_timeout_cfg wdt_err_config = {
		/* Reset SoC when timer expires. */
		.flags = WDT_FLAG_RESET_SOC,

		/* Expire watchdog after max window */
		.window.min = 0,
		.window.max = BIT_MASK(21) + 1U, /* More than max allowed */
	};

	ret = wdt_install_timeout(wdt, &wdt_err_config);
	__ASSERT(ret < 0, "Expected error for invalid timeout config, got: %d", ret);
	LOG_INF("Invalid timeout test: PASS - Got expected error\n");

	/* Normal Watchdog Operation */

	ret = wdt_install_timeout(wdt, &wdt_config);
	__ASSERT(ret == 0, "Watchdog install timeout failed: %d", ret);
	LOG_INF("Watchdog timeout configured successfully\n");

	while (!is_wdt_on) {
		LOG_INF("Press button to enable watchdog\n");
		k_sleep(K_MSEC(1000));
	}

	err = wdt_setup(wdt, 0);
	__ASSERT(err == 0, "Watchdog setup failed: %d", err);
	LOG_INF("Watchdog enabled successfully\n");

	/* Feeding watchdog before timer expires. */
	LOG_INF("Feeding watchdog %d times\n", CONFIG_QCC730_WDT_FEED_TRIES);
	for (int i = 0; i < CONFIG_QCC730_WDT_FEED_TRIES; ++i) {
		k_sleep(K_MSEC(CONFIG_QCC730_WDT_FEED_INTERVAL_MS));
		LOG_INF("Feeding watchdog...\n");
		ret = wdt_feed(wdt, 0);
		__ASSERT(ret == 0, "Watchdog feed failed: %d", ret);
	}

	/* Waiting for the SoC reset. */
	while (1) {
		LOG_INF("No more feeds, waiting for reset\n");
		k_sleep(K_MSEC(CONFIG_QCC730_WDT_FEED_INTERVAL_MS));
	}
	return 0;
}
