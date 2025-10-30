/*
 * Copyright (c) 2025 Qualcomm Technologies
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <zephyr/ztest.h>
#include <zephyr/ztress.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/crc.h>

#include <string.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(qspi_flash_stress, LOG_LEVEL_INF);

#define ZTRESS_ITERATIONS                  3000U
#define ZTRESS_ERASE_WRITE_READ_ITERATIONS 1000U
#define ZTRESS_TEST_TIMEOUT_MS             60000U
#define ZTRESS_MINIMUM_OPERATIONS_PER_SEC  30U
#define ZTRESS_MINIMUM_READ_KIBS           100.0
#define ZTRESS_MINIMUM_WRITE_KIBS          100.0
#define ZTRESS_ERRORS_PERCENTAGE_TOLERANCE 1 /* % */

/* Default test region: last 64 KiB of the flash */
#ifndef QSPI_STRESS_REGION_SIZE
#define QSPI_STRESS_REGION_SIZE KB(64U)
#endif

#define PATTERN_SEED 0x5A5A5A5A

#define MAX_BUFFER_SIZE 1024U

/* The driver to test */
static const struct device *const flash_dev = DEVICE_DT_GET_ONE(qcom_qspi_qcc730_nor);

/* Global counters */
static atomic_t operations_cnt;
static atomic_t errors_cnt;
static atomic_t timeouts_cnt;

/* Fixture */
struct qspi_flash_stress_fixture {
	uint32_t region_offs;
	size_t region_size;
	size_t sector_size;
	size_t page_size;
	bool has_page_layout;
	/* Backup of original contents */
	uint8_t *backup_buf;
};

/* Thread stats */
struct qspi_thread_ctx {
	const struct device *dev;
	uint32_t region_offs;
	size_t region_size;
	size_t page_size;
	size_t sector_size;
	uint32_t thread_id;
	uint32_t total_threads;

	uint32_t operations;
	uint32_t errors;
	uint32_t timeouts;

	/* Bandwidth stats */
	uint64_t rd_bytes;
	uint64_t rd_time_cycles;
	uint64_t wr_bytes;
	uint64_t wr_time_cycles;
};

/* CPU frequency to calculate bandwidth */
static uint32_t cpu_freq_hz = CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC;

/* Helpers */

/* Convert errno to string */
static const char *err2str(int err)
{
	return (err == 0) ? "OK" : strerror(-err);
}

/* Check performance for no. of operations, elapsed time, minimum operations, read/write speed*/
static void perf_check(uint32_t total_ops, int64_t elapsed_ms, uint32_t min_ops,
		       uint32_t timeout_ms, double *read_kibs, double *write_kibs)
{
	uint32_t ops_per_sec = (uint32_t)((uint64_t)total_ops * 1000U / (uint64_t)elapsed_ms);
	LOG_INF("  Performance: %u ops/sec (elapsed %lld ms)", ops_per_sec, elapsed_ms);

	zassert_true(ops_per_sec >= ZTRESS_MINIMUM_OPERATIONS_PER_SEC,
		     "Operations per second too low: %u < %u", ops_per_sec,
		     ZTRESS_MINIMUM_OPERATIONS_PER_SEC);
	zassert_true(elapsed_ms < timeout_ms, "Test exceeded timeout");
	zassert_true(total_ops >= min_ops, "Insufficient ops: %u < %u", total_ops, min_ops);
	if (read_kibs) {
		zassert_true(*read_kibs >= ZTRESS_MINIMUM_READ_KIBS,
			     "Read bandwidth too low: %.2f KiB/s < %.2f KiB/s", *read_kibs,
			     ZTRESS_MINIMUM_READ_KIBS);
	}
	if (write_kibs) {
		zassert_true(*write_kibs >= ZTRESS_MINIMUM_WRITE_KIBS,
			     "Write bandwidth too low: %.2f KiB/s < %.2f KiB", *write_kibs,
			     ZTRESS_MINIMUM_WRITE_KIBS);
	}
}

/* Check error tolerance */
static void op_err_tolerance_check(uint32_t total_ops)
{
	int32_t tol = (total_ops * ZTRESS_ERRORS_PERCENTAGE_TOLERANCE) / 100U;
	LOG_INF("  Error tolerance: %d (%.u%% of %u)", tol, ZTRESS_ERRORS_PERCENTAGE_TOLERANCE,
		total_ops);
	zassert_within(atomic_get(&errors_cnt), 0, tol, "Too many errors");
	zassert_within(atomic_get(&timeouts_cnt), 0, tol, "Too many timeouts");
}

