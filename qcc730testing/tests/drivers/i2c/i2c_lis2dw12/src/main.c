/*
 * Copyright (c) 2025 Qualcomm Technologies, Inc. and/or its subsidiaries
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/ztest.h>
#include <zephyr/pm/device.h>
#include "ferm_i2c.h"

#define CONTINUOUS_TRANSFER_COUNT_TEST 1000U
#define LIS2DW12_DUMMY_INIT_VAL        99
#define LIS2DW12_TEST_TOLERANCE        0.9
#define LIS2DW12_STATIONARY_VAL        (double)0 /* Measurement result when sensor is not moved */
#define LIS2DW12_EARTH_ACCEL_VAL       (double)9.81 /* Earth acceleration in m/s^2 */

static const struct device *const lis2dw12_dev = DEVICE_DT_GET_ONE(st_lis2dw12);
static const struct device *const i2c_dev = DEVICE_DT_GET_ONE(qcom_qcc730_i2c);

ZTEST(lis2dw12_sensor_api_test, test_read_accel_values)
{
	struct sensor_value x_val = {LIS2DW12_DUMMY_INIT_VAL, LIS2DW12_DUMMY_INIT_VAL},
			    y_val = {LIS2DW12_DUMMY_INIT_VAL, LIS2DW12_DUMMY_INIT_VAL},
			    z_val = {LIS2DW12_DUMMY_INIT_VAL, LIS2DW12_DUMMY_INIT_VAL};
	int32_t ret = 0;

	ret = sensor_sample_fetch_chan(lis2dw12_dev, SENSOR_CHAN_ACCEL_XYZ);
	zassert_ok(ret, "Sample fetch failed!");

	ret = sensor_channel_get(lis2dw12_dev, SENSOR_CHAN_ACCEL_X, &x_val);
	zassert_ok(ret, "Failed to get X-axis acceleration!");

	ret = sensor_channel_get(lis2dw12_dev, SENSOR_CHAN_ACCEL_Y, &y_val);
	zassert_ok(ret, "Failed to get Y-axis acceleration!");

	ret = sensor_channel_get(lis2dw12_dev, SENSOR_CHAN_ACCEL_Z, &z_val);
	zassert_ok(ret, "Failed to get Z-axis acceleration!");

	TC_PRINT("Acceleration read OK: X=%.3f, Y=%.3f, Z=%.3f (m/s^2)\n",
		 (double)sensor_value_to_double(&x_val), (double)sensor_value_to_double(&y_val),
		 (double)sensor_value_to_double(&z_val));
}

ZTEST(lis2dw12_sensor_api_test, test_read_accel_values_fast_mode)
{
	struct sensor_value x_val = {LIS2DW12_DUMMY_INIT_VAL, LIS2DW12_DUMMY_INIT_VAL},
			    y_val = {LIS2DW12_DUMMY_INIT_VAL, LIS2DW12_DUMMY_INIT_VAL},
			    z_val = {LIS2DW12_DUMMY_INIT_VAL, LIS2DW12_DUMMY_INIT_VAL};
	int32_t ret = 0;

	/* Set fast speed mode */
	ret = i2c_configure(i2c_dev, I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_FAST));
	zassert_ok(ret, "Setting fast mode failed");

	ret = sensor_sample_fetch_chan(lis2dw12_dev, SENSOR_CHAN_ACCEL_XYZ);
	zassert_ok(ret, "Channel not specified, sample fetch from sensor should fail!");

	ret = sensor_channel_get(lis2dw12_dev, SENSOR_CHAN_ACCEL_X, &x_val);
	zassert_ok(ret, "Failed to get X-axis acceleration!");

	ret = sensor_channel_get(lis2dw12_dev, SENSOR_CHAN_ACCEL_Y, &y_val);
	zassert_ok(ret, "Failed to get Y-axis acceleration!");

	ret = sensor_channel_get(lis2dw12_dev, SENSOR_CHAN_ACCEL_Z, &z_val);
	zassert_ok(ret, "Failed to get Z-axis acceleration!");

	TC_PRINT("Acceleration read OK: X=%.3f, Y=%.3f, Z=%.3f (m/s^2)\n",
		 (double)sensor_value_to_double(&x_val), (double)sensor_value_to_double(&y_val),
		 (double)sensor_value_to_double(&z_val));

	/* Return to standard speed */
	ret = i2c_configure(i2c_dev, I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_STANDARD));
	zassert_ok(ret, "Setting standard mode failed");
}

