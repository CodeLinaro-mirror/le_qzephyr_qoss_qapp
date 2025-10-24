/*
 * Copyright (c) 2025 Qualcomm Technologies, Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * UART Power Management API Test
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/ztest.h>
#include <zephyr/pm/device.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(uart_pm_test, LOG_LEVEL_INF);

static const struct device *const uart_dev = DEVICE_DT_GET_ONE(qcom_qcc730_uart);

ZTEST(uart_pm_api_test, test_multiple_suspend_resume_cycles)
{
	int ret;
	struct uart_config cfg = {0};

	zassert_true(device_is_ready(uart_dev), "UART device is not ready");
	zassert_ok(uart_config_get(uart_dev, &cfg), "uart_config_get failed");

	/**
	 * Sleep time added in the test after uart functions because the serial terminal didn't get
	 * all characters otherwise
	 */

	k_msleep(5);

	for (int i = 0; i < 5; i++) {
		LOG_INF("Cycle %d start", i + 1);

		/* No logging between suspend and resume */
		ret = pm_device_action_run(uart_dev, PM_DEVICE_ACTION_SUSPEND);
		zassert_equal(ret, 0, "Suspend failed on cycle %d", i + 1);
		k_msleep(10);

		ret = pm_device_action_run(uart_dev, PM_DEVICE_ACTION_RESUME);
		zassert_equal(ret, 0, "Resume failed on cycle %d", i + 1);
		k_msleep(15);

		/* Verify after resume */
		ret = uart_configure(uart_dev, &cfg);
		zassert_equal(ret, 0, "Configure failed after cycle %d", i + 1);
		k_msleep(5);

		LOG_INF("Cycle %d complete", i + 1);
	}
}

ZTEST(uart_pm_api_test, test_suspend_resume_success)
{
	int ret;

	zassert_true(device_is_ready(uart_dev), "UART device is not ready");

	struct uart_config cfg = {0};
	ret = uart_config_get(uart_dev, &cfg);
	zassert_ok(ret, "uart_config_get failed before suspend");

	/**
	 * Sleep time added in the test after uart functions because the serial terminal didn't get
	 * all characters otherwise
	 */
	k_msleep(10);

	LOG_INF("Suspending UART device...");
	ret = pm_device_action_run(uart_dev, PM_DEVICE_ACTION_SUSPEND);
	zassert_equal(ret, 0, "Suspend action failed with ret=%d", ret);
	k_msleep(10);
	LOG_INF("Suspending UART device...");

	/* NOT logging while suspended. Collect results only. */
	enum pm_device_state state;
	int ret_cfg_during_suspend;
	int ret_poll_in_during_suspend;
	unsigned char ch = 0;

	(void)pm_device_state_get(uart_dev, &state);
	ret_cfg_during_suspend = uart_configure(uart_dev, &cfg);
	k_msleep(10);
	ret_poll_in_during_suspend = uart_poll_in(uart_dev, &ch);
	k_msleep(10);

	/* Resume, then assert/log */
	ret = pm_device_action_run(uart_dev, PM_DEVICE_ACTION_RESUME);
	zassert_equal(ret, 0, "Resume action failed with ret=%d", ret);
	k_msleep(20);

	zassert_equal(state, PM_DEVICE_STATE_SUSPENDED, "Device not in suspended state");
	zassert_equal(ret_cfg_during_suspend, -EBUSY,
		      "uart_configure during suspend should return -EBUSY, got %d",
		      ret_cfg_during_suspend);
	zassert_equal(ret_poll_in_during_suspend, -EBUSY,
		      "uart_poll_in during suspend should return -EBUSY, got %d",
		      ret_poll_in_during_suspend);

	/* After resume, ops should work again */
	ret = uart_configure(uart_dev, &cfg);
	zassert_equal(ret, 0, "uart_configure after resume failed with ret=%d", ret);
	k_msleep(10);

	int ret_poll_after = uart_poll_in(uart_dev, &ch);
	k_msleep(10);
	zassert_true(ret_poll_after == 0 || ret_poll_after == -ENODATA,
		     "uart_poll_in after resume unexpected ret=%d", ret_poll_after);

	LOG_INF("UART PM test passed");
}

static void *uart_pm_test_setup(void)
{
	zassert_true(device_is_ready(uart_dev), "UART device is not ready");
	LOG_INF("UART PM test suite setup complete");
	return NULL;
}

static void uart_pm_test_before(void *fixture)
{
	int ret;
	enum pm_device_state state;

	ARG_UNUSED(fixture);

	/* Ensure device is in active state before each test */
	ret = pm_device_state_get(uart_dev, &state);
	if (ret == 0 && state == PM_DEVICE_STATE_SUSPENDED) {
		pm_device_action_run(uart_dev, PM_DEVICE_ACTION_RESUME);
		k_msleep(15);
	}
}

ZTEST_SUITE(uart_pm_api_test, NULL, uart_pm_test_setup, uart_pm_test_before, NULL, NULL);