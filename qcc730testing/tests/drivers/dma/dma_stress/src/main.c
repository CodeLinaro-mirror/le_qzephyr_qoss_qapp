/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <zephyr/ztest.h>
#include <zephyr/ztress.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/sys/util.h>
#include <zephyr/linker/devicetree_regions.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/pm/device.h>
#include <zephyr/logging/log.h>

#include <string.h>

LOG_MODULE_REGISTER(dma_stress, CONFIG_DMA_LOG_LEVEL);

/* DXE device */
#define DXE_NODE DT_NODELABEL(dxe0)

/* Test configuration */
#define ZTRESS_TIMEOUT          K_SECONDS(12)
#define ZTRESS_ITERATIONS       3000U
#define ZTRESS_NO_PREEMPTION    0U
#define MIN_OPS_PER_SECOND      100U
#define ERROR_TOLERANCE_PERCENT 1U

/* Buffer sizes */
#define MAX_BLOCK_SIZE    0x1000U
#define LARGE_BLOCK_SIZE  (MAX_BLOCK_SIZE / 2U)
#define MEDIUM_BLOCK_SIZE (LARGE_BLOCK_SIZE / 2U)
#define SMALL_BLOCK_SIZE  (MEDIUM_BLOCK_SIZE / 2U)

#define NUM_STRESS_CHANNELS 6
#define STRESS_BUFFER_SIZE  MAX_BLOCK_SIZE

/* SRAM buffers */
static __aligned(4) uint8_t sram_src[NUM_STRESS_CHANNELS][STRESS_BUFFER_SIZE];
static __aligned(4) uint8_t sram_dst[NUM_STRESS_CHANNELS][STRESS_BUFFER_SIZE];

/* RRAM buffers */
static __aligned(4) uint8_t rram_dst[NUM_STRESS_CHANNELS][STRESS_BUFFER_SIZE] Z_GENERIC_SECTION(
	LINKER_DT_NODE_REGION_NAME(DT_NODELABEL(rram)));

/* Semaphores for DMA completion tracking - one per channel */
static K_SEM_DEFINE(dma_done_sem0, 0, 1);
static K_SEM_DEFINE(dma_done_sem1, 0, 1);
static K_SEM_DEFINE(dma_done_sem2, 0, 1);
static K_SEM_DEFINE(dma_done_sem3, 0, 1);
static K_SEM_DEFINE(dma_done_sem4, 0, 1);
static K_SEM_DEFINE(dma_done_sem5, 0, 1);

static struct k_sem *dma_done_sem[NUM_STRESS_CHANNELS] = {
	&dma_done_sem0, &dma_done_sem1, &dma_done_sem2,
	&dma_done_sem3, &dma_done_sem4, &dma_done_sem5,
};

/* Statistics tracking */
static atomic_t error_counter;

struct dma_ztress_context {
	const struct device *dev;
	atomic_t *errors;
	uint32_t operations;
	uint32_t channel;
	size_t size;
	uint8_t priority;
	uint8_t pattern;
};

struct dma_stress_fixture {
	const struct device *dev;
};

/* DMA callback */
static void dma_callback(const struct device *dev, void *user_data, uint32_t channel, int status)
{
	ARG_UNUSED(dev);

	atomic_t *errors = (atomic_t *)user_data;

	if (status != 0) {
		atomic_inc(errors);
		LOG_ERR("DMA transfer failed on channel %u: %d", channel, status);
	}

	k_sem_give(dma_done_sem[channel]);
}

