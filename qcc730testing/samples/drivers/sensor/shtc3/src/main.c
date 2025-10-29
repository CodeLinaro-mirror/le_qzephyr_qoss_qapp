/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

#define WAIT_TIME_MS 2000

#define SHTC3_NODE    shtc3
#define SHTC3_NODE_ID DT_NODELABEL(SHTC3_NODE)

/* The devicetree node identifier for the "led0" alias. */
#define LED0_NODE DT_ALIAS(led0)

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
static const struct device *shtc3 = DEVICE_DT_GET(SHTC3_NODE_ID);

int main(void)
{
	int ret;
	bool led_on = true;
	struct sensor_value result_cnvrtd[2];

	if (!gpio_is_ready_dt(&led)) {
		return 0;
	}

	ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		return 0;
	}

	if (!device_is_ready(shtc3)) {
		printf("Error - shtc3 not initialized! \n");
		return -ENODEV;
	}

	while (1) {
		ret = sensor_sample_fetch(shtc3);
		sensor_channel_get(shtc3, SENSOR_CHAN_HUMIDITY, &result_cnvrtd[0]);
		if (ret < 0) {
			printf("Error while reading humidity from shtc3: %d \n", ret);
			continue;
		}

		sensor_channel_get(shtc3, SENSOR_CHAN_AMBIENT_TEMP, &result_cnvrtd[1]);
		if (ret < 0) {
			printf("Error while reading temperature from shtc3: %d \n", ret);
			continue;
		}

		ret = gpio_pin_toggle_dt(&led);
		if (ret < 0) {
			return 0;
		}

		led_on = !led_on;
		printf("Humidity: %d%%  Temperature: %d °C \n", result_cnvrtd[0].val1,
		       result_cnvrtd[1].val1);
		k_msleep(WAIT_TIME_MS);
	}
	return 0;
}
