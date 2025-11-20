/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <zephyr/ztest.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>
#include <zephyr/ztest_test.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztress.h>

LOG_MODULE_REGISTER(counter_stress, CONFIG_COUNTER_LOG_LEVEL);

/* Test configuration */
#define ZTRESS_TIMEOUT              K_SECONDS(10)
#define ZTRESS_ENDURANCE_TIMEOUT    K_SECONDS(60)
#define ZTRESS_ITERATIONS           3000U
#define ZTRESS_ENDURANCE_ITERATIONS 10000U
#define ZTRESS_NO_PREEMPTION        0U
#define MIN_OPS_PER_SECOND          1500U
#define ERROR_TOLERANCE_PERCENT     1U
#define ALARM_TICKS_SHORT           100U
#define ALARM_TICKS_LONG            1000U

struct counter_ztress_context {
	const struct device *dev;
	atomic_t *errors;
	atomic_t *alarm_fired;
	atomic_t *busy_errors;
	uint32_t operations;
};

static atomic_t error_counter;
static atomic_t alarm_counter;
static atomic_t busy_counter;

struct counter_stress_fixture {
	const struct device *dev;
};

/* Alarm callback for stress tests */
static void alarm_callback(const struct device *dev, uint8_t chan_id, uint32_t ticks,
			   void *user_data)
{
	atomic_t *counter = (atomic_t *)user_data;

	atomic_inc(counter);
	LOG_DBG("Alarm callback fired, count: %ld", atomic_get(counter));
}

/* Ztress handler: Start/Stop operations */
static bool ztress_start_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct counter_ztress_context *ctx = (struct counter_ztress_context *)user_data;
	const int ret = counter_start(ctx->dev);

	if (ret != 0) {
		atomic_inc(ctx->errors);
		LOG_ERR("Counter start failed: %d", ret);
	}

	ctx->operations++;

	if (last) {
		return false;
	}

	if ((cnt % 100U) == 0U) {
		k_yield();
	}

	return true;
}

/* Ztress handler: Stop operations */
static bool ztress_stop_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct counter_ztress_context *ctx = (struct counter_ztress_context *)user_data;
	const int ret = counter_stop(ctx->dev);

	if (ret != 0) {
		atomic_inc(ctx->errors);
		LOG_ERR("Counter stop failed: %d", ret);
	}

	ctx->operations++;

	if (last) {
		return false;
	}

	if ((cnt % 100U) == 0U) {
		k_yield();
	}

	return true;
}

/* Ztress handler: Get value operations */
static bool ztress_get_value_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct counter_ztress_context *ctx = (struct counter_ztress_context *)user_data;
	uint64_t value = 0ULL;
	const int ret = counter_get_value_64(ctx->dev, &value);

	if (ret != 0) {
		atomic_inc(ctx->errors);
		LOG_ERR("Get value failed: %d", ret);
	}

	ctx->operations++;

	if (last) {
		return false;
	}

	return true;
}

/* Ztress handler: Set alarm with relative ticks */
static bool ztress_set_alarm_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct counter_ztress_context *ctx = (struct counter_ztress_context *)user_data;
	struct counter_alarm_cfg alarm_cfg = {0};
	int ret = 0;

	/* Alternate between short and long delays */
	const uint32_t ticks = (cnt % 2U) ? ALARM_TICKS_SHORT : ALARM_TICKS_LONG;

	alarm_cfg.ticks = ticks;
	alarm_cfg.callback = alarm_callback;
	alarm_cfg.user_data = ctx->alarm_fired;

	ret = counter_set_channel_alarm(ctx->dev, 0, &alarm_cfg);
	if (ret == -EBUSY) {
		/* EBUSY is expected when alarm already set, not an error */
		atomic_inc(ctx->busy_errors);
	} else if (ret != 0) {
		atomic_inc(ctx->errors);
		LOG_ERR("Set alarm failed: %d", ret);
	} else {
		// Added to avoid MISRA warnings
	}

	ctx->operations++;

	if (last) {
		return false;
	}

	if ((cnt % 100U) == 0U) {
		k_yield();
	}

	return true;
}

/* Ztress handler: Cancel alarm operations */
static bool ztress_cancel_alarm_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct counter_ztress_context *ctx = (struct counter_ztress_context *)user_data;
	const int ret = counter_cancel_channel_alarm(ctx->dev, 0);

	if (ret != 0) {
		atomic_inc(ctx->errors);
		LOG_ERR("Cancel alarm failed: %d", ret);
	}

	ctx->operations++;

	if (last) {
		return false;
	}

	if ((cnt % 100U) == 0U) {
		k_yield();
	}

	return true;
}

