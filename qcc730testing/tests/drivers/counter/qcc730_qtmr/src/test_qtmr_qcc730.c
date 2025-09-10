/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/drivers/counter/counter_qcc730_qtmr.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(test_qtmr, CONFIG_COUNTER_LOG_LEVEL);

#define SHORT_TIMEOUT_SEC      2U
#define MED_TIMEOUT_SEC        12U
#define LONG_TIMEOUT_SEC       20U
#define ALLOWED_MARGIN_MS(sec) ((sec) * 1000U * 0.01) // 1%

static struct k_sem alarm_sem;

static void alarm_callback(const struct device *dev, uint8_t chan_id, uint32_t ticks,
			   void *user_data)
{
	k_sem_give(&alarm_sem);
}

static void test_start_stop(const struct device *dev)
{
	int ret;

	TC_PRINT("Testing start/stop on: %s\n", dev->name);

	ret = counter_start(dev);
	zassert_ok(ret, "counter_start() failed: %d", ret);

	ret = counter_stop(dev);
	zassert_ok(ret, "counter_stop() failed: %d", ret);

	TC_PRINT("======================\n");
}

static void test_get_value_64(const struct device *dev)
{
	uint64_t val1, val2;

	TC_PRINT("Testing get_value_64 on: %s\n", dev->name);

	counter_start(dev);
	zassert_ok(counter_get_value_64(dev, &val1), "Failed to get value");
	k_sleep(K_MSEC(10));
	zassert_ok(counter_get_value_64(dev, &val2), "Failed to get value");

	zassert_true(val2 > val1, "Timer is not counting");

	counter_stop(dev);

	TC_PRINT("======================\n");
}

static void test_alarm(const struct device *dev, uint32_t sec)
{
	uint64_t now;
	uint64_t ticks = (uint64_t)sec * counter_get_frequency(dev);
	struct counter_alarm_cfg alarm_cfg = {
		.flags = 0,
		.ticks = (uint32_t)ticks,
		.callback = alarm_callback,
		.user_data = NULL,
	};

	TC_PRINT("Testing alarm on: %s\n", dev->name);

	k_sem_reset(&alarm_sem);

	TC_PRINT("Setting alarm for %u sec (%llu ticks) on %s\n", sec, ticks, dev->name);

	uint64_t start_ms = k_uptime_get();
	counter_start(dev);
	if (sec != LONG_TIMEOUT_SEC) {
		zassert_ok(counter_set_channel_alarm(dev, 0, &alarm_cfg),
			   "Failed to set relative alarm");
	} else {
		counter_get_value_64(dev, &now);
		ticks += now;
		alarm_cfg.flags = COUNTER_ALARM_CFG_ABSOLUTE;
		zassert_ok(qtmr_qcc730_set_alarm_absolute(dev, 0, &alarm_cfg, ticks),
			   "Failed to set absolute alarm");
	}

	if (sec >= MED_TIMEOUT_SEC) {
		TC_PRINT("Waiting up to %u sec for alarm to fire...\n", sec);
	}

	// Wait for the alarm to fire
	zassert_ok(k_sem_take(&alarm_sem, K_SECONDS(sec + 2)), // +2 seconds margin
		   "Alarm for %u sec did not fire", sec);

	uint64_t end_ms = k_uptime_get();
	uint64_t elapsed = end_ms - start_ms;
	uint64_t expected = (uint64_t)sec * 1000;
	uint64_t margin = ALLOWED_MARGIN_MS(sec);

	TC_PRINT("Alarm fired on %s after %llu ms (expected %llu ms ±%llu ms)\n", dev->name,
		 elapsed, expected, margin);

	zassert_true(elapsed >= expected - margin && elapsed <= expected + margin,
		     "Alarm fired outside allowed margin: got %llu ms, expected %llu ±%llu ms",
		     elapsed, expected, margin);

	counter_stop(dev);

	TC_PRINT("======================\n");
}

