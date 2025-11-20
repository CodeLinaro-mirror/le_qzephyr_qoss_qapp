/*
 * Copyright (c) 2025 Qualcomm Technologies, Inc. and/or its subsidiaries
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/ztest.h>
#include <zephyr/dt-bindings/pinctrl/qcom-qcc730-pinctrl.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/devicetree.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include "soc.h"

LOG_MODULE_REGISTER(uart_pinctrl_test, LOG_LEVEL_INF);

/* On QCC730, pinctrl_soc_pin_t is a 32-bit encoding */

static const struct device *const uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart0));

/* Pin control config pointer for uart0 device for pinctrl_apply_state() tests */
#define UART0_NODE DT_NODELABEL(uart0)
PINCTRL_DT_DEV_CONFIG_DECLARE(UART0_NODE);
static struct pinctrl_dev_config *const uart_pcfg = PINCTRL_DT_DEV_CONFIG_GET(UART0_NODE);

/**
 * @brief Test UART pinctrl option register changes
 *
 * This test verifies that when pinctrl is configured with different UART
 * options, the PMU_BOOT_STRAP_CONFIGURATION_STATUS.CFG_UART_OPTION register is
 * correctly set. This validates the UART driver's integration with the pinctrl
 * subsystem by using the actual UART device's pinctrl configuration from DT.
 */

/*
 * Pull pin arrays for each UART option from Devicetree via zephyr,user
 * using dynamic pinctrl helpers. This avoids adding extra pinctrl states
 * to the uart0 device while still reusing the SoC's pin group definitions.
 */
PINCTRL_DT_STATE_PINS_DEFINE(DT_PATH(zephyr_user), uart0_option0);
PINCTRL_DT_STATE_PINS_DEFINE(DT_PATH(zephyr_user), uart0_option1);
PINCTRL_DT_STATE_PINS_DEFINE(DT_PATH(zephyr_user), uart0_option2);
PINCTRL_DT_STATE_PINS_DEFINE(DT_PATH(zephyr_user), uart0_option3);
PINCTRL_DT_STATE_PINS_DEFINE(DT_PATH(zephyr_user), uart0_option0_sleep);
PINCTRL_DT_STATE_PINS_DEFINE(DT_PATH(zephyr_user), uart0_option1_sleep);
PINCTRL_DT_STATE_PINS_DEFINE(DT_PATH(zephyr_user), uart0_option2_sleep);
PINCTRL_DT_STATE_PINS_DEFINE(DT_PATH(zephyr_user), uart0_option3_sleep);

/* On QCC730, pinctrl_soc_pin_t is a 32-bit encoding */

static void log_pin_encodings(const char *label, const pinctrl_soc_pin_t *pins, size_t cnt)
{
	LOG_INF("DT pins for %s: count=%u", label, (unsigned int)cnt);
	for (size_t i = 0; i < cnt; ++i) {
		uint32_t u = (uint32_t)pins[i];
		LOG_INF("  pin[%u]: pin=%u func=%u mode=%u uart_opt=%u pu=%u pd=%u ds=%u",
			(unsigned int)i, (unsigned int)QCOM_PINMUX_GET_PIN_NUM(u),
			(unsigned int)QCOM_PINMUX_GET_FUNC_SEL(u),
			(unsigned int)QCOM_PINMUX_GET_MODE(u),
			(unsigned int)QCOM_PINMUX_GET_UART_OPT(u),
			(unsigned int)QCOM_PINMUX_GET_PULL_UP(u),
			(unsigned int)QCOM_PINMUX_GET_PULL_DOWN(u),
			(unsigned int)QCOM_PINMUX_GET_DRIVER_STRENGTH(u));
	}
}