/* Dump bandwidth statistics for read and write */
static void dump_bw_stats(struct qspi_thread_ctx *ctx, uint8_t count, double *read_kibs,
			  double *write_kibs)
{
	uint64_t rd_bytes = 0, rd_cycles = 0, wr_bytes = 0, wr_cycles = 0;

	for (uint8_t i = 0; i < count; i++) {
		rd_bytes += ctx[i].rd_bytes;
		rd_cycles += ctx[i].rd_time_cycles;
		wr_bytes += ctx[i].wr_bytes;
		wr_cycles += ctx[i].wr_time_cycles;
	}

	*read_kibs = (rd_cycles > 0)
			     ? ((double)rd_bytes / 1024.0) / ((double)rd_cycles / cpu_freq_hz)
			     : 0.0;
	*write_kibs = (wr_cycles > 0)
			      ? ((double)wr_bytes / 1024.0) / ((double)wr_cycles / cpu_freq_hz)
			      : 0.0;

	LOG_INF("  Bandwidth summary:");
	LOG_INF("    READ:  bytes=%llu, time=%.3f s, avg=%.2f KiB/s", (unsigned long long)rd_bytes,
		(double)rd_cycles / cpu_freq_hz, *read_kibs);
	LOG_INF("    WRITE: bytes=%llu, time=%.3f s, avg=%.2f KiB/s", (unsigned long long)wr_bytes,
		(double)wr_cycles / cpu_freq_hz, *write_kibs);
}

/* Dump per-thread statistics */
static uint32_t dump_thread_stats(struct qspi_thread_ctx *ctx, uint8_t count)
{
	uint32_t tot = 0;
	for (uint8_t i = 0; i < count; i++) {
		LOG_INF("Thread[%u]: ops=%u, errs=%u, timeouts=%u", i, ctx[i].operations,
			ctx[i].errors, ctx[i].timeouts);
		tot += ctx[i].operations;
	}
	LOG_INF("  Total ops=%ld, errs=%ld, timeouts=%ld", atomic_get(&operations_cnt),
		atomic_get(&errors_cnt), atomic_get(&timeouts_cnt));
	return tot;
}

/* Generate pseudo-random sequence byte */
static inline uint8_t pr_byte(uint32_t seed, uint32_t offset)
{
	uint32_t v = seed ^ (uint32_t)offset * 1234567890u;
	v ^= (v >> 13);
	v *= 0xDEADBEEF;
	return (uint8_t)v;
}

/* Fill buffer with a pattern of pseudo-random sequence */
static void fill_pattern(uint8_t *buf, size_t len, uint32_t seed, uint32_t base_off)
{
	for (size_t i = 0; i < len; i++) {
		buf[i] = pr_byte(seed, base_off + (uint32_t)i);
	}
}

/* Verify buffer against pseudo-random sequence pattern */
static int verify_pattern(const uint8_t *buf, size_t len, uint32_t seed, uint32_t base_off)
{
	for (size_t i = 0; i < len; i++) {
		if (buf[i] != pr_byte(seed, base_off + (uint32_t)i)) {
			return -EIO;
		}
	}
	return 0;
}

/* Busy-wait small jitter */
static inline void tiny_delay(void)
{
	k_busy_wait(sys_rand32_get() & 0x3FF); /* up to ~1ms */
}

/* Calls flash_read and measures cycles it takes to execute */
static inline int timed_flash_read(struct qspi_thread_ctx *ctx, uint32_t off, void *buf, size_t len)
{
	uint32_t t0 = k_cycle_get_32();
	int ret = flash_read(ctx->dev, off, buf, len);
	uint64_t dt = k_cycle_get_32() - t0;

	if (ret == 0) {
		ctx->rd_bytes += len;
		ctx->rd_time_cycles += dt;
	}
	return ret;
}

/* Calls flash_write and measures cycles it takes to execute */
static inline int timed_flash_write(struct qspi_thread_ctx *ctx, uint32_t off, const void *buf,
				    size_t len)
{
	uint32_t t0 = k_cycle_get_32();
	int ret = flash_write(ctx->dev, off, buf, len);
	uint64_t dt = k_cycle_get_32() - t0;

	if (ret == 0) {
		ctx->wr_bytes += len;
		ctx->wr_time_cycles += dt;
	}
	return ret;
}

