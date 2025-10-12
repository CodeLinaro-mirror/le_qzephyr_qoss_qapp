/*
 * Copyright (c) 2025 Qualcomm Technologies, Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * I2C Stress Test Suite for QCC730.
 * Uses ztress framework for robustness validation.
 */

#include <zephyr/ztest.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/ztress.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/random/random.h>

#include <string.h>

LOG_MODULE_REGISTER(i2c_stress_test, LOG_LEVEL_INF);

/* Test configuration */
#define TEST_I2C_ADDR         0x50 /* Dummy address for loopback tests */
#define LIS2DW12_ADDR         0x19 /* LIS2DW12 I2C address */
#define LIS2DW12_REG_WHO_AM_I 0x0F
#define LIS2DW12_WHO_AM_I_VAL 0x44

#define ZTRESS_ITERATIONS                  3000
#define ZTRESS_SENSOR_SCAN_ITERATIONS      1000
#define ZTRESS_HIGH_ITERATIONS             10000
#define ZTRESS_NO_PREEMPTION               0
#define ZTRESS_TEST_TIMEOUT_MS             10000U
#define ZTRESS_MINIMUM_OPERATIONS_PER_SEC  1500U
#define ZTRESS_ENDURANCE_ITERATIONS        50000U
#define ZTRESS_ENDURANCE_TIMEOUT_MS        150000U
#define ZTRESS_ERRORS_PERCENTAGE_TOLEARNCE 1

static const struct device *const lis2dw12_dev = DEVICE_DT_GET_ONE(st_lis2dw12);
static const struct device *const i2c_dev = DEVICE_DT_GET_ONE(qcom_qcc730_i2c);

/* Test fixture */
struct i2c_stress_fixture {
	bool use_sensor_addr;
};

/* Stress test context */
struct i2c_stress_context {
	const struct device *dev;
	uint16_t addr;
	uint32_t errors;
	uint32_t operations;
	uint32_t timeouts;
};

/* Global counters */
static atomic_t operation_counter;
static atomic_t error_counter;
static atomic_t timeout_counter;
static struct k_sem test_sem;

/**
 * Helper to convert numeric error value to string.
 */
const char *get_i2c_error_string(int err)
{
	return (err == 0 ? "Success" : strerror(-err));
}

/**
 * Helper to check performance.
 */
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

/**
 * Helper to check communication problems.
 */
void check_communication_problems(const uint32_t total_ops)
{
	const int32_t tolerance = (total_ops * (ZTRESS_ERRORS_PERCENTAGE_TOLEARNCE) / 100U);
	LOG_INF("Communication problems tolerance: %u", tolerance);
	zassert_within(atomic_get(&error_counter), 0, tolerance,
		       "Amount of errors is beyond the tolerance!");
	zassert_within(atomic_get(&timeout_counter), 0, tolerance,
		       "Amount of timeouts is beyond the tolerance!");
}

/**
 * Helper to send message to the sensor without Zephyr sensor API.
 */
int32_t read_who_am_i_reg(void)
{
	int32_t ret = 0;
	uint8_t who_am_i = 0U;
	const uint8_t reg = LIS2DW12_REG_WHO_AM_I;
	int64_t duration = 0;
	const int64_t start = k_uptime_get();

	ret = i2c_write_read(i2c_dev, LIS2DW12_ADDR, &reg, sizeof(reg), &who_am_i,
			     sizeof(who_am_i));
	duration = k_uptime_get() - start;

	if (ret != 0 || who_am_i != LIS2DW12_WHO_AM_I_VAL) {
		LOG_ERR("I2C failed: %s(%d), who_am_i: 0x%02x", get_i2c_error_string(ret), ret,
			who_am_i);
	}

	if (duration > 10) {
		LOG_WRN("WHO_AM_I took longer than expected %lld ms", duration);
	}

	return ret;
}

/**
 * Helper to print and get information about total amount of operations.
 */
static uint32_t get_total_operations(struct i2c_stress_context *ctx, const uint8_t count)
{
	uint32_t total_ops = 0U;
	LOG_INF("Per-thread statistics:");
	for (uint8_t i = 0U; i < count; i++) {
		LOG_INF("Thread[%u] summary: operations=%u, errors=%u, timeouts=%u", i,
			ctx[i].operations, ctx[i].errors, ctx[i].timeouts);
		total_ops += ctx[i].operations;
	}

	LOG_INF("  Total operations: %ld", atomic_get(&operation_counter));
	LOG_INF("  Total errors: %ld", atomic_get(&error_counter));
	LOG_INF("  Total timeouts: %ld", atomic_get(&timeout_counter));

	return total_ops;
}

