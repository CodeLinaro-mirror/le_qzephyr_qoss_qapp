/*
 * Copyright (c) 2025 Qualcomm Technologies, Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * UART Stress Test
 */

#include <zephyr/ztest.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/ztress.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/random/random.h>

LOG_MODULE_REGISTER(uart_stress, LOG_LEVEL_INF);

/* Test configuration */
#define ZTRESS_ITERATIONS                  1000U
#define ZTRESS_ENDURANCE_POLL_ITERATIONS   10000U
#define ZTRESS_MIN_PREEMPTIONS             50U
#define ZTRESS_TEST_TIMEOUT_MS             10000U
#define ZTRESS_ENDURANCE_TEST_TIMEOUT_MS   60000U
#define ZTRESS_ENDURANCE_TIMEOUT           K_SECONDS(60)
#define ZTRESS_MINIMUM_OPERATIONS_PER_SEC  1500U
#define THROUGHPUT_TEST_SIZE               (100 * 1024) /* 100KB for throughput test */
#define ZTRESS_NO_PREEMPTION               0
#define ZTRESS_ERRORS_PERCENTAGE_TOLEARNCE 0.1
#define MSEC_IN_SEC                        1000U
#define UART_ZTRESS_BITS_PER_BYTE_8N1      10U /* 1 (start) + 8 (data) + 1 (stop) */

/* Test patterns for data integrity */
static const uint8_t test_patterns[] = {0xA1U, 0xB2U, 0xC3U, 0xD4U, 0xE5U, 0xF6U, 0x00U, 0xFFU};

/* Global test variables */
static const struct device *uart_dev;
static atomic_t error_counter;
static atomic_t operation_counter;
static atomic_t bytes_transmitted;

/* Test fixture */
struct uart_stress_fixture {
	const struct device *dev;
	struct uart_config original_cfg;
};

/* Ztress context */
struct uart_ztress_context {
	const struct device *dev;
	atomic_t *errors;
	uint32_t operations;
	uint32_t bytes_transferred;
};

/* UART configuration combinations */
static const struct uart_config test_configs[] = {{.baudrate = 115200U,
						   .parity = UART_CFG_PARITY_NONE,
						   .stop_bits = UART_CFG_STOP_BITS_1,
						   .data_bits = UART_CFG_DATA_BITS_8,
						   .flow_ctrl = UART_CFG_FLOW_CTRL_NONE},
						  {.baudrate = 230400U,
						   .parity = UART_CFG_PARITY_EVEN,
						   .stop_bits = UART_CFG_STOP_BITS_1,
						   .data_bits = UART_CFG_DATA_BITS_8,
						   .flow_ctrl = UART_CFG_FLOW_CTRL_NONE},
						  {.baudrate = 460800U,
						   .parity = UART_CFG_PARITY_ODD,
						   .stop_bits = UART_CFG_STOP_BITS_2,
						   .data_bits = UART_CFG_DATA_BITS_7,
						   .flow_ctrl = UART_CFG_FLOW_CTRL_NONE},
						  {.baudrate = 921600U,
						   .parity = UART_CFG_PARITY_NONE,
						   .stop_bits = UART_CFG_STOP_BITS_1,
						   .data_bits = UART_CFG_DATA_BITS_8,
						   .flow_ctrl = UART_CFG_FLOW_CTRL_NONE}};

/* Helper to print and get information about total amount of operations. */
static uint32_t get_total_operations(struct uart_ztress_context *ctx, const uint8_t count)
{
	uint32_t total_ops = 0U;
	LOG_INF("Per-thread statistics:");
	for (uint8_t i = 0U; i < count; i++) {
		LOG_INF("Thread[%u] summary: operations=%u, errors=%ln", i, ctx[i].operations,
			ctx[i].errors);
		total_ops += ctx[i].operations;
	}

	LOG_INF("  Total operations: %ld", atomic_get(&operation_counter));
	LOG_INF("  Total errors: %ld", atomic_get(&error_counter));

	return total_ops;
}

/* Helper to check communication problems. */
void check_communication_problems(const uint32_t total_ops)
{
	const int32_t tolerance = (total_ops * (ZTRESS_ERRORS_PERCENTAGE_TOLEARNCE) / 100U);
	LOG_INF("Communication problems tolerance: %u", tolerance);
	zassert_within(atomic_get(&error_counter), 0, tolerance,
		       "Amount of errors is beyond the tolerance!");
}

