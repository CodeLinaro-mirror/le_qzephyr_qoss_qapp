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
#define MAX_BLOCK_SIZE     (0x3FFC)
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

static void build_dxe_transfer_blocks(struct dma_block_config *head, int n,
				      const uint32_t *src_addrs, const uint32_t *dst_addrs,
				      const size_t *sizes)
{
	for (int i = 0; i < n; i++) {
		memset(&head[i], 0, sizeof(head[i]));
		head[i].block_size = sizes[i];
		head[i].source_address = src_addrs[i];
		head[i].dest_address = dst_addrs[i];
		head[i].next_block = (i + 1 < n) ? &head[i + 1] : NULL;
	}
}

static void prep_dxe_transfer(struct dma_config *cfg, uint8_t prio, struct dma_block_config *head,
			      int nblocks, enum dma_channel_direction dir, void (*cb)())
{
	cfg->channel_direction = dir;
	cfg->source_data_size = 4U;
	cfg->dest_data_size = 4U;
	cfg->source_burst_length = 1U;
	cfg->dest_burst_length = 1U;
	cfg->dma_callback = cb;
	cfg->block_count = nblocks;
	cfg->head_block = head;
	cfg->channel_priority = prio;
}

static int run_dxe_transfer(const struct device *dxe, uint32_t channel, uint8_t prio,
			    struct dma_block_config *head, int nblocks,
			    enum dma_channel_direction dir)
{
	struct dma_config cfg = (struct dma_config){0};

	prep_dxe_transfer(&cfg, prio, head, nblocks, dir, dxe_cb);

	cb_status = -EAGAIN;
	k_sem_reset(&done_sem);

	int ret = dma_config(dxe, channel, &cfg);
	if (ret) {
		return ret;
	}

	ret = dma_start(dxe, channel);
	if (ret) {
		return ret;
	}

	if (k_sem_take(&done_sem, K_SECONDS(1)) != 0) {
		return -ETIMEDOUT;
	}
	return cb_status;
}

static void verify_dxe_transfer(const uint32_t *src_addrs, const uint32_t *dst_addrs,
				const size_t *sizes, int nblocks, const char *label)
{
	for (int i = 0; i < nblocks; i++) {
		const void *s = (const void *)src_addrs[i];
		const void *d = (const void *)dst_addrs[i];
		size_t n = sizes[i];

		zassert_mem_equal(d, s, n, "%s: block %d mismatch (len=%u)", label, i, (unsigned)n);
	}
}

static void get_src_dst_addrs(uint32_t *arr, uint8_t *base, const size_t *sizes, int n,
			      size_t start_off)
{
	/* ensure initial offset is 4-byte aligned */
	size_t off = (start_off + 3u) & ~3u;

	for (int i = 0; i < n; i++) {
		/* every block must start aligned (needed for DXE & RRAM CPU reads) */
		off = (off + 3u) & ~3u;
		arr[i] = (uint32_t)(base + off);

		off += sizes[i];
	}
}

typedef struct dxe_case {
	const char *name;
	int nblocks;
	size_t sizes[3];
	uint32_t channel;
	uint8_t priority;
} dxe_case_t;

static const dxe_case_t dxe_test_cases[] = {
	{"1 block, 128B, channel 0, priority 0", 1, {128, 0, 0}, 0, 0},
	{"3 blocks, 64B each, channel 0, priority 3", 3, {64, 64, 64}, 0, 3},
	{"3 blocks, 64B, 128B, 256B, channel 1, priority 7", 3, {64, 128, 256}, 1, 7},
};

/* DXE transfer test:
 * 1) SRAM to SRAM
 * 2) SRAM to RRAM
 * 3) RRAM to RRAM
 * 4) RRAM to SRAM
 */