/**
 * Test suite setup.
 */
static void *i2c_stress_setup(void)
{
	static struct i2c_stress_fixture fixture = {0};
	int ret = 0;

	/* Check if the sensor device driver is ready */
	zassume_true(device_is_ready(i2c_dev), "I2C device not ready");
	if (device_is_ready(lis2dw12_dev)) {
		fixture.use_sensor_addr = (read_who_am_i_reg() == 0);
	} else {
		LOG_WRN("LIS2DW12 device driver not ready, using controller-only tests");
		fixture.use_sensor_addr = false;
	}

	/* Initialize I2C controller to standard speed */
	ret = i2c_configure(i2c_dev, I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_FAST));
	zassume_ok(ret, "Failed to configure I2C");

	k_sem_init(&test_sem, 0, 1);
	atomic_set(&operation_counter, 0);
	atomic_set(&error_counter, 0);
	atomic_set(&timeout_counter, 0);

	LOG_INF("I2C stress test setup complete for QCC730");
	LOG_INF("Test mode: %s", fixture.use_sensor_addr ? "With LIS2DW12" : "Controller only");

	return &fixture;
}

/**
 * Test case setup.
 */
static void i2c_stress_before(void *)
{
	int ret = 0;

	/* Reset to standard speed */
	ret = i2c_configure(i2c_dev, I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_FAST));
	zassert_ok(ret, "Failed to reset I2C configuration");

	/* Clear counters */
	atomic_set(&operation_counter, 0);
	atomic_set(&error_counter, 0);
	atomic_set(&timeout_counter, 0);
	k_sem_reset(&test_sem);
}

/**
 * Test teardown.
 */
static void i2c_stress_teardown(void *f)
{
	LOG_INF("Test completed - Operations: %ld, Errors: %ld, Timeouts: %ld",
		atomic_get(&operation_counter), atomic_get(&error_counter),
		atomic_get(&timeout_counter));

	atomic_set(&operation_counter, 0);
	atomic_set(&error_counter, 0);
	atomic_set(&timeout_counter, 0);
}

ZTEST_SUITE(i2c_stress, NULL, i2c_stress_setup, i2c_stress_before, NULL, i2c_stress_teardown);

/**
 * Ztress handler: Configuration changes only (no actual transfers).
 */
static bool ztress_i2c_config_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct i2c_stress_context *ctx = (struct i2c_stress_context *)user_data;
	uint32_t speed_config = 0U;
	int ret = 0;

	if ((cnt % 2) == 0) {
		speed_config = I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_STANDARD);
	} else {
		speed_config = I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_FAST);
	}

	ret = i2c_configure(ctx->dev, speed_config);

	if (ret != 0) {
		atomic_inc(&error_counter);
		ctx->errors++;
		LOG_ERR("Config change failed: %d", ret);
	} else {
		ctx->operations++;
		atomic_inc(&operation_counter);
	}

	if (last) {
		LOG_DBG("Config thread completed: %u operations", ctx->operations);
		return false;
	}

	return true;
}

/**
 * Ztress handler: Multi-device access testing.
 * Tests rapid switching between different I2C addresses to validate address configuration changes.
 */
static bool ztress_i2c_multi_device_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct i2c_stress_context *ctx = (struct i2c_stress_context *)user_data;
	uint8_t reg_addr = 0U;
	uint8_t data = 0U;
	int ret = 0;
	uint8_t addr_index = 0U;
	uint8_t scan_addr = 0U;
	const uint8_t scan_addresses[] = {
		LIS2DW12_ADDR,
		TEST_I2C_ADDR, /* When LIS2DW12 is connected, writing to this address should also be successful */
	};

	addr_index = cnt % ARRAY_SIZE(scan_addresses);
	scan_addr = scan_addresses[addr_index];

	ret = i2c_write_read(ctx->dev, scan_addr, &reg_addr, sizeof(reg_addr), &data, sizeof(data));

	if (ret == -ETIMEDOUT) {
		atomic_inc(&timeout_counter);
		atomic_inc(&error_counter);
		ctx->errors++;
		ctx->timeouts++;
		LOG_DBG("Scan timeout at address 0x%02x", scan_addr);
	}

	ctx->operations++;
	atomic_inc(&operation_counter);

	if (last) {
		LOG_DBG("Multi-device thread completed: %u operations, %u errors", ctx->operations,
			ctx->errors);
		return false;
	}

	return true;
}