static void assert_pin_encodings(const pinctrl_soc_pin_t *pins, size_t cnt, uint8_t expected_opt,
				 uint8_t expected_gpio_a, uint8_t expected_gpio_b)
{
	/* Expect exactly two pins: TX and RX */
	zassert_true(cnt == 2U, "Expected 2 pins, got %u", (unsigned int)cnt);

	bool seen_a = false;
	bool seen_b = false;

	for (size_t i = 0; i < cnt; ++i) {
		uint32_t u = (uint32_t)pins[i];

		/* Function and mode derived from DTS must be UART / PERIPHERAL */
		zassert_equal(QCOM_PINMUX_GET_FUNC_SEL(u), QCC730_FUNC_UART,
			      "FUNC_SEL not UART: got %u", QCOM_PINMUX_GET_FUNC_SEL(u));
		zassert_equal(QCOM_PINMUX_GET_MODE(u), QCOM_PIN_MODE_PERIPHERAL,
			      "MODE not PERIPHERAL: got %u", QCOM_PINMUX_GET_MODE(u));

		/* UART option embedded in the pin encoding must match expected */
		zassert_equal(QCOM_PINMUX_GET_UART_OPT(u), expected_opt,
			      "UART_OPT mismatch: expected %u got %u", expected_opt,
			      QCOM_PINMUX_GET_UART_OPT(u));

		/* Default groups specify bias-pull-up and high-drive-strength */
		zassert_equal(QCOM_PINMUX_GET_PULL_UP(u), 1U, "PULL_UP expected 1, got %u",
			      QCOM_PINMUX_GET_PULL_UP(u));
		zassert_equal(QCOM_PINMUX_GET_PULL_DOWN(u), 0U, "PULL_DOWN expected 0, got %u",
			      QCOM_PINMUX_GET_PULL_DOWN(u));
		zassert_equal(QCOM_PINMUX_GET_DRIVER_STRENGTH(u), 1U,
			      "DRIVE_STRENGTH expected 1, got %u",
			      QCOM_PINMUX_GET_DRIVER_STRENGTH(u));

		uint32_t pin_num = QCOM_PINMUX_GET_PIN_NUM(u);
		if (pin_num == expected_gpio_a) {
			seen_a = true;
		} else if (pin_num == expected_gpio_b) {
			seen_b = true;
		}
	}

	zassert_true(seen_a && seen_b, "Unexpected GPIOs in pins: expected %u and %u",
		     expected_gpio_a, expected_gpio_b);
}

/*
 * Assert helper for UART sleep pin groups: expects two pins with
 * UART func, PERIPHERAL mode, the given UART option, and sleep
 * semantics (pull-up=0, pull-down=0, drive-strength=0) and the
 * expected GPIO pair.
 */
static void assert_sleep_pin_encodings(const pinctrl_soc_pin_t *pins, size_t cnt,
				       uint8_t expected_opt, uint8_t expected_gpio_a,
				       uint8_t expected_gpio_b)
{
	zassert_true(cnt == 2U, "Expected 2 pins, got %u", (unsigned int)cnt);

	bool seen_a = false;
	bool seen_b = false;

	for (size_t i = 0; i < cnt; ++i) {
		uint32_t u = (uint32_t)pins[i];

		zassert_equal(QCOM_PINMUX_GET_FUNC_SEL(u), QCC730_FUNC_UART,
			      "FUNC_SEL not UART: got %u", QCOM_PINMUX_GET_FUNC_SEL(u));
		zassert_equal(QCOM_PINMUX_GET_MODE(u), QCOM_PIN_MODE_PERIPHERAL,
			      "MODE not PERIPHERAL: got %u", QCOM_PINMUX_GET_MODE(u));
		zassert_equal(QCOM_PINMUX_GET_UART_OPT(u), expected_opt,
			      "UART_OPT mismatch: expected %u got %u", expected_opt,
			      QCOM_PINMUX_GET_UART_OPT(u));

		/* Sleep groups: bias-disable -> no pulls, low drive */
		zassert_equal(QCOM_PINMUX_GET_PULL_UP(u), 0U, "PULL_UP expected 0, got %u",
			      QCOM_PINMUX_GET_PULL_UP(u));
		zassert_equal(QCOM_PINMUX_GET_PULL_DOWN(u), 0U, "PULL_DOWN expected 0, got %u",
			      QCOM_PINMUX_GET_PULL_DOWN(u));
		zassert_equal(QCOM_PINMUX_GET_DRIVER_STRENGTH(u), 0U,
			      "DRIVE_STRENGTH expected 0, got %u",
			      QCOM_PINMUX_GET_DRIVER_STRENGTH(u));

		uint32_t pin_num = QCOM_PINMUX_GET_PIN_NUM(u);
		if (pin_num == expected_gpio_a) {
			seen_a = true;
		} else if (pin_num == expected_gpio_b) {
			seen_b = true;
		}
	}

	zassert_true(seen_a && seen_b, "Unexpected GPIOs in pins: expected %u and %u",
		     expected_gpio_a, expected_gpio_b);
}