/* Execute DMA transfer */
static int dma_transfer(struct dma_ztress_context *ctx)
{
	struct dma_block_config block = {
		.block_size = ctx->size,
		.source_address = (uint32_t)(uintptr_t)sram_src[ctx->channel],
		.dest_address = (uint32_t)(uintptr_t)sram_dst[ctx->channel],
		.next_block = NULL,
	};

	struct dma_config config = {
		.channel_direction = MEMORY_TO_MEMORY,
		.dma_callback = dma_callback,
		.user_data = ctx->errors,
		.block_count = 1,
		.head_block = &block,
		.channel_priority = ctx->priority,
	};

	k_sem_reset(dma_done_sem[ctx->channel]);

	int ret = dma_config(ctx->dev, ctx->channel, &config);

	if (ret != 0) {
		atomic_inc(ctx->errors);
		LOG_ERR("DMA config failed on channel %u: %d", ctx->channel, ret);
		return ret;
	}

	ret = dma_start(ctx->dev, ctx->channel);

	if (ret != 0) {
		atomic_inc(ctx->errors);
		LOG_ERR("DMA start failed on channel %u: %d", ctx->channel, ret);
		return ret;
	}

	ret = k_sem_take(dma_done_sem[ctx->channel], K_SECONDS(1));

	if (ret != 0) {
		atomic_inc(ctx->errors);
		LOG_ERR("DMA failed to take semaphore on channel %u: %d", ctx->channel, ret);
		return -ETIMEDOUT;
	}

	return 0;
}

/* Execute multi-block DMA transfer */
static int dma_transfer_multiblock(struct dma_ztress_context *ctx, uint32_t *src_addrs,
				   uint32_t *dst_addrs, size_t *sizes, uint8_t nblocks)
{
	zassert_true(nblocks <= 3U, "Multi-block transfer limited to 3 blocks maximum!");
	struct dma_block_config blocks[3] = {0};

	for (uint8_t i = 0U; i < nblocks; i++) {
		blocks[i] = (struct dma_block_config){
			.block_size = sizes[i],
			.source_address = src_addrs[i],
			.dest_address = dst_addrs[i],
			.next_block = (i + 1 < nblocks) ? &blocks[i + 1] : NULL,
		};
	}

	struct dma_config config = {
		.channel_direction = MEMORY_TO_MEMORY,
		.dma_callback = dma_callback,
		.user_data = ctx->errors,
		.block_count = nblocks,
		.head_block = &blocks[0],
		.channel_priority = ctx->priority,
	};

	k_sem_reset(dma_done_sem[ctx->channel]);

	int ret = dma_config(ctx->dev, ctx->channel, &config);

	if (ret != 0) {
		atomic_inc(ctx->errors);
		LOG_ERR("DMA config failed on channel %u: %d", ctx->channel, ret);
		return ret;
	}

	ret = dma_start(ctx->dev, ctx->channel);

	if (ret != 0) {
		atomic_inc(ctx->errors);
		LOG_ERR("DMA start failed on channel %u: %d", ctx->channel, ret);
		return ret;
	}

	ret = k_sem_take(dma_done_sem[ctx->channel], K_SECONDS(1));

	if (ret != 0) {
		atomic_inc(ctx->errors);
		LOG_ERR("DMA failed to take semaphore on channel %u: %d", ctx->channel, ret);
		return -ETIMEDOUT;
	}

	return 0;
}

/* Ztress handler: Concurrent channel transfers */
static bool ztress_concurrent_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct dma_ztress_context *ctx = (struct dma_ztress_context *)user_data;
	int ret = 0;

	(void)memset(sram_src[ctx->channel], ctx->pattern, ctx->size);
	(void)memset(sram_dst[ctx->channel], 0x00, ctx->size);

	ret = dma_transfer(ctx);

	ctx->operations++;

	if (last) {
		if (ret == 0) {
			zassert_mem_equal(
				sram_src[ctx->channel], sram_dst[ctx->channel], ctx->size,
				"Data mismatch in ztress_concurrent_handler on channel %u",
				ctx->channel);
		} else {
			LOG_ERR("Failed in ztress_concurrent_handler, memory after "
				"test will not be checked!");
		}
		return false;
	}

	return ret == 0;
}

