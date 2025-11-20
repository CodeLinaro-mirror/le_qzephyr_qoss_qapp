/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <zephyr/ztest.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>
#include <zephyr/ztest_test.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztress.h>

LOG_MODULE_REGISTER(rtc_stress, CONFIG_RTC_LOG_LEVEL);

/* Test configuration */
#define ZTRESS_TIMEOUT              K_SECONDS(10)
#define ZTRESS_ENDURANCE_TIMEOUT    K_SECONDS(60)
#define ZTRESS_ITERATIONS           3000U
#define ZTRESS_ENDURANCE_ITERATIONS 10000U
#define ZTRESS_NO_PREEMPTION        0U
#define MIN_OPS_PER_SECOND          1500U
#define ERROR_TOLERANCE_PERCENT     1U
#define ALARM_ID                    0U

/* Base time for tests (2025-01-01 00:00:00 UTC) */
#define BASE_YEAR                   125
#define BASE_MONTH                  0
#define BASE_DAY                    1

#define SECONDS_PER_MINUTE          60U
#define MINUTES_PER_HOUR            60U
#define HOURS_PER_DAY               24U
#define SECONDS_PER_HOUR            3600U
#define SECONDS_PER_DAY             86400U
#define NANOSECONDS_PER_MILLISECOND 1000000U
#define NANOSECONDS_PER_SECOND      1000000000U
#define DST_AUTO_DETECT             -1

struct rtc_ztress_context {
	const struct device *dev;
	atomic_t *errors;
	atomic_t *alarm_fired;
	uint32_t operations;
	int64_t base_time_ms;
};

static atomic_t error_counter;
static atomic_t alarm_counter;

struct rtc_stress_fixture {
	const struct device *dev;
};

/* Ztress handler: Set time operations */
static bool ztress_set_time_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct rtc_ztress_context *ctx = (struct rtc_ztress_context *)user_data;
	struct rtc_time time = {0};
	int ret = 0;

	time.tm_year = BASE_YEAR;
	time.tm_mon = BASE_MONTH;
	time.tm_mday = BASE_DAY + (cnt / SECONDS_PER_DAY);
	time.tm_hour = (cnt / SECONDS_PER_HOUR) % HOURS_PER_DAY;
	time.tm_min = (cnt / SECONDS_PER_MINUTE) % MINUTES_PER_HOUR;
	time.tm_sec = cnt % SECONDS_PER_MINUTE;
	time.tm_nsec = (cnt * NANOSECONDS_PER_MILLISECOND) % NANOSECONDS_PER_SECOND;
	time.tm_isdst = DST_AUTO_DETECT;

	ret = rtc_set_time(ctx->dev, &time);

	if (ret != 0) {
		atomic_inc(ctx->errors);
		LOG_ERR("Set time failed: %d", ret);
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

/* Ztress handler: Get time operations */
static bool ztress_get_time_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct rtc_ztress_context *ctx = (struct rtc_ztress_context *)user_data;
	struct rtc_time time = {0};
	int ret = 0;

	ret = rtc_get_time(ctx->dev, &time);

	if (ret != 0 && ret != -ENODATA) {
		atomic_inc(ctx->errors);
		LOG_ERR("Get time failed: %d", ret);
	} else if (ret == 0) {
		if (time.tm_sec > 59 || time.tm_min > 59 || time.tm_hour > 23 || time.tm_mday < 1 ||
		    time.tm_mday > 31 || time.tm_mon > 11) {
			atomic_inc(ctx->errors);
			LOG_ERR("Invalid time read: %d-%d-%d %d:%d:%d", time.tm_year, time.tm_mon,
				time.tm_mday, time.tm_hour, time.tm_min, time.tm_sec);
		}
	}

	ctx->operations++;

	if (last) {
		return false;
	}

	return true;
}

/* Ztress handler: Alarm set/get/cancel/pending operations */
static bool ztress_alarm_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct rtc_ztress_context *ctx = (struct rtc_ztress_context *)user_data;
	struct rtc_time alarm_time = {0};
	uint16_t read_mask = 0U;
	int ret = 0;
	int pending = 0;

	/* Cycle through different alarm operations */
	switch (cnt % 4U) {
	case 0:
		ret = rtc_alarm_get_time(ctx->dev, ALARM_ID, &read_mask, &alarm_time);
		if (ret != 0) {
			atomic_inc(ctx->errors);
			LOG_ERR("Failed to get alarm time: %d", ret);
		}
		break;

	case 1:
	case 2:
		pending = rtc_alarm_is_pending(ctx->dev, ALARM_ID);
		if (pending < 0) {
			atomic_inc(ctx->errors);
			LOG_ERR("Failed to check alarm pending: %d", pending);
		}
		break;

	case 3:
		ret = rtc_alarm_set_time(ctx->dev, ALARM_ID, 0, NULL);
		if (ret != 0) {
			atomic_inc(ctx->errors);
			LOG_ERR("Alarm cancel failed: %d", ret);
		}
		break;
	}

	ctx->operations++;

	if (last) {
		return false;
	}

	if ((cnt % 10U) == 0U) {
		k_yield();
	}

	return true;
}