/*
 * Sample PU/PD/DS PMU registers for a GPIO pair without asserting/logging.
 * Use the captured values later (after restoring console) to run asserts.
 */
typedef struct {
	uint8_t pu_a, pu_b;
	uint8_t pd_a, pd_b;
	uint8_t ds_a, ds_b;
} pmu_bits_t;

static pmu_bits_t get_pmu_pu_pd_ds_bits(PMU_BASE_pmu_Type *pmu, uint8_t gpio_a, uint8_t gpio_b)
{
	pmu_bits_t r;
	uint32_t pu = pmu->PMU_CFG_IOPAD_PU.reg;
	uint32_t pd = pmu->PMU_CFG_IOPAD_PD.reg;
	uint32_t ds = pmu->PMU_CFG_IOPAD_DS.reg;

	/*
	 * Each PMU_CFG_IOPAD_* register is a bitfield with one bit per GPIO index
	 * across GPIO0..GPIO14 (15 pins total on QCC730). We only sample the two
	 * pins relevant to the currently applied UART option in this helper. To
	 * read a specific pin's state, shift the register right by the pin number
	 * (gpio_a/gpio_b) and mask the least-significant bit.
	 */
	r.pu_a = (pu >> gpio_a) & 1U; /* pull-up bit for gpio_a */
	r.pu_b = (pu >> gpio_b) & 1U; /* pull-up bit for gpio_b */
	r.pd_a = (pd >> gpio_a) & 1U; /* pull-down bit for gpio_a */
	r.pd_b = (pd >> gpio_b) & 1U; /* pull-down bit for gpio_b */
	r.ds_a = (ds >> gpio_a) & 1U; /* drive-strength bit for gpio_a */
	r.ds_b = (ds >> gpio_b) & 1U; /* drive-strength bit for gpio_b */
	return r;
}