/* Ztress handler: Rapid small transfers */
static bool ztress_rapid_small_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct dma_ztress_context *ctx = (struct dma_ztress_context *)user_data;
	int ret = 0;

	(void)memset(sram_src[ctx->channel], cnt & 0xFF, ctx->size);
	(void)memset(sram_dst[ctx->channel], 0x00, ctx->size);

	ret = dma_transfer(ctx);

	ctx->operations++;

	if (last) {
		if (ret == 0) {
			zassert_mem_equal(
				sram_src[ctx->channel], sram_dst[ctx->channel], ctx->size,
				"Data mismatch in ztress_rapid_small_handler on channel %u",
				ctx->channel);
		} else {
			LOG_ERR("Failed in ztress_concurrent_handler, memory after "
				"test will not be checked!");
		}
		return false;
	}

	return ret == 0;
}

/* Ztress handler: Memory region transfers */
static bool ztress_memory_region_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct dma_ztress_context *ctx = (struct dma_ztress_context *)user_data;
	int ret = 0;

	(void)memset(sram_src[ctx->channel], 0xAA, ctx->size);

	/* SRAM to RRAM transfer */
	struct dma_block_config block = {
		.block_size = ctx->size,
		.source_address = (uint32_t)(uintptr_t)sram_src[ctx->channel],
		.dest_address = (uint32_t)(uintptr_t)rram_dst[ctx->channel],
		.next_block = NULL,
	};

	struct dma_config config = {
		.channel_direction = MEMORY_TO_MEMORY,
		.dma_callback = dma_callback,
		.user_data = ctx->errors,
		.block_count = 1,
		.head_block = &block,
		.channel_priority = ctx->priority,
	};

	k_sem_reset(dma_done_sem[ctx->channel]);

	ret = dma_config(ctx->dev, ctx->channel, &config);

	if (ret == 0) {
		ret = dma_start(ctx->dev, ctx->channel);
		if (ret == 0) {
			ret = k_sem_take(dma_done_sem[ctx->channel], K_SECONDS(1)) == 0
				      ? 0
				      : -ETIMEDOUT;
		}
	}

	if (ret != 0) {
		atomic_inc(ctx->errors);
	}

	ctx->operations++;

	if (last) {
		if (ret == 0) {
			zassert_mem_equal(
				sram_src[ctx->channel], rram_dst[ctx->channel], ctx->size,
				"Data mismatch in ztress_memory_region_handler on channel %u",
				ctx->channel);
		} else {
			LOG_ERR("Failed in ztress_memory_region_handler, memory after test "
				"will not be checked!");
		}
		return false;
	}

	return ret == 0;
}

/* Ztress handler: Variable block sizes */
static bool ztress_block_sizes_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct dma_ztress_context *ctx = (struct dma_ztress_context *)user_data;
	int ret = 0;

	(void)memset(sram_src[ctx->channel], ctx->pattern, ctx->size);
	(void)memset(sram_dst[ctx->channel], 0x00, ctx->size);

	ret = dma_transfer(ctx);

	ctx->operations++;

	if (last) {
		if (ret == 0) {
			zassert_mem_equal(
				sram_src[ctx->channel], sram_dst[ctx->channel], ctx->size,
				"Data mismatch in ztress_block_sizes_handler on channel %u",
				ctx->channel);
		} else {
			LOG_ERR("Failed in ztress_block_sizes_handler, memory after test "
				"will not be checked!");
		}
		return false;
	}

	return ret == 0;
}

/* Ztress handler: Multi-block transfers */
static bool ztress_multiblock_handler(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct dma_ztress_context *ctx = (struct dma_ztress_context *)user_data;
	const uint8_t nblocks = 3U;
	size_t sizes[3] = {SMALL_BLOCK_SIZE, MEDIUM_BLOCK_SIZE, LARGE_BLOCK_SIZE};
	uint32_t src_addrs[3], dst_addrs[3];
	size_t offset = 0U;
	int ret = 0;

	for (int i = 0; i < nblocks; i++) {
		src_addrs[i] = (uint32_t)(uintptr_t)&sram_src[ctx->channel][offset];
		dst_addrs[i] = (uint32_t)(uintptr_t)&sram_dst[ctx->channel][offset];
		(void)memset(&sram_src[ctx->channel][offset], 0x33 + i, sizes[i]);
		(void)memset(&sram_dst[ctx->channel][offset], 0x00, sizes[i]);
		offset += sizes[i];
	}

	zassert_true(offset <= STRESS_BUFFER_SIZE,
		     "Multi-block total size %zu exceeds buffer size %u", offset,
		     STRESS_BUFFER_SIZE);

	ret = dma_transfer_multiblock(ctx, src_addrs, dst_addrs, sizes, nblocks);

	ctx->operations++;

	if (last) {
		if (ret == 0) {
			zassert_mem_equal(
				sram_src[ctx->channel], sram_dst[ctx->channel], offset,
				"Data mismatch in ztress_multiblock_handler on channel %u",
				ctx->channel);
		} else {
			LOG_ERR("Failed in ztress_multiblock_handler, memory after test "
				"will not be checked!");
		}
		return false;
	}

	return ret == 0;
}