static void *qspi_stress_setup(void)
{
	static struct qspi_flash_stress_fixture fix = {0};

	zassume_true(device_is_ready(flash_dev), "Flash device not ready");

	const struct flash_parameters *p = flash_get_parameters(flash_dev);
	zassume_not_null(p, "flash_get_parameters returned NULL");
	fix.page_size = p->write_block_size;

#if IS_ENABLED(CONFIG_FLASH_PAGE_LAYOUT)
	struct flash_pages_info info;
	int rc = flash_get_page_info_by_offs(flash_dev, 0, &info);
	zassume_ok(rc, "No page info");
	fix.sector_size = info.size;
#else
	fix.has_page_layout = false;
	fix.sector_size = 4096U;
#endif

	uint64_t flash_size = 0;
	flash_get_size(flash_dev, &flash_size);

	/* Choose test region: last 64 KiB */
#ifdef QSPI_STRESS_REGION_OFFSET
	fix.region_offs = QSPI_STRESS_REGION_OFFSET;
#else
	fix.region_offs = (uint32_t)(flash_size - QSPI_STRESS_REGION_SIZE);
#endif
	fix.region_size = QSPI_STRESS_REGION_SIZE;

	/* Align region start to sector boundary to keep erases simple */
	uint32_t aligned =
		(fix.region_offs / (uint32_t)fix.sector_size) * (uint32_t)fix.sector_size;
	if (aligned != fix.region_offs) {
		LOG_WRN("Aligning test region from 0x%08x to sector boundary 0x%08x",
			(uint32_t)fix.region_offs, (uint32_t)aligned);
		fix.region_offs = aligned;
	}

	/* Allocate backup buffer and back up existing contents */
	LOG_INF("Backing up original contents...");
	fix.backup_buf = (uint8_t *)k_malloc(fix.region_size);
	zassume_not_null(fix.backup_buf, "Backup buffer alloc failed");
	int ret = flash_read(flash_dev, fix.region_offs, fix.backup_buf, fix.region_size);
	zassume_ok(ret, "Initial backup read failed: %s", err2str(ret));

	atomic_set(&operations_cnt, 0);
	atomic_set(&errors_cnt, 0);
	atomic_set(&timeouts_cnt, 0);

	LOG_INF("QSPI stress setup:");
	LOG_INF("  region: [0x%08x .. 0x%08x) size=%u", (uint32_t)fix.region_offs,
		(uint32_t)(fix.region_offs + fix.region_size), (uint32_t)fix.region_size);
	LOG_INF("  sector_size=%u, page_size=%u", (uint32_t)fix.sector_size,
		(uint32_t)fix.page_size);

	return &fix;
}

static void qspi_stress_before(void *f)
{
	struct qspi_flash_stress_fixture *fix = f;

	/* Clear global counters */
	atomic_set(&operations_cnt, 0);
	atomic_set(&errors_cnt, 0);
	atomic_set(&timeouts_cnt, 0);

	/* Erase test region before each test */
	int ret = flash_erase(flash_dev, fix->region_offs, fix->region_size);
	zassert_ok(ret, "Failed to erase test region before case: %s", err2str(ret));
}

static void qspi_stress_teardown(void *f)
{
	struct qspi_flash_stress_fixture *fix = f;

	/* Restore original flash contentss */
	if (fix->backup_buf) {
		int ret = flash_erase(flash_dev, fix->region_offs, fix->region_size);
		if (ret == 0) {
			ret = flash_write(flash_dev, fix->region_offs, fix->backup_buf,
					  fix->region_size);
		}
		if (ret != 0) {
			LOG_ERR("Failed to restore original contents: %s", err2str(ret));
		} else {
			LOG_INF("Restored original contents");
		}
		k_free(fix->backup_buf);
		fix->backup_buf = NULL;
	}

	LOG_INF("QSPI stress suite teardown: ops=%ld errs=%ld timeouts=%ld",
		atomic_get(&operations_cnt), atomic_get(&errors_cnt), atomic_get(&timeouts_cnt));
}

ZTEST_SUITE(qspi_flash_stress, NULL, qspi_stress_setup, qspi_stress_before, NULL,
	    qspi_stress_teardown);

/* ZTRESS handlers */

