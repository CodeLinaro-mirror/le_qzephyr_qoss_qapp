/*
 * Copyright (c) 2025 Qualcomm Technologies, Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * GPIO Stress Test Suite for QCC730
 * Uses ztress framework for robustness validation
 */

#include <zephyr/ztest.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/ztress.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(gpio_qcc730_test, LOG_LEVEL_INF);

#define GPIO_NODE      DT_ALIAS(gpio_test)
#define TEST_PIN_OUT_0 0
#define TEST_PIN_IN_0  1
#define TEST_PIN_OUT_1 2
#define TEST_PIN_IN_1  3
#define TEST_PIN_OUT_2 4
#define TEST_PIN_IN_2  5
#define TEST_PIN_OUT_3 6
#define TEST_PIN_IN_3  7
#define MAX_GPIO_PIN   14

/* Stress test configuration */
#define ZTRESS_ITERATIONS             1000
#define ZTRESS_HIGH_NUM_OF_ITERATIONS 10000
#define ZTRESS_MIN_PREEMPTIONS        100
#define ZTRESS_NO_PREEMPTION          0
#define ZTRESS_TEST_TIMEOUT           K_SECONDS(10)
#define ZTRESS_TOLERANCE_PERCENTAGE   5

/* Endurance test configuration */
#define ZTRESS_ENDURANCE_ITERATIONS                100000U
#define ZTRESS_ENDURANCE_SECONDS                   70U
#define ZTRESS_EXPECTED_MINIMAL_OPERATIONS_PER_SEC 1000U
#define ZTRESS_EXPECTED_TOTAL_MINIMAL_OPERATIONS                                                   \
	(ZTRESS_ENDURANCE_SECONDS * ZTRESS_EXPECTED_MINIMAL_OPERATIONS_PER_SEC)

static const struct device *gpio_dev;
static struct gpio_callback gpio_cb_data;
static atomic_t interrupt_counter;
static atomic_t error_counter;
static struct k_sem interrupt_sem;

/* Test fixture for GPIO tests */
struct gpio_stress_fixture {
	const struct device *dev;
	bool initialized;
	uint32_t test_iterations;
};

/* Ztress context */
struct ztress_context {
	const struct device *dev;
	gpio_pin_t pin;
	atomic_t *counter;
	atomic_t *errors;
	uint32_t operations;
};

static void *gpio_test_setup(void)
{
	static struct gpio_stress_fixture fixture = {0};

	gpio_dev = DEVICE_DT_GET(GPIO_NODE);
	zassume_true(device_is_ready(gpio_dev), "GPIO device not ready");

	fixture.dev = gpio_dev;
	fixture.initialized = true;
	fixture.test_iterations = 0;

	k_sem_init(&interrupt_sem, 0, 1);
	atomic_set(&interrupt_counter, 0);
	atomic_set(&error_counter, 0);

	LOG_INF("GPIO test setup complete for QCC730");

	return &fixture;
}

static void gpio_test_before(void *f)
{
	struct gpio_stress_fixture *fixture = (struct gpio_stress_fixture *)f;
	int ret = 0;

	for (gpio_pin_t i = 0U; i <= MAX_GPIO_PIN; i++) {
		ret = gpio_pin_configure(fixture->dev, i, GPIO_INPUT);
		zassert_equal(ret, 0, "Failed to configure GPIO_INPUT in gpio_test_before");
	}

	/* Reset counters */
	atomic_set(&interrupt_counter, 0);
	atomic_set(&error_counter, 0);
	k_sem_reset(&interrupt_sem);

	LOG_DBG("Test case setup complete");
}

static void gpio_test_teardown(void *f)
{
	struct gpio_stress_fixture *fixture = (struct gpio_stress_fixture *)f;
	int ret = 0;

	for (gpio_pin_t i = 0U; i <= MAX_GPIO_PIN; i++) {
		ret = gpio_pin_configure(fixture->dev, i, GPIO_INPUT);
		zassert_equal(ret, 0, "Failed to configure GPIO_INPUT in gpio_test_teardown");
	}

	LOG_INF("Test teardown complete, total errors: %ld", atomic_get(&error_counter));
}

