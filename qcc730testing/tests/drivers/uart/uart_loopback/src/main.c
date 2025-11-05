/*
 * Copyright (c) 2025 Qualcomm Technologies, Inc.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * UART loopback test
 */

#include <zephyr/ztest.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/crc.h>
#include <zephyr/logging/log_ctrl.h>

LOG_MODULE_REGISTER(uart_test, LOG_LEVEL_INF);

/* Test configuration */
#define TEST_DURATION_MS                  5000U
#define TX_THREAD_STACK_SIZE              2048U
#define RX_THREAD_STACK_SIZE              2048U
#define TX_THREAD_PRIORITY                5
#define RX_THREAD_PRIORITY                5
#define LED_BLINK_DURATION_MS             500
#define LED_BLINK_TOTAL_OBSERVING_TIME_MS 5000
#define VALIDATION_BUFFER_SIZE            32768UL

/* LED GPIO */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* Global test variables */
static const struct device *uart_dev;
static atomic_t tx_counter;
static atomic_t rx_counter;
static atomic_t error_counter;
static atomic_t rx_no_data;
static atomic_t tx_running;
static atomic_t rx_running;

/* Data validation buffers */
static uint8_t tx_validation_buffer[VALIDATION_BUFFER_SIZE];
static uint8_t rx_validation_buffer[VALIDATION_BUFFER_SIZE];
static uint32_t tx_buffer_idx = 0U;
static uint32_t rx_buffer_idx = 0U;

/* Thread stacks */
K_THREAD_STACK_DEFINE(tx_thread_stack, TX_THREAD_STACK_SIZE);
K_THREAD_STACK_DEFINE(rx_thread_stack, RX_THREAD_STACK_SIZE);
static struct k_thread tx_thread_data;
static struct k_thread rx_thread_data;

/*
 * Blink LED for predefined time configured in macros
 */
static void led_blink_for_predefined_time(void)
{
	const uint32_t blink_times =
		(LED_BLINK_TOTAL_OBSERVING_TIME_MS / LED_BLINK_DURATION_MS) / 2;

	for (uint32_t i = 0; i < blink_times; i++) {
		LOG_INF("Toggling led...");
		(void)gpio_pin_set_dt(&led, 1);
		k_msleep(LED_BLINK_DURATION_MS);
		(void)gpio_pin_set_dt(&led, 0);
		k_msleep(LED_BLINK_DURATION_MS);
	}

	/* Ensure LED is OFF after blinking */
	(void)gpio_pin_set_dt(&led, 0);
}

/*
 * Initialize LED used for informing user about progress
 */
static int led_init(void)
{
	if (!gpio_is_ready_dt(&led)) {
		return -ENODEV;
	}

	int ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		return ret;
	}

	(void)gpio_pin_set_dt(&led, 0);
	return 0;
}

/*
 * Data Validation - CRC calculation
 */
static uint16_t calculate_buffer_crc(const uint8_t *buffer, uint32_t length)
{
	return crc16_itu_t(0, buffer, length);
}

/*
 * TX Thread - Sends incrementing byte sequence
 */
static void tx_thread_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uint8_t tx_byte = 0U;

	while (atomic_get(&tx_running)) {
		uart_poll_out(uart_dev, tx_byte);

		if (tx_buffer_idx < VALIDATION_BUFFER_SIZE) {
			tx_validation_buffer[tx_buffer_idx++] = tx_byte;
		}

		atomic_inc(&tx_counter);
		tx_byte++;
		k_yield();
	}
}

/*
 * RX Thread - Receives and validates data
 */
static void rx_thread_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	unsigned char rx_byte = 0U;
	int ret = 0;

	while (atomic_get(&rx_running)) {
		ret = uart_poll_in(uart_dev, &rx_byte);

		if (ret == 0) {
			if (rx_buffer_idx < VALIDATION_BUFFER_SIZE) {
				rx_validation_buffer[rx_buffer_idx++] = rx_byte;
			}
			atomic_inc(&rx_counter);
		} else if (ret == -EBUSY) {
			atomic_inc(&error_counter);
		} else if (ret == -ENODATA) {
			atomic_inc(&rx_no_data);
			k_yield();
		} else {
			// sanity check for unexpected error
			atomic_inc(&error_counter);
		}
		rx_byte = 0U;
	}
}

/*
 * Test Setup/Teardown
 */
static void *uart_concurrent_setup(void)
{
	LOG_INF("=== UART Stress Test Setup ===");

	uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	if (!device_is_ready(uart_dev)) {
		LOG_ERR("UART device not ready");
		return NULL;
	}

	LOG_INF("UART device ready");

	if (led_init() < 0) {
		LOG_WRN("LED init failed, continuing without LED");
	}

	return NULL;
}

static void uart_concurrent_before(void *fixture)
{
	ARG_UNUSED(fixture);

	atomic_set(&tx_counter, 0);
	atomic_set(&rx_counter, 0);
	atomic_set(&error_counter, 0);
	atomic_set(&rx_no_data, 0);
	atomic_set(&tx_running, 0);
	atomic_set(&rx_running, 0);

	tx_buffer_idx = 0U;
	rx_buffer_idx = 0U;

	memset(tx_validation_buffer, 0, VALIDATION_BUFFER_SIZE);
	memset(rx_validation_buffer, 0, VALIDATION_BUFFER_SIZE);
}