/* Handler: concurrent readers verifying a pseudo-random sequence pattern */
static bool ztress_flash_read_verify(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct qspi_thread_ctx *ctx = user_data;
	const size_t max_len = MIN(MAX_BUFFER_SIZE, ctx->region_size);
	uint8_t buf[MAX_BUFFER_SIZE];
	size_t len = 16U + (sys_rand32_get() % (max_len - 16U));
	uint32_t offs = ctx->region_offs + (sys_rand32_get() % (ctx->region_size - len));
	int ret = timed_flash_read(ctx, offs, buf, len);
	if (ret != 0) {
		ctx->errors++;
		atomic_inc(&errors_cnt);
		goto out;
	}

	ret = verify_pattern(buf, len, PATTERN_SEED, offs);
	if (ret != 0) {
		ctx->errors++;
		atomic_inc(&errors_cnt);
	}

out:
	ctx->operations++;
	atomic_inc(&operations_cnt);
	tiny_delay();
	return !last;
}

/* Handler: Erase-wrtite-read cycles for random buffers within region */
static bool ztress_flash_erase_write_read(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct qspi_thread_ctx *ctx = user_data;
	/* Choose a sector boundary inside the region */
	size_t sectors = ctx->region_size / ctx->sector_size;
	uint32_t soffs = ctx->region_offs +
			 (uint32_t)(((cnt * ctx->total_threads + ctx->thread_id) % sectors) *
				    ctx->sector_size);

	/* Erase sector */
	int ret = flash_erase(ctx->dev, soffs, ctx->sector_size);
	if (ret != 0) {
		ctx->errors++;
		atomic_inc(&errors_cnt);
		goto out_cnt;
	}

	/* Program random-length buffers within the sector with pseudo-random sequence pattern */
	uint32_t seed = 0xC0FFEE00u ^ (uint32_t)soffs;
	uint8_t buf[512];
	/* Choose random length between 128 bytes and sector size */
	size_t remain = 128U + (sys_rand32_get() % (ctx->sector_size - 128U + 1));
	uint32_t off = soffs;
	while (remain) {
		size_t chunk = MIN((size_t)sizeof(buf), remain);
		fill_pattern(buf, chunk, seed, off);
		ret = timed_flash_write(ctx, off, buf, chunk);
		if (ret != 0) {
			ctx->errors++;
			atomic_inc(&errors_cnt);
			break;
		}
		/* Verify chunk */
		uint8_t verify[512];
		ret = timed_flash_read(ctx, off, verify, chunk);
		if (ret != 0 || memcmp(verify, buf, chunk) != 0) {
			ctx->errors++;
			atomic_inc(&errors_cnt);
			break;
		}

		off += chunk;
		remain -= chunk;
	}

out_cnt:
	ctx->operations++;
	atomic_inc(&operations_cnt);
	tiny_delay();
	return !last;
}

/* Handler: Erase-wrtite-read cycles for full sector size */
static bool ztress_flash_write_read_sector(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct qspi_thread_ctx *ctx = user_data;
	/* Choose a sector boundary inside the region */
	size_t sectors = ctx->region_size / ctx->sector_size;
	uint32_t soffs = ctx->region_offs +
			 (uint32_t)(((cnt * ctx->total_threads + ctx->thread_id) % sectors) *
				    ctx->sector_size);

	/* Erase sector */
	int ret = flash_erase(ctx->dev, soffs, ctx->sector_size);
	if (ret != 0) {
		ctx->errors++;
		atomic_inc(&errors_cnt);
		goto out_cnt;
	}

	/* Program full sector with a pseudo-random sequence pattern */
	uint32_t seed = 0xC0FFEE00u ^ (uint32_t)soffs;
	uint8_t pagebuf[512]; /* big enough for typical page sizes */
	size_t remain = ctx->sector_size;
	uint32_t off = soffs;
	while (remain) {
		size_t chunk = MIN((size_t)sizeof(pagebuf), remain);
		fill_pattern(pagebuf, chunk, seed, off);
		ret = timed_flash_write(ctx, off, pagebuf, chunk);
		if (ret != 0) {
			ctx->errors++;
			atomic_inc(&errors_cnt);
			break;
		}

		off += chunk;
		remain -= chunk;
	}

	/* Verify programmed pattern after full sector write */
	seed = 0xC0FFEE00u ^ (uint32_t)soffs;
	remain = ctx->sector_size;
	off = soffs;
	while (remain) {
		size_t chunk = MIN((size_t)sizeof(pagebuf), remain);
		fill_pattern(pagebuf, chunk, seed, off);
		uint8_t verify[512];
		ret = timed_flash_read(ctx, off, verify, chunk);
		if (ret != 0 || memcmp(verify, pagebuf, chunk) != 0) {
			ctx->errors++;
			atomic_inc(&errors_cnt);
			break;
		}

		off += chunk;
		remain -= chunk;
	}

out_cnt:
	ctx->operations++;
	atomic_inc(&operations_cnt);
	tiny_delay();
	return !last;
}