ZTEST(uart_pinctrl, test_uart_option_register_changes)
{
	int ret;
	PMU_BASE_pmu_Type *pmu = (PMU_BASE_pmu_Type *)DT_REG_ADDR(DT_NODELABEL(pmu));

	zassert_true(device_is_ready(uart_dev), "UART device is not ready");

	/* Variables to store register values */
	uint32_t cfg_uart_option_after_option0;
	uint32_t cfg_uart_option_after_option1;
	uint32_t cfg_uart_option_after_option2;
	uint32_t cfg_uart_option_after_option3;

	/* Capture PU/PD/DS observations per option (defer asserts until console restored) */
	pmu_bits_t bits_opt0, bits_opt1, bits_opt2, bits_opt3;

	/* Arrays uart0_optionX_pins[] are defined above by macros */

	/**
	 * Sleep time added in the test after uart functions because the serial terminal didn't get
	 * all characters otherwise
	 */
	k_msleep(10);

	LOG_INF("Testing UART pinctrl option changes...");

	/* Log DTS-derived pin encodings for each option */
	log_pin_encodings("uart0_option0", uart0_option0_pins, ARRAY_SIZE(uart0_option0_pins));
	log_pin_encodings("uart0_option1", uart0_option1_pins, ARRAY_SIZE(uart0_option1_pins));
	log_pin_encodings("uart0_option2", uart0_option2_pins, ARRAY_SIZE(uart0_option2_pins));
	log_pin_encodings("uart0_option3", uart0_option3_pins, ARRAY_SIZE(uart0_option3_pins));

	/* Sanity-check DTS-derived pin encodings for each option */
	assert_pin_encodings(uart0_option0_pins, ARRAY_SIZE(uart0_option0_pins),
			     QCC730_UART_OPTION_0, QCC730_GPIO_11, QCC730_GPIO_12);
	assert_pin_encodings(uart0_option1_pins, ARRAY_SIZE(uart0_option1_pins),
			     QCC730_UART_OPTION_1, QCC730_GPIO_14, QCC730_GPIO_13);
	assert_pin_encodings(uart0_option2_pins, ARRAY_SIZE(uart0_option2_pins),
			     QCC730_UART_OPTION_2, QCC730_GPIO_9, QCC730_GPIO_10);
	assert_pin_encodings(uart0_option3_pins, ARRAY_SIZE(uart0_option3_pins),
			     QCC730_UART_OPTION_3, QCC730_GPIO_3, QCC730_GPIO_1);

	/* NOT logging while changing pins. Collect results only. */

	/* Test UART Option 0 */
	ret = pinctrl_configure_pins(uart0_option0_pins, ARRAY_SIZE(uart0_option0_pins),
				     PINCTRL_REG_NONE);
	k_msleep(5);
	cfg_uart_option_after_option0 =
		pmu->PMU_BOOT_STRAP_CONFIGURATION_STATUS.bit.CFG_UART_OPTION;
	/* Sample PU/PD/DS for option0 (GPIO11, GPIO12) */
	bits_opt0 = get_pmu_pu_pd_ds_bits(pmu, QCC730_GPIO_11, QCC730_GPIO_12);

	/* Test UART Option 1 (board default) */
	ret = pinctrl_configure_pins(uart0_option1_pins, ARRAY_SIZE(uart0_option1_pins),
				     PINCTRL_REG_NONE);
	k_msleep(5);
	cfg_uart_option_after_option1 =
		pmu->PMU_BOOT_STRAP_CONFIGURATION_STATUS.bit.CFG_UART_OPTION;
	/* Sample PU/PD/DS for option1 (GPIO14, GPIO13) */
	bits_opt1 = get_pmu_pu_pd_ds_bits(pmu, QCC730_GPIO_14, QCC730_GPIO_13);

	/* Test UART Option 2 */
	ret = pinctrl_configure_pins(uart0_option2_pins, ARRAY_SIZE(uart0_option2_pins),
				     PINCTRL_REG_NONE);
	k_msleep(5);
	cfg_uart_option_after_option2 =
		pmu->PMU_BOOT_STRAP_CONFIGURATION_STATUS.bit.CFG_UART_OPTION;
	/* Sample PU/PD/DS for option2 (GPIO9, GPIO10) */
	bits_opt2 = get_pmu_pu_pd_ds_bits(pmu, QCC730_GPIO_9, QCC730_GPIO_10);

	/* Test UART Option 3 */
	ret = pinctrl_configure_pins(uart0_option3_pins, ARRAY_SIZE(uart0_option3_pins),
				     PINCTRL_REG_NONE);
	k_msleep(5);
	cfg_uart_option_after_option3 =
		pmu->PMU_BOOT_STRAP_CONFIGURATION_STATUS.bit.CFG_UART_OPTION;
	/* Sample PU/PD/DS for option3 (GPIO3, GPIO1) */
	bits_opt3 = get_pmu_pu_pd_ds_bits(pmu, QCC730_GPIO_3, QCC730_GPIO_1);

	/* Restore UART to option 1 (board default) */
	ret = pinctrl_configure_pins(uart0_option1_pins, ARRAY_SIZE(uart0_option1_pins),
				     PINCTRL_REG_NONE);
	k_msleep(20);

	/* Log the observed UART option register values (after restoring console) */
	LOG_INF("CFG_UART_OPTION observed: opt0=%u opt1=%u opt2=%u opt3=%u",
		(unsigned int)cfg_uart_option_after_option0,
		(unsigned int)cfg_uart_option_after_option1,
		(unsigned int)cfg_uart_option_after_option2,
		(unsigned int)cfg_uart_option_after_option3);

	/* Now assert after restoring console */
	zassert_equal(cfg_uart_option_after_option0, QCC730_UART_OPTION_0,
		      "CFG_UART_OPTION should be %d for option 0, got %d", QCC730_UART_OPTION_0,
		      cfg_uart_option_after_option0);

	zassert_equal(cfg_uart_option_after_option1, QCC730_UART_OPTION_1,
		      "CFG_UART_OPTION should be %d for option 1, got %d", QCC730_UART_OPTION_1,
		      cfg_uart_option_after_option1);

	zassert_equal(cfg_uart_option_after_option2, QCC730_UART_OPTION_2,
		      "CFG_UART_OPTION should be %d for option 2, got %d", QCC730_UART_OPTION_2,
		      cfg_uart_option_after_option2);

	zassert_equal(cfg_uart_option_after_option3, QCC730_UART_OPTION_3,
		      "CFG_UART_OPTION should be %d for option 3, got %d", QCC730_UART_OPTION_3,
		      cfg_uart_option_after_option3);

	/* Now assert PU/PD/DS bits for each option (after restoring console) */
	zassert_equal(bits_opt0.pu_a, 1U, "opt0 PU GPIO11 expected 1, got %u", bits_opt0.pu_a);
	zassert_equal(bits_opt0.pu_b, 1U, "opt0 PU GPIO12 expected 1, got %u", bits_opt0.pu_b);
	zassert_equal(bits_opt0.pd_a, 0U, "opt0 PD GPIO11 expected 0, got %u", bits_opt0.pd_a);
	zassert_equal(bits_opt0.pd_b, 0U, "opt0 PD GPIO12 expected 0, got %u", bits_opt0.pd_b);
	zassert_equal(bits_opt0.ds_a, 1U, "opt0 DS GPIO11 expected 1, got %u", bits_opt0.ds_a);
	zassert_equal(bits_opt0.ds_b, 1U, "opt0 DS GPIO12 expected 1, got %u", bits_opt0.ds_b);

	zassert_equal(bits_opt1.pu_a, 1U, "opt1 PU GPIO14 expected 1, got %u", bits_opt1.pu_a);
	zassert_equal(bits_opt1.pu_b, 1U, "opt1 PU GPIO13 expected 1, got %u", bits_opt1.pu_b);
	zassert_equal(bits_opt1.pd_a, 0U, "opt1 PD GPIO14 expected 0, got %u", bits_opt1.pd_a);
	zassert_equal(bits_opt1.pd_b, 0U, "opt1 PD GPIO13 expected 0, got %u", bits_opt1.pd_b);
	zassert_equal(bits_opt1.ds_a, 1U, "opt1 DS GPIO14 expected 1, got %u", bits_opt1.ds_a);
	zassert_equal(bits_opt1.ds_b, 1U, "opt1 DS GPIO13 expected 1, got %u", bits_opt1.ds_b);

	zassert_equal(bits_opt2.pu_a, 1U, "opt2 PU GPIO9 expected 1, got %u", bits_opt2.pu_a);
	zassert_equal(bits_opt2.pu_b, 1U, "opt2 PU GPIO10 expected 1, got %u", bits_opt2.pu_b);
	zassert_equal(bits_opt2.pd_a, 0U, "opt2 PD GPIO9 expected 0, got %u", bits_opt2.pd_a);
	zassert_equal(bits_opt2.pd_b, 0U, "opt2 PD GPIO10 expected 0, got %u", bits_opt2.pd_b);
	zassert_equal(bits_opt2.ds_a, 1U, "opt2 DS GPIO9 expected 1, got %u", bits_opt2.ds_a);
	zassert_equal(bits_opt2.ds_b, 1U, "opt2 DS GPIO10 expected 1, got %u", bits_opt2.ds_b);

	zassert_equal(bits_opt3.pu_a, 1U, "opt3 PU GPIO3 expected 1, got %u", bits_opt3.pu_a);
	zassert_equal(bits_opt3.pu_b, 1U, "opt3 PU GPIO1 expected 1, got %u", bits_opt3.pu_b);
	zassert_equal(bits_opt3.pd_a, 0U, "opt3 PD GPIO3 expected 0, got %u", bits_opt3.pd_a);
	zassert_equal(bits_opt3.pd_b, 0U, "opt3 PD GPIO1 expected 0, got %u", bits_opt3.pd_b);
	zassert_equal(bits_opt3.ds_a, 1U, "opt3 DS GPIO3 expected 1, got %u", bits_opt3.ds_a);
	zassert_equal(bits_opt3.ds_b, 1U, "opt3 DS GPIO1 expected 1, got %u", bits_opt3.ds_b);

	LOG_INF("UART pinctrl test passed");
}

