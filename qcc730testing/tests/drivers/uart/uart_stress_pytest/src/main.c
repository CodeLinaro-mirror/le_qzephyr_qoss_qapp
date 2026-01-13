/*
 * Copyright (c) 2025 Qualcomm Technologies, Inc.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * UART RX/TX stress tests with host communication
 */

#include <zephyr/ztest.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>
#include <zephyr/random/random.h>

LOG_MODULE_REGISTER(uart_stress, LOG_LEVEL_INF);

#define RX_TEST_SIZE      (CONFIG_RX_TEST_SIZE_KB * 1024)
#define TX_TEST_SIZE      (CONFIG_TX_TEST_SIZE_KB * 1024)
#define PROGRESS_INTERVAL (CONFIG_PROGRESS_INTERVAL_KB * 1024)

static const struct device *const uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

/* Test 1: RX stress test - receive 1MB from host and verify CRC */
ZTEST(uart_large_transfer, test_1_rx_1mb_from_host)
{
	uint32_t bytes_received = 0U;
	uint16_t crc16_calculated = 0U;
	uint32_t crc16_received = 0U;
	int64_t start_time = 0, end_time = 0;
	int ret = 0;
	uint32_t percent = 0U;
	unsigned char byte = 0U;
	uint32_t last_bytes = 0;
	int64_t last_log_time = 0;
	//uint32_t err = 0;

	zassume_true(device_is_ready(uart_dev), "UART device not ready");
	LOG_INF("Starting RX stress test - receiving %u bytes from host", RX_TEST_SIZE);

	/* Signal to host that we're ready for RX test */
	printk("RX_TEST_READY\n");

	start_time = k_uptime_get();
	last_log_time = start_time;
	printk("RX loop enter\r\n");
	while (bytes_received < RX_TEST_SIZE) {
		byte = 0U;
		ret = uart_poll_in(uart_dev, &byte);

		if (ret == 0) {
			crc16_calculated = crc16_itu_t(crc16_calculated, &byte, 1);
			++bytes_received;

			if ((bytes_received % 4096) == 0) {
				LOG_INF("RX 4096 Bytes more(%u) elapsed_since_last_log=%lld ms", bytes_received,
					k_uptime_get() - last_log_time);
				last_bytes = bytes_received;
        		last_log_time = k_uptime_get();
				//err = uart_err_check(uart_dev);
				//if (err) {
    			//	LOG_ERR("UART error: 0x%08x(inside while)", err);
				//}
			}

			//if (bytes_received >= 10481664 && (bytes_received % 512) == 0) {
    		//	LOG_INF("RX debug after 60KB: %u bytes", bytes_received);
			//}


			if ((bytes_received % PROGRESS_INTERVAL) == 0) {
				percent = (bytes_received * 100) / RX_TEST_SIZE;
				if (percent % 10 == 0) {
					LOG_INF("RX Progress: %u%%", percent);
				}
			}
		} else {
			k_usleep(1);
		}
        
		if (k_uptime_get() - start_time > 1200000) {
			LOG_ERR("RX timeout: bytes_received=%u, expected=%u",
                bytes_received, RX_TEST_SIZE);
				break;
		}
	}
    
	LOG_INF("RX loop exit (or timeout): bytes_received=%u", bytes_received);
	//err = uart_err_check(uart_dev);
	//if (err) {
	//	LOG_ERR("UART error: 0x%08x (out while)", err);
	//}
	printk("RX loop exit, total bytes = %u\r\n", bytes_received);

	end_time = k_uptime_get();
	k_usleep(100);
	printk("RX_CRC_READY\n");
	/* Receive CRC16 from host (2 bytes, big-endian) */
	LOG_INF("Receiving CRC16 from host...");
	int64_t crc_start = k_uptime_get();
	bool crc_timeout = false;
	for (uint8_t i = 0U; i < 2U; i++) {
		byte = 0U;
		while (uart_poll_in(uart_dev, &byte) != 0) {
			k_sleep(K_USEC(10));
			if (k_uptime_get() - crc_start > 1000*60) {
				LOG_ERR("RX CRC receive timeout after %lld ms, received bytes=%u",
						(long long)(k_uptime_get() - crc_start), i);
				crc_timeout = true;
				break;
        	}
		}

		if (crc_timeout) {
        	break;
    	}

		crc16_received = (crc16_received << 8U) | byte;
	}

	if (crc_timeout) {
		printk("RX_TEST_FAIL\n");
		zassert_true(false, "RX CRC receive timeout");
		return;
	}

	/* Verify CRC */
	if (crc16_calculated == crc16_received) {
		LOG_INF("RX CRC verification PASSED (0x%04x)", crc16_calculated);
		printk("RX_TEST_PASS\n");
	} else {
		LOG_ERR("RX CRC mismatch! Expected: 0x%04x, Got: 0x%04x", crc16_received,
			crc16_calculated);
		printk("RX_TEST_FAIL\n");
		zassert_equal(crc16_calculated, crc16_received, "RX CRC verification failed");
	}

	/* Report statistics */
	uint32_t duration_ms = (uint32_t)(end_time - start_time);
	uint32_t throughput = (bytes_received * 1000) / duration_ms;

	LOG_INF("RX Test statistics:");
	LOG_INF("  Total bytes received: %u", bytes_received);
	LOG_INF("  Duration: %u ms", duration_ms);
	LOG_INF("  RX Throughput: %u bytes/sec", throughput);
	LOG_INF("  Effective RX rate: %u bps", throughput * 8);

	/* Verify complete transfer */
	zassert_equal(bytes_received, RX_TEST_SIZE, "Incomplete RX transfer: %u/%u bytes",
		      bytes_received, RX_TEST_SIZE);
}