/* Handler: Perform ranom writes to flash without verifying if contents chagned */
static bool ztress_flash_unaligned_writes(void *user_data, uint32_t cnt, bool last, int prio)
{
	struct qspi_thread_ctx *ctx = user_data;
	uint8_t local[64];

	/* Pick 1..64 bytes, but not larger than the region */
	const size_t max_len =
		MIN(sizeof(local), ctx->region_size ? ctx->region_size : sizeof(local));
	size_t len = 1U + (sys_rand32_get() % max_len);

	/* Keep the write fully inside the region */
	const off_t max_start = (off_t)(ctx->region_size - len);
	off_t offs = ctx->region_offs + (max_start ? (sys_rand32_get() % max_start) : 0);

	fill_pattern(local, len, (PATTERN_SEED ^ cnt), offs);

	int ret = flash_write(ctx->dev, offs, local, len);
	if (ret != 0) {
		ctx->errors++;
	}
	/* We don't care about the result or timing here. The purpose of this handler is just to
	 * poke the driver. */

	ctx->operations++;
	atomic_inc(&operations_cnt);

	ARG_UNUSED(prio);
	return !last;
}

/* Prefill region with a stable pattern */
static void prefill_region(const struct device *dev, uint32_t offs, size_t size, uint32_t seed)
{
	int ret = flash_erase(dev, offs, size);
	zassert_ok(ret, "Prefill erase failed: %s", err2str(ret));

	/* Write in chunks */
	uint8_t buf[512];
	size_t remain = size;
	uint32_t cur = offs;
	while (remain) {
		size_t n = MIN(remain, sizeof(buf));
		fill_pattern(buf, n, seed, cur);
		ret = flash_write(dev, cur, buf, n);
		zassert_ok(ret, "Prefill write failed @0x%x: %s", (uint32_t)cur, err2str(ret));
		cur += n;
		remain -= n;
	}
}

/* Test cases */

/* Concurrent read-only verification from multiple threads. */
ZTEST_F(qspi_flash_stress, test_concurrent_reads)
{
	struct qspi_thread_ctx ctx[4];
	memset(ctx, 0, sizeof(ctx));
	int64_t t0, dt;
	uint32_t min_expected, total_ops;
	double read_kibs, write_kibs;

	LOG_INF("Starting concurrent read verification stress test");

	/* Prepare stable data */
	prefill_region(flash_dev, fixture->region_offs, fixture->region_size, PATTERN_SEED);

	for (uint8_t i = 0; i < ARRAY_SIZE(ctx); i++) {
		ctx[i].dev = flash_dev;
		ctx[i].region_offs = fixture->region_offs;
		ctx[i].region_size = fixture->region_size;
		ctx[i].page_size = fixture->page_size;
		ctx[i].sector_size = fixture->sector_size;
	}

	ztress_set_timeout(K_MSEC(ZTRESS_TEST_TIMEOUT_MS));
	t0 = k_uptime_get();

	ZTRESS_EXECUTE(ZTRESS_THREAD(ztress_flash_read_verify, &ctx[0], ZTRESS_ITERATIONS, 0,
				     Z_TIMEOUT_TICKS(0)),
		       ZTRESS_THREAD(ztress_flash_read_verify, &ctx[1], ZTRESS_ITERATIONS, 0,
				     Z_TIMEOUT_TICKS(0)),
		       ZTRESS_THREAD(ztress_flash_read_verify, &ctx[2], ZTRESS_ITERATIONS, 0,
				     Z_TIMEOUT_TICKS(0)),
		       ZTRESS_THREAD(ztress_flash_read_verify, &ctx[3], ZTRESS_ITERATIONS, 0,
				     Z_TIMEOUT_TICKS(0)));

	dt = k_uptime_get() - t0;
	total_ops = dump_thread_stats(ctx, ARRAY_SIZE(ctx));
	dump_bw_stats(ctx, ARRAY_SIZE(ctx), &read_kibs, &write_kibs);
	min_expected = ARRAY_SIZE(ctx) * ZTRESS_ITERATIONS;
	perf_check(total_ops, dt, min_expected, ZTRESS_TEST_TIMEOUT_MS, &read_kibs, NULL);
	op_err_tolerance_check(total_ops);
}