/*
 * Validate that all four UART sleep option groups have correct encodings
 * and that applying them updates CFG_UART_OPTION accordingly. Restore to
 * option 1 at the end.
 */
ZTEST(uart_pinctrl, test_uart_sleep_option_register_changes)
{
	int ret;
	PMU_BASE_pmu_Type *pmu = (PMU_BASE_pmu_Type *)DT_REG_ADDR(DT_NODELABEL(pmu));

	zassert_true(device_is_ready(uart_dev), "UART device is not ready");

	uint32_t opt_after_sleep0, opt_after_sleep1, opt_after_sleep2, opt_after_sleep3;

	/* Capture PU/PD/DS observations per sleep option (defer asserts) */
	pmu_bits_t sbits0, sbits1, sbits2, sbits3;

	/* Log encodings */
	log_pin_encodings("uart0_option0_sleep", uart0_option0_sleep_pins,
			  ARRAY_SIZE(uart0_option0_sleep_pins));
	log_pin_encodings("uart0_option1_sleep", uart0_option1_sleep_pins,
			  ARRAY_SIZE(uart0_option1_sleep_pins));
	log_pin_encodings("uart0_option2_sleep", uart0_option2_sleep_pins,
			  ARRAY_SIZE(uart0_option2_sleep_pins));
	log_pin_encodings("uart0_option3_sleep", uart0_option3_sleep_pins,
			  ARRAY_SIZE(uart0_option3_sleep_pins));

	/* Assert encodings per sleep expectations: pu=0, pd=0, ds=0 */
	assert_sleep_pin_encodings(uart0_option0_sleep_pins, ARRAY_SIZE(uart0_option0_sleep_pins),
				   QCC730_UART_OPTION_0, QCC730_GPIO_11, QCC730_GPIO_12);
	assert_sleep_pin_encodings(uart0_option1_sleep_pins, ARRAY_SIZE(uart0_option1_sleep_pins),
				   QCC730_UART_OPTION_1, QCC730_GPIO_14, QCC730_GPIO_13);
	assert_sleep_pin_encodings(uart0_option2_sleep_pins, ARRAY_SIZE(uart0_option2_sleep_pins),
				   QCC730_UART_OPTION_2, QCC730_GPIO_9, QCC730_GPIO_10);
	assert_sleep_pin_encodings(uart0_option3_sleep_pins, ARRAY_SIZE(uart0_option3_sleep_pins),
				   QCC730_UART_OPTION_3, QCC730_GPIO_3, QCC730_GPIO_1);

	/* Apply each sleep option and sample CFG_UART_OPTION; do not log until restored */
	ret = pinctrl_configure_pins(uart0_option0_sleep_pins, ARRAY_SIZE(uart0_option0_sleep_pins),
				     PINCTRL_REG_NONE);
	k_msleep(5);
	opt_after_sleep0 = pmu->PMU_BOOT_STRAP_CONFIGURATION_STATUS.bit.CFG_UART_OPTION;
	/* Sample PU/PD/DS for sleep0 (GPIO11, GPIO12) */
	sbits0 = get_pmu_pu_pd_ds_bits(pmu, QCC730_GPIO_11, QCC730_GPIO_12);

	ret = pinctrl_configure_pins(uart0_option1_sleep_pins, ARRAY_SIZE(uart0_option1_sleep_pins),
				     PINCTRL_REG_NONE);
	k_msleep(5);
	opt_after_sleep1 = pmu->PMU_BOOT_STRAP_CONFIGURATION_STATUS.bit.CFG_UART_OPTION;
	/* Sample PU/PD/DS for sleep1 (GPIO14, GPIO13) */
	sbits1 = get_pmu_pu_pd_ds_bits(pmu, QCC730_GPIO_14, QCC730_GPIO_13);

	ret = pinctrl_configure_pins(uart0_option2_sleep_pins, ARRAY_SIZE(uart0_option2_sleep_pins),
				     PINCTRL_REG_NONE);
	k_msleep(5);
	opt_after_sleep2 = pmu->PMU_BOOT_STRAP_CONFIGURATION_STATUS.bit.CFG_UART_OPTION;
	/* Sample PU/PD/DS for sleep2 (GPIO9, GPIO10) */
	sbits2 = get_pmu_pu_pd_ds_bits(pmu, QCC730_GPIO_9, QCC730_GPIO_10);

	ret = pinctrl_configure_pins(uart0_option3_sleep_pins, ARRAY_SIZE(uart0_option3_sleep_pins),
				     PINCTRL_REG_NONE);
	k_msleep(5);
	opt_after_sleep3 = pmu->PMU_BOOT_STRAP_CONFIGURATION_STATUS.bit.CFG_UART_OPTION;
	/* Sample PU/PD/DS for sleep3 (GPIO3, GPIO1) */
	sbits3 = get_pmu_pu_pd_ds_bits(pmu, QCC730_GPIO_3, QCC730_GPIO_1);

	/* Restore console to option 1 */
	ret = pinctrl_configure_pins(uart0_option1_pins, ARRAY_SIZE(uart0_option1_pins),
				     PINCTRL_REG_NONE);
	k_msleep(20);

	LOG_INF("sleep CFG_UART_OPTION observed: s0=%u s1=%u s2=%u s3=%u",
		(unsigned int)opt_after_sleep0, (unsigned int)opt_after_sleep1,
		(unsigned int)opt_after_sleep2, (unsigned int)opt_after_sleep3);

	zassert_equal(opt_after_sleep0, QCC730_UART_OPTION_0, "sleep0 expected 0, got %u",
		      opt_after_sleep0);
	zassert_equal(opt_after_sleep1, QCC730_UART_OPTION_1, "sleep1 expected 1, got %u",
		      opt_after_sleep1);
	zassert_equal(opt_after_sleep2, QCC730_UART_OPTION_2, "sleep2 expected 2, got %u",
		      opt_after_sleep2);
	zassert_equal(opt_after_sleep3, QCC730_UART_OPTION_3, "sleep3 expected 3, got %u",
		      opt_after_sleep3);

	/* Now assert PU/PD/DS bits for each sleep option (after restoring console) */
	zassert_equal(sbits0.pu_a, 0U, "sleep0 PU GPIO11 expected 0, got %u", sbits0.pu_a);
	zassert_equal(sbits0.pu_b, 0U, "sleep0 PU GPIO12 expected 0, got %u", sbits0.pu_b);
	zassert_equal(sbits0.pd_a, 0U, "sleep0 PD GPIO11 expected 0, got %u", sbits0.pd_a);
	zassert_equal(sbits0.pd_b, 0U, "sleep0 PD GPIO12 expected 0, got %u", sbits0.pd_b);
	zassert_equal(sbits0.ds_a, 0U, "sleep0 DS GPIO11 expected 0, got %u", sbits0.ds_a);
	zassert_equal(sbits0.ds_b, 0U, "sleep0 DS GPIO12 expected 0, got %u", sbits0.ds_b);

	zassert_equal(sbits1.pu_a, 0U, "sleep1 PU GPIO14 expected 0, got %u", sbits1.pu_a);
	zassert_equal(sbits1.pu_b, 0U, "sleep1 PU GPIO13 expected 0, got %u", sbits1.pu_b);
	zassert_equal(sbits1.pd_a, 0U, "sleep1 PD GPIO14 expected 0, got %u", sbits1.pd_a);
	zassert_equal(sbits1.pd_b, 0U, "sleep1 PD GPIO13 expected 0, got %u", sbits1.pd_b);
	zassert_equal(sbits1.ds_a, 0U, "sleep1 DS GPIO14 expected 0, got %u", sbits1.ds_a);
	zassert_equal(sbits1.ds_b, 0U, "sleep1 DS GPIO13 expected 0, got %u", sbits1.ds_b);

	zassert_equal(sbits2.pu_a, 0U, "sleep2 PU GPIO9 expected 0, got %u", sbits2.pu_a);
	zassert_equal(sbits2.pu_b, 0U, "sleep2 PU GPIO10 expected 0, got %u", sbits2.pu_b);
	zassert_equal(sbits2.pd_a, 0U, "sleep2 PD GPIO9 expected 0, got %u", sbits2.pd_a);
	zassert_equal(sbits2.pd_b, 0U, "sleep2 PD GPIO10 expected 0, got %u", sbits2.pd_b);
	zassert_equal(sbits2.ds_a, 0U, "sleep2 DS GPIO9 expected 0, got %u", sbits2.ds_a);
	zassert_equal(sbits2.ds_b, 0U, "sleep2 DS GPIO10 expected 0, got %u", sbits2.ds_b);

	zassert_equal(sbits3.pu_a, 0U, "sleep3 PU GPIO3 expected 0, got %u", sbits3.pu_a);
	zassert_equal(sbits3.pu_b, 0U, "sleep3 PU GPIO1 expected 0, got %u", sbits3.pu_b);
	zassert_equal(sbits3.pd_a, 0U, "sleep3 PD GPIO3 expected 0, got %u", sbits3.pd_a);
	zassert_equal(sbits3.pd_b, 0U, "sleep3 PD GPIO1 expected 0, got %u", sbits3.pd_b);
	zassert_equal(sbits3.ds_a, 0U, "sleep3 DS GPIO3 expected 0, got %u", sbits3.ds_a);
	zassert_equal(sbits3.ds_b, 0U, "sleep3 DS GPIO1 expected 0, got %u", sbits3.ds_b);

	LOG_INF("UART pinctrl sleep test passed");
}


