/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/printk.h>
#include <inttypes.h>

#define SLEEP_MS 1

/*
 * Get button configuration from the devicetree sw0 and sw1 aliases. This is mandatory.
 */
#define SW0_NODE DT_ALIAS(sw0)
#if !DT_NODE_HAS_STATUS_OKAY(SW0_NODE)
#error "Unsupported board: sw0 devicetree alias is not defined"
#endif
static const struct gpio_dt_spec button0 = GPIO_DT_SPEC_GET_OR(SW0_NODE, gpios, {0});

#define SW1_NODE DT_ALIAS(sw1)
#if !DT_NODE_HAS_STATUS_OKAY(SW1_NODE)
#error "Unsupported board: sw1 devicetree alias is not defined"
#endif
static const struct gpio_dt_spec button1 = GPIO_DT_SPEC_GET_OR(SW1_NODE, gpios, {0});

static struct gpio_callback button0_cb_data;
static struct gpio_callback button1_cb_data;

void button0_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	printk("Button0 pressed at %" PRIu32 "\n", k_cycle_get_32());
}

void button1_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	printk("Button1 pressed at %" PRIu32 "\n", k_cycle_get_32());
}

int main(void)
{
	int ret;

	if (!gpio_is_ready_dt(&button0)) {
		printk("Error: button0 device %s is not ready\n", button0.port->name);
		return 0;
	}

	if (!gpio_is_ready_dt(&button1)) {
		printk("Error: button1 device %s is not ready\n", button1.port->name);
		return 0;
	}

	ret = gpio_pin_configure_dt(&button0, GPIO_INPUT);
	if (ret != 0) {
		printk("Error %d: failed to configure %s pin %d\n", ret, button0.port->name,
		       button0.pin);
		return 0;
	}

	ret = gpio_pin_configure_dt(&button1, GPIO_INPUT);
	if (ret != 0) {
		printk("Error %d: failed to configure %s pin %d\n", ret, button1.port->name,
		       button1.pin);
		return 0;
	}

	ret = gpio_pin_interrupt_configure_dt(&button0, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret != 0) {
		printk("Error %d: failed to configure interrupt on %s pin %d\n", ret,
		       button0.port->name, button0.pin);
		return 0;
	}

	ret = gpio_pin_interrupt_configure_dt(&button1, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret != 0) {
		printk("Error %d: failed to configure interrupt on %s pin %d\n", ret,
		       button1.port->name, button1.pin);
		return 0;
	}

	gpio_init_callback(&button0_cb_data, button0_pressed, BIT(button0.pin));
	gpio_add_callback(button0.port, &button0_cb_data);
	printk("Set up button at %s pin %d\n", button0.port->name, button0.pin);

	gpio_init_callback(&button1_cb_data, button1_pressed, BIT(button1.pin));
	gpio_add_callback(button1.port, &button1_cb_data);
	printk("Set up button at %s pin %d\n", button1.port->name, button1.pin);

	printk("Press any button\n");
	while (1) {
		k_msleep(SLEEP_MS);
	}

	return 0;
}