/* Erase/write/read cycles from two threads. */
ZTEST_F(qspi_flash_stress, test_erase_write_read)
{
	struct qspi_thread_ctx ctx[2];
	memset(ctx, 0, sizeof(ctx));
	uint32_t total_ops, min_expected;
	int64_t t0, dt;
	double read_kibs, write_kibs;

	LOG_INF("Starting mixed random-length buffers erase/write/read stress test");

	for (uint8_t i = 0; i < ARRAY_SIZE(ctx); i++) {
		ctx[i].dev = flash_dev;
		ctx[i].region_offs = fixture->region_offs;
		ctx[i].region_size = fixture->region_size;
		ctx[i].page_size = fixture->page_size;
		ctx[i].sector_size = fixture->sector_size;
		ctx[i].thread_id = i;
		ctx[i].total_threads = ARRAY_SIZE(ctx);
	}

	ztress_set_timeout(K_MSEC(ZTRESS_TEST_TIMEOUT_MS));
	t0 = k_uptime_get();

	ZTRESS_EXECUTE(ZTRESS_THREAD(ztress_flash_erase_write_read, &ctx[0],
				     ZTRESS_ERASE_WRITE_READ_ITERATIONS, 0, Z_TIMEOUT_TICKS(0)),
		       ZTRESS_THREAD(ztress_flash_erase_write_read, &ctx[1],
				     ZTRESS_ERASE_WRITE_READ_ITERATIONS, 0, Z_TIMEOUT_TICKS(0)));

	dt = k_uptime_get() - t0;
	total_ops = dump_thread_stats(ctx, ARRAY_SIZE(ctx));
	dump_bw_stats(ctx, ARRAY_SIZE(ctx), &read_kibs, &write_kibs);
	min_expected = 2 * ZTRESS_ERASE_WRITE_READ_ITERATIONS;
	perf_check(total_ops, dt, min_expected, ZTRESS_TEST_TIMEOUT_MS, &read_kibs, &write_kibs);
	op_err_tolerance_check(total_ops);
}

/* Full sector write and read combined with unaligned writes. */
ZTEST_F(qspi_flash_stress, test_write_read_sector)
{
	struct qspi_thread_ctx ctx[2];
	memset(ctx, 0, sizeof(ctx));
	int64_t t0, dt;
	uint32_t min_expected, total_ops;
	double read_kibs, write_kibs;

	LOG_INF("Starting sector erase/write/read + unaligned writes stress test");

	/* Split region into two */
	const uint32_t half = fixture->region_size / 2U;

	/* Thread 0 (sector erase/write/read) on lower half */
	ctx[0].dev = flash_dev;
	ctx[0].region_offs = fixture->region_offs;
	ctx[0].region_size = half;
	ctx[0].page_size = fixture->page_size;
	ctx[0].sector_size = fixture->sector_size;
	ctx[0].thread_id = 0;
	ctx[0].total_threads = 2;

	/* Thread 1 (unaligned writes) on upper half */
	ctx[1].dev = flash_dev;
	ctx[1].region_offs = fixture->region_offs + half;
	ctx[1].region_size = half;
	ctx[1].page_size = fixture->page_size;
	ctx[1].sector_size = fixture->sector_size;
	ctx[1].thread_id = 1;
	ctx[1].total_threads = 2;

	/* Prepare: erase upper half once so unaligned writes succeed */
	int rc = flash_erase(flash_dev, ctx[1].region_offs, ctx[1].region_size);
	zassert_ok(rc, "Pre-erase for unaligned writer failed");

	ztress_set_timeout(K_MSEC(ZTRESS_TEST_TIMEOUT_MS));
	t0 = k_uptime_get();

	ZTRESS_EXECUTE(ZTRESS_THREAD(ztress_flash_write_read_sector, &ctx[0],
				     ZTRESS_ERASE_WRITE_READ_ITERATIONS, 0, Z_TIMEOUT_TICKS(0)),
		       ZTRESS_THREAD(ztress_flash_unaligned_writes, &ctx[1], ZTRESS_ITERATIONS, 0,
				     Z_TIMEOUT_TICKS(0)));

	dt = k_uptime_get() - t0;
	total_ops = dump_thread_stats(ctx, ARRAY_SIZE(ctx));
	dump_bw_stats(ctx, ARRAY_SIZE(ctx), &read_kibs, &write_kibs);
	min_expected = ZTRESS_ERASE_WRITE_READ_ITERATIONS + ZTRESS_ITERATIONS;
	perf_check(total_ops, dt, min_expected, ZTRESS_TEST_TIMEOUT_MS, &read_kibs, &write_kibs);
	op_err_tolerance_check(total_ops);
}