/* Helper: Calculate total operations */
static uint32_t get_total_operations(struct dma_ztress_context *ctx, uint8_t count)
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
	const atomic_val_t errors = atomic_get(&error_counter);
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

	zassert_true(elapsed_ms <= timeout_ms, "Test should complete before timeout");
	zassert_true(total_ops >= min_expected, "Operations %u below minimum %u", total_ops,
		     min_expected);

	zassert_true(ops_per_sec >= MIN_OPS_PER_SECOND, "Throughput %u ops/sec below minimum %u",
		     ops_per_sec, MIN_OPS_PER_SECOND);

	max_errors = (total_ops * ERROR_TOLERANCE_PERCENT) / 100U;
	zassert_true(atomic_get(&error_counter) <= max_errors, "Too many errors: %ld (max: %u)",
		     atomic_get(&error_counter), max_errors);
}

/* Helper: DMA cleanup */
static void dma_cleanup(struct dma_stress_fixture *fixture)
{
	atomic_set(&error_counter, 0);

	/* Stop all channels */
	for (uint8_t i = 0U; i < NUM_STRESS_CHANNELS; i++) {
		dma_stop(fixture->dev, i);
		k_sem_reset(dma_done_sem[i]);
	}
}

/* Test setup */
static void *dma_stress_setup(void)
{
	static struct dma_stress_fixture fixture = {0};
	fixture.dev = DEVICE_DT_GET(DXE_NODE);
	zassert_true(device_is_ready(fixture.dev), "DMA device not ready");
	return &fixture;
}

/* Before each test */
static void dma_stress_before(void *f)
{
	dma_cleanup((struct dma_stress_fixture *)f);
}

/* After each test */
static void dma_stress_teardown(void *f)
{
	dma_cleanup((struct dma_stress_fixture *)f);
}

/* Test: Concurrent channel operations */
ZTEST_F(dma_stress, test_dma_concurrent_channels)
{
	const uint8_t concurrent_operations = 3U;
	struct dma_ztress_context ctx[3] = {{.dev = fixture->dev,
					     .errors = &error_counter,
					     .operations = 0,
					     .channel = 0,
					     .size = MEDIUM_BLOCK_SIZE,
					     .priority = 0,
					     .pattern = 0x11},
					    {.dev = fixture->dev,
					     .errors = &error_counter,
					     .operations = 0,
					     .channel = 1,
					     .size = MEDIUM_BLOCK_SIZE,
					     .priority = 4,
					     .pattern = 0x22},
					    {.dev = fixture->dev,
					     .errors = &error_counter,
					     .operations = 0,
					     .channel = 2,
					     .size = MEDIUM_BLOCK_SIZE,
					     .priority = 7,
					     .pattern = 0x33}};
	uint32_t total_ops = 0U;
	int64_t start_time = 0, elapsed_time = 0;
	const uint32_t min_expected = ZTRESS_ITERATIONS * concurrent_operations;

	LOG_INF("Starting DMA concurrent channels test");

	ztress_set_timeout(ZTRESS_TIMEOUT);
	start_time = k_uptime_get();

	ZTRESS_EXECUTE(ZTRESS_THREAD(ztress_concurrent_handler, &ctx[0], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(50)),
		       ZTRESS_THREAD(ztress_concurrent_handler, &ctx[1], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(50)),
		       ZTRESS_THREAD(ztress_concurrent_handler, &ctx[2], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(50)));

	elapsed_time = k_uptime_get() - start_time;
	total_ops = get_total_operations(ctx, concurrent_operations);

	LOG_INF("Concurrent test completed:");
	for (int i = 0; i < concurrent_operations; i++) {
		LOG_INF("  Channel %u operations: %u", ctx[i].channel, ctx[i].operations);
	}
	LOG_INF("  Total errors: %ld", atomic_get(&error_counter));

	check_errors();
	check_performance(total_ops, elapsed_time, min_expected,
			  k_ticks_to_ms_floor32(ZTRESS_TIMEOUT.ticks));
}