ZTEST_SUITE(gpio_stress, NULL, gpio_test_setup, gpio_test_before, NULL, gpio_test_teardown);

/* Ztress handler: Rapid configuration changes */
static bool ztress_config_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct ztress_context *ctx = (struct ztress_context *)user_data;
	int ret = 0;
	gpio_flags_t configs[] = {GPIO_OUTPUT, GPIO_INPUT, GPIO_INPUT | GPIO_PULL_UP,
				  GPIO_INPUT | GPIO_PULL_DOWN};
	gpio_flags_t config = configs[cnt % ARRAY_SIZE(configs)];

	ret = gpio_pin_configure(ctx->dev, ctx->pin, config);

	if (ret != 0) {
		atomic_inc(ctx->errors);
		LOG_ERR("Config failed on pin %d: %d", ctx->pin, ret);
	} else {
		ctx->operations++;
	}

	if (last) {
		return false;
	}

	return true;
}

/* Ztress handler: Rapid toggle operations */
static bool ztress_toggle_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct ztress_context *ctx = (struct ztress_context *)user_data;
	int ret = 0;

	ret = gpio_pin_toggle(ctx->dev, ctx->pin);

	if (ret != 0) {
		atomic_inc(ctx->errors);
		LOG_ERR("Toggle failed on pin %d: %d", ctx->pin, ret);
	} else {
		ctx->operations++;
	}

	if (last) {
		return false;
	}

	if ((cnt % 100U) == 0U) {
		k_yield();
	}

	return true;
}

/* Ztress handler: Continuous read operations */
static bool ztress_read_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct ztress_context *ctx = (struct ztress_context *)user_data;
	int val = 0;

	val = gpio_pin_get(ctx->dev, ctx->pin);

	if (val < 0) {
		atomic_inc(ctx->errors);
		LOG_ERR("Read failed on pin %d: %d", ctx->pin, val);
	} else {
		ctx->operations++;
		if (val > 1) {
			atomic_inc(ctx->errors);
			LOG_ERR("Invalid read value %d on pin %d", val, ctx->pin);
		}
	}

	if (last) {
		return false;
	}

	return true;
}

/* Ztress handler: Port-wide operations */
static bool ztress_port_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct ztress_context *ctx = (struct ztress_context *)user_data;
	gpio_port_value_t port_val = 0U;
	gpio_port_pins_t mask = BIT(TEST_PIN_OUT_0) | BIT(TEST_PIN_OUT_1);
	int ret = 0;

	switch (cnt % 4) {
	case 0:
		ret = gpio_port_set_bits_raw(ctx->dev, mask);
		break;
	case 1:
		ret = gpio_port_clear_bits_raw(ctx->dev, mask);
		break;
	case 2:
		ret = gpio_port_toggle_bits(ctx->dev, mask);
		break;
	case 3:
	default:
		ret = gpio_port_get_raw(ctx->dev, &port_val);
		break;
	}

	if (ret != 0) {
		atomic_inc(ctx->errors);
		LOG_ERR("Port operation failed: %d", ret);
	} else {
		ctx->operations++;
	}

	if (last) {
		return false;
	}

	return true;
}

