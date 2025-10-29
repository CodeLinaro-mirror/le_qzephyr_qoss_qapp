/*
 * @brief STM32 SPI Shell example
 *
 * Copyright (c) 2025 Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <string.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(spi_master, LOG_LEVEL_INF);

/* Get nodes from device tree */
#define LED_NODE       DT_ALIAS(led0)
#define SPI_CS_DT_SPEC SPI_CS_GPIOS_DT_SPEC_GET(DT_NODELABEL(spi_slave))

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED_NODE, gpios);
#if DT_NODE_EXISTS(DT_NODELABEL(spi2))
const struct device *const spi2_dev_ptr = DEVICE_DT_GET(DT_NODELABEL(spi2));
#define SPI2_NODE DT_NODELABEL(spi2)
#else
#error "SPI2 node not found in device tree."
#endif

#define MAX_BUFFER_SIZE 256

struct spi_bridge_data {
	uint8_t spi_tx_buffer[MAX_BUFFER_SIZE];
	uint8_t spi_rx_buffer[MAX_BUFFER_SIZE];
	bool led_state;
	const struct device *spi_dev;
	struct k_mutex spi_mutex;
};

static struct spi_bridge_data bridge_data;

/* Default SPI Config */
static struct spi_config spi_cfg = {.frequency = 1000000, /* 1 MHz */
				    .operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
				    .slave = 0, /* Slave device number */
				    .cs = {
					    /**
					     * This pin specification is needed for automatic CS
					     * control Set this to NULL for manual control
					     */
					    .gpio = SPI_CS_DT_SPEC,
					    .delay = 0 /* No delay after CS release */
				    }};

/* Helper function to parse hex string to bytes */
static int hex_string_to_bytes(const char *hex_str, uint8_t *bytes, size_t max_bytes)
{
	size_t hex_len = strlen(hex_str);
	size_t byte_count = 0;

	/* Remove "0x" prefix if present */
	if (hex_len >= 2 && hex_str[0] == '0' && (hex_str[1] == 'x' || hex_str[1] == 'X')) {
		hex_str += 2;
		hex_len -= 2;
	}

	/* Each byte needs 2 hex characters */
	if (hex_len % 2 != 0) {
		return -EINVAL;
	}

	byte_count = hex_len / 2;
	if (byte_count > max_bytes) {
		return -E2BIG;
	}

	for (size_t i = 0; i < byte_count; i++) {
		char byte_str[3] = {hex_str[i * 2], hex_str[i * 2 + 1], '\0'};
		char *endptr;
		unsigned long val = strtoul(byte_str, &endptr, 16);

		if (*endptr != '\0' || val > 0xFF) {
			return -EINVAL;
		}

		bytes[i] = (uint8_t)val;
	}

	return byte_count;
}

/* SPI Tx-Rx Function */
static int spi_tx_rx(uint8_t *tx_data, uint8_t *rx_data, size_t len)
{
	int ret;

	k_mutex_lock(&bridge_data.spi_mutex, K_FOREVER);

	memcpy(bridge_data.spi_tx_buffer, tx_data, len);
	memset(bridge_data.spi_rx_buffer, 0, len);

	struct spi_buf tx_buf = {.buf = bridge_data.spi_tx_buffer, .len = len};

	struct spi_buf rx_buf = {.buf = bridge_data.spi_rx_buffer, .len = len};

	struct spi_buf_set tx_buf_set = {.buffers = &tx_buf, .count = 1};

	struct spi_buf_set rx_buf_set = {.buffers = &rx_buf, .count = 1};

	/* Send entire buffer at once */
	struct spi_config temp_cfg = spi_cfg;

	ret = spi_transceive(bridge_data.spi_dev, &temp_cfg, &tx_buf_set, &rx_buf_set);
	if (ret < 0) {
		k_mutex_unlock(&bridge_data.spi_mutex);
		return ret;
	}

	/* Copy received data back */
	memcpy(rx_data, bridge_data.spi_rx_buffer, len);

	k_mutex_unlock(&bridge_data.spi_mutex);
	return 0;
}

