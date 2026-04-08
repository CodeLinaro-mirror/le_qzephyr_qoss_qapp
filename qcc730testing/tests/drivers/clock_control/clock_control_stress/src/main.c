/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <zephyr/ztest.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>
#include <zephyr/ztest_test.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztress.h>
#include <zephyr/dt-bindings/clock/qcom-qcc730-clock.h>

LOG_MODULE_REGISTER(clock_control_stress, CONFIG_CLOCK_CONTROL_LOG_LEVEL);

/* Test configuration */
#define ZTRESS_TIMEOUT              K_SECONDS(10)
#define ZTRESS_ENDURANCE_TIMEOUT    K_SECONDS(80)
#define ZTRESS_ITERATIONS           3000U
#define ZTRESS_ENDURANCE_ITERATIONS 100000U
#define ZTRESS_HIGH_ITERATIONS      10000U
#define ZTRESS_NO_PREEMPTION        0U
#define ZTRESS_MIN_PREEMPTIONS      100U
#define MIN_OPS_PER_SECOND          1500U
#define ERROR_TOLERANCE_PERCENT     1U
#define ZTRESS_ENDURANCE_SECONDS    60U

/* Clock subsystems to test - only those that actually use clock_control API */
/* UART and GPIO are skipped to not brake communication */
static const clock_control_subsys_t test_subsystems[] = {
	(clock_control_subsys_t)QCC730_CLOCK_I2C,
	(clock_control_subsys_t)QCC730_CLOCK_QTIMER,
	(clock_control_subsys_t)QCC730_CLOCK_WDOG,
};

static const char *subsys_names[] = {
	"I2C",
	"QTIMER",
	"WDOG",
};

struct clock_ztress_context {
	const struct device *dev;
	clock_control_subsys_t subsys;
	atomic_t *errors;
	uint32_t operations;
};

static atomic_t error_counter;

struct clock_stress_fixture {
	const struct device *dev;
};

/* Helper: Get subsystem name */
static const char *get_subsys_name(const clock_control_subsys_t subsys)
{
	for (size_t i = 0U; i < ARRAY_SIZE(test_subsystems); i++) {
		if ((uint32_t)test_subsystems[i] == (uint32_t)subsys) {
			return subsys_names[i];
		}
	}
	return "UNKNOWN";
}

/* Ztress handler: Clock ON operations */
static bool ztress_clock_on_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct clock_ztress_context *ctx = (struct clock_ztress_context *)user_data;
	int ret = 0;

	ret = clock_control_on(ctx->dev, ctx->subsys);

	if (ret != 0 && ret != -EALREADY) {
		atomic_inc(ctx->errors);
		LOG_ERR("Clock ON failed for %s: %d", get_subsys_name(ctx->subsys), ret);
	}

	ctx->operations++;

	if (last) {
		return false;
	}

	return true;
}

/* Ztress handler: Clock OFF operations */
static bool ztress_clock_off_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct clock_ztress_context *ctx = (struct clock_ztress_context *)user_data;
	int ret = 0;

	ret = clock_control_off(ctx->dev, ctx->subsys);

	if (ret != 0) {
		atomic_inc(ctx->errors);
		LOG_ERR("Clock OFF failed for %s: %d", get_subsys_name(ctx->subsys), ret);
	}

	ctx->operations++;

	if (last) {
		return false;
	}

	return true;
}

/* Ztress handler: Clock get_status operations */
static bool ztress_clock_status_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct clock_ztress_context *ctx = (struct clock_ztress_context *)user_data;

	const enum clock_control_status status = clock_control_get_status(ctx->dev, ctx->subsys);

	if (status == CLOCK_CONTROL_STATUS_UNKNOWN) {
		atomic_inc(ctx->errors);
		LOG_ERR("Clock status UNKNOWN for %s", get_subsys_name(ctx->subsys));
	}

	ctx->operations++;

	if (last) {
		return false;
	}

	return true;
}

/* Ztress handler: Clock toggle (ON/OFF) operations */
static bool ztress_clock_toggle_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct clock_ztress_context *ctx = (struct clock_ztress_context *)user_data;
	int ret = 0;

	if (cnt % 2 == 0) {
		ret = clock_control_on(ctx->dev, ctx->subsys);
		if (ret != 0 && ret != -EALREADY) {
			atomic_inc(ctx->errors);
		}
	} else {
		ret = clock_control_off(ctx->dev, ctx->subsys);
		if (ret != 0) {
			atomic_inc(ctx->errors);
		}
	}

	ctx->operations++;

	if (last) {
		return false;
	}

	if ((cnt % 5000U) == 0U) {
		k_yield();
	}

	return true;
}

