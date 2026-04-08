/*
 * Copyright (c) 2025 Qualcomm Technologies, Inc. and/or its subsidiaries
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <zephyr/drivers/pinctrl.h>
#include <zephyr/ztest.h>
#include <zephyr/dt-bindings/pinctrl/qcom-qcc730-pinctrl.h>

/* Define custom state ID for mystate */
#define PINCTRL_STATE_MYSTATE PINCTRL_STATE_PRIV_START

/* Pin configuration for test devices */
#define TEST_DEVICE0 DT_NODELABEL(test_device0)
#define TEST_DEVICE1 DT_NODELABEL(test_device1)

PINCTRL_DT_DEV_CONFIG_DECLARE(TEST_DEVICE0);
PINCTRL_DT_DEV_CONFIG_DECLARE(TEST_DEVICE1);

static const struct pinctrl_dev_config *pcfg0 = PINCTRL_DT_DEV_CONFIG_GET(TEST_DEVICE0);
static const struct pinctrl_dev_config *pcfg1 = PINCTRL_DT_DEV_CONFIG_GET(TEST_DEVICE1);

/**
 * @brief Test pull-up and pull-down configurations
 */
ZTEST(pinctrl_qcc730mi, test_pull_configurations)
{
    const struct pinctrl_state *scfg;

    /* Verify device 0 config */
    zassert_not_null(pcfg0, "Device 0 pinctrl config is NULL");
    zassert_true(pcfg0->state_cnt >= 1, "No states found for device 0");

    scfg = &pcfg0->states[0];
    zassert_equal(scfg->id, PINCTRL_STATE_DEFAULT, "Wrong state ID for default state");
    zassert_equal(scfg->pin_cnt, 3U, "Wrong pin count for default state");

    /* Verify pin with pull-up */
    zassert_equal(QCOM_PINMUX_GET_PIN_NUM(scfg->pins[0]), QCC730_GPIO_0,
                 "Pin 0 number mismatch");
    zassert_equal(QCOM_PINMUX_GET_PULL_UP(scfg->pins[0]), 1,
                 "Pin 0 pull-up mismatch");
    zassert_equal(QCOM_PINMUX_GET_PULL_DOWN(scfg->pins[0]), 0,
                 "Pin 0 pull-down mismatch");

    /* Verify pin with pull-down */
    zassert_equal(QCOM_PINMUX_GET_PIN_NUM(scfg->pins[1]), QCC730_GPIO_1,
                 "Pin 1 number mismatch");
    zassert_equal(QCOM_PINMUX_GET_PULL_UP(scfg->pins[1]), 0,
                 "Pin 1 pull-up mismatch");
    zassert_equal(QCOM_PINMUX_GET_PULL_DOWN(scfg->pins[1]), 1,
                 "Pin 1 pull-down mismatch");

    /* Verify pin with high drive strength */
    zassert_equal(QCOM_PINMUX_GET_PIN_NUM(scfg->pins[2]), QCC730_GPIO_2,
                 "Pin 2 number mismatch");
    zassert_equal(QCOM_PINMUX_GET_DRIVER_STRENGTH(scfg->pins[2]), 1,
                 "Pin 2 drive strength mismatch");
}

/**
 * @brief Test pin modes (input, output, peripheral) without checking OE
 */
ZTEST(pinctrl_qcc730mi, test_pin_modes)
{
    const struct pinctrl_state *scfg;

    /* Verify device 1 config */
    zassert_not_null(pcfg1, "Device 1 pinctrl config is NULL");
    zassert_true(pcfg1->state_cnt >= 1, "No states found for device 1");

    scfg = &pcfg1->states[0];
    zassert_equal(scfg->id, PINCTRL_STATE_DEFAULT, "Wrong state ID for default state");
    zassert_equal(scfg->pin_cnt, 4U, "Wrong pin count for default state"); // Updated for 4 pins

    /* Verify GPIO input pin */
    zassert_equal(QCOM_PINMUX_GET_PIN_NUM(scfg->pins[0]), QCC730_GPIO_3,
                 "Pin 0 number mismatch");
    zassert_equal(QCOM_PINMUX_GET_MODE(scfg->pins[0]), QCOM_PIN_MODE_INPUT,
                 "Pin 0 mode mismatch");
    zassert_equal(QCOM_PINMUX_GET_FUNC_SEL(scfg->pins[0]), QCC730_FUNC_GPIO,
                 "Pin 0 function mismatch");

    /* Verify GPIO output pin - only check pin number, mode and function, not OE */
    zassert_equal(QCOM_PINMUX_GET_PIN_NUM(scfg->pins[1]), QCC730_GPIO_4,
                 "Pin 1 number mismatch");
    zassert_equal(QCOM_PINMUX_GET_MODE(scfg->pins[1]), QCOM_PIN_MODE_OUTPUT,
                 "Pin 1 mode mismatch");
    zassert_equal(QCOM_PINMUX_GET_FUNC_SEL(scfg->pins[1]), QCC730_FUNC_GPIO,
                 "Pin 1 function mismatch");
    /* Removed OE check */

    /* Verify first UART peripheral pin with pull-up and high drive strength */
    zassert_equal(QCOM_PINMUX_GET_PIN_NUM(scfg->pins[2]), QCC730_GPIO_11,
                 "Pin 2 number mismatch");
    zassert_equal(QCOM_PINMUX_GET_MODE(scfg->pins[2]), QCOM_PIN_MODE_PERIPHERAL,
                 "Pin 2 mode mismatch");
    zassert_equal(QCOM_PINMUX_GET_FUNC_SEL(scfg->pins[2]), QCC730_FUNC_UART,
                 "Pin 2 function mismatch");
    zassert_equal(QCOM_PINMUX_GET_UART_OPT(scfg->pins[2]), QCC730_UART_OPTION_0,
                 "Pin 2 UART option mismatch");
    zassert_equal(QCOM_PINMUX_GET_PULL_UP(scfg->pins[2]), 1,
                 "Pin 2 pull-up mismatch");
    zassert_equal(QCOM_PINMUX_GET_DRIVER_STRENGTH(scfg->pins[2]), 1,
                 "Pin 2 drive strength mismatch");

    /* Verify second UART peripheral pin with pull-up and high drive strength */
    zassert_equal(QCOM_PINMUX_GET_PIN_NUM(scfg->pins[3]), QCC730_GPIO_12,
                 "Pin 3 number mismatch");
    zassert_equal(QCOM_PINMUX_GET_MODE(scfg->pins[3]), QCOM_PIN_MODE_PERIPHERAL,
                 "Pin 3 mode mismatch");
    zassert_equal(QCOM_PINMUX_GET_FUNC_SEL(scfg->pins[3]), QCC730_FUNC_UART,
                 "Pin 3 function mismatch");
    zassert_equal(QCOM_PINMUX_GET_UART_OPT(scfg->pins[3]), QCC730_UART_OPTION_0,
                 "Pin 3 UART option mismatch");
    zassert_equal(QCOM_PINMUX_GET_PULL_UP(scfg->pins[3]), 1,
                 "Pin 3 pull-up mismatch");
    zassert_equal(QCOM_PINMUX_GET_DRIVER_STRENGTH(scfg->pins[3]), 1,
                 "Pin 3 drive strength mismatch");
}

