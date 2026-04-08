
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ring_service.h"
LOG_MODULE_REGISTER(qcc730_slave, LOG_LEVEL_INF);
const struct device *spi_dev = DEVICE_DT_GET(DT_NODELABEL(qcspi));
/* Static buffer for TX test */
static uint8_t send_buffer[1500];
/* Static buffer for RX test */
static uint8_t recv_buffer[1500];
/* Static buffer for loop test */
static uint8_t loop_buffer[1500];
static void print_received_data(const uint8_t *data, size_t len)
{
    bool is_printable = true;

    /* Check if data is printable ASCII string */
    for (size_t i = 0; i < len; i++) {
        if (data[i] != 0 && (data[i] < 0x20 || data[i] > 0x7E)) {
            is_printable = false;
            break;
        }
    }

    if (is_printable) {
        /* Print as string */
        LOG_INF("Received string (%d bytes): %.*s", len, len, data);
    } else {
        /* Print as hex */
        LOG_HEXDUMP_INF(data, len, "Received hex data:");
    }
}
static void ring_event_callback(uint8_t ring_id, void *user_data)
{
    int ret;
    int packet_count = 0;
    ARG_UNUSED(user_data);
    do {
        ret = ring_recv(ring_id, recv_buffer, sizeof(recv_buffer), K_NO_WAIT);
        if (ret > 0) {
            packet_count++;
            switch (ring_id) {
            case RING_0:
                break;
            case RING_1:
                break;
            case RING_2:
                memcpy(loop_buffer, recv_buffer, ret);
                ring_send(ring_id, loop_buffer, ret, K_MSEC(1000));
                break;
            default:
                break;
            }
        } else if (ret < 0) {
            LOG_ERR("Failed to receive data: %d", ret);
            break; /* Exit on error */
        }
        /* ret == 0 means no more data available, loop will exit */
    } while (ret > 0);
    if (packet_count > 0) {
        LOG_INF("Total processed %d data packets from ring %d", packet_count, ring_id);
    } else {
        LOG_WRN("No data available from ring %d", ring_id);
    }
}
/* Burst test thread infrastructure */
#define BURST_THREAD_STACK_SIZE 2048
#define BURST_THREAD_PRIORITY K_PRIO_PREEMPT(5) /* Lower priority - allows shell and RX to run */
static K_THREAD_STACK_DEFINE(burst_thread_stack, BURST_THREAD_STACK_SIZE);
static struct k_thread burst_thread_data;
static k_tid_t burst_thread_id;
/* Burst test parameters */
struct burst_test_params {
    const struct shell *shell;
    uint8_t ring_id;
    uint32_t count;
    uint32_t size;
    bool running;
    struct k_sem completion_sem;
};
static struct burst_test_params burst_params;
#define MAX_TRACKED_THREADS 10
/* Burst test thread function */
static void burst_test_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    struct burst_test_params *params = &burst_params;
    const struct shell *sh = params->shell;
    uint8_t ring_id = params->ring_id;
    uint32_t count = params->count;
    uint32_t size = params->size;

    uint32_t success_count = 0;
    uint32_t fail_count = 0;
    uint32_t start_time = k_uptime_get_32();
    uint64_t start_cycles = k_cycle_get_64();

    /* Determine if this is time-based or count-based test */
    bool time_based = (count == 0);
    uint32_t target_duration_ms = size; /* When count=0, size parameter is duration in ms */

    if (time_based) {
        shell_print(sh,
                    "[BURST_THREAD] Starting time-based burst test on ring %u: %u ms duration, packet size %u bytes",
                    ring_id, target_duration_ms, 1500);
        size = 1500; /* Use fixed 1500 byte packets for time-based test */
    } else {
        shell_print(sh, "[BURST_THREAD] Starting count-based burst test on ring %u: %u packets of %u bytes", ring_id,
                    count, size);
    }

    uint32_t i = 0;
    while (params->running) {
        /* Check termination condition */
        if (time_based) {
            uint32_t current_time = k_uptime_get_32();
            if ((current_time - start_time) >= target_duration_ms) {
                break; /* Time limit reached */
            }
        } else {
            if (i >= count) {
                break; /* Packet count reached */
            }
        }

        /* Add retry logic for -EAGAIN */
        int ret;
        uint32_t retry_count = 0;
        const uint32_t max_retries = 100;

        do {
            ret = ring_send(ring_id, send_buffer, size, K_MSEC(1000));
            if (ret == -EAGAIN) {
                retry_count++;
                k_usleep(10); /* Small delay before retry */
            }
        } while (ret == -EAGAIN && retry_count < max_retries);

        if (ret < 0) {
            fail_count++;
            if (retry_count >= max_retries) {
                shell_error(sh, "[BURST_THREAD] Packet %u failed after %u retries: %d", i, retry_count, ret);
            } else {
                shell_error(sh, "[BURST_THREAD] Packet %u failed: %d", i, ret);
            }
        } else {
            success_count++;
            if (retry_count > 0 && (i % 100 == 0)) {
                shell_print(sh, "[BURST_THREAD] Packet %u sent after %u retries", i, retry_count);
            }
        }

        i++;

        /* Yield to allow RX work queue and shell to run */
        k_yield();
    }

    uint32_t elapsed_time = k_uptime_get_32() - start_time;
    uint64_t elapsed_cycles = k_cycle_get_64() - start_cycles;

    shell_print(sh, "=== Burst Test Results ===");
    if (time_based) {
        shell_print(sh, "Test Duration: %u ms (target: %u ms)", elapsed_time, target_duration_ms);
    }
    shell_print(sh, "Success: %u packets", success_count);
    shell_print(sh, "Failed: %u packets", fail_count);
    shell_print(sh, "Total Packets: %u", i);
    shell_print(sh, "Time: %u ms", elapsed_time);
    shell_print(sh, "Cycles: %llu", elapsed_cycles);

    if (elapsed_time > 0) {
        /* Use 64-bit arithmetic to prevent integer overflow */
        uint64_t total_bits = (uint64_t)success_count * size * 1000 * 8;
        uint32_t throughput = (uint32_t)(total_bits / elapsed_time);
        shell_print(sh, "Throughput: %u bits/sec (%.2f Mbps)", throughput, throughput / 1000000.0);

        /* Packets per second */
        uint32_t pps = (success_count * 1000) / elapsed_time;
        shell_print(sh, "Packet Rate: %u packets/sec", pps);
    }

    params->running = false;
    k_sem_give(&params->completion_sem);
}
/* Shell command: Get ring service status */
static int cmd_ring_status(const struct shell *sh, size_t argc, char **argv)
{
    int ring_num = ring_get_num();
    shell_print(sh, "=== Ring Service Status ===");

    /* Show status for all configured rings (0-7) */
    for (uint8_t ring_id = 0; ring_id < ring_num; ring_id++) {
        int tx_avail = ring_get_tx_available(ring_id);
        int rx_avail = ring_get_rx_available(ring_id);
        struct ring_stats stats;

        /* Skip unconfigured rings */
        if (tx_avail == -EINVAL || rx_avail == -EINVAL) {
            continue;
        }

        shell_print(sh, "\n--- Ring %u ---", ring_id);

        if (tx_avail >= 0) {
            shell_print(sh, "TX Available: %d descriptors", tx_avail);
        } else {
            shell_print(sh, "TX Available: Error %d", tx_avail);
        }

        if (rx_avail >= 0) {
            shell_print(sh, "RX Available: %d descriptors", rx_avail);
        } else {
            shell_print(sh, "RX Available: Error %d", rx_avail);
        }

        if (ring_get_stats(ring_id, &stats) == 0) {
            shell_print(sh, "TX Count: %u", stats.tx_count);
            shell_print(sh, "RX Count: %u", stats.rx_count);
            shell_print(sh, "TX Errors: %u", stats.tx_errors);
            shell_print(sh, "RX Errors: %u", stats.rx_errors);
        }
    }

    return 0;
}
/* Shell command: Send string data via ring service */
static int cmd_ring_send(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 3) {
        shell_error(sh, "Usage: ring send <ring_id> <string>");
        shell_error(sh, "Example: ring send 0 \"Hello QCC730\"");
        return -EINVAL;
    }

    uint8_t ring_id = (uint8_t)strtoul(argv[1], NULL, 0);
    if (ring_id >= MAX_RINGS) {
        shell_error(sh, "Invalid ring_id (0-%d)", MAX_RINGS - 1);
        return -EINVAL;
    }

    /* Concatenate all arguments into one string (starting from argv[2]) */
    size_t offset = 0;

    for (size_t i = 2; i < argc && offset < sizeof(send_buffer) - 1; i++) {
        size_t arg_len = strlen(argv[i]);
        if (offset + arg_len + 1 > sizeof(send_buffer) - 1) {
            break;
        }

        if (i > 2) {
            send_buffer[offset++] = ' ';
        }

        memcpy(send_buffer + offset, argv[i], arg_len);
        offset += arg_len;
    }
    send_buffer[offset] = '\0';

    shell_print(sh, "Sending %zu bytes on ring %u: \"%s\"", offset, ring_id, send_buffer);

    int ret = ring_send(ring_id, (const uint8_t *)send_buffer, offset, K_MSEC(1000));
    if (ret < 0) {
        shell_error(sh, "Ring send failed: %d", ret);
        return ret;
    }

    shell_print(sh, "Data sent successfully");
    return 0;
}
/* Helper function to parse hex string to bytes */
static int hex_string_to_bytes(const char *hex_str, uint8_t *bytes, size_t max_bytes)
{
    size_t hex_len = strlen(hex_str);
    size_t byte_count = 0;
    /* Remove "0x" prefix if present */
    if (hex_len >= 2 && hex_str[0] == '0' && (hex_str[1] == 'x' || hex_str[1] == 'X')) {
        hex_str += 2;
        hex_len -= 2;
    }
    /* Each byte needs 2 hex characters */
    if (hex_len % 2 != 0) {
        return -EINVAL;
    }
    byte_count = hex_len / 2;
    if (byte_count > max_bytes) {
        return -E2BIG;
    }
    for (size_t i = 0; i < byte_count; i++) {
        char byte_str[3] = {hex_str[i * 2], hex_str[i * 2 + 1], '\0'};
        char *endptr;
        unsigned long val = strtoul(byte_str, &endptr, 16);
        if (*endptr != '\0' || val > 0xFF) {
            return -EINVAL;
        }
        bytes[i] = (uint8_t)val;
    }
    return byte_count;
}
/* Shell command: Send hex data via ring service */
static int cmd_ring_send_hex(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 3) {
        shell_error(sh, "Usage: ring send_hex <ring_id> <hex_string>");
        shell_error(sh, "Example: ring send_hex 0 AABBCCDD");
        shell_error(sh, "Example: ring send_hex 1 0x1122334455");
        return -EINVAL;
    }

    uint8_t ring_id = (uint8_t)strtoul(argv[1], NULL, 0);
    if (ring_id >= MAX_RINGS) {
        shell_error(sh, "Invalid ring_id (0-%d)", MAX_RINGS - 1);
        return -EINVAL;
    }

    int byte_count = hex_string_to_bytes(argv[2], send_buffer, sizeof(send_buffer));
    if (byte_count < 0) {
        shell_error(sh, "Invalid hex string format");
        return byte_count;
    }

    shell_print(sh, "Sending %d bytes (hex) on ring %u:", byte_count, ring_id);
    for (int i = 0; i < byte_count; i++) {
        shell_fprintf(sh, SHELL_NORMAL, "%02X ", send_buffer[i]);
        if ((i + 1) % 16 == 0) {
            shell_print(sh, "");
        }
    }
    if (byte_count % 16 != 0) {
        shell_print(sh, "");
    }

    int ret = ring_send(ring_id, send_buffer, byte_count, K_MSEC(1000));
    if (ret < 0) {
        shell_error(sh, "Ring send failed: %d", ret);
        return ret;
    }

    shell_print(sh, "Data sent successfully");
    return 0;
}
/* Shell command: Send test pattern via ring service */
static int cmd_ring_send_pattern(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 3) {
        shell_error(sh, "Usage: ring send_pattern <ring_id> <length>");
        shell_error(sh, "Example: ring send_pattern 0 100");
        shell_error(sh, "Sends incrementing pattern: 0x00, 0x01, 0x02, ...");
        return -EINVAL;
    }

    uint8_t ring_id = (uint8_t)strtoul(argv[1], NULL, 0);
    if (ring_id >= MAX_RINGS) {
        shell_error(sh, "Invalid ring_id (0-%d)", MAX_RINGS - 1);
        return -EINVAL;
    }

    uint32_t len = strtoul(argv[2], NULL, 0);
    if (len == 0 || len > 1500) {
        shell_error(sh, "Invalid length (1-1500)");
        return -EINVAL;
    }

    for (uint32_t i = 0; i < len; i++) {
        send_buffer[i] = i & 0xFF;
    }

    shell_print(sh, "Sending %u bytes test pattern on ring %u (0x00-0x%02X)", len, ring_id, (len - 1) & 0xFF);

    int ret = ring_send(ring_id, send_buffer, len, K_MSEC(1000));
    if (ret < 0) {
        shell_error(sh, "Ring send failed: %d", ret);
        return ret;
    }

    shell_print(sh, "Test pattern sent successfully");
    return 0;
}
/* Shell command: Send multiple packets via ring service (High Priority Thread) */
static int cmd_ring_send_burst(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 4) {
        shell_error(sh, "Usage: ring send_burst <ring_id> <count|0> <size|duration_ms>");
        shell_error(sh, "");
        shell_error(sh, "Count-based mode:");
        shell_error(sh, "  ring send_burst <ring_id> <count> <size>");
        shell_error(sh, "  Example: ring send_burst 0 100 1500");
        shell_error(sh, "  Sends <count> packets of <size> bytes each on ring <ring_id>");
        shell_error(sh, "");
        shell_error(sh, "Time-based mode (for stress testing):");
        shell_error(sh, "  ring send_burst <ring_id> 0 <duration_ms>");
        shell_error(sh, "  Example: ring send_burst 1 0 60000");
        shell_error(sh, "  Sends 1500-byte packets continuously for <duration_ms> milliseconds on ring <ring_id>");
        return -EINVAL;
    }

    uint8_t ring_id = (uint8_t)strtoul(argv[1], NULL, 0);
    if (ring_id >= MAX_RINGS) {
        shell_error(sh, "Invalid ring_id (0-%d)", MAX_RINGS - 1);
        return -EINVAL;
    }

    uint32_t count = strtoul(argv[2], NULL, 0);
    uint32_t size = strtoul(argv[3], NULL, 0);

    /* Validate parameters based on mode */
    if (count == 0) {
        /* Time-based mode */
        if (size == 0 || size > 3600000) { /* Max 1 hour */
            shell_error(sh, "Invalid duration (1-3600000 ms, i.e., 1ms to 1 hour)");
            return -EINVAL;
        }
        shell_print(sh, "Time-based stress test mode on ring %u: %u ms duration", ring_id, size);
    } else {
        /* Count-based mode */
        if (count > 100000) {
            shell_error(sh, "Invalid count (1-100000)");
            return -EINVAL;
        }

        if (size == 0 || size > 1500) {
            shell_error(sh, "Invalid size (1-1500)");
            return -EINVAL;
        }
    }

    /* Check if burst thread is already running */
    if (burst_params.running) {
        shell_error(sh, "Burst test already running. Please wait for completion.");
        return -EBUSY;
    }

    /* Initialize burst test parameters */
    burst_params.shell = sh;
    burst_params.ring_id = ring_id;
    burst_params.count = count;
    burst_params.size = size;
    burst_params.running = true;
    k_sem_init(&burst_params.completion_sem, 0, 1);

    if (count == 0) {
        shell_print(sh, "Starting time-based burst test in high priority thread on ring %u...", ring_id);
        shell_print(sh, "Duration: %u ms, Packet size: 1500 bytes", size);
    } else {
        shell_print(sh, "Starting count-based burst test in high priority thread on ring %u...", ring_id);
        shell_print(sh, "Parameters: %u packets of %u bytes each", count, size);
    }

    /* Create and start high priority thread */
    burst_thread_id = k_thread_create(&burst_thread_data, burst_thread_stack, K_THREAD_STACK_SIZEOF(burst_thread_stack),
                                      burst_test_thread, NULL, NULL, NULL, BURST_THREAD_PRIORITY, 0, K_NO_WAIT);

    if (!burst_thread_id) {
        shell_error(sh, "Failed to create burst test thread");
        burst_params.running = false;
        return -ENOMEM;
    }

    k_thread_name_set(burst_thread_id, "burst_test");

    /* Wait for completion with timeout (adjust based on mode) */
    uint32_t timeout_sec;
    if (count == 0) {
        /* Time-based: timeout = duration + 30 seconds buffer */
        timeout_sec = (size / 1000) + 30;
    } else {
        /* Count-based: 30 seconds default */
        timeout_sec = 30;
    }

    int ret = k_sem_take(&burst_params.completion_sem, K_SECONDS(timeout_sec));
    if (ret != 0) {
        shell_error(sh, "Burst test timeout or error: %d", ret);
        burst_params.running = false;
        k_thread_abort(burst_thread_id);
        return ret;
    }

    shell_print(sh, "Burst test completed successfully");
    return 0;
}
/* Create ring service shell subcommands */
SHELL_STATIC_SUBCMD_SET_CREATE(sub_ring_cmds, SHELL_CMD(status, NULL, "Show ring service status", cmd_ring_status),
                               SHELL_CMD(send, NULL, "Send string data to QCC730", cmd_ring_send),
                               SHELL_CMD(send_hex, NULL, "Send hex data to QCC730", cmd_ring_send_hex),
                               SHELL_CMD(send_pattern, NULL, "Send test pattern to QCC730", cmd_ring_send_pattern),
                               SHELL_CMD(send_burst, NULL, "Send multiple packets to QCC730 (High Priority Thread)",
                                         cmd_ring_send_burst),
                               SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(ring, &sub_ring_cmds, "Ring service commands", NULL);
int main(void)
{
    int ret;
    /* Initialize SPI device */
    if (!device_is_ready(spi_dev)) {
        printk("SPI device not ready\n");
        return -ENODEV;
    }
    /* Register callback to automatically print received data */
    ret = ring_register_callback(ring_event_callback, NULL);
    if (ret < 0) {
        LOG_ERR("Failed to register ring callback: %d", ret);
    } else {
        LOG_INF("Ring callback registered successfully");
    }
    LOG_INF("SPI slave verification app started");
    while (1) {
        k_sleep(K_MSEC(1));
    }
    return 0;
}
