/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef QCC730_DEVICE_SUBSYS_H
#define QCC730_DEVICE_SUBSYS_H

#include <zephyr/dt-bindings/clock/qcom-qcc730-clock.h>

struct test_clock_data {
	clock_control_subsys_t subsys;
	uint32_t startup_us;
};

struct test_clock_device {
	const struct device *dev;
	const struct test_clock_data *subsys_data;
	size_t subsys_cnt;
};

static const struct test_clock_data qcc730_subsys_data[] = {
	{
		.subsys = (clock_control_subsys_t)QCC730_CLOCK_UART,
		.startup_us = 100,
	},
	{
		.subsys = (clock_control_subsys_t)QCC730_CLOCK_I2C,
		.startup_us = 100,
	},
	{
		.subsys = (clock_control_subsys_t)QCC730_CLOCK_SPI,
		.startup_us = 100,
	},
	{
		.subsys = (clock_control_subsys_t)QCC730_CLOCK_QTIMER,
		.startup_us = 100,
	},
	{
		.subsys = (clock_control_subsys_t)QCC730_CLOCK_WDOG,
		.startup_us = 100,
	},
	{
		.subsys = (clock_control_subsys_t)QCC730_CLOCK_GPIO,
		.startup_us = 100,
	},

};

static const struct test_clock_device devices[] = {
	{
		.dev = DEVICE_DT_GET(DT_NODELABEL(clock_controller)),
		.subsys_data = qcc730_subsys_data,
		.subsys_cnt = ARRAY_SIZE(qcc730_subsys_data),
	},
};

#endif /* QCC730_DEVICE_SUBSYS_H */