/* Test: Rapid reconfiguration */
ZTEST_F(dma_stress, test_dma_rapid_reconfig)
{
	const uint8_t concurrent_operations = 2U;
	struct dma_ztress_context ctx[2] = {{.dev = fixture->dev,
					     .errors = &error_counter,
					     .operations = 0,
					     .channel = 0,
					     .size = SMALL_BLOCK_SIZE,
					     .priority = 2,
					     .pattern = 0},
					    {.dev = fixture->dev,
					     .errors = &error_counter,
					     .operations = 0,
					     .channel = 1,
					     .size = MEDIUM_BLOCK_SIZE,
					     .priority = 3,
					     .pattern = 0}};
	uint32_t total_ops = 0U;
	int64_t start_time = 0, elapsed_time = 0;
	const uint32_t min_expected = ZTRESS_ITERATIONS * concurrent_operations;

	LOG_INF("Starting DMA rapid reconfiguration test");

	ztress_set_timeout(ZTRESS_TIMEOUT);
	start_time = k_uptime_get();

	ZTRESS_EXECUTE(ZTRESS_THREAD(ztress_rapid_small_handler, &ctx[0], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(10)),
		       ZTRESS_THREAD(ztress_rapid_small_handler, &ctx[1], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(30)));

	elapsed_time = k_uptime_get() - start_time;
	total_ops = get_total_operations(ctx, concurrent_operations);

	LOG_INF("Rapid reconfig test completed:");
	for (int i = 0; i < concurrent_operations; i++) {
		LOG_INF("  Channel %u operations: %u", ctx[i].channel, ctx[i].operations);
	}
	LOG_INF("  Total errors: %ld", atomic_get(&error_counter));

	check_errors();
	check_performance(total_ops, elapsed_time, min_expected,
			  k_ticks_to_ms_floor32(ZTRESS_TIMEOUT.ticks));
}

/* Test: Memory regions */
ZTEST_F(dma_stress, test_dma_memory_regions)
{
	const uint8_t concurrent_operations = 1U;
	struct dma_ztress_context ctx[1] = {{.dev = fixture->dev,
					     .errors = &error_counter,
					     .operations = 0,
					     .channel = 0,
					     .size = MEDIUM_BLOCK_SIZE,
					     .priority = 1,
					     .pattern = 0xAA}};
	uint32_t total_ops = 0U;
	int64_t start_time = 0, elapsed_time = 0;
	const uint32_t min_expected = ZTRESS_ITERATIONS;

	LOG_INF("Starting DMA memory regions test");

	ztress_set_timeout(ZTRESS_TIMEOUT);
	start_time = k_uptime_get();

	ZTRESS_EXECUTE(ZTRESS_THREAD(ztress_memory_region_handler, &ctx[0], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(40)));

	elapsed_time = k_uptime_get() - start_time;
	total_ops = get_total_operations(ctx, concurrent_operations);

	LOG_INF("Memory regions test completed:");
	LOG_INF("  Channel 0 operations: %u", ctx[0].operations);
	LOG_INF("  Total errors: %ld", atomic_get(&error_counter));

	check_errors();
	check_performance(total_ops, elapsed_time, min_expected,
			  k_ticks_to_ms_floor32(ZTRESS_TIMEOUT.ticks));
}