/* Ztress handler: Mixed operations (ON/OFF/STATUS) */
static bool ztress_clock_mixed_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct clock_ztress_context *ctx = (struct clock_ztress_context *)user_data;
	int ret = 0;

	switch (cnt % 3U) {
	case 0U:
		ret = clock_control_on(ctx->dev, ctx->subsys);
		if (ret != 0 && ret != -EALREADY) {
			atomic_inc(ctx->errors);
		}
		break;
	case 1U:
		ret = clock_control_off(ctx->dev, ctx->subsys);
		if (ret != 0) {
			atomic_inc(ctx->errors);
		}
		break;
	case 2U:
		const enum clock_control_status status =
			clock_control_get_status(ctx->dev, ctx->subsys);
		if (status == CLOCK_CONTROL_STATUS_UNKNOWN) {
			atomic_inc(ctx->errors);
		}
		break;
	}

	ctx->operations++;

	if (last) {
		return false;
	}

	return true;
}

/* Helper: Calculate total operations */
static uint32_t get_total_operations(struct clock_ztress_context *ctx, uint8_t count)
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
		ops_per_sec = (total_ops * 1000U) / (uint32_t)elapsed_ms;
	}

	LOG_INF("Performance: %u ops in %lld ms (%u ops/sec)", total_ops, elapsed_ms, ops_per_sec);

	zassert_true((uint32_t)elapsed_ms <= timeout_ms, "Test should complete before timeout was hit!");
	zassert_true(total_ops >= min_expected, "Operations %u below minimum %u", total_ops,
		     min_expected);

	zassert_true(ops_per_sec >= MIN_OPS_PER_SECOND, "Throughput %u ops/sec below minimum %u",
		     ops_per_sec, MIN_OPS_PER_SECOND);

	max_errors = (total_ops * ERROR_TOLERANCE_PERCENT) / 100U;
	zassert_true(atomic_get(&error_counter) <= max_errors, "Too many errors: %ld (max: %u)",
		     atomic_get(&error_counter), max_errors);
}

/* Helper: Execute clock cleanup operations */
static void clock_cleanup(struct clock_stress_fixture *fixture)
{
	int ret = 0;
	atomic_set(&error_counter, 0);

	/* Turn ON all clocks to known state */
	for (size_t i = 0U; i < ARRAY_SIZE(test_subsystems); i++) {
		ret = clock_control_on(fixture->dev, test_subsystems[i]);
		if (ret != 0 && ret != -EALREADY) {
			LOG_ERR("Failed to turn ON %s during cleanup: %d", subsys_names[i], ret);
		}
	}
}

/* Test setup */
static void *clock_stress_setup(void)
{
	static struct clock_stress_fixture fixture = {0};
	fixture.dev = DEVICE_DT_GET(DT_NODELABEL(clock_controller));
	zassert_true(device_is_ready(fixture.dev), "Clock control device not ready");
	return &fixture;
}

/* Before each test */
static void clock_stress_before(void *f)
{
	clock_cleanup((struct clock_stress_fixture *)f);
}

/* After each test */
static void clock_stress_teardown(void *f)
{
	clock_cleanup((struct clock_stress_fixture *)f);
}

/* Test: Concurrent operations */
ZTEST_F(clock_stress, test_concurrent_operations)
{
	const uint8_t concurrent_operations = 3U;
	struct clock_ztress_context ctx[3] = {{.dev = fixture->dev,
					       .subsys = test_subsystems[0],
					       .errors = &error_counter,
					       .operations = 0},
					      {.dev = fixture->dev,
					       .subsys = test_subsystems[1],
					       .errors = &error_counter,
					       .operations = 0},
					      {.dev = fixture->dev,
					       .subsys = test_subsystems[2],
					       .errors = &error_counter,
					       .operations = 0}};
	uint32_t total_ops = 0U;
	int64_t start_time = 0, elapsed_time = 0;
	const uint32_t min_expected = ZTRESS_ITERATIONS * concurrent_operations;

	LOG_INF("Starting clock control concurrent operations test");

	ztress_set_timeout(ZTRESS_TIMEOUT);
	start_time = k_uptime_get();

	ZTRESS_EXECUTE(ZTRESS_THREAD(ztress_clock_on_handler, &ctx[0], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
		       ZTRESS_THREAD(ztress_clock_off_handler, &ctx[1], ZTRESS_ITERATIONS,
				     ZTRESS_MIN_PREEMPTIONS, Z_TIMEOUT_TICKS(0)),
		       ZTRESS_THREAD(ztress_clock_status_handler, &ctx[2], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)));

	elapsed_time = k_uptime_get() - start_time;
	total_ops = get_total_operations(ctx, concurrent_operations);

	LOG_INF("Concurrent test completed:");
	LOG_INF("  ON operations (%s): %u", get_subsys_name(ctx[0].subsys), ctx[0].operations);
	LOG_INF("  OFF operations (%s): %u", get_subsys_name(ctx[1].subsys), ctx[1].operations);
	LOG_INF("  STATUS operations (%s): %u", get_subsys_name(ctx[2].subsys), ctx[2].operations);
	LOG_INF("  Total errors: %ld", atomic_get(&error_counter));

	check_errors();
	check_performance(total_ops, elapsed_time, min_expected,
			  k_ticks_to_ms_floor32(ZTRESS_TIMEOUT.ticks));
}