ZTEST(dxe_suite, test_dxe_transfer)
{
	int run_ret = 0;
	const struct device *dxe = get_dxe_dev();

	for (size_t t = 0; t < ARRAY_SIZE(dxe_test_cases); t++) {
		const dxe_case_t *c = &dxe_test_cases[t];
		uint32_t s_src[3], s_dst[3], r_src[3], r_dst[3];

		get_src_dst_addrs(s_src, sram_src, c->sizes, c->nblocks, 0);
		get_src_dst_addrs(s_dst, sram_dst, c->sizes, c->nblocks, 0);
		get_src_dst_addrs(r_src, rram_src, c->sizes, c->nblocks, 0);
		get_src_dst_addrs(r_dst, rram_dst, c->sizes, c->nblocks, 0);

		/* SRAM to SRAM */
		TC_PRINT("%s: SRAM to SRAM\n", c->name);
		for (int i = 0; i < c->nblocks; i++) {
			memset((void *)s_src[i], 0x5A + i, c->sizes[i]);
			memset((void *)s_dst[i], 0x1E, c->sizes[i]);
		}
		struct dma_block_config blocks_ss[3];
		build_dxe_transfer_blocks(blocks_ss, c->nblocks, s_src, s_dst, c->sizes);
		run_ret = run_dxe_transfer(dxe, c->channel, c->priority, &blocks_ss[0], c->nblocks,
					   MEMORY_TO_MEMORY);
		zassert_equal(run_ret, 0, "SRAM to SRAM transfer failed");
		verify_dxe_transfer(s_src, s_dst, c->sizes, c->nblocks, "SRAM to SRAM");

		/* SRAM to RRAM */
		TC_PRINT("%s: SRAM to RRAM\n", c->name);
		for (int i = 0; i < c->nblocks; i++) {
			memset((void *)s_src[i], 0x11 + i, c->sizes[i]);
		}
		struct dma_block_config blocks_sr[3];
		build_dxe_transfer_blocks(blocks_sr, c->nblocks, s_src, r_dst, c->sizes);
		run_ret = run_dxe_transfer(dxe, c->channel, c->priority, &blocks_sr[0], c->nblocks,
					   MEMORY_TO_MEMORY);
		zassert_equal(run_ret, 0, "SRAM to RRAM transfer failed");
		verify_dxe_transfer(s_src, r_dst, c->sizes, c->nblocks, "SRAM to RRAM");

		/* RRAM to RRAM */
		TC_PRINT("%s: RRAM to RRAM\n", c->name);
		struct dma_block_config blocks_rr[3];
		build_dxe_transfer_blocks(blocks_rr, c->nblocks, r_src, r_dst, c->sizes);
		run_ret = run_dxe_transfer(dxe, c->channel, c->priority, &blocks_rr[0], c->nblocks,
					   MEMORY_TO_MEMORY);
		zassert_equal(run_ret, 0, "RRAM to RRAM transfer failed");
		verify_dxe_transfer(r_src, r_dst, c->sizes, c->nblocks, "RRAM to RRAM");

		/* RRAM to SRAM */
		TC_PRINT("%s: RRAM to SRAM\n", c->name);
		for (int i = 0; i < c->nblocks; i++) {
			memset((void *)s_dst[i], 0xEE, c->sizes[i]);
		}
		struct dma_block_config blocks_rs[3];
		build_dxe_transfer_blocks(blocks_rs, c->nblocks, r_src, s_dst, c->sizes);
		run_ret = run_dxe_transfer(dxe, c->channel, c->priority, &blocks_rs[0], c->nblocks,
					   MEMORY_TO_MEMORY);
		zassert_equal(run_ret, 0, "RRAM to SRAM transfer failed");
		verify_dxe_transfer(r_src, s_dst, c->sizes, c->nblocks, "RRAM to SRAM");
	}
}