/* Helper: Calculate total operations */
static uint32_t get_total_operations(struct counter_ztress_context *ctx, uint8_t count)
{
	uint32_t total = 0U;
	for (uint8_t i = 0U; i < count; i++) {
		total += ctx[i].operations;
	}
	return total;
}

/* Helper: Check for errors */
static void check_errors(void)
{
	const atomic_val_t errors = atomic_get(&error_counter);
	zassert_equal(errors, 0, "Errors detected during stress test: %ld", errors);
}

/* Helper: Check performance */
static void check_performance(uint32_t total_ops, int64_t elapsed_ms, uint32_t min_expected,
			      uint32_t timeout_ms)
{
	uint32_t ops_per_sec = 0U;
	uint32_t max_errors = 0U;

	if (elapsed_ms > 0) {
		ops_per_sec = (total_ops * 1000U) / elapsed_ms;
	}

	LOG_INF("Performance: %u ops in %lld ms (%u ops/sec)", total_ops, elapsed_ms, ops_per_sec);

	zassert_true(elapsed_ms <= timeout_ms, "Test should complete before timeout was hit!");
	zassert_true(total_ops >= min_expected, "Operations %u below minimum %u", total_ops,
		     min_expected);

	zassert_true(ops_per_sec >= MIN_OPS_PER_SECOND, "Throughput %u ops/sec below minimum %u",
		     ops_per_sec, MIN_OPS_PER_SECOND);

	max_errors = (total_ops * ERROR_TOLERANCE_PERCENT) / 100U;
	zassert_true(atomic_get(&error_counter) <= max_errors, "Too many errors: %ld (max: %u)",
		     atomic_get(&error_counter), max_errors);
}

/* Helper: Counter cleanup */
static void counter_cleanup(struct counter_stress_fixture *fixture)
{
	atomic_set(&error_counter, 0);
	atomic_set(&alarm_counter, 0);
	atomic_set(&busy_counter, 0);
	counter_cancel_channel_alarm(fixture->dev, 0);
	counter_start(fixture->dev);
}

/* Test setup */
static void *counter_stress_setup(void)
{
	static struct counter_stress_fixture fixture = {0};

	fixture.dev = DEVICE_DT_GET(DT_ALIAS(counter0));
	zassert_true(device_is_ready(fixture.dev), "Counter device not ready");

	return &fixture;
}

/* Before each test */
static void counter_stress_before(void *f)
{
	counter_cleanup((struct counter_stress_fixture *)f);
}

/* After each test */
static void counter_stress_teardown(void *f)
{
	counter_cleanup((struct counter_stress_fixture *)f);
}