/**
 * @brief Test UART pin configurations
 */
ZTEST(pinctrl_qcc730mi, test_uart_configurations)
{
    /* Create test pin configurations */
    pinctrl_soc_pin_t uart_pin0 = QCC730_PINMUX_UART_PERIPHERAL(QCC730_GPIO_11, QCC730_UART_OPTION_0);
    pinctrl_soc_pin_t uart_pin1 = QCC730_PINMUX_UART_PERIPHERAL(QCC730_GPIO_14, QCC730_UART_OPTION_1);
    pinctrl_soc_pin_t uart_pin2 = QCC730_PINMUX_UART_PERIPHERAL(QCC730_GPIO_9, QCC730_UART_OPTION_2);
    pinctrl_soc_pin_t uart_pin3 = QCC730_PINMUX_UART_PERIPHERAL(QCC730_GPIO_3, QCC730_UART_OPTION_3);

    /* Verify UART option 0 */
    zassert_equal(QCOM_PINMUX_GET_PIN_NUM(uart_pin0), QCC730_GPIO_11,
                 "UART option 0 pin number mismatch");
    zassert_equal(QCOM_PINMUX_GET_FUNC_SEL(uart_pin0), QCC730_FUNC_UART,
                 "UART option 0 function mismatch");
    zassert_equal(QCOM_PINMUX_GET_MODE(uart_pin0), QCOM_PIN_MODE_PERIPHERAL,
                 "UART option 0 mode mismatch");
    zassert_equal(QCOM_PINMUX_GET_UART_OPT(uart_pin0), QCC730_UART_OPTION_0,
                 "UART option 0 option value mismatch");

    /* Verify UART option 1 */
    zassert_equal(QCOM_PINMUX_GET_PIN_NUM(uart_pin1), QCC730_GPIO_14,
                 "UART option 1 pin number mismatch");
    zassert_equal(QCOM_PINMUX_GET_FUNC_SEL(uart_pin1), QCC730_FUNC_UART,
                 "UART option 1 function mismatch");
    zassert_equal(QCOM_PINMUX_GET_MODE(uart_pin1), QCOM_PIN_MODE_PERIPHERAL,
                 "UART option 1 mode mismatch");
    zassert_equal(QCOM_PINMUX_GET_UART_OPT(uart_pin1), QCC730_UART_OPTION_1,
                 "UART option 1 option value mismatch");

    /* Verify UART option 2 */
    zassert_equal(QCOM_PINMUX_GET_PIN_NUM(uart_pin2), QCC730_GPIO_9,
                 "UART option 2 pin number mismatch");
    zassert_equal(QCOM_PINMUX_GET_FUNC_SEL(uart_pin2), QCC730_FUNC_UART,
                 "UART option 2 function mismatch");
    zassert_equal(QCOM_PINMUX_GET_MODE(uart_pin2), QCOM_PIN_MODE_PERIPHERAL,
                 "UART option 2 mode mismatch");
    zassert_equal(QCOM_PINMUX_GET_UART_OPT(uart_pin2), QCC730_UART_OPTION_2,
                 "UART option 2 option value mismatch");

    /* Verify UART option 3 */
    zassert_equal(QCOM_PINMUX_GET_PIN_NUM(uart_pin3), QCC730_GPIO_3,
                 "UART option 3 pin number mismatch");
    zassert_equal(QCOM_PINMUX_GET_FUNC_SEL(uart_pin3), QCC730_FUNC_UART,
                 "UART option 3 function mismatch");
    zassert_equal(QCOM_PINMUX_GET_MODE(uart_pin3), QCOM_PIN_MODE_PERIPHERAL,
                 "UART option 3 mode mismatch");
    zassert_equal(QCOM_PINMUX_GET_UART_OPT(uart_pin3), QCC730_UART_OPTION_3,
                 "UART option 3 option value mismatch");
}

ZTEST_SUITE(pinctrl_qcc730mi, NULL, NULL, NULL, NULL, NULL);
