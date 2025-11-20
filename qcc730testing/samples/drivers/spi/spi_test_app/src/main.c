/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <stdio.h>
#include <string.h>

const struct device *spi_dev = DEVICE_DT_GET(DT_NODELABEL(qcspi));

int main(void)
{
	if (!device_is_ready(spi_dev)) {
		printk("SPI device not ready\n");
		return -ENODEV;
	}

	printk("SPI slave verification app started\n");

	while (1) {
		k_sleep(K_MSEC(1));
	}

	return 0;
}