/* Test: Block sizes */
ZTEST_F(dma_stress, test_dma_block_sizes)
{
	const uint8_t concurrent_operations = 3U;
	struct dma_ztress_context ctx[3] = {{.dev = fixture->dev,
					     .errors = &error_counter,
					     .operations = 0,
					     .channel = 0,
					     .size = SMALL_BLOCK_SIZE,
					     .priority = 0,
					     .pattern = 0x11},
					    {.dev = fixture->dev,
					     .errors = &error_counter,
					     .operations = 0,
					     .channel = 1,
					     .size = LARGE_BLOCK_SIZE,
					     .priority = 7,
					     .pattern = 0x22},
					    {.dev = fixture->dev,
					     .errors = &error_counter,
					     .operations = 0,
					     .channel = 2,
					     .size = 0,
					     .priority = 4,
					     .pattern = 0x33}};
	uint32_t total_ops = 0U;
	int64_t start_time = 0, elapsed_time = 0;
	const uint32_t min_expected = ZTRESS_ITERATIONS * concurrent_operations;

	LOG_INF("Starting DMA block sizes test");

	ztress_set_timeout(ZTRESS_TIMEOUT);
	start_time = k_uptime_get();

	ZTRESS_EXECUTE(ZTRESS_THREAD(ztress_block_sizes_handler, &ctx[0], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(10)),
		       ZTRESS_THREAD(ztress_block_sizes_handler, &ctx[1], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(100)),
		       ZTRESS_THREAD(ztress_multiblock_handler, &ctx[2], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(60)));

	elapsed_time = k_uptime_get() - start_time;
	total_ops = get_total_operations(ctx, concurrent_operations);

	LOG_INF("Block sizes test completed:");
	for (int i = 0; i < concurrent_operations; i++) {
		LOG_INF("  Channel %u operations: %u", ctx[i].channel, ctx[i].operations);
	}
	LOG_INF("  Total errors: %ld", atomic_get(&error_counter));

	check_errors();
	check_performance(total_ops, elapsed_time, min_expected,
			  k_ticks_to_ms_floor32(ZTRESS_TIMEOUT.ticks));
}

/* Test: Priority stress */
ZTEST_F(dma_stress, test_dma_priority_stress)
{
	const uint8_t concurrent_operations = 2U;
	struct dma_ztress_context ctx[2] = {{.dev = fixture->dev,
					     .errors = &error_counter,
					     .operations = 0,
					     .channel = 0,
					     .size = LARGE_BLOCK_SIZE,
					     .priority = 0,
					     .pattern = 0xDD},
					    {.dev = fixture->dev,
					     .errors = &error_counter,
					     .operations = 0,
					     .channel = 1,
					     .size = SMALL_BLOCK_SIZE,
					     .priority = 7,
					     .pattern = 0xEE}};
	uint32_t total_ops = 0U;
	int64_t start_time = 0, elapsed_time = 0;
	const uint32_t min_expected = ZTRESS_ITERATIONS * concurrent_operations;

	LOG_INF("Starting DMA priority stress test");

	ztress_set_timeout(ZTRESS_TIMEOUT);
	start_time = k_uptime_get();

	ZTRESS_EXECUTE(ZTRESS_THREAD(ztress_block_sizes_handler, &ctx[0], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(100)),
		       ZTRESS_THREAD(ztress_block_sizes_handler, &ctx[1], ZTRESS_ITERATIONS,
				     ZTRESS_NO_PREEMPTION, Z_TIMEOUT_TICKS(10)));

	elapsed_time = k_uptime_get() - start_time;
	total_ops = get_total_operations(ctx, concurrent_operations);

	LOG_INF("Priority stress test completed:");
	LOG_INF("  Low priority (ch0) operations: %u", ctx[0].operations);
	LOG_INF("  High priority (ch1) operations: %u", ctx[1].operations);
	LOG_INF("  Total errors: %ld", atomic_get(&error_counter));

	check_errors();
	check_performance(total_ops, elapsed_time, min_expected,
			  k_ticks_to_ms_floor32(ZTRESS_TIMEOUT.ticks));
}

ZTEST_SUITE(dma_stress, NULL, dma_stress_setup, dma_stress_before, dma_stress_teardown, NULL);