/*
 * Validate pinctrl_configure_pins() error returns for:
 * - Invalid pin number (> GPIO14) -> -EINVAL
 * - Unsupported function (e.g., I2C/SPI/QSPI) -> -ENOTSUP
 */
ZTEST(uart_pinctrl, test_pinctrl_configure_pins_error_returns)
{
	int ret;

	/* Invalid pin: GPIO15 (out of range 0..14) */
	const pinctrl_soc_pin_t invalid_pin =
		QCC730_PINMUX_UART_PERIPHERAL(15U, QCC730_UART_OPTION_0);
	ret = pinctrl_configure_pins(&invalid_pin, 1, PINCTRL_REG_NONE);
	zassert_equal(ret, -EINVAL, "Expected -EINVAL for invalid pin, got %d", ret);

	/* Unsupported function on a valid pin: I2C on GPIO1 */
	const pinctrl_soc_pin_t unsupported_func_i2c =
		QCC730_PINMUX_PERIPHERAL(QCC730_GPIO_1, QCC730_FUNC_I2C);
	ret = pinctrl_configure_pins(&unsupported_func_i2c, 1, PINCTRL_REG_NONE);
	zassert_equal(ret, -ENOTSUP, "Expected -ENOTSUP for unsupported function (I2C), got %d", ret);

	/* Unsupported function on a valid pin: QSPI on GPIO1 */
	const pinctrl_soc_pin_t unsupported_func_qspi =
		QCC730_PINMUX_PERIPHERAL(QCC730_GPIO_1, QCC730_FUNC_QSPI);
	ret = pinctrl_configure_pins(&unsupported_func_qspi, 1, PINCTRL_REG_NONE);
	zassert_equal(ret, -ENOTSUP, "Expected -ENOTSUP for unsupported function (QSPI), got %d", ret);
}

/*
 * Validate pinctrl_apply_state() error return when applying a non-existent state id.
 * We purposely pass an out-of-range enum value to trigger -ENOENT from lookup.
 */
ZTEST(uart_pinctrl, test_pinctrl_apply_state_error_returns)
{
	int ret;
	/* 
	 * Use an out-of-range id (99) to trigger -ENOENT from lookup.
	 */
	ret = pinctrl_apply_state(uart_pcfg, (uint8_t)99);
	zassert_equal(ret, -ENOENT, "Expected -ENOENT for unknown state, got %d", ret);
}

static void *uart_pinctrl_test_setup(void)
{
	zassert_true(device_is_ready(uart_dev), "UART device is not ready");
	LOG_INF("UART pinctrl test suite setup complete");
	return NULL;
}

ZTEST_SUITE(uart_pinctrl, NULL, uart_pinctrl_test_setup, NULL, NULL, NULL);