/* Test 2: TX stress test - send data to host for verification */
ZTEST(uart_large_transfer, test_2_tx_1mb_to_host)
{
	uint32_t bytes_sent = 0U;
	uint16_t crc16_calculated = 0U;
	int64_t start_time = 0, end_time = 0;
	unsigned char byte = 0U;
	int ret = 0;

	zassume_true(device_is_ready(uart_dev), "UART device not ready");
	LOG_INF("Starting TX stress test - sending data to host");

	/* Signal to host that we're ready for TX test */
	printk("TX_TEST_READY\n");

	/* Wait for START signal from host (5 bytes: "START") */
	LOG_INF("Waiting for host START signal...");
	const char expected[] = "START";
	uint8_t match_idx = 0U;

	while (match_idx < 5) {
		ret = uart_poll_in(uart_dev, &byte);
		if (ret == 0) {
			match_idx = byte == expected[match_idx] ? match_idx + 1U : 0U;
		} else {
			k_usleep(10);
		}
	}

	/* Give host time to enter raw mode */
	k_msleep(400);

	start_time = k_uptime_get();
	while (bytes_sent < TX_TEST_SIZE) {
		byte = (unsigned char)(bytes_sent & 0xFFU);
		uart_poll_out(uart_dev, byte);
		crc16_calculated = crc16_itu_t(crc16_calculated, &byte, 1);
		++bytes_sent;

		if ((bytes_sent % 1024) == 0) {
			k_yield();
		}
	}
	end_time = k_uptime_get();

	/* Send CRC16 to host */
	uart_poll_out(uart_dev, (crc16_calculated >> 8U) & 0xFFU);
	uart_poll_out(uart_dev, crc16_calculated & 0xFFU);

	char result[5] = {0};
	for (uint8_t i = 0U; i < 4; i++) {
		while (uart_poll_in(uart_dev, &byte) != 0) {
			k_usleep(10);
		}
		result[i] = byte;
	}

	LOG_INF("Sent CRC16: 0x%04x", crc16_calculated);
	if (strncmp(result, "PASS", 4) == 0) {
		LOG_INF("TX verification PASSED");
		printk("TX_TEST_PASS\n");
	} else {
		LOG_ERR("TX verification FAILED");
		printk("TX_TEST_FAIL\n");
		zassert_true(false, "TX verification failed");
	}

	uint32_t duration_ms = (uint32_t)(end_time - start_time);
	uint32_t throughput = (bytes_sent * 1000) / duration_ms;

	LOG_INF("TX Test statistics:");
	LOG_INF("  Total bytes sent: %u", bytes_sent);
	LOG_INF("  Duration: %u ms", duration_ms);
	LOG_INF("  TX Throughput: %u bytes/sec", throughput);
	LOG_INF("  Effective TX rate: %u bps", throughput * 8);

	zassert_equal(bytes_sent, TX_TEST_SIZE, "Incomplete TX transfer: %u/%u bytes", bytes_sent,
		      TX_TEST_SIZE);
}

ZTEST_SUITE(uart_large_transfer, NULL, NULL, NULL, NULL, NULL);