/**
 * Ztress handler: Write to sensor if available.
 */
static bool ztress_i2c_sensor_write_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct i2c_stress_context *ctx = (struct i2c_stress_context *)user_data;
	int ret = 0;

	ret = read_who_am_i_reg();

	if (ret == -ETIMEDOUT) {
		atomic_inc(&timeout_counter);
		atomic_inc(&error_counter);
		ctx->errors++;
		ctx->timeouts++;
	} else if (ret != 0) {
		atomic_inc(&error_counter);
		ctx->errors++;
	}

	ctx->operations++;
	atomic_inc(&operation_counter);

	if (last) {
		LOG_DBG("Sensor write thread completed: %u operations", ctx->operations);
		return false;
	}

	return true;
}

/**
 * Ztress handler: Zero-length transfers (lowest overhead).
 */
static bool ztress_i2c_zero_len_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct i2c_stress_context *ctx = (struct i2c_stress_context *)user_data;
	int ret = 0;

	/* Zero message transfer should always succeed */
	ret = i2c_transfer(ctx->dev, NULL, 0, ctx->addr);

	if (ret == -ETIMEDOUT) {
		atomic_inc(&timeout_counter);
		atomic_inc(&error_counter);
		ctx->errors++;
		ctx->timeouts++;
	} else if (ret != 0) {
		atomic_inc(&error_counter);
		ctx->errors++;
	}

	ctx->operations++;
	atomic_inc(&operation_counter);

	if (last) {
		LOG_DBG("Zero-len thread completed: %u operations", ctx->operations);
		return false;
	}

	return true;
}

/**
 * Test: Concurrent I2C operations.
 */