ZTEST_F(gpio_stress, test_ztress_concurrent_operations)
{
	atomic_val_t num_of_errors = 0;

	struct ztress_context ctx[4] = {{.dev = fixture->dev,
					 .pin = TEST_PIN_OUT_0,
					 .counter = &interrupt_counter,
					 .errors = &error_counter,
					 .operations = 0},
					{.dev = fixture->dev,
					 .pin = TEST_PIN_OUT_1,
					 .counter = &interrupt_counter,
					 .errors = &error_counter,
					 .operations = 0},
					{.dev = fixture->dev,
					 .pin = TEST_PIN_IN_0,
					 .counter = &interrupt_counter,
					 .errors = &error_counter,
					 .operations = 0},
					{.dev = fixture->dev,
					 .pin = TEST_PIN_IN_1,
					 .counter = &interrupt_counter,
					 .errors = &error_counter,
					 .operations = 0}};

	LOG_INF("Starting ztress concurrent operations test");
	LOG_INF("This test validates GPIO stability under multiple concurrent contexts");

	/* Set stress test timeout */
	ztress_set_timeout(ZTRESS_TEST_TIMEOUT);

	/* Execute stress test with multiple handlers */
	ZTRESS_EXECUTE(ZTRESS_THREAD(ztress_toggle_handler, &ctx[0], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
		       ZTRESS_THREAD(ztress_config_handler, &ctx[1], ZTRESS_ITERATIONS,
				     ZTRESS_MIN_PREEMPTIONS, Z_TIMEOUT_TICKS(0)),
		       ZTRESS_THREAD(ztress_read_handler, &ctx[2], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
		       ZTRESS_THREAD(ztress_port_handler, &ctx[3], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)));

	/* Report results */
	num_of_errors = atomic_get(&error_counter);
	LOG_INF("Ztress test completed:");
	LOG_INF("  Toggle operations: %u", ctx[0].operations);
	LOG_INF("  Config operations: %u", ctx[1].operations);
	LOG_INF("  Read operations: %u", ctx[2].operations);
	LOG_INF("  Port operations: %u", ctx[3].operations);
	LOG_INF("  Total errors: %ld", num_of_errors);

	/* Verify requirements */
	zassert_equal(num_of_errors, 0, "Errors detected during stress test");
	zassert_true(ctx[0].operations == ZTRESS_ITERATIONS,
		     "Toggle operations %u does not match expected value %d!", ZTRESS_ITERATIONS,
		     ctx[0].operations);
	zassert_true(ctx[1].operations == ZTRESS_ITERATIONS,
		     "Config operations %u does not match expected value %d!", ZTRESS_ITERATIONS,
		     ctx[1].operations);
	zassert_true(ctx[2].operations == ZTRESS_ITERATIONS,
		     "Read operations %u does not match expected value %d!", ZTRESS_ITERATIONS,
		     ctx[2].operations);
	zassert_true(ctx[3].operations == ZTRESS_ITERATIONS,
		     "Port operations %u does not match expected value %d!", ZTRESS_ITERATIONS,
		     ctx[3].operations);
}

ZTEST_F(gpio_stress, test_ztress_high_frequency)
{
	const uint32_t total_num_of_threads = 2;
	struct ztress_context ctx[2] = {0};
	uint32_t pins[] = {TEST_PIN_OUT_0, TEST_PIN_OUT_1};

	LOG_INF("Starting high-frequency toggle stress test");

	for (gpio_pin_t i = 0U; i < ARRAY_SIZE(pins); i++) {
		ctx[i].dev = fixture->dev;
		ctx[i].pin = pins[i];
		ctx[i].counter = &interrupt_counter;
		ctx[i].errors = &error_counter;
		ctx[i].operations = 0;
	}

	ztress_set_timeout(ZTRESS_TEST_TIMEOUT);

	ZTRESS_EXECUTE(ZTRESS_THREAD(ztress_toggle_handler, &ctx[0], ZTRESS_HIGH_NUM_OF_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(0)),
		       ZTRESS_THREAD(ztress_toggle_handler, &ctx[1], ZTRESS_HIGH_NUM_OF_ITERATIONS,
				     ZTRESS_MIN_PREEMPTIONS, Z_TIMEOUT_TICKS(0)));

	LOG_INF("High-frequency test results:");
	LOG_INF("  Pin %d: %u toggles", pins[0], ctx[0].operations);
	LOG_INF("  Pin %d: %u toggles", pins[1], ctx[1].operations);
	LOG_INF("  Errors: %ld", atomic_get(&error_counter));

	/* Verify high toggle rate was achieved */
	uint32_t total_toggles = ctx[0].operations + ctx[1].operations;
	zassert_true(total_toggles >= (total_num_of_threads * ZTRESS_HIGH_NUM_OF_ITERATIONS),
		     "Toggle rate too low: %u", total_toggles);
	zassert_equal(atomic_get(&error_counter), 0, "Errors in high-frequency test");
}

#ifdef CONFIG_GPIO_QCC730_INTERRUPT

static void gpio_interrupt_callback(const struct device *dev, struct gpio_callback *cb,
				    uint32_t pins)
{
	atomic_inc(&interrupt_counter);
	k_sem_give(&interrupt_sem);
}

/* Ztress handler: Generate interrupts */
static bool ztress_interrupt_generator(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct ztress_context *ctx = (struct ztress_context *)user_data;
	int ret = 0;

	ctx->operations++;

	if (last) {
		return false;
	}

	/* Toggle output pin to generate interrupt on connected input */
	ret = gpio_pin_toggle(ctx->dev, ctx->pin);
	if (ret != 0) {
		atomic_inc(ctx->errors);
	}

	/* Small delay to allow interrupt processing */
	k_busy_wait(2);

	return true;
}

ZTEST_F(gpio_stress, test_ztress_interrupt_storm)
{
	struct ztress_context gen_ctx = {.dev = fixture->dev,
					 .pin = TEST_PIN_OUT_3,
					 .counter = &interrupt_counter,
					 .errors = &error_counter,
					 .operations = 0};
	int ret = 0;
	uint32_t toggles = 0U;
	uint32_t expected_interrupts = 0U;
	uint32_t received_interrupts = 0U;
	uint32_t min_expected_interrupts = 0U;

	LOG_INF("Starting interrupt storm stress test");

	ret = gpio_pin_configure(fixture->dev, TEST_PIN_OUT_3, GPIO_OUTPUT_LOW);
	zassert_equal(ret, 0, "Failed to configure output pin");

	ret = gpio_pin_configure(fixture->dev, TEST_PIN_IN_2, GPIO_INPUT);
	zassert_equal(ret, 0, "Failed to configure input pin");

	ret = gpio_pin_interrupt_configure(fixture->dev, TEST_PIN_IN_2, GPIO_INT_EDGE_RISING);
	zassert_equal(ret, 0, "Failed to configure interrupt");

	gpio_init_callback(&gpio_cb_data, gpio_interrupt_callback, BIT(TEST_PIN_IN_2));
	ret = gpio_add_callback(fixture->dev, &gpio_cb_data);
	zassert_equal(ret, 0, "Failed to add callback");

	ztress_set_timeout(ZTRESS_TEST_TIMEOUT);

	ZTRESS_EXECUTE(ZTRESS_THREAD(ztress_interrupt_generator, &gen_ctx, ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(1)));

	gpio_remove_callback(fixture->dev, &gpio_cb_data);
	gpio_pin_interrupt_configure(fixture->dev, TEST_PIN_IN_2, GPIO_INT_DISABLE);

	toggles = gen_ctx.operations;
	expected_interrupts = toggles / 2;
	received_interrupts = atomic_get(&interrupt_counter);
	min_expected_interrupts = expected_interrupts * (100 - ZTRESS_TOLERANCE_PERCENTAGE) / 100;

	LOG_INF("Interrupt storm test results:");
	LOG_INF("  Pin toggles performed: %u", toggles);
	LOG_INF("  Expected rising-edge interrupts: %u", expected_interrupts);
	LOG_INF("  Interrupts received: %u", received_interrupts);
	LOG_INF("  Errors: %ld", atomic_get(&error_counter));

	zassert_equal(atomic_get(&error_counter), 0, "Errors during interrupt generation");

	zassert_true(received_interrupts >= min_expected_interrupts,
		     "Received too few interrupts: %u, expected at least %u (95%%)",
		     received_interrupts, min_expected_interrupts);

	zassert_true(received_interrupts <= expected_interrupts,
		     "Received too many interrupts: %u, expected at most %u", received_interrupts,
		     expected_interrupts);
}

#endif /* CONFIG_GPIO_QCC730_INTERRUPT */

/* Long-duration stress handler */
static bool ztress_endurance_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct ztress_context *ctx = (struct ztress_context *)user_data;
	int ret = 0;

	ctx->operations++;

	if (last) {
		LOG_INF("Endurance test completed: %u operations", ctx->operations);
		return false;
	}

	/* Perform mixed operations */
	switch (cnt % 5) {
	case 0:
		ret = gpio_pin_set(ctx->dev, ctx->pin, 1);
		break;
	case 1:
		ret = gpio_pin_set(ctx->dev, ctx->pin, 0);
		break;
	case 2:
		ret = gpio_pin_toggle(ctx->dev, ctx->pin);
		break;
	case 3:
		ret = gpio_pin_configure(ctx->dev, ctx->pin, GPIO_OUTPUT);
		break;
	case 4:
		ret = gpio_pin_get(ctx->dev, ctx->pin);
		break;
	}

	if (ret < 0) {
		atomic_inc(ctx->errors);
		LOG_ERR("Endurance operation failed at %u: %d", ctx->operations, ret);
	}

	/* Progress report every 10000 operations */
	if ((ctx->operations % 10000) == 0) {
		LOG_INF("\nEndurance progress: %u operations\n", ctx->operations);
	}

	return true;
}