/* Helper to check performance. */
void check_performance(const uint32_t total_ops, const int64_t elapsed_time,
		       const uint32_t min_ops_expected, const uint64_t timeout)
{
	uint32_t operations_per_sec = (uint32_t)(((uint64_t)total_ops * 1000U) / elapsed_time);
	LOG_INF("  Performance: %u operations/sec", operations_per_sec);

	zassert_true(operations_per_sec >= ZTRESS_MINIMUM_OPERATIONS_PER_SEC,
		     "Performance too small, expected at least %u operations/sec",
		     ZTRESS_MINIMUM_OPERATIONS_PER_SEC);
	zassert_true(elapsed_time < timeout, "Test should complete before timeout was hit!");
	zassert_true(total_ops >= min_ops_expected,
		     "Insufficient operations completed: %u (expected >= %u)", total_ops,
		     min_ops_expected);
}

/* Test setup */
static void *uart_stress_setup(void)
{
	static struct uart_stress_fixture fixture = {};

	uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	zassume_true(device_is_ready(uart_dev), "UART device not ready");
	fixture.dev = uart_dev;

	/* Save original configuration */
	uart_config_get(uart_dev, &fixture.original_cfg);
	LOG_INF("UART stress test setup complete for QCC730");
	return &fixture;
}

/* Test case setup */
static void uart_stress_before(void *f)
{
	struct uart_stress_fixture *fixture = (struct uart_stress_fixture *)f;

	/* Reset to original configuration */
	uart_configure(fixture->dev, &fixture->original_cfg);

	/* Clear counters */
	atomic_set(&error_counter, 0);
	atomic_set(&operation_counter, 0);
	atomic_set(&bytes_transmitted, 0);
}

/* Test teardown */
static void uart_stress_teardown(void *f)
{
	struct uart_stress_fixture *fixture = (struct uart_stress_fixture *)f;

	/* Restore original configuration */
	uart_configure(fixture->dev, &fixture->original_cfg);

	LOG_INF("Test completed - Operations: %ld, Errors: %ld, TX: %ld bytes.",
		atomic_get(&operation_counter), atomic_get(&error_counter),
		atomic_get(&bytes_transmitted));
}

/* Ztress handler: Configuration changes */
static bool ztress_config_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct uart_ztress_context *ctx = (struct uart_ztress_context *)user_data;
	const struct uart_config *cfg = &test_configs[cnt % ARRAY_SIZE(test_configs)];

	int ret = uart_configure(ctx->dev, cfg);

	if (ret != 0) {
		atomic_inc(ctx->errors);
		LOG_ERR("Config change failed: %d", ret);
	}

	ctx->operations++;
	atomic_inc(&operation_counter);

	/* Yield occasionally for push preemption */
	if ((cnt % 32) == 0) {
		k_yield();
	}

	if (last) {
		LOG_DBG("Config thread completed: %u operations", ctx->operations);

		ret = uart_configure(ctx->dev, &cfg[0]);
		if (ret != 0) {
			atomic_inc(ctx->errors);
			LOG_ERR("Config change failed at exit: %d", ret);
		}
		return false;
	}

	return true;
}

/* Ztress handler: Poll mode TX stress */
static bool ztress_poll_tx_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct uart_ztress_context *ctx = (struct uart_ztress_context *)user_data;
	const uint8_t tx_char = test_patterns[cnt % ARRAY_SIZE(test_patterns)];

	uart_poll_out(ctx->dev, tx_char);
	ctx->operations++;
	ctx->bytes_transferred++;
	atomic_inc(&operation_counter);
	atomic_inc(&bytes_transmitted);

	if (last) {
		LOG_DBG("Poll TX handler thread completed: %u operations", ctx->operations);
		return false;
	}

	return true;
}

/* Test: Configuration stress */
ZTEST_F(uart_stress, test_config_stress)
{
	const uint8_t concurrent_operations = 4U;
	struct uart_ztress_context ctx[4] = {0};
	uint32_t total_ops = 0U;
	int64_t start_time = 0, elapsed_time = 0;
	uint32_t min_expected = 0U;

	LOG_INF("Starting UART configuration stress test");
	LOG_INF("This test validates configuration stability under concurrent changes");

	/* Initialize contexts */
	for (uint8_t i = 0U; i < concurrent_operations; i++) {
		ctx[i].dev = fixture->dev;
		ctx[i].errors = &error_counter;
		ctx[i].operations = 0U;
	}

	ztress_set_timeout(K_MSEC(ZTRESS_TEST_TIMEOUT_MS));
	start_time = k_uptime_get();
	/* Execute concurrent configuration changes */
	ZTRESS_EXECUTE(ZTRESS_THREAD(ztress_config_handler, &ctx[0], ZTRESS_ITERATIONS,
				     ZTRESS_MIN_PREEMPTIONS, Z_TIMEOUT_TICKS(0)),
		       ZTRESS_THREAD(ztress_config_handler, &ctx[1], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
		       ZTRESS_THREAD(ztress_config_handler, &ctx[2], ZTRESS_ITERATIONS,
				     ZTRESS_MIN_PREEMPTIONS, Z_TIMEOUT_TICKS(0)),
		       ZTRESS_THREAD(ztress_config_handler, &ctx[3], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)));
	min_expected = concurrent_operations * ZTRESS_ITERATIONS;
	elapsed_time = k_uptime_get() - start_time;

	/* Verify results */
	LOG_INF("Configuration stress test completed in %lld ms", elapsed_time);
	total_ops = get_total_operations(ctx, concurrent_operations);
	check_communication_problems(total_ops);
	check_performance(total_ops, elapsed_time, min_expected, ZTRESS_TEST_TIMEOUT_MS);
}