/* Test: Concurrent start/stop operations */
ZTEST_F(counter_stress, test_concurrent_start_stop)
{
	const uint8_t concurrent_operations = 3U;
	struct counter_ztress_context ctx[3] = {
		{.dev = fixture->dev, .errors = &error_counter, .operations = 0},
		{.dev = fixture->dev, .errors = &error_counter, .operations = 0},
		{.dev = fixture->dev, .errors = &error_counter, .operations = 0}};
	uint32_t total_ops = 0U;
	int64_t start_time = 0, elapsed_time = 0;
	const uint32_t min_expected = ZTRESS_ITERATIONS * concurrent_operations;

	LOG_INF("Starting counter start/stop stress test");

	ztress_set_timeout(ZTRESS_TIMEOUT);
	start_time = k_uptime_get();

	ZTRESS_EXECUTE(ZTRESS_THREAD(ztress_start_handler, &ctx[0], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
		       ZTRESS_THREAD(ztress_stop_handler, &ctx[1], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
		       ZTRESS_THREAD(ztress_get_value_handler, &ctx[2], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)));

	elapsed_time = k_uptime_get() - start_time;
	total_ops = get_total_operations(ctx, concurrent_operations);

	LOG_INF("Start/Stop test completed:");
	LOG_INF("  Start operations: %u", ctx[0].operations);
	LOG_INF("  Stop operations: %u", ctx[1].operations);
	LOG_INF("  Get value operations: %u", ctx[2].operations);
	LOG_INF("  Total errors: %ld", atomic_get(&error_counter));

	check_errors();
	check_performance(total_ops, elapsed_time, min_expected,
			  k_ticks_to_ms_floor32(ZTRESS_TIMEOUT.ticks));
}

/* Test: Alarm with relative ticks */
ZTEST_F(counter_stress, test_alarm_relative_mode)
{
	const uint8_t concurrent_operations = 2U;
	struct counter_ztress_context ctx[2] = {
		{.dev = fixture->dev,
		 .errors = &error_counter,
		 .alarm_fired = &alarm_counter,
		 .busy_errors = &busy_counter,
		 .operations = 0},
		{.dev = fixture->dev, .errors = &error_counter, .operations = 0}};
	uint32_t total_ops = 0U;
	int64_t start_time = 0, elapsed_time = 0;
	const uint32_t min_expected = ZTRESS_ITERATIONS * concurrent_operations;

	LOG_INF("Starting counter relative alarm stress test");

	ztress_set_timeout(ZTRESS_TIMEOUT);
	start_time = k_uptime_get();

	ZTRESS_EXECUTE(ZTRESS_THREAD(ztress_set_alarm_handler, &ctx[0], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
		       ZTRESS_THREAD(ztress_cancel_alarm_handler, &ctx[1], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)));

	elapsed_time = k_uptime_get() - start_time;
	total_ops = get_total_operations(ctx, concurrent_operations);

	LOG_INF("Relative alarm test completed:");
	LOG_INF("  Set alarm operations: %u", ctx[0].operations);
	LOG_INF("  Cancel alarm operations: %u", ctx[1].operations);
	LOG_INF("  Alarms fired: %ld", atomic_get(&alarm_counter));
	LOG_INF("  EBUSY occurrences: %ld", atomic_get(&busy_counter));
	LOG_INF("  Total errors: %ld", atomic_get(&error_counter));

	check_errors();
	check_performance(total_ops, elapsed_time, min_expected,
			  k_ticks_to_ms_floor32(ZTRESS_TIMEOUT.ticks));
}

/* Test: Endurance test */
ZTEST_F(counter_stress, test_endurance)
{
	const uint8_t concurrent_operations = 4U;
	struct counter_ztress_context ctx[4] = {
		{.dev = fixture->dev, .errors = &error_counter, .operations = 0},
		{.dev = fixture->dev, .errors = &error_counter, .operations = 0},
		{.dev = fixture->dev,
		 .errors = &error_counter,
		 .alarm_fired = &alarm_counter,
		 .busy_errors = &busy_counter,
		 .operations = 0},
		{.dev = fixture->dev, .errors = &error_counter, .operations = 0}};
	uint32_t total_ops = 0U;
	int64_t start_time = 0, elapsed_time = 0;
	const uint32_t min_expected = ZTRESS_ENDURANCE_ITERATIONS * concurrent_operations;

	LOG_INF("Starting counter endurance test");

	ztress_set_timeout(ZTRESS_ENDURANCE_TIMEOUT);
	start_time = k_uptime_get();

	ZTRESS_EXECUTE(ZTRESS_THREAD(ztress_start_handler, &ctx[0], ZTRESS_ENDURANCE_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
		       ZTRESS_THREAD(ztress_stop_handler, &ctx[1], ZTRESS_ENDURANCE_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
		       ZTRESS_THREAD(ztress_set_alarm_handler, &ctx[2], ZTRESS_ENDURANCE_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
		       ZTRESS_THREAD(ztress_cancel_alarm_handler, &ctx[3],
				     ZTRESS_ENDURANCE_ITERATIONS, ZTRESS_NO_PREEMPTION,
				     Z_TIMEOUT_TICKS(0)));

	elapsed_time = k_uptime_get() - start_time;
	total_ops = get_total_operations(ctx, concurrent_operations);

	LOG_INF("Endurance test completed:");
	LOG_INF("  Start operations: %u", ctx[0].operations);
	LOG_INF("  Stop operations: %u", ctx[1].operations);
	LOG_INF("  Set alarm operations: %u", ctx[2].operations);
	LOG_INF("  Cancel alarm operations: %u", ctx[3].operations);
	LOG_INF("  Total errors: %ld", atomic_get(&error_counter));
	LOG_INF("  Duration: %lld ms", elapsed_time);

	check_errors();
	check_performance(total_ops, elapsed_time, min_expected,
			  k_ticks_to_ms_floor32(ZTRESS_ENDURANCE_TIMEOUT.ticks));
}

ZTEST_SUITE(counter_stress, NULL, counter_stress_setup, counter_stress_before,
	    counter_stress_teardown, NULL);