ZTEST(lis2dw12_sensor_api_test, test_free_standing)
{
	struct sensor_value x = {LIS2DW12_DUMMY_INIT_VAL, LIS2DW12_DUMMY_INIT_VAL},
			    y = {LIS2DW12_DUMMY_INIT_VAL, LIS2DW12_DUMMY_INIT_VAL},
			    z = {LIS2DW12_DUMMY_INIT_VAL, LIS2DW12_DUMMY_INIT_VAL};
	int32_t ret = 0;

	ret = sensor_sample_fetch_chan(lis2dw12_dev, SENSOR_CHAN_ACCEL_XYZ);
	zassert_ok(ret, "Sample fetch failed!");

	ret = sensor_channel_get(lis2dw12_dev, SENSOR_CHAN_ACCEL_X, &x);
	zassert_ok(ret, "Failed to get X-axis acceleration!");

	ret = sensor_channel_get(lis2dw12_dev, SENSOR_CHAN_ACCEL_Y, &y);
	zassert_ok(ret, "Failed to get Y-axis acceleration!");

	ret = sensor_channel_get(lis2dw12_dev, SENSOR_CHAN_ACCEL_Z, &z);
	zassert_ok(ret, "Failed to get Z-axis acceleration!");

	zassert_within(sensor_value_to_double(&x), LIS2DW12_STATIONARY_VAL, LIS2DW12_TEST_TOLERANCE,
		       "Sensor should not move! X-axis acceleration is not close to 0!");
	zassert_within(sensor_value_to_double(&y), LIS2DW12_STATIONARY_VAL, LIS2DW12_TEST_TOLERANCE,
		       "Sensor should not move! Y-axis acceleration is not close to 0!");
	zassert_within(sensor_value_to_double(&z),
		       LIS2DW12_EARTH_ACCEL_VAL - LIS2DW12_TEST_TOLERANCE,
		       LIS2DW12_EARTH_ACCEL_VAL + LIS2DW12_TEST_TOLERANCE,
		       "Sensor should not move! Z-axis acceleration is not close to 0!");
}

ZTEST(lis2dw12_sensor_api_test, test_continuous_data_transfer)
{
	struct sensor_value x = {LIS2DW12_DUMMY_INIT_VAL, LIS2DW12_DUMMY_INIT_VAL};
	int32_t ret = 0;

	TC_PRINT("Starting continuous data transfer test (%u samples)...\n",
		 CONTINUOUS_TRANSFER_COUNT_TEST);

	for (uint32_t i = 0; i < CONTINUOUS_TRANSFER_COUNT_TEST; i++) {
		ret = sensor_sample_fetch_chan(lis2dw12_dev, SENSOR_CHAN_ACCEL_X);
		zassert_ok(ret, "Sample fetch failed!");
		ret = sensor_channel_get(lis2dw12_dev, SENSOR_CHAN_ACCEL_X, &x);
		zassert_ok(ret, "sensor_channel_get failed on iteration %u!", i);
		TC_PRINT("Got sample %u: X=%.3f m/s^2\n", i, (double)sensor_value_to_double(&x));
		zassert_within(sensor_value_to_double(&x), LIS2DW12_STATIONARY_VAL,
			       LIS2DW12_TEST_TOLERANCE,
			       "Sensor should not move! X-axis accel is not close to 0!");
		/* Ensure we get fresh data from sample on each iteration */
		x.val1 = LIS2DW12_DUMMY_INIT_VAL;
		x.val2 = LIS2DW12_DUMMY_INIT_VAL;
		k_sleep(K_MSEC(5));
	}
}

ZTEST(lis2dw12_sensor_api_test, test_error_handling)
{
	int ret;

	/* Verify that configuration without Controller mode will fail */
	ret = i2c_configure(i2c_dev, 0);
	zassert_equal(ret, -EINVAL,
		      "Configuration without CONTROLLER mode set should be impossible");

	/* Verify that zero messages to send will return without error */
	ret = i2c_transfer(i2c_dev, NULL, 0, 0);
	zassert_equal(ret, 0, "Call i2c_transfer without messages failed");

	/* Verify that messages with 10bit addressing will return error */
	struct i2c_msg msg_with_10bit_addr = {.flags = I2C_MSG_ADDR_10_BITS};
	ret = i2c_transfer(i2c_dev, &msg_with_10bit_addr, 1, 0);
	zassert_equal(ret, -ENOTSUP, "10bit addressing wasn't rejected properly");
}

ZTEST(lis2dw12_sensor_api_test, test_suspend_resume_success)
{
	int ret;

	/* First do a normal I2C transfer to ensure device is working
	 * (WHO_AM_I register) */
	uint8_t write_buf[1] = {0x44};
	ret = i2c_write(i2c_dev, write_buf, sizeof(write_buf), 0x19);
	zassert_true(ret == 0, "Initial I2C write failed");

	/* Suspend device */
	ret = pm_device_action_run(i2c_dev, PM_DEVICE_ACTION_SUSPEND);
	zassert_true(ret == 0, "Suspend action failed");

	ret = i2c_write(i2c_dev, write_buf, sizeof(write_buf), 0x19);
	/* -EBUSY error is expected */
	zassert_true(ret < 0, "I2C write after suspend failed unexpectedly");

	/* Resume device explicitly */
	ret = pm_device_action_run(i2c_dev, PM_DEVICE_ACTION_RESUME);
	zassert_true(ret == 0, "Resume action failed");

	/* After resume, transfer must work again */
	ret = i2c_write(i2c_dev, write_buf, sizeof(write_buf), 0x19);
	zassert_true(ret == 0, "I2C write after resume failed");
}

static void *lis2dw12_test_setup(void)
{
	zassert_true(device_is_ready(i2c_dev), "I2C device is not ready");
	zassert_true(device_is_ready(lis2dw12_dev), "LIS2DW12 sensor device is not ready");
	return NULL;
}

ZTEST_SUITE(lis2dw12_sensor_api_test, NULL, lis2dw12_test_setup, NULL, NULL, NULL);