static void test_error_handling(const struct device *dev)
{
	uint64_t now;
	struct counter_alarm_cfg alarm_cfg = {
		.flags = COUNTER_ALARM_CFG_ABSOLUTE,
		.ticks = 1000,
		.callback = alarm_callback,
	};

	TC_PRINT("Testing error handling on: %s\n", dev->name);

	counter_start(dev);

	// Try setting a relative alarm with ABSOLUTE flag -> should fail
	zassert_equal(counter_set_channel_alarm(dev, 0, &alarm_cfg), -ENOTSUP,
		      "Expected -ENOTSUP for absolute flag in set_alarm");

	// Try setting absolute alarm with timestamp in the past
	counter_get_value_64(dev, &now);
	zassert_equal(qtmr_qcc730_set_alarm_absolute(dev, 0, &alarm_cfg, now - 1), -EINVAL,
		      "Expected -EINVAL for past alarm time");

	// Try setting absolute alarm with timestamp exceeding the maximum value;
	zassert_equal(qtmr_qcc730_set_alarm_absolute(dev, 0, &alarm_cfg, BIT64_MASK(56) + 1),
		      -EINVAL, "Expected -EINVAL for past alarm time");

	// Set a valid alarm
	counter_get_value_64(dev, &now);
	uint64_t future_ticks = now + SHORT_TIMEOUT_SEC * counter_get_frequency(dev);
	k_sem_reset(&alarm_sem);
	zassert_ok(qtmr_qcc730_set_alarm_absolute(dev, 0, &alarm_cfg, future_ticks),
		   "Failed to set first alarm");

	// Try setting a second alarm without cancelling
	counter_get_value_64(dev, &now);
	future_ticks = now + SHORT_TIMEOUT_SEC * counter_get_frequency(dev);
	zassert_equal(qtmr_qcc730_set_alarm_absolute(dev, 0, &alarm_cfg, future_ticks), -EBUSY,
		      "Expected -EBUSY for setting second alarm");

	// Cancel alarm, then try NULL callback
	zassert_ok(counter_cancel_channel_alarm(dev, 0), "Failed to cancel alarm");

	alarm_cfg.callback = NULL;
	alarm_cfg.flags = 0;
	alarm_cfg.ticks = SHORT_TIMEOUT_SEC * counter_get_frequency(dev);
	zassert_equal(counter_set_channel_alarm(dev, 0, &alarm_cfg), 0,
		      "Alarm with NULL callback should succeed, but not crash");

	counter_stop(dev);

	TC_PRINT("======================\n");
}

void setup_alarm_sem(void *fixture)
{
	k_sem_init(&alarm_sem, 0, 1);
}

ZTEST(qtmr_qcc730, test_frame0)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(frame0));
	zassert_true(device_is_ready(dev), "Device not ready");
	test_start_stop(dev);
	test_get_value_64(dev);
	test_alarm(dev, SHORT_TIMEOUT_SEC);
	test_alarm(dev, MED_TIMEOUT_SEC);
	test_alarm(dev, LONG_TIMEOUT_SEC);
	test_error_handling(dev);
}

ZTEST(qtmr_qcc730, test_frame1)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(frame1));
	zassert_true(device_is_ready(dev), "Device not ready");
	test_start_stop(dev);
	test_get_value_64(dev);
	test_alarm(dev, SHORT_TIMEOUT_SEC);
	test_alarm(dev, MED_TIMEOUT_SEC);
	test_alarm(dev, LONG_TIMEOUT_SEC);
	test_error_handling(dev);
}

ZTEST(qtmr_qcc730, test_frame2)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(frame2));
	zassert_true(device_is_ready(dev), "Device not ready");
	test_start_stop(dev);
	test_get_value_64(dev);
	test_alarm(dev, SHORT_TIMEOUT_SEC);
	test_alarm(dev, MED_TIMEOUT_SEC);
	test_alarm(dev, LONG_TIMEOUT_SEC);
	test_error_handling(dev);
}

ZTEST(qtmr_qcc730, test_frame3)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(frame3));
	zassert_true(device_is_ready(dev), "Device not ready");
	test_start_stop(dev);
	test_get_value_64(dev);
	test_alarm(dev, SHORT_TIMEOUT_SEC);
	test_alarm(dev, MED_TIMEOUT_SEC);
	test_alarm(dev, LONG_TIMEOUT_SEC);
	test_error_handling(dev);
}

ZTEST(qtmr_qcc730, test_frame4)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(frame4));
	zassert_true(device_is_ready(dev), "Device not ready");
	test_start_stop(dev);
	test_get_value_64(dev);
	test_alarm(dev, SHORT_TIMEOUT_SEC);
	test_alarm(dev, MED_TIMEOUT_SEC);
	test_alarm(dev, LONG_TIMEOUT_SEC);
	test_error_handling(dev);
}

ZTEST_SUITE(qtmr_qcc730, NULL, NULL, setup_alarm_sem, NULL, NULL);