/* Shell command: Send hex bytes via SPI */
static int cmd_spi_send_hex(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2) {
		shell_error(sh, "Usage: spi send_hex <hex_string>");
		shell_error(sh, "Example: spi send_hex AABBCCDD");
		shell_error(sh, "Example: spi send_hex 0x1122334455");
		return -EINVAL;
	}

	uint8_t tx_data[MAX_BUFFER_SIZE];
	uint8_t rx_data[MAX_BUFFER_SIZE];

	int byte_count = hex_string_to_bytes(argv[1], tx_data, MAX_BUFFER_SIZE);
	if (byte_count < 0) {
		shell_error(sh, "Invalid hex string format");
		return byte_count;
	}

	int ret = spi_tx_rx(tx_data, rx_data, byte_count);
	if (ret < 0) {
		shell_error(sh, "SPI transaction failed: %d", ret);
		return ret;
	}

	shell_print(sh, "TX[%d]: ", byte_count);
	for (int i = 0; i < byte_count; i++) {
		shell_fprintf(sh, SHELL_NORMAL, "%02X ", tx_data[i]);
	}
	shell_print(sh, "");

	shell_print(sh, "RX[%d]: ", byte_count);
	for (int i = 0; i < byte_count; i++) {
		shell_fprintf(sh, SHELL_NORMAL, "%02X ", rx_data[i]);
	}
	shell_print(sh, "");

	shell_print(sh, "RX ASCII: ");
	for (int i = 0; i < byte_count; i++) {
		if (rx_data[i] >= 32 && rx_data[i] <= 126) {
			shell_fprintf(sh, SHELL_NORMAL, "%c", rx_data[i]);
		} else {
			shell_fprintf(sh, SHELL_NORMAL, ".");
		}
	}
	shell_print(sh, "");

	return 0;
}

/* Shell command: Configure SPI frequency */
static int cmd_spi_config_freq(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2) {
		shell_error(sh, "Usage: spi config_freq <frequency_hz>");
		shell_error(sh, "Example: spi config_freq 20000000");
		return -EINVAL;
	}

	unsigned long freq = strtoul(argv[1], NULL, 10);
	if (freq == 0 || freq > 20000000) { /* Max 20MHz */
		shell_error(sh, "Invalid frequency (1 - 20000000 Hz)/20MHz");
		return -EINVAL;
	}

	spi_cfg.frequency = freq;
	shell_print(sh, "SPI frequency set to %lu Hz", freq);
	return 0;
}

/* Shell command: Get SPI status */
static int cmd_spi_status(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "=== SPI Bridge Status ===");
	shell_print(sh, "SPI Device: %s", bridge_data.spi_dev->name);
	shell_print(sh, "SPI Frequency: %u Hz", spi_cfg.frequency);
	shell_print(sh, "LED State: %s", bridge_data.led_state ? "ON" : "OFF");
	shell_print(sh, "Buffer Size: %d bytes", MAX_BUFFER_SIZE);
	return 0;
}

/* Create shell subcommands */
SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_spi_cmds, SHELL_CMD(send_hex, NULL, "Send hex bytes via SPI", cmd_spi_send_hex),
	SHELL_CMD(config_freq, NULL, "Configure SPI frequency", cmd_spi_config_freq),
	SHELL_CMD(status, NULL, "Show SPI bridge status", cmd_spi_status),
	SHELL_SUBCMD_SET_END /* Array terminator */
);

SHELL_CMD_REGISTER(spi, &sub_spi_cmds, "SPI bridge commands", NULL);

int main(void)
{
	int ret;
	LOG_INF("Starting SPI2 Master example for STM32 Nucleo Boards");

	/* Initialize bridge data structure */
	memset(&bridge_data, 0, sizeof(bridge_data));
	k_mutex_init(&bridge_data.spi_mutex);

	/* Initialize LED */
	ret = gpio_is_ready_dt(&led);
	if (ret < 0) {
		LOG_ERR("LED GPIO device not ready");
		return ret;
	}

	ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure LED GPIO: %d", ret);
		return ret;
	}

	bridge_data.spi_dev = spi2_dev_ptr;
	ret = device_is_ready(bridge_data.spi_dev);
	if (ret < 0) {
		LOG_ERR("SPI2 device not ready");
		return ret;
	}

	LOG_INF("SPI2 device initialized");

	LOG_INF("SPI Shell ready - type 'spi help' for commands");

	/* Main thread can do other work or just sleep */
	while (1) {
        /* Blink LED on Nucleo Board every 200 millisecond */
		k_msleep(200);
		bridge_data.led_state = !bridge_data.led_state;
		gpio_pin_set_dt(&led, bridge_data.led_state);
	}
}