/* Ztress Alarm callback for stress tests */
static void alarm_fired_callback(const struct device *dev, uint16_t id, void *user_data)
{
	atomic_t *counter = (atomic_t *)user_data;

	atomic_inc(counter);
	LOG_DBG("Alarm callback fired, count: %ld", atomic_get(counter));
}

/* Ztress handler: Alarm callback registration */
static bool ztress_callback_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct rtc_ztress_context *ctx = (struct rtc_ztress_context *)user_data;
	uint16_t supported_mask = 0U;
	int ret = 0;

	/* Cycle through operations */
	switch (cnt % 3U) {
	case 0:
		ret = rtc_alarm_get_supported_fields(ctx->dev, ALARM_ID, &supported_mask);
		if (ret != 0) {
			atomic_inc(ctx->errors);
			LOG_ERR("Failed to get supported fields: %d", ret);
		} else {
			/* Validate the mask is non-zero and contains expected fields */
			uint16_t expected = RTC_ALARM_TIME_MASK_SECOND |
					    RTC_ALARM_TIME_MASK_MINUTE | RTC_ALARM_TIME_MASK_HOUR |
					    RTC_ALARM_TIME_MASK_MONTHDAY |
					    RTC_ALARM_TIME_MASK_MONTH | RTC_ALARM_TIME_MASK_YEAR;

			if (supported_mask != expected) {
				atomic_inc(ctx->errors);
				LOG_ERR("Invalid supported mask: 0x%x (expected 0x%x)",
					supported_mask, expected);
			}
		}
		break;

	case 1:
		ret = rtc_alarm_set_callback(ctx->dev, ALARM_ID, alarm_fired_callback,
					     ctx->alarm_fired);
		if (ret != 0) {
			atomic_inc(ctx->errors);
			LOG_ERR("Callback set failed: %d", ret);
		}
		break;

	case 2:
		ret = rtc_alarm_set_callback(ctx->dev, ALARM_ID, NULL, NULL);
		if (ret != 0) {
			atomic_inc(ctx->errors);
			LOG_ERR("Callback clear failed: %d", ret);
		}
		break;
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
static uint32_t get_total_operations(struct rtc_ztress_context *ctx, uint8_t count)
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
	atomic_val_t errors = atomic_get(&error_counter);
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

/* Helper: Initialize RTC with base time */
static void init_rtc_time(const struct device *dev)
{
	struct rtc_time time = {0};
	int ret = 0;

	time.tm_year = BASE_YEAR;
	time.tm_mon = BASE_MONTH;
	time.tm_mday = BASE_DAY;
	time.tm_isdst = -1;

	ret = rtc_set_time(dev, &time);
	zassert_equal(ret, 0, "Failed to initialize RTC time: %d", ret);
}

/* Helper: Execute RTC cleanup operations */
static void rtc_cleanup(struct rtc_stress_fixture *fixture)
{
	atomic_set(&error_counter, 0);
	atomic_set(&alarm_counter, 0);
	rtc_alarm_set_time(fixture->dev, ALARM_ID, 0, NULL);
	rtc_alarm_set_callback(fixture->dev, ALARM_ID, NULL, NULL);
	init_rtc_time(fixture->dev);
}

/* Test setup */
static void *rtc_stress_setup(void)
{
	static struct rtc_stress_fixture fixture = {0};

	fixture.dev = DEVICE_DT_GET(DT_ALIAS(rtc));
	zassert_true(device_is_ready(fixture.dev), "RTC device not ready");

	return &fixture;
}

/* Before each test */
static void rtc_stress_before(void *f)
{
	rtc_cleanup((struct rtc_stress_fixture *)f);
}

/* After each test */
static void rtc_stress_teardown(void *f)
{
	rtc_cleanup((struct rtc_stress_fixture *)f);
}

/* Test: Concurrent operations */
ZTEST_F(rtc_stress, test_concurrent_operations)
{
	const uint8_t concurrent_operations = 4U;
	struct rtc_ztress_context ctx[4] = {{.dev = fixture->dev,
					     .errors = &error_counter,
					     .alarm_fired = &alarm_counter,
					     .operations = 0},
					    {.dev = fixture->dev,
					     .errors = &error_counter,
					     .alarm_fired = &alarm_counter,
					     .operations = 0},
					    {.dev = fixture->dev,
					     .errors = &error_counter,
					     .alarm_fired = &alarm_counter,
					     .operations = 0},
					    {.dev = fixture->dev,
					     .errors = &error_counter,
					     .alarm_fired = &alarm_counter,
					     .operations = 0}};
	uint32_t total_ops = 0U;
	int64_t start_time = 0, elapsed_time = 0;
	const uint32_t min_expected = ZTRESS_ITERATIONS * concurrent_operations;

	LOG_INF("Starting RTC concurrent operations test");

	ztress_set_timeout(ZTRESS_TIMEOUT);
	start_time = k_uptime_get();

	ZTRESS_EXECUTE(ZTRESS_THREAD(ztress_set_time_handler, &ctx[0], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
		       ZTRESS_THREAD(ztress_get_time_handler, &ctx[1], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
		       ZTRESS_THREAD(ztress_alarm_handler, &ctx[2], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
		       ZTRESS_THREAD(ztress_callback_handler, &ctx[3], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)));

	elapsed_time = k_uptime_get() - start_time;
	total_ops = get_total_operations(ctx, concurrent_operations);

	LOG_INF("Concurrent test completed:");
	LOG_INF("  Set time operations: %u", ctx[0].operations);
	LOG_INF("  Get time operations: %u", ctx[1].operations);
	LOG_INF("  Alarm operations: %u", ctx[2].operations);
	LOG_INF("  Callback operations: %u", ctx[3].operations);
	LOG_INF("  Total errors: %ld", atomic_get(&error_counter));

	check_errors();
	check_performance(total_ops, elapsed_time, min_expected,
			  k_ticks_to_ms_floor32(ZTRESS_TIMEOUT.ticks));
}

/* Test: Endurance test with time accuracy validation */
ZTEST_F(rtc_stress, test_endurance)
{
	const uint8_t concurrent_operations = 3U;
	struct rtc_ztress_context ctx[3] = {{.dev = fixture->dev,
					     .errors = &error_counter,
					     .alarm_fired = &alarm_counter,
					     .operations = 0U},
					    {.dev = fixture->dev,
					     .errors = &error_counter,
					     .alarm_fired = &alarm_counter,
					     .operations = 0U},
					    {.dev = fixture->dev,
					     .errors = &error_counter,
					     .alarm_fired = &alarm_counter,
					     .operations = 0U}};
	uint32_t total_ops = 0U;
	int64_t start_time = 0, elapsed_time = 0;
	const uint32_t min_expected = ZTRESS_ENDURANCE_ITERATIONS * concurrent_operations;

	LOG_INF("Starting RTC endurance test");

	ztress_set_timeout(ZTRESS_ENDURANCE_TIMEOUT);
	start_time = k_uptime_get();

	ZTRESS_EXECUTE(ZTRESS_THREAD(ztress_set_time_handler, &ctx[0], ZTRESS_ENDURANCE_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
		       ZTRESS_THREAD(ztress_get_time_handler, &ctx[1], ZTRESS_ENDURANCE_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
		       ZTRESS_THREAD(ztress_alarm_handler, &ctx[2], ZTRESS_ENDURANCE_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)));

	elapsed_time = k_uptime_get() - start_time;
	total_ops = get_total_operations(ctx, concurrent_operations);

	LOG_INF("Endurance test completed:");
	LOG_INF("  Set time operations: %u", ctx[0].operations);
	LOG_INF("  Get time operations: %u", ctx[1].operations);
	LOG_INF("  Alarm operations: %u", ctx[2].operations);
	LOG_INF("  Total errors: %ld", atomic_get(&error_counter));
	LOG_INF("  Duration: %lld ms", elapsed_time);

	check_errors();
	check_performance(total_ops, elapsed_time, min_expected,
			  k_ticks_to_ms_floor32(ZTRESS_ENDURANCE_TIMEOUT.ticks));
}

ZTEST_SUITE(rtc_stress, NULL, rtc_stress_setup, rtc_stress_before, rtc_stress_teardown, NULL);