static void uart_concurrent_after(void *fixture)
{
	ARG_UNUSED(fixture);

	atomic_set(&tx_running, 0);
	atomic_set(&rx_running, 0);
}

/*
 * Main Test
 */
ZTEST(uart_concurrent, test_uart_stress)
{
	int64_t start_time = 0, elapsed_time = 0;
	uint32_t tx_ops = 0U, rx_ops = 0U, errors = 0U;
	uint16_t tx_crc = 0U, rx_crc = 0U;
	bool test_passed = false;
	unsigned char dummy = 0U;
	int drain_count = 0;
	const uint32_t log_level = log_filter_get(NULL, 0, 0, true);

	LOG_INF("=== Starting UART Stress Test ===");
	LOG_INF("Test will run for %u ms", TEST_DURATION_MS);

	/* Signal test start */
	led_blink_for_predefined_time();

	/* Disable logging during test */
	LOG_INF("Disabling logging...");
	log_filter_set(NULL, 0, LOG_LEVEL_NONE, 0);
	k_msleep(500);

	/* Drain ring buffer of any residual data */
	while (uart_poll_in(uart_dev, &dummy) == 0 && drain_count < 10000) {
		drain_count++;
	}

	/* Start test threads */
	atomic_set(&tx_running, 1);
	atomic_set(&rx_running, 1);

	k_thread_create(&tx_thread_data, tx_thread_stack, K_THREAD_STACK_SIZEOF(tx_thread_stack),
			tx_thread_entry, NULL, NULL, NULL, TX_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&tx_thread_data, "uart_tx");

	k_thread_create(&rx_thread_data, rx_thread_stack, K_THREAD_STACK_SIZEOF(rx_thread_stack),
			rx_thread_entry, NULL, NULL, NULL, RX_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&rx_thread_data, "uart_rx");

	/* Run test for configured duration */
	start_time = k_uptime_get();
	k_msleep(TEST_DURATION_MS);

	/* Stop TX first, then allow RX to drain */
	atomic_set(&tx_running, 0);
	k_thread_join(&tx_thread_data, K_FOREVER);
	elapsed_time = k_uptime_get() - start_time;

	/* Give RX time to drain remaining data */
	k_msleep(100);
	atomic_set(&rx_running, 0);
	k_thread_join(&rx_thread_data, K_FOREVER);

	/* Re-enable logging */
	log_filter_set(NULL, 0, log_level, 0);
	k_msleep(100);

	/* Collect results */
	tx_ops = atomic_get(&tx_counter);
	rx_ops = atomic_get(&rx_counter);
	errors = atomic_get(&error_counter);

	/* Calculate CRCs on actual transmitted/received data */
	const uint32_t tx_crc_len =
		(tx_buffer_idx < VALIDATION_BUFFER_SIZE) ? tx_buffer_idx : VALIDATION_BUFFER_SIZE;
	const uint32_t rx_crc_len =
		(rx_buffer_idx < VALIDATION_BUFFER_SIZE) ? rx_buffer_idx : VALIDATION_BUFFER_SIZE;

	tx_crc = calculate_buffer_crc(tx_validation_buffer, tx_crc_len);
	rx_crc = calculate_buffer_crc(rx_validation_buffer, rx_crc_len);

	LOG_INF("Test completed in %lld ms", elapsed_time);
	LOG_INF("Drained %d bytes from ring buffer before test", drain_count);

	/* Assertions - check in logical order */
	zassert_equal(errors, 0, "Errors detected: %u", errors);
	zassert_true(tx_ops > 0, "No TX operations");
	zassert_true(rx_ops > 0, "No RX operations");
	zassert_equal(tx_ops, rx_ops, "TX/RX count mismatch: TX=%u RX=%u", tx_ops, rx_ops);
	zassert_equal(tx_crc, rx_crc, "CRC mismatch - TX:0x%04X RX:0x%04X", tx_crc, rx_crc);

	/* Determine overall pass/fail */
	test_passed = (tx_ops > 0) && (rx_ops == tx_ops) && (errors == 0) && (tx_crc == rx_crc);

	/* Signal test end */
	led_blink_for_predefined_time();

	/* Display final result via LED */
	LOG_INF("Signalling results for %u ms", LED_BLINK_TOTAL_OBSERVING_TIME_MS);
	(void)gpio_pin_set_dt(&led, test_passed ? 1 : 0);
	k_msleep(LED_BLINK_TOTAL_OBSERVING_TIME_MS);

	/* Print detailed results */
	LOG_INF("=== Test Results ===");
	LOG_INF("TX bytes: %u", tx_ops);
	LOG_INF("RX bytes: %u", rx_ops);
	LOG_INF("Errors: %u", errors);
	LOG_INF("TX CRC: 0x%04X (buffer: %u bytes)", tx_crc, tx_crc_len);
	LOG_INF("RX CRC: 0x%04X (buffer: %u bytes)", rx_crc, rx_crc_len);

	LOG_INF("Test %s", test_passed ? "PASSED" : "FAILED");
}

ZTEST_SUITE(uart_concurrent, NULL, uart_concurrent_setup, uart_concurrent_before,
	    uart_concurrent_after, NULL);