/* Stop transfer test */
ZTEST(dxe_suite, test_stop_transfer)
{
	const struct device *dxe = get_dxe_dev();

	struct dma_block_config b = {0};
	b.block_size = MAX_BLOCK_SIZE;
	b.source_address = (uint32_t)(uintptr_t)sram_src;
	b.dest_address = (uint32_t)(uintptr_t)sram_dst;

	memset(sram_src, 0x55, b.block_size);
	memset(sram_dst, 0xAA, b.block_size);

	struct dma_config cfg = (struct dma_config){0};
	cfg.channel_direction = MEMORY_TO_MEMORY;
	cfg.dma_callback = dxe_cb;
	cfg.block_count = 1;
	cfg.head_block = &b;

	int ret = dma_config(dxe, 0, &cfg);
	zassert_ok(ret, "dma_config failed (%d)", ret);

	ret = dma_start(dxe, 0);
	zassert_ok(ret, "dma_start failed (%d)", ret);

	ret = dma_stop(dxe, 0);
	zassert_ok(ret, "dma_stop failed (%d)", ret);

	int cmp = memcmp(sram_src, sram_dst, b.block_size);
	zassert_not_equal(cmp, 0, "DMA stop failed, destination matches source");
}

/* Boundary xfr size test */
ZTEST(dxe_suite, test_bounday_xfr_size)
{
	const struct device *dxe = get_dxe_dev();

	struct dma_block_config b = {0};
	b.block_size = MAX_BLOCK_SIZE;
	b.source_address = (uint32_t)(uintptr_t)sram_src;
	b.dest_address = (uint32_t)(uintptr_t)sram_dst;

	memset(sram_src, 0x55, MAX_BLOCK_SIZE);
	memset(sram_dst, 0xAA, MAX_BLOCK_SIZE);

	struct dma_config cfg = (struct dma_config){0};
	cfg.channel_direction = MEMORY_TO_MEMORY;
	cfg.dma_callback = dxe_cb;
	cfg.block_count = 1;
	cfg.head_block = &b;

	int ret = dma_config(dxe, 0, &cfg);
	zassert_ok(ret, "dma_config failed (%d)", ret);

	ret = dma_start(dxe, 0);
	zassert_ok(ret, "dma_start failed (%d)", ret);

	/* Get transaction status */
	struct dma_status status;
	ret = dma_get_status(dxe, 0, &status);
	TC_PRINT("DMA status: ret=%d busy=%u dir=%u pend_len=%u\n", ret, status.busy, status.dir,
		 status.pending_length);
	zassert_equal(ret, 0, "dma_get_status failed (%d)", ret);
	zassert_true(status.busy, "Expected DMA to be busy");
	zassert_equal(status.dir, MEMORY_TO_MEMORY, "Unexpected DMA direction");
	zassert_between_inclusive(status.pending_length, 1, MAX_BLOCK_SIZE,
				  "Unexpected pending length");

	/* wait for transfer to complete; fail if timed-out */
	if (k_sem_take(&done_sem, K_SECONDS(1)) != 0) {
		ztest_test_fail();
	}

	zassert_mem_equal(sram_dst, sram_src, b.block_size, "Maximum size transfer failed");
}