ZTEST_F(gpio_stress, test_ztress_endurance)
{
	const uint32_t total_num_of_threads = 3;
	const uint32_t expected_minimal_num_of_operations =
		ZTRESS_EXPECTED_TOTAL_MINIMAL_OPERATIONS * total_num_of_threads;
	struct ztress_context ctx[3] = {0};
	uint32_t pins[] = {TEST_PIN_OUT_0, TEST_PIN_OUT_1, TEST_PIN_OUT_2};
	uint32_t total_ops = 0U;

	LOG_INF("Starting endurance stress test");
	LOG_INF("This test validates long-term stability and memory leaks");

	for (gpio_pin_t i = 0U; i < ARRAY_SIZE(ctx); i++) {
		ctx[i].dev = fixture->dev;
		ctx[i].pin = pins[i];
		ctx[i].counter = &interrupt_counter;
		ctx[i].errors = &error_counter;
		ctx[i].operations = 0;
	}

	ztress_set_timeout(K_SECONDS(ZTRESS_ENDURANCE_SECONDS));
	/* Run endurance test */
	ZTRESS_EXECUTE(ZTRESS_THREAD(ztress_endurance_handler, &ctx[0], ZTRESS_ENDURANCE_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(1)),
		       ZTRESS_THREAD(ztress_endurance_handler, &ctx[1], ZTRESS_ENDURANCE_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(2)),
		       ZTRESS_THREAD(ztress_endurance_handler, &ctx[2], ZTRESS_ENDURANCE_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(3)));

	/* Report results */
	LOG_INF("Endurance test completed:");
	for (uint32_t i = 0U; i < ARRAY_SIZE(ctx); i++) {
		const uint32_t operations_per_sec = (ctx[i].operations / ZTRESS_ENDURANCE_SECONDS);
		LOG_INF("  Pin %d: %u operations (%u operations / sec)", pins[i], ctx[i].operations,
			operations_per_sec);
		total_ops += ctx[i].operations;
	}
	LOG_INF("  Total operations: %u", total_ops);
	LOG_INF("  Total errors: %ld", atomic_get(&error_counter));

	/* Verify requirements */
	zassert_true(total_ops > expected_minimal_num_of_operations,
		     "Insufficient operations: %u. Expected: %u", total_ops,
		     expected_minimal_num_of_operations);
	zassert_equal(atomic_get(&error_counter), 0, "Errors in endurance test");

	LOG_INF("Endurance test PASSED - GPIO driver stable over %u operations", total_ops);
}
