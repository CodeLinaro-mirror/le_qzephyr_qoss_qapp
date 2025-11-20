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

const struct device *uart0 = DEVICE_DT_GET(DT_NODELABEL(uart0));

struct uart_config uart_cfg = {
	.baudrate = 115200,
	.parity = UART_CFG_PARITY_NONE,
	.stop_bits = UART_CFG_STOP_BITS_1,
	.flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
	.data_bits = UART_CFG_DATA_BITS_8,
};

void send_str(const struct device *uart, char *str)
{
	int msg_len = strlen(str);

	printk("Loopback: ");
	for (int i = 0; i < msg_len; i++) {
		uart_poll_out(uart, str[i]);
	}
	printk("\n");
}

void recv_str(const struct device *uart, char *str)
{
	char *head = str;
	char c;

	while (1) {
		while (-ENODATA == uart_poll_in(uart, &c)) {
			k_sleep(K_MSEC(1));
		}

		*head++ = c;

		/* Enter button means end of reading */
		if ('\n' == c || '\r' == c) {
			break;
		}
	}
	*head = '\0';
}

int main(void)
{
	int rc;
	char recv_buf[128];

	rc = uart_configure(uart0, &uart_cfg);
	if (rc) {
		printk("Could not configure device %s \nExiting Program. \n", uart0->name);
		return rc;
	}

	printk("Write something and press Enter \n");

	while (1) {

		/* Wait some time for the messages to arrive to the second uart. */
		recv_str(uart0, recv_buf);
		/* Software loopback */
		send_str(uart0, recv_buf);

		k_sleep(K_MSEC(1));
	}

	return 0;
}