#ifdef CONFIG_SERIAL_QCC730_INTERRUPT_SUPPORT
/* Test: Interrupt storm test - DISABLED AS 730 HAS SINGLE UART */
ZTEST_F(uart_stress, test_interrupt_storm)
{
	LOG_WRN("Interrupt storm test SKIPPED - not possible with single UART used for console");
	LOG_WRN("This test requires loopback which would break host communication");
	LOG_WRN("Interrupt functionality is tested indirectly through the separate test in pytest");

	ztest_test_skip();
}
#endif

/* Test: Throughput test */
ZTEST_F(uart_stress, test_throughput)
{
	uint32_t bytes_sent = 0U;
	int64_t start_time = 0, elapsed_time = 0;
	uint32_t throughput = 0U;
	uint8_t tx_byte = 0U;

	LOG_INF("Starting throughput test at %u baud", fixture->original_cfg.baudrate);

	start_time = k_uptime_get();

	/* Send data for throughput measurement */
	while (bytes_sent < THROUGHPUT_TEST_SIZE) {
		uart_poll_out(fixture->dev, tx_byte++);
		bytes_sent++;

		/* Yield periodically to simulate minor disturbances */
		if ((bytes_sent % 1024U) == 0) {
			k_yield();
		}
	}

	elapsed_time = k_uptime_get() - start_time;

	/* Calculate throughput */
	throughput = (bytes_sent * MSEC_IN_SEC) / elapsed_time;

	LOG_INF("Throughput test completed:");
	LOG_INF("  Bytes sent: %u", bytes_sent);
	LOG_INF("  Duration: %lld ms", elapsed_time);
	LOG_INF("  Throughput: %u bytes/sec", throughput);

	/* Theoretical maximum (considering 10 bits per byte for 8N1) */
	uint32_t theoretical_max_bytes_per_sec =
		fixture->original_cfg.baudrate / UART_ZTRESS_BITS_PER_BYTE_8N1;
	uint32_t min_acceptable = (theoretical_max_bytes_per_sec * 80) /
				  100; /* 80% efficiency, as we also printing on serial port */

	zassert_true(throughput >= min_acceptable,
		     "Throughput too low: %u bytes/sec (expected >%u)", throughput, min_acceptable);
}

/* Test: Endurance test */
ZTEST_F(uart_stress, test_endurance)
{
	const uint8_t concurrent_operations = 3U;
	struct uart_ztress_context ctx[3] = {0};
	uint32_t total_ops = 0U;
	int64_t start_time = 0, elapsed_time = 0;
	uint32_t min_expected = 0U;

	LOG_INF("Starting UART endurance test");

	/* Initialize contexts */
	for (uint8_t i = 0U; i < concurrent_operations; i++) {
		ctx[i].dev = fixture->dev;
		ctx[i].errors = &error_counter;
		ctx[i].operations = 0;
	}

	ztress_set_timeout(K_MSEC(ZTRESS_ENDURANCE_TEST_TIMEOUT_MS));
	start_time = k_uptime_get();

	/* Execute endurance test */
	ZTRESS_EXECUTE(
		ZTRESS_THREAD(ztress_config_handler, &ctx[0], ZTRESS_ITERATIONS,
			      ZTRESS_MIN_PREEMPTIONS, Z_TIMEOUT_TICKS(0)),
		ZTRESS_THREAD(ztress_poll_tx_handler, &ctx[1], ZTRESS_ENDURANCE_POLL_ITERATIONS,
			      ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
		ZTRESS_THREAD(ztress_poll_tx_handler, &ctx[2], ZTRESS_ENDURANCE_POLL_ITERATIONS,
			      ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)));
	min_expected = ZTRESS_ITERATIONS + 2 * ZTRESS_ENDURANCE_POLL_ITERATIONS;
	elapsed_time = k_uptime_get() - start_time;

	/* Verify results */
	LOG_INF("Endurance stress test completed in %lld ms", elapsed_time);
	total_ops = get_total_operations(ctx, concurrent_operations);
	check_communication_problems(total_ops);
	check_performance(total_ops, elapsed_time, min_expected, ZTRESS_ENDURANCE_TEST_TIMEOUT_MS);
}

ZTEST_SUITE(uart_stress, NULL, uart_stress_setup, uart_stress_before, uart_stress_teardown, NULL);