/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <zephyr/linker/devicetree_regions.h>
#include <zephyr/pm/device.h>

static const struct device *get_dxe_dev(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(dxe0));
	zassert_true(device_is_ready(dev), "DXE device not ready");
	return dev;
}

/* Max block size for DXE transfer that is used by tests*/
#define MAX_BLOCK_SIZE     (0xFF)
/* Tests use no more than 3 blocks per transfer */
#define TOTAL_BUFFERS_SIZE (5 * MAX_BLOCK_SIZE)

/* SRAM buffers */
static __aligned(4) uint8_t sram_src[TOTAL_BUFFERS_SIZE];
static __aligned(4) uint8_t sram_dst[TOTAL_BUFFERS_SIZE];

/* RRAM buffers */
static __aligned(4) uint8_t rram_src[TOTAL_BUFFERS_SIZE] Z_GENERIC_SECTION(
	LINKER_DT_NODE_REGION_NAME(DT_NODELABEL(rram))) = {[0 ...(TOTAL_BUFFERS_SIZE - 1)] = 0x5A};

static __aligned(4) uint8_t rram_dst[TOTAL_BUFFERS_SIZE] Z_GENERIC_SECTION(
	LINKER_DT_NODE_REGION_NAME(DT_NODELABEL(rram))) = {[0 ...(TOTAL_BUFFERS_SIZE - 1)] = 0xA5};

static K_SEM_DEFINE(done_sem, 0, 1);
static volatile int cb_status;

static void dxe_cb(const struct device *dev, void *user_data, uint32_t channel, int status)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
	TC_PRINT("DXE callback: ch=%u status=%d\n", channel, status);
	cb_status = status;
	k_sem_give(&done_sem);
}


static void dma_pm_suspend_resume_check(const struct device *dxe)
{
	int ret;

	/* Prepare a small valid config */
	struct dma_block_config b = {0};
	b.block_size = 64;
	b.source_address = (uint32_t)(uintptr_t)sram_src;
	b.dest_address = (uint32_t)(uintptr_t)sram_dst;

	struct dma_config cfg = (struct dma_config){0};
	cfg.channel_direction = MEMORY_TO_MEMORY;
	cfg.dma_callback = dxe_cb;
	cfg.block_count = 1;
	cfg.head_block = &b;

	/* Suspend device */
	ret = pm_device_action_run(dxe, PM_DEVICE_ACTION_SUSPEND);
	zassert_ok(ret, "pm suspend failed: %d", ret);

	/* While suspended, DMA API calls should return an error (negative) */
	ret = dma_config(dxe, 0, &cfg);
	zassert_true(ret < 0, "dma_config() expected error when suspended, got %d", ret);

	ret = dma_start(dxe, 0);
	zassert_true(ret < 0, "dma_start() expected error when suspended, got %d", ret);

	ret = dma_stop(dxe, 0);
	zassert_true(ret < 0, "dma_stop() expected error when suspended, got %d", ret);

	struct dma_status status;
	ret = dma_get_status(dxe, 0, &status);
	zassert_true(ret < 0, "dma_get_status() expected error when suspended, got %d", ret);

	/* Resume device */
	ret = pm_device_action_run(dxe, PM_DEVICE_ACTION_RESUME);
	zassert_ok(ret, "pm resume failed: %d", ret);

	/* After resume device should work again — run a short transfer */
	memset(sram_src, 0x33, b.block_size);
	memset(sram_dst, 0xCC, b.block_size);
	k_sem_reset(&done_sem);

	ret = dma_config(dxe, 0, &cfg);
	zassert_ok(ret, "dma_config() failed after resume: %d", ret);

	ret = dma_start(dxe, 0);
	zassert_ok(ret, "dma_start() failed after resume: %d", ret);

	zassert_ok(k_sem_take(&done_sem, K_SECONDS(1)), "DMA did not complete after resume");
}

ZTEST(dxe_pm_suite, test_pm)
{
	const struct device *dxe = get_dxe_dev();
	dma_pm_suspend_resume_check(dxe);
}

ZTEST_SUITE(dxe_pm_suite, NULL, NULL, NULL, NULL, NULL);