/* Error handling test */
ZTEST(dxe_suite, test_error_cases)
{
	const struct device *dxe = get_dxe_dev();

	/* Zero-length block */
	{
		struct dma_block_config b = {0};
		b.block_size = 0;
		b.source_address = (uint32_t)(uintptr_t)sram_src;
		b.dest_address = (uint32_t)(uintptr_t)sram_dst;

		struct dma_config cfg = (struct dma_config){0};
		cfg.channel_direction = MEMORY_TO_MEMORY;
		cfg.dma_callback = dxe_cb;
		cfg.block_count = 1;
		cfg.head_block = &b;

		int ret = dma_config(dxe, 0, &cfg);
		zassert_true(ret < 0, "Expected -EINVAL for zero block size");
	}

	/* Oversized block (> 0x3FFC) */
	{
		struct dma_block_config b = {0};
		b.block_size = 0x3FFC + 1U;
		b.source_address = (uint32_t)(uintptr_t)sram_src;
		b.dest_address = (uint32_t)(uintptr_t)sram_dst;

		struct dma_config cfg = (struct dma_config){0};
		cfg.channel_direction = MEMORY_TO_MEMORY;
		cfg.dma_callback = dxe_cb;
		cfg.block_count = 1;
		cfg.head_block = &b;

		int ret = dma_config(dxe, 0, &cfg);
		zassert_equal(ret, -EINVAL, "Expected -EINVAL for oversized block");
	}

	/* Misaligned address */
	{
		struct dma_block_config b = {0};
		b.block_size = 64;
		b.source_address = (uint32_t)(uintptr_t)(sram_src + 1); /* misaligned */
		b.dest_address = (uint32_t)(uintptr_t)sram_dst;

		struct dma_config cfg = (struct dma_config){0};
		cfg.channel_direction = MEMORY_TO_MEMORY;
		cfg.dma_callback = dxe_cb;
		cfg.block_count = 1;
		cfg.head_block = &b;

		int ret = dma_config(dxe, 0, &cfg);
		zassert_equal(ret, -EINVAL, "Expected -EINVAL for misaligned source");
	}

	/* Unsupported direction */
	{
		struct dma_block_config b = {0};
		b.block_size = 64;
		b.source_address = (uint32_t)(uintptr_t)sram_src;
		b.dest_address = (uint32_t)(uintptr_t)sram_dst;

		struct dma_config cfg = (struct dma_config){0};
		cfg.channel_direction = PERIPHERAL_TO_MEMORY;
		cfg.dma_callback = dxe_cb;
		cfg.block_count = 1;
		cfg.head_block = &b;

		int ret = dma_config(dxe, 0, &cfg);
		zassert_equal(ret, -EINVAL, "Expected -EINVAL for unsupported direction");
	}

	/* NULL head_block */
	{
		struct dma_config cfg = (struct dma_config){0};
		cfg.channel_direction = MEMORY_TO_MEMORY;
		cfg.dma_callback = dxe_cb;
		cfg.block_count = 1;
		cfg.head_block = NULL;

		int ret = dma_config(dxe, 0, &cfg);
		zassert_equal(ret, -EINVAL, "Expected -EINVAL for NULL head_block");
	}

	/* Channel busy */
	{
		uint32_t s_src[5], s_dst[5];
		size_t sizes[5] = {[0 ... 4] = 0x3FFC};
		get_src_dst_addrs(s_src, sram_src, sizes, 5, 0);
		get_src_dst_addrs(s_dst, sram_dst, sizes, 5, 0);
		for (int i = 0; i < 5; i++) {
			memset((void *)s_src[i], 0x5A + i, sizes[i]);
			memset((void *)s_dst[i], 0x1E, sizes[i]);
		}
		struct dma_block_config blocks[5];
		build_dxe_transfer_blocks(blocks, ARRAY_SIZE(blocks), s_src, s_dst, sizes);
		struct dma_config cfg = (struct dma_config){0};
		prep_dxe_transfer(&cfg, 0, blocks, 5, MEMORY_TO_MEMORY, dxe_cb);
		if (dma_config(dxe, 0, &cfg) < 0) {
			ztest_test_fail();
		}
		if (dma_start(dxe, 0) < 0) {
			ztest_test_fail();
		}
		int ret = dma_config(dxe, 0, &cfg);
		zassert_equal(ret, -EBUSY, "Expected -EBUSY for channel busy");

		/* wait for current transfer to complete; fail if timed-out */
		if (k_sem_take(&done_sem, K_SECONDS(1)) != 0) {
			ztest_test_fail();
		}

		if (dma_config(dxe, 0, &cfg) < 0) {
			ztest_test_fail();
		}
		if (dma_start(dxe, 0) < 0) {
			ztest_test_fail();
		}
		ret = dma_start(dxe, 0);
		zassert_equal(ret, -EBUSY, "Expected -EBUSY for channel busy");
	}

	/* NULL status */
	{
		int ret = dma_get_status(dxe, 0, NULL);
		zassert_equal(ret, -EINVAL, "Expected -EINVAL for NULL status");
	}
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

ZTEST(dxe_suite, test_pm)
{
	const struct device *dxe = get_dxe_dev();
	dma_pm_suspend_resume_check(dxe);
}

/* Register the suite after all tests are defined */
ZTEST_SUITE(dxe_suite, NULL, NULL, NULL, NULL, NULL);