ZTEST_F(i2c_stress, test_concurrent_operations)
{
	const uint8_t concurrent_operations = 4U;
	uint32_t min_expected = 0U;
	uint32_t total_ops = 0U;
	uint32_t expected_sensor_operations = 0U;
	uint32_t expected_no_sensor_operations = 0U;
	int64_t start_time = 0, elapsed_time = 0;
	struct i2c_stress_context ctx[4] = {0};
	uint16_t test_addr = fixture->use_sensor_addr ? LIS2DW12_ADDR : TEST_I2C_ADDR;

	LOG_INF("Starting concurrent I2C operations stress test");
	LOG_INF("This test validates thread safety and synchronization");
	LOG_INF("Using address: 0x%02x (%s)", test_addr,
		fixture->use_sensor_addr ? "LIS2DW12" : "dummy");

	/* Initialize contexts */
	for (uint8_t i = 0U; i < concurrent_operations; i++) {
		ctx[i].dev = i2c_dev;
		ctx[i].addr = test_addr;
		ctx[i].errors = 0;
		ctx[i].operations = 0;
		ctx[i].timeouts = 0;
	}

	ztress_set_timeout(K_MSEC(ZTRESS_TEST_TIMEOUT_MS));
	start_time = k_uptime_get();

	if (fixture->use_sensor_addr) {
		/* With sensor: mix of config, scan, sensor ops, and zero-len */
		ZTRESS_EXECUTE(
			ZTRESS_THREAD(ztress_i2c_config_handler, &ctx[0], ZTRESS_ITERATIONS,
				      ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
			ZTRESS_THREAD(ztress_i2c_multi_device_handler, &ctx[1],
				      ZTRESS_SENSOR_SCAN_ITERATIONS, ZTRESS_NO_PREEMPTION,
				      Z_TIMEOUT_TICKS(0)),
			ZTRESS_THREAD(ztress_i2c_sensor_write_handler, &ctx[2], ZTRESS_ITERATIONS,
				      ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
			ZTRESS_THREAD(ztress_i2c_zero_len_handler, &ctx[3], ZTRESS_ITERATIONS,
				      ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)));
		expected_sensor_operations = 3 * ZTRESS_ITERATIONS + ZTRESS_SENSOR_SCAN_ITERATIONS;
	} else {
		/* Without sensor: config changes and zero-length only (no timeouts) */
		ZTRESS_EXECUTE(
			ZTRESS_THREAD(ztress_i2c_config_handler, &ctx[0], ZTRESS_ITERATIONS,
				      ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
			ZTRESS_THREAD(ztress_i2c_config_handler, &ctx[1], ZTRESS_ITERATIONS,
				      ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
			ZTRESS_THREAD(ztress_i2c_zero_len_handler, &ctx[2], ZTRESS_ITERATIONS,
				      ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
			ZTRESS_THREAD(ztress_i2c_zero_len_handler, &ctx[3], ZTRESS_ITERATIONS,
				      ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)));
		expected_no_sensor_operations = concurrent_operations * ZTRESS_ITERATIONS;
	}
	elapsed_time = k_uptime_get() - start_time;

	/* Report results */
	LOG_INF("Concurrent test completed in %lld ms", elapsed_time);
	total_ops = get_total_operations(ctx, concurrent_operations);
	min_expected = fixture->use_sensor_addr ? expected_sensor_operations
						: expected_no_sensor_operations;
	check_performance(total_ops, elapsed_time, min_expected, ZTRESS_TEST_TIMEOUT_MS);
	check_communication_problems(total_ops);
}

/**
 * Test: High-frequency configuration changes.
 */
ZTEST_F(i2c_stress, test_high_frequency_config)
{
	const uint8_t concurrent_operations = 2U;
	struct i2c_stress_context ctx[2] = {0};
	uint32_t total_ops = 0U;
	uint32_t min_expected = 0U;
	int64_t start_time = 0, elapsed_time = 0;

	LOG_INF("Starting high-frequency I2C configuration test");
	LOG_INF("This tests driver configuration stability");

	/* Initialize contexts */
	for (uint8_t i = 0U; i < concurrent_operations; i++) {
		ctx[i].dev = i2c_dev;
		ctx[i].addr = TEST_I2C_ADDR;
		ctx[i].errors = 0;
		ctx[i].operations = 0;
		ctx[i].timeouts = 0;
	}

	ztress_set_timeout(K_MSEC(ZTRESS_TEST_TIMEOUT_MS));
	start_time = k_uptime_get();

	/* Execute high-frequency config changes */
	ZTRESS_EXECUTE(ZTRESS_THREAD(ztress_i2c_config_handler, &ctx[0], ZTRESS_HIGH_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
		       ZTRESS_THREAD(ztress_i2c_zero_len_handler, &ctx[1], ZTRESS_HIGH_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)));
	min_expected = concurrent_operations * ZTRESS_HIGH_ITERATIONS;
	elapsed_time = k_uptime_get() - start_time;

	/* Report results */
	total_ops = get_total_operations(ctx, concurrent_operations);
	LOG_INF("High-frequency test completed in %lld ms", elapsed_time);
	LOG_INF("  Config changes: %u", ctx[0].operations);
	LOG_INF("  Zero-len transfers: %u", ctx[1].operations);

	check_performance(total_ops, elapsed_time, min_expected, ZTRESS_TEST_TIMEOUT_MS);
}

/**
 * Ztress handler: Sensor register operations.
 */
static bool ztress_sensor_register_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct i2c_stress_context *ctx = (struct i2c_stress_context *)user_data;
	struct sensor_value accel[3] = {0};
	int ret = 0;

	/* Alternate between different sensor operations */
	switch (cnt % 2) {
	case 0:
		/* Read acceleration data */
		ret = sensor_sample_fetch_chan(ctx->dev, SENSOR_CHAN_ACCEL_XYZ);
		if (ret == 0) {
			ret = sensor_channel_get(ctx->dev, SENSOR_CHAN_ACCEL_X, &accel[0]);
			ret |= sensor_channel_get(ctx->dev, SENSOR_CHAN_ACCEL_Y, &accel[1]);
			ret |= sensor_channel_get(ctx->dev, SENSOR_CHAN_ACCEL_Z, &accel[2]);
		}
		break;
	case 1:
		/* Read single axis */
		ret = sensor_sample_fetch_chan(ctx->dev, SENSOR_CHAN_ACCEL_X);
		if (ret == 0) {
			ret = sensor_channel_get(ctx->dev, SENSOR_CHAN_ACCEL_X, &accel[0]);
		}
		break;
	}

	if (ret != 0) {
		atomic_inc(&error_counter);
		ctx->errors++;
		LOG_DBG("Sensor operation failed: %d", ret);
	}

	ctx->operations++;
	atomic_inc(&operation_counter);

	if (last) {
		return false;
	}

	return true;
}

/**
 * Test: Sensor communication stress.
 */
ZTEST_F(i2c_stress, test_sensor_communication_stress)
{
	const uint8_t concurrent_operations = 4U;
	struct i2c_stress_context ctx[4] = {0};
	uint32_t total_ops = 0U;
	uint32_t min_expected = 0U;
	int64_t start_time = 0, elapsed_time = 0;

	/* Skip if sensor not available */
	if (!fixture->use_sensor_addr) {
		ztest_test_skip();
		return;
	}

	LOG_INF("Starting LIS2DW12 sensor communication stress test");

	/* Initialize contexts */
	for (uint8_t i = 0U; i < concurrent_operations; i++) {
		ctx[i].dev = lis2dw12_dev;
		ctx[i].addr = LIS2DW12_ADDR;
		ctx[i].errors = 0;
		ctx[i].operations = 0;
		ctx[i].timeouts = 0;
	}

	ztress_set_timeout(K_MSEC(ZTRESS_TEST_TIMEOUT_MS));
	start_time = k_uptime_get();

	/* Execute sensor stress test via official API and direct data read-write */
	ZTRESS_EXECUTE(ZTRESS_THREAD(ztress_sensor_register_handler, &ctx[0], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
		       ZTRESS_THREAD(ztress_sensor_register_handler, &ctx[1], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
		       ZTRESS_THREAD(ztress_i2c_sensor_write_handler, &ctx[2], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
		       ZTRESS_THREAD(ztress_sensor_register_handler, &ctx[3], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)));
	min_expected = concurrent_operations * ZTRESS_ITERATIONS;
	elapsed_time = k_uptime_get() - start_time;

	/* Report results */
	LOG_INF("Concurrent test completed in %lld ms", elapsed_time);
	total_ops = get_total_operations(ctx, concurrent_operations);
	check_performance(total_ops, elapsed_time, min_expected, ZTRESS_TEST_TIMEOUT_MS);
	check_communication_problems(total_ops);
}

/**
 * Test: Sensor endurance stress.
 */
ZTEST_F(i2c_stress, test_sensor_endurance_stress)
{
	const uint8_t concurrent_operations = 4U;
	struct i2c_stress_context ctx[4] = {0};
	uint32_t total_ops = 0U;
	uint32_t min_expected = 0U;
	int64_t start_time = 0, elapsed_time = 0;

	/* Skip if sensor not available */
	if (!fixture->use_sensor_addr) {
		ztest_test_skip();
		return;
	}

	LOG_INF("Starting LIS2DW12 sensor endurance stress test");

	/* Initialize contexts */
	for (uint8_t i = 0U; i < concurrent_operations; i++) {
		ctx[i].dev = lis2dw12_dev;
		ctx[i].addr = LIS2DW12_ADDR;
		ctx[i].errors = 0;
		ctx[i].operations = 0;
		ctx[i].timeouts = 0;
	}

	ztress_set_timeout(K_MSEC(ZTRESS_ENDURANCE_TIMEOUT_MS));
	start_time = k_uptime_get();

	/* Execute sensor stress test via official API and direct data read-write */
	ZTRESS_EXECUTE(
		ZTRESS_THREAD(ztress_sensor_register_handler, &ctx[0], ZTRESS_ENDURANCE_ITERATIONS,
			      ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
		ZTRESS_THREAD(ztress_sensor_register_handler, &ctx[1], ZTRESS_ENDURANCE_ITERATIONS,
			      ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
		ZTRESS_THREAD(ztress_i2c_sensor_write_handler, &ctx[2], ZTRESS_ENDURANCE_ITERATIONS,
			      ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
		ZTRESS_THREAD(ztress_sensor_register_handler, &ctx[3], ZTRESS_ENDURANCE_ITERATIONS,
			      ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)));
	min_expected = concurrent_operations * ZTRESS_ENDURANCE_ITERATIONS;
	elapsed_time = k_uptime_get() - start_time;

	/* Report results */
	LOG_INF("Endurance test completed in %lld ms", elapsed_time);
	total_ops = get_total_operations(ctx, concurrent_operations);
	check_performance(total_ops, elapsed_time, min_expected, ZTRESS_ENDURANCE_TIMEOUT_MS);
	check_communication_problems(total_ops);
}
