/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/util.h>

/* Build a reset_dt_spec from a node label's resets phandle */
#define RESET_SPEC_OR_NULL(_nodelabel)                                                             \
	COND_CODE_1(DT_NODE_EXISTS(DT_NODELABEL(_nodelabel)),               \
		    (RESET_DT_SPEC_GET(DT_NODELABEL(_nodelabel))),   \
		    ({ .dev = NULL, .id = 0U; }))

/* Peripherals under test */
static const struct reset_dt_spec spec_gpioa = RESET_SPEC_OR_NULL(gpioa);
static const struct reset_dt_spec spec_i2c0 = RESET_SPEC_OR_NULL(i2c0);
static const struct reset_dt_spec spec_uart0 = RESET_SPEC_OR_NULL(uart0);
static const struct reset_dt_spec spec_qtimer = RESET_SPEC_OR_NULL(qtimer);

/* Test case for all peripherals */
static void reset_test_case(const struct reset_dt_spec *spec, const char *name)
{
	int ret_assert = 0, ret_deassert = 0, ret_toggle = 0, ret_status_assert = 0,
	    ret_status_deassert = 0, ret_status_toggle = 0;
	uint8_t status_assert = 0U, status_deassert = 0U, status_toggle = 0U;
	const struct device *uart0_dev = NULL;
	struct uart_config uart_cfg = {0};

	if (spec->dev == NULL) {
		TC_PRINT("SKIP %-8s: node label not present in DTS\n", name);
		ztest_test_skip();
	}

	if (!device_is_ready(spec->dev)) {
		ztest_test_skip();
	}

	/* 1) assert -> status=1 */
	ret_assert = reset_line_assert(spec->dev, spec->id);
	ret_status_assert = reset_status(spec->dev, spec->id, &status_assert);

	/* 2) deassert -> status=0 */
	ret_deassert = reset_line_deassert(spec->dev, spec->id);
	ret_status_deassert = reset_status(spec->dev, spec->id, &status_deassert);

	/* 3) toggle (assert+deassert) -> status=0 */
	ret_toggle = reset_line_toggle(spec->dev, spec->id);
	ret_status_toggle = reset_status(spec->dev, spec->id, &status_toggle);

	if (strcmp(name, "uart0") == 0) {
		uart0_dev = DEVICE_DT_GET(DT_NODELABEL(uart0));
		uart_cfg.baudrate = DT_PROP(DT_NODELABEL(uart0), current_speed);
		uart_cfg.parity = UART_CFG_PARITY_NONE;
		uart_cfg.stop_bits = UART_CFG_STOP_BITS_1;
		uart_cfg.data_bits = UART_CFG_DATA_BITS_8;
		(void)uart_configure(uart0_dev, &uart_cfg);
	}

	zassert_ok(ret_assert, "%s: assert failed (%d)", name, ret_assert);
	zassert_ok(ret_status_assert, "%s: status(read) after assert failed (%d)", name,
		   ret_status_assert);
	zassert_equal(status_assert, 1U, "%s: expected status=1 after assert, got %u", name,
		      status_assert);
	zassert_ok(ret_deassert, "%s: deassert failed (%d)", name, ret_deassert);
	zassert_ok(ret_status_deassert, "%s: status(read) after deassert failed (%d)", name,
		   ret_status_deassert);
	zassert_equal(status_deassert, 0U, "%s: expected status=0 after deassert, got %u", name,
		      status_deassert);
	zassert_ok(ret_toggle, "%s: toggle failed (%d)", name, ret_toggle);
	zassert_ok(ret_status_toggle, "%s: status(read) after toggle failed (%d)", name,
		   ret_status_toggle);
	zassert_equal(status_toggle, 0U, "%s: expected status=0 after toggle, got %u", name,
		      status_toggle);
}

ZTEST(qcc730_reset, test_gpioa_reset)
{
	reset_test_case(&spec_gpioa, "gpioa");
}

ZTEST(qcc730_reset, test_i2c0_reset)
{
	reset_test_case(&spec_i2c0, "i2c0");
}

ZTEST(qcc730_reset, test_uart0_reset)
{
	reset_test_case(&spec_uart0, "uart0");
}

ZTEST(qcc730_reset, test_qtimer_reset)
{
	reset_test_case(&spec_qtimer, "qtimer");
}

ZTEST(qcc730_reset, test_invalid_id)
{
	int ret = 0;
	/* Pick any available device */
	const struct device *ctrl = spec_gpioa.dev    ? spec_gpioa.dev
				    : spec_i2c0.dev   ? spec_i2c0.dev
				    : spec_uart0.dev  ? spec_uart0.dev
				    : spec_qtimer.dev ? spec_qtimer.dev
						      : NULL;

	if (!ctrl) {
		ztest_test_skip();
	}

	/* Test if bad id will be rejected*/
	uint32_t bad_id = (2U << 5U) | 0U;

	ret = reset_status(ctrl, bad_id, (uint8_t[1]){0});
	zassert_equal(ret, -EINVAL, "status on invalid ID should be -EINVAL, got %d", ret);

	ret = reset_line_assert(ctrl, bad_id);
	zassert_equal(ret, -EINVAL, "assert on invalid ID should be -EINVAL, got %d", ret);

	ret = reset_line_deassert(ctrl, bad_id);
	zassert_equal(ret, -EINVAL, "deassert on invalid ID should be -EINVAL, got %d", ret);
}

ZTEST_SUITE(qcc730_reset, NULL, NULL, NULL, NULL, NULL);