/* Test: Mixed operations across all subsystems */
ZTEST_F(clock_stress, test_mixed_subsystems)
{
	const uint8_t concurrent_operations = 3U;
	struct clock_ztress_context ctx[3] = {{.dev = fixture->dev,
					       .subsys = test_subsystems[0],
					       .errors = &error_counter,
					       .operations = 0},
					      {.dev = fixture->dev,
					       .subsys = test_subsystems[1],
					       .errors = &error_counter,
					       .operations = 0},
					      {.dev = fixture->dev,
					       .subsys = test_subsystems[2],
					       .errors = &error_counter,
					       .operations = 0}};
	uint32_t total_ops = 0U;
	int64_t start_time = 0, elapsed_time = 0;
	const uint32_t min_expected = ZTRESS_ITERATIONS * concurrent_operations;

	LOG_INF("Starting mixed operations test across multiple subsystems");

	ztress_set_timeout(ZTRESS_TIMEOUT);
	start_time = k_uptime_get();

	ZTRESS_EXECUTE(ZTRESS_THREAD(ztress_clock_mixed_handler, &ctx[0], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
		       ZTRESS_THREAD(ztress_clock_mixed_handler, &ctx[1], ZTRESS_ITERATIONS,
				     ZTRESS_MIN_PREEMPTIONS, Z_TIMEOUT_TICKS(1)),
		       ZTRESS_THREAD(ztress_clock_mixed_handler, &ctx[2], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(2)));

	elapsed_time = k_uptime_get() - start_time;
	total_ops = get_total_operations(ctx, concurrent_operations);

	LOG_INF("Mixed operations test completed:");
	for (uint8_t i = 0; i < concurrent_operations; i++) {
		LOG_INF("  %s: %u operations", get_subsys_name(ctx[i].subsys), ctx[i].operations);
	}
	LOG_INF("  Total errors: %ld", atomic_get(&error_counter));

	check_errors();
	check_performance(total_ops, elapsed_time, min_expected,
			  k_ticks_to_ms_floor32(ZTRESS_TIMEOUT.ticks));
}

/* Test: Endurance test with time accuracy validation */
ZTEST_F(clock_stress, test_endurance)
{
	const uint8_t concurrent_operations = 3U;
	struct clock_ztress_context ctx[3] = {{.dev = fixture->dev,
					       .subsys = test_subsystems[0],
					       .errors = &error_counter,
					       .operations = 0U},
					      {.dev = fixture->dev,
					       .subsys = test_subsystems[1],
					       .errors = &error_counter,
					       .operations = 0U},
					      {.dev = fixture->dev,
					       .subsys = test_subsystems[2],
					       .errors = &error_counter,
					       .operations = 0U}};
	uint32_t total_ops = 0U;
	int64_t start_time = 0, elapsed_time = 0;
	const uint32_t min_expected = ZTRESS_ENDURANCE_ITERATIONS * concurrent_operations;

	LOG_INF("Starting clock control endurance test");

	ztress_set_timeout(ZTRESS_ENDURANCE_TIMEOUT);
	start_time = k_uptime_get();

	ZTRESS_EXECUTE(
		ZTRESS_THREAD(ztress_clock_toggle_handler, &ctx[0], ZTRESS_ENDURANCE_ITERATIONS,
			      ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
		ZTRESS_THREAD(ztress_clock_toggle_handler, &ctx[1], ZTRESS_ENDURANCE_ITERATIONS,
			      ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
		ZTRESS_THREAD(ztress_clock_toggle_handler, &ctx[2], ZTRESS_ENDURANCE_ITERATIONS,
			      ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)));

	elapsed_time = k_uptime_get() - start_time;
	total_ops = get_total_operations(ctx, concurrent_operations);

	LOG_INF("Endurance test completed:");
	LOG_INF("  %s: %u operations", get_subsys_name(ctx[0].subsys), ctx[0].operations);
	LOG_INF("  %s: %u operations", get_subsys_name(ctx[1].subsys), ctx[1].operations);
	LOG_INF("  %s: %u operations", get_subsys_name(ctx[2].subsys), ctx[2].operations);
	LOG_INF("  Total errors: %ld", atomic_get(&error_counter));
	LOG_INF("  Duration: %lld ms", elapsed_time);

	check_errors();
	check_performance(total_ops, elapsed_time, min_expected,
			  k_ticks_to_ms_floor32(ZTRESS_ENDURANCE_TIMEOUT.ticks));
}

ZTEST_SUITE(clock_stress, NULL, clock_stress_setup, clock_stress_before, clock_stress_teardown,
	    NULL);