/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>
#include <stdbool.h>
#include <inttypes.h>

#define LOG_LEVEL LOG_LEVEL_DBG

#define WDT_FEED_TRIES    8
#define WDT_TIMEOUT       2000U
#define WDG_FEED_INTERVAL 500U

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

	if (!gpio_is_ready_dt(&button0)) {
		LOG_ERR("Error: button0 device %s is not ready\n", button0.port->name);
		return 0;
	}

	ret = gpio_pin_configure_dt(&button0, GPIO_INPUT);
	if (ret != 0) {
		LOG_ERR("Error %d: failed to configure %s pin %d\n", ret, button0.port->name,
			button0.pin);
		return 0;
	}

	ret = gpio_pin_interrupt_configure_dt(&button0, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret != 0) {
		LOG_ERR("Error %d: failed to configure interrupt on %s pin %d\n", ret,
			button0.port->name, button0.pin);
		return 0;
	}

	gpio_init_callback(&button0_cb_data, button0_pressed, BIT(button0.pin));
	gpio_add_callback(button0.port, &button0_cb_data);

	if (!device_is_ready(wdt)) {
		LOG_ERR("%s: device not ready.\n", wdt->name);
		return 0;
	}

	struct wdt_timeout_cfg wdt_config = {
		/* Reset SoC when timer expires. */
		.flags = WDT_FLAG_RESET_SOC,

		/* Expire watchdog after max window */
		.window.min = 0,
		.window.max = WDT_TIMEOUT,
	};

	ret = wdt_install_timeout(wdt, &wdt_config);
	if (ret < 0) {
		LOG_ERR("Watchdog install error\n");
		return 0;
	}

	while (!is_wdt_on) {
		LOG_INF("Press button to enable watchdog\n");
		k_sleep(K_MSEC(1000));
	}

	err = wdt_setup(wdt, 0);
	if (err < 0) {
		LOG_ERR("Watchdog setup error\n");
		return 0;
	}

	/* Feeding watchdog before timer expires. */
	LOG_INF("Feeding watchdog %d times\n", WDT_FEED_TRIES);
	for (int i = 0; i < WDT_FEED_TRIES; ++i) {
		k_sleep(K_MSEC(WDG_FEED_INTERVAL));
		LOG_INF("Feeding watchdog...\n");
		wdt_feed(wdt, 0);
	}

	/* Waiting for the SoC reset. */
	while (1) {
		LOG_INF("No more feeds, waiting for reset\n");
		k_sleep(K_MSEC(WDG_FEED_INTERVAL));
	}
	return 0;
}
