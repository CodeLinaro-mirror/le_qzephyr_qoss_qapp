/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
#include "qc_port_config.h"
#if defined(QC_OS_ZEPHYR) && defined(SPI_DEMO_ENABLE)
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/stats/stats.h>
#include <string.h>
#include <stdlib.h>
#include "ring_transport.h"
#include "qcspi_driver.h"
#include "qc_osal.h"
#define CONFIG_RING_SERVICE
#ifdef CONFIG_RING_SERVICE
#include "ring_service.h"
#endif

LOG_MODULE_REGISTER(qcc730_host, LOG_LEVEL_INF);

/* Get nodes from device tree */
#define LED_NODE DT_ALIAS(led0)
#define SPI_CS_DT_SPEC SPI_CS_GPIOS_DT_SPEC_GET(DT_NODELABEL(spi_slave))

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED_NODE, gpios);
#if DT_NODE_EXISTS(DT_NODELABEL(spi1))
const struct device *const spi1_dev_ptr = DEVICE_DT_GET(DT_NODELABEL(spi1));
#define SPI1_NODE DT_NODELABEL(spi1)
#else
#error "SPI1 node not found in device tree."
#endif

#define MAX_BUFFER_SIZE 256

ring_transport_dev_t qcspi_dev;

struct spi_bridge_data {
    uint8_t spi_tx_buffer[MAX_BUFFER_SIZE];
    uint8_t spi_rx_buffer[MAX_BUFFER_SIZE];
    bool led_state;
    const struct device *spi_dev;
    struct k_mutex spi_mutex;
};

static struct spi_bridge_data bridge_data;
/* Static buffer for TX test */
static uint8_t send_buffer[1500];
/* Static buffer for RX test */
static uint8_t recv_buffer[1500];
/* Default SPI Config */
static struct spi_config spi_cfg = {.frequency = 10000000, /* 1 MHz */
                                    .operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
                                    .slave = 0, /* Slave device number */
                                    .cs = {
                                        /**
                                         * This pin specification is needed for automatic CS
                                         * control Set this to NULL for manual control
                                         */
                                        .gpio = SPI_CS_DT_SPEC,
                                        .delay = 0 /* No delay after CS release */
                                    }};

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

/* SPI Tx-Rx Function */
static int spi_tx_rx(uint8_t *tx_data, uint8_t *rx_data, size_t len)
{
    int ret;

    k_mutex_lock(&bridge_data.spi_mutex, K_FOREVER);

    memcpy(bridge_data.spi_tx_buffer, tx_data, len);
    memset(bridge_data.spi_rx_buffer, 0, len);

    struct spi_buf tx_buf = {.buf = bridge_data.spi_tx_buffer, .len = len};

    struct spi_buf rx_buf = {.buf = bridge_data.spi_rx_buffer, .len = len};

    struct spi_buf_set tx_buf_set = {.buffers = &tx_buf, .count = 1};

    struct spi_buf_set rx_buf_set = {.buffers = &rx_buf, .count = 1};

    /* Send entire buffer at once */
    struct spi_config temp_cfg = spi_cfg;

    ret = spi_transceive(bridge_data.spi_dev, &temp_cfg, &tx_buf_set, &rx_buf_set);
    if (ret < 0) {
        k_mutex_unlock(&bridge_data.spi_mutex);
        return ret;
    }

    /* Copy received data back */
    memcpy(rx_data, bridge_data.spi_rx_buffer, len);

    k_mutex_unlock(&bridge_data.spi_mutex);
    return 0;
}

/* Shell command: Send hex bytes via SPI */
static int cmd_spi_send_hex(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 2) {
        shell_error(sh, "Usage: spi send_hex <hex_string>");
        shell_error(sh, "Example: spi send_hex AABBCCDD");
        shell_error(sh, "Example: spi send_hex 0x1122334455");
        return -EINVAL;
    }

    uint8_t tx_data[MAX_BUFFER_SIZE];
    uint8_t rx_data[MAX_BUFFER_SIZE];

    int byte_count = hex_string_to_bytes(argv[1], tx_data, MAX_BUFFER_SIZE);
    if (byte_count < 0) {
        shell_error(sh, "Invalid hex string format");
        return byte_count;
    }

    int ret = spi_tx_rx(tx_data, rx_data, byte_count);
    if (ret < 0) {
        shell_error(sh, "SPI transaction failed: %d", ret);
        return ret;
    }

    shell_print(sh, "TX[%d]: ", byte_count);
    for (int i = 0; i < byte_count; i++) {
        shell_fprintf(sh, SHELL_NORMAL, "%02X ", tx_data[i]);
    }
    shell_print(sh, "");

    shell_print(sh, "RX[%d]: ", byte_count);
    for (int i = 0; i < byte_count; i++) {
        shell_fprintf(sh, SHELL_NORMAL, "%02X ", rx_data[i]);
    }
    shell_print(sh, "");

    shell_print(sh, "RX ASCII: ");
    for (int i = 0; i < byte_count; i++) {
        if (rx_data[i] >= 32 && rx_data[i] <= 126) {
            shell_fprintf(sh, SHELL_NORMAL, "%c", rx_data[i]);
        } else {
            shell_fprintf(sh, SHELL_NORMAL, ".");
        }
    }
    shell_print(sh, "");

    return 0;
}

/* Shell command: Configure SPI frequency */
static int cmd_spi_config_freq(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 2) {
        shell_error(sh, "Usage: spi config_freq <frequency_hz>");
        shell_error(sh, "Example: spi config_freq 20000000");
        return -EINVAL;
    }

    unsigned long freq = strtoul(argv[1], NULL, 10);
    if (freq == 0 || freq > 20000000) { /* Max 20MHz */
        shell_error(sh, "Invalid frequency (1 - 20000000 Hz)/20MHz");
        return -EINVAL;
    }

    spi_cfg.frequency = freq;
    shell_print(sh, "SPI frequency set to %lu Hz", freq);
    return 0;
}

/* Shell command: Get SPI status */
static int cmd_spi_status(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "=== SPI Bridge Status ===");
    shell_print(sh, "SPI Device: %s", bridge_data.spi_dev->name);
    shell_print(sh, "SPI Frequency: %u Hz", spi_cfg.frequency);
    shell_print(sh, "LED State: %s", bridge_data.led_state ? "ON" : "OFF");
    shell_print(sh, "Buffer Size: %d bytes", MAX_BUFFER_SIZE);
    return 0;
}

/* Shell command: Test QCSPI transport initialization */
static int cmd_qcspi_init(const struct shell *sh, size_t argc, char **argv)
{
    ring_transport_dev_t qcspi_dev = qcspi_transport_get_device();

    if (!qcspi_dev) {
        shell_error(sh, "QCSPI transport device not ready");
        return -ENODEV;
    }

    shell_print(sh, "QCSPI transport initialized successfully");

    /* Get QCC730 slave ID */
    uint8_t slave_id[3] = {0};
    qcspi_get_slaveid(slave_id);
    shell_print(sh, "QCC730 Slave ID: %02X %02X %02X", slave_id[0], slave_id[1], slave_id[2]);

    return 0;
}

/* Shell command: Read QCC730 memory via QCSPI transport */
static int cmd_qcspi_read(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 3) {
        shell_error(sh, "Usage: qcspi read <address> <length>");
        shell_error(sh, "Example: qcspi read 0x20000000 16");
        return -EINVAL;
    }

    ring_transport_dev_t qcspi_dev = qcspi_transport_get_device();

    if (!qcspi_dev) {
        shell_error(sh, "QCSPI transport device not ready");
        return -ENODEV;
    }

    uint32_t addr = strtoul(argv[1], NULL, 0);
    uint32_t len = strtoul(argv[2], NULL, 0);

    if (len == 0 || len > MAX_BUFFER_SIZE) {
        shell_error(sh, "Invalid length (1-%d)", MAX_BUFFER_SIZE);
        return -EINVAL;
    }

    uint8_t read_data[MAX_BUFFER_SIZE];
    int ret = ring_transport_read(qcspi_dev, addr, read_data, len);
    if (ret < 0) {
        shell_error(sh, "QCSPI read failed: %d", ret);
        return ret;
    }

    shell_print(sh, "Read %u bytes from 0x%08X:", len, addr);
    for (uint32_t i = 0; i < len; i++) {
        if (i % 16 == 0) {
            shell_fprintf(sh, SHELL_NORMAL, "\n%08X: ", addr + i);
        }
        shell_fprintf(sh, SHELL_NORMAL, "%02X ", read_data[i]);
    }
    shell_print(sh, "");

    return 0;
}

/* Shell command: Write QCC730 memory via QCSPI transport */
static int cmd_qcspi_write(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 3) {
        shell_error(sh, "Usage: qcspi write <address> <hex_data>");
        shell_error(sh, "Example: qcspi write 0x20000000 AABBCCDD");
        return -EINVAL;
    }

    ring_transport_dev_t qcspi_dev = qcspi_transport_get_device();

    if (!qcspi_dev) {
        shell_error(sh, "QCSPI transport device not ready");
        return -ENODEV;
    }

    uint32_t addr = strtoul(argv[1], NULL, 0);

    uint8_t write_data[MAX_BUFFER_SIZE];
    int len = hex_string_to_bytes(argv[2], write_data, MAX_BUFFER_SIZE);
    if (len < 0) {
        shell_error(sh, "Invalid hex data format");
        return len;
    }

    int ret = ring_transport_write(qcspi_dev, addr, write_data, len);
    if (ret < 0) {
        shell_error(sh, "QCSPI write failed: %d", ret);
        return ret;
    }

    shell_print(sh, "Wrote %d bytes to 0x%08X", len, addr);

    return 0;
}

/* Shell command: Read QCC730 register via QCSPI */
static int cmd_qcspi_reg_read(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 2) {
        shell_error(sh, "Usage: qcspi reg_read <reg_addr>");
        shell_error(sh, "Example: qcspi reg_read 0x0C");
        return -EINVAL;
    }

    ring_transport_dev_t qcspi_dev = qcspi_transport_get_device();

    if (!qcspi_dev) {
        shell_error(sh, "QCSPI transport device not ready");
        return -ENODEV;
    }

    uint8_t reg_addr = (uint8_t)strtoul(argv[1], NULL, 0);
    uint32_t reg_val;

    int ret = ring_transport_reg_read(qcspi_dev, reg_addr, &reg_val);
    if (ret < 0) {
        shell_error(sh, "QCSPI register read failed: %d", ret);
        return ret;
    }

    shell_print(sh, "Register 0x%02X = 0x%08X", reg_addr, reg_val);

    return 0;
}

/* Shell command: Write QCC730 register via QCSPI */
static int cmd_qcspi_reg_write(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 3) {
        shell_error(sh, "Usage: qcspi reg_write <reg_addr> <value>");
        shell_error(sh, "Example: qcspi reg_write 0x0C 0x12345678");
        return -EINVAL;
    }

    ring_transport_dev_t qcspi_dev = qcspi_transport_get_device();

    if (!qcspi_dev) {
        shell_error(sh, "QCSPI transport device not ready");
        return -ENODEV;
    }

    uint8_t reg_addr = (uint8_t)strtoul(argv[1], NULL, 0);
    uint32_t reg_val = strtoul(argv[2], NULL, 0);

    int ret = ring_transport_reg_write(qcspi_dev, reg_addr, reg_val);
    if (ret < 0) {
        shell_error(sh, "QCSPI register write failed: %d", ret);
        return ret;
    }

    shell_print(sh, "Wrote 0x%08X to register 0x%02X", reg_val, reg_addr);

    return 0;
}

/* Shell command: Trigger interrupt on QCC730 */
static int cmd_qcspi_interrupt(const struct shell *sh, size_t argc, char **argv)
{
    ring_transport_dev_t qcspi_dev = qcspi_transport_get_device();

    if (!qcspi_dev) {
        shell_error(sh, "QCSPI transport device not ready");
        return -ENODEV;
    }

    int ret = ring_transport_interrupt(qcspi_dev);
    if (ret < 0) {
        shell_error(sh, "QCSPI interrupt failed: %d", ret);
        return ret;
    }

    shell_print(sh, "A2F interrupt triggered successfully");

    return 0;
}

/* Shell command: Trigger interrupt on QCC730 */
static int cmd_qcspi_reset(const struct shell *sh, size_t argc, char **argv)
{
    ring_transport_dev_t qcspi_dev = qcspi_transport_get_device();

    if (!qcspi_dev) {
        shell_error(sh, "QCSPI transport device not ready");
        return -ENODEV;
    }

    int ret = ring_transport_reset(qcspi_dev);
    if (ret < 0) {
        shell_error(sh, "QCSPI reset failed: %d", ret);
        return ret;
    }

    shell_print(sh, "QCSPI reset successfully");

    return 0;
}

/* Shell command: Test ring_transport_write performance */
static int cmd_qcspi_test_transfer(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 4) {
        shell_error(sh, "Usage: qcspi test_transfer <address> <size> <count>");
        shell_error(sh, "Example: qcspi test_transfer 0x20000000 1024 1000");
        shell_error(sh, "  address: target memory address (e.g., 0x20000000)");
        shell_error(sh, "  size: bytes per write (1-2048)");
        shell_error(sh, "  count: number of write iterations (1-100000)");
        return -EINVAL;
    }

    ring_transport_dev_t qcspi_dev = qcspi_transport_get_device();
    if (!qcspi_dev) {
        shell_error(sh, "QCSPI transport device not ready");
        return -ENODEV;
    }

    uint32_t addr = strtoul(argv[1], NULL, 0);
    uint32_t size = strtoul(argv[2], NULL, 0);
    uint32_t count = strtoul(argv[3], NULL, 0);

    /* Validate parameters */
    if (size == 0 || size > 2048) {
        shell_error(sh, "Invalid size (1-2048 bytes)");
        return -EINVAL;
    }

    if (count == 0 || count > 100000) {
        shell_error(sh, "Invalid count (1-100000)");
        return -EINVAL;
    }

    /* Allocate test buffer */
    static uint8_t test_buffer[2048];

    /* Fill buffer with test pattern */
    for (uint32_t i = 0; i < size; i++) {
        test_buffer[i] = i & 0xFF;
    }

    shell_print(sh, "=== QCSPI Transport Write Performance Test ===");
    shell_print(sh, "Target Address: 0x%08X", addr);
    shell_print(sh, "Write Size: %u bytes", size);
    shell_print(sh, "Iterations: %u", count);
    shell_print(sh, "Starting test...");

    /* Record start time */
    uint32_t start_time = k_uptime_get_32();
    uint64_t start_cycles = k_cycle_get_64();

    uint32_t success_count = 0;
    uint32_t fail_count = 0;

    /* Perform write test */
    for (uint32_t i = 0; i < count; i++) {
        int ret = ring_transport_write(qcspi_dev, addr, test_buffer, size);
        if (ret < 0) {
            fail_count++;
            if (fail_count <= 10) { /* Only print first 10 errors */
                shell_error(sh, "Write %u failed: %d", i, ret);
            }
        } else {
            success_count++;
        }

        /* Print progress every 1000 iterations */
        if ((i + 1) % 1000 == 0) {
            shell_print(sh, "Progress: %u/%u writes completed", i + 1, count);
        }
    }

    /* Record end time */
    uint32_t elapsed_time = k_uptime_get_32() - start_time;
    uint64_t elapsed_cycles = k_cycle_get_64() - start_cycles;

    /* Calculate statistics */
    uint64_t total_bytes = (uint64_t)success_count * size;

    shell_print(sh, "");
    shell_print(sh, "=== Test Results ===");
    shell_print(sh, "Success: %u writes", success_count);
    shell_print(sh, "Failed: %u writes", fail_count);
    shell_print(sh, "Total Bytes Written: %llu bytes", total_bytes);
    shell_print(sh, "Elapsed Time: %u ms", elapsed_time);
    shell_print(sh, "Elapsed Cycles: %llu", elapsed_cycles);

    if (elapsed_time > 0) {
        /* Calculate throughput in bytes/sec */
        uint64_t bytes_per_sec = (total_bytes * 1000) / elapsed_time;

        /* Calculate throughput in bits/sec */
        uint64_t bits_per_sec = bytes_per_sec * 8;

        /* Calculate writes per second */
        uint32_t writes_per_sec = (success_count * 1000) / elapsed_time;

        /* Calculate average time per write in microseconds */
        uint32_t avg_time_us = (elapsed_time * 1000) / success_count;

        shell_print(sh, "");
        shell_print(sh, "=== Performance Metrics ===");
        shell_print(sh, "Throughput: %llu bytes/sec (%llu KB/sec)", bytes_per_sec, bytes_per_sec / 1024);
        shell_print(sh, "Throughput: %llu bps (%llu Kbps, %llu Mbps)", bits_per_sec, bits_per_sec / 1024,
                    bits_per_sec / (1024 * 1024));
        shell_print(sh, "Write Rate: %u writes/sec", writes_per_sec);
        shell_print(sh, "Average Time per Write: %u us", avg_time_us);

        if (elapsed_cycles > 0) {
            uint64_t cycles_per_write = elapsed_cycles / success_count;
            shell_print(sh, "Average Cycles per Write: %llu", cycles_per_write);
        }
    }

    if (fail_count > 0) {
        shell_error(sh, "Test completed with %u failures", fail_count);
        return -EIO;
    }

    shell_print(sh, "Test completed successfully");
    return 0;
}

/* Create shell subcommands */
SHELL_STATIC_SUBCMD_SET_CREATE(sub_spi_cmds, SHELL_CMD(send_hex, NULL, "Send hex bytes via SPI", cmd_spi_send_hex),
                               SHELL_CMD(config_freq, NULL, "Configure SPI frequency", cmd_spi_config_freq),
                               SHELL_CMD(status, NULL, "Show SPI bridge status", cmd_spi_status),
                               SHELL_SUBCMD_SET_END /* Array terminator */
);

SHELL_CMD_REGISTER(spi, &sub_spi_cmds, "SPI bridge commands", NULL);

/* Create QCSPI shell subcommands */
SHELL_STATIC_SUBCMD_SET_CREATE(sub_qcspi_cmds, SHELL_CMD(init, NULL, "Initialize QCSPI transport", cmd_qcspi_init),
                               SHELL_CMD(read, NULL, "Read QCC730 memory", cmd_qcspi_read),
                               SHELL_CMD(write, NULL, "Write QCC730 memory", cmd_qcspi_write),
                               SHELL_CMD(reg_read, NULL, "Read QCC730 register", cmd_qcspi_reg_read),
                               SHELL_CMD(reg_write, NULL, "Write QCC730 register", cmd_qcspi_reg_write),
                               SHELL_CMD(interrupt, NULL, "Trigger A2F interrupt", cmd_qcspi_interrupt),
                               SHELL_CMD(reset, NULL, "Trigger qcspi sw reset", cmd_qcspi_reset),
                               SHELL_CMD(test_transfer, NULL, "Test qcspi_transfer performance",
                                         cmd_qcspi_test_transfer),
                               SHELL_SUBCMD_SET_END /* Array terminator */
);

SHELL_CMD_REGISTER(qcspi, &sub_qcspi_cmds, "QCSPI transport commands", NULL);

#ifdef CONFIG_RING_SERVICE
/* GPIO interrupt pin for ring service (PC12) */
#define RING_GPIO_NODE DT_NODELABEL(gpioc)
#define RING_GPIO_PIN 12

static const struct gpio_dt_spec ring_gpio = {
    .port = DEVICE_DT_GET(RING_GPIO_NODE), .pin = RING_GPIO_PIN, .dt_flags = GPIO_INT_EDGE_RISING};

static struct gpio_callback ring_gpio_cb_data;

/* Forward declarations */
void ring_host_rx_callback(uint8_t ring_id, void *user_data);
static void ring_gpio_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins);

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
            ret = ring_send(ring_id, send_buffer, size, 10);
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
        shell_print(sh, "Throughput: %u bps", throughput);

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

    int ret = ring_send(ring_id, (const uint8_t *)send_buffer, offset, 1000);
    if (ret < 0) {
        shell_error(sh, "Ring send failed: %d", ret);
        return ret;
    }

    shell_print(sh, "Data sent successfully");
    return 0;
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

    int ret = ring_send(ring_id, send_buffer, byte_count, 1000);
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

    int ret = ring_send(ring_id, send_buffer, len, 1000);
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
        if (size == 0 || size > 36000000) { /* Max 1 hour */
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
        timeout_sec = 300;
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

/**
 * @brief GPIO interrupt handler for ring service
 *
 * Called when QCC730 signals data availability via GPIO interrupt.
 * Triggers ring service RX processing in work queue context.
 */
static void ring_gpio_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    /* Trigger ring service RX processing */
    ring_rx_handler();
}
#endif /* CONFIG_RING_SERVICE */

int main(void)
{
    int ret;
    QC_OSAL_LOG_INF("Starting SPI1 Master example for STM32 Nucleo Boards");

    /* Initialize bridge data structure */
    memset(&bridge_data, 0, sizeof(bridge_data));
    k_mutex_init(&bridge_data.spi_mutex);

    /* Initialize LED */
    ret = gpio_is_ready_dt(&led);
    if (ret < 0) {
        QC_OSAL_LOG_ERR("LED GPIO device not ready");
        return ret;
    }

    ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        QC_OSAL_LOG_ERR("Failed to configure LED GPIO: %d", ret);
        return ret;
    }

    bridge_data.spi_dev = spi1_dev_ptr;
    if (!device_is_ready(bridge_data.spi_dev)) {
        QC_OSAL_LOG_ERR("SPI1 device not ready");
        return -ENODEV;
    }

    QC_OSAL_LOG_INF("SPI1 device initialized");

    QC_OSAL_LOG_INF("SPI Shell ready - type 'spi help' and 'qcspi help' for commands");

#ifdef CONFIG_RING_SERVICE
    init_qring();
    /* Register callback to handle data from QCC730 */
    ret = ring_register_callback(ring_host_rx_callback, NULL);
    if (ret == 0) {
        QC_OSAL_LOG_INF("Ring callback registered successfully");
    } else {
        QC_OSAL_LOG_ERR("Failed to register ring callback: %d", ret);
    }

    /* Setup GPIO interrupt using Zephyr native APIs */
    if (!gpio_is_ready_dt(&ring_gpio)) {
        QC_OSAL_LOG_ERR("Ring GPIO device not ready");
    } else {
        /* Configure GPIO as input with interrupt */
        ret = gpio_pin_configure_dt(&ring_gpio, GPIO_INPUT);
        if (ret < 0) {
            QC_OSAL_LOG_ERR("Failed to configure ring GPIO pin: %d", ret);
        } else {
            /* Initialize and add GPIO callback */
            gpio_init_callback(&ring_gpio_cb_data, ring_gpio_isr, BIT(ring_gpio.pin));
            ret = gpio_add_callback(ring_gpio.port, &ring_gpio_cb_data);
            if (ret < 0) {
                QC_OSAL_LOG_ERR("Failed to add ring GPIO callback: %d", ret);
            } else {
                /* Enable interrupt */
                ret = gpio_pin_interrupt_configure_dt(&ring_gpio, GPIO_INT_EDGE_RISING);
                if (ret < 0) {
                    QC_OSAL_LOG_ERR("Failed to configure ring GPIO interrupt: %d", ret);
                } else {
                    QC_OSAL_LOG_INF("Ring GPIO interrupt configured successfully (PC%d)", RING_GPIO_PIN);
                }
            }
        }
    }
#endif

    /* Main thread can do other work or just sleep */
    while (1) {
        /* Blink LED on Nucleo Board every 200 millisecond */
        k_msleep(200);
        bridge_data.led_state = !bridge_data.led_state;
        gpio_pin_set_dt(&led, bridge_data.led_state);
    }
}

#ifdef CONFIG_RING_SERVICE

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
        QC_OSAL_LOG_INF("Received string (%d bytes): %.*s", len, len, data);
    } else {
        /* Print as hex - fallback to Zephyr LOG for hex dump */
        LOG_HEXDUMP_INF(data, len, "Received hex data:");
    }
}

/**
 * @brief Ring service RX callback - handles data from QCC730
 *
 * Self-retriggering version - checks for more data after processing
 * and re-submits work if needed to handle lost GPIO interrupts
 */
void ring_host_rx_callback(uint8_t ring_id, void *user_data)
{
    int ret;
    int packet_count = 0;

    ARG_UNUSED(user_data);

    // QC_OSAL_LOG_INF("Ring RX event received, reading all available data...");

    /* Loop to read all available data packets */
    do {
        ret = ring_recv(ring_id, recv_buffer, sizeof(recv_buffer), 0);
        if (ret > 0) {
            packet_count++;
            switch (ring_id) {
            case RING_CONFIG:
                break;
            case RING_DATA:
                break;
            case RING_LOOPBACK:
                print_received_data(recv_buffer, ret);
                break;
            case RING_ERROR:
                break;
            default:
                break;
            }
        } else if (ret < 0) {
            QC_OSAL_LOG_ERR("Failed to receive data: %d", ret);
            break; /* Exit on error */
        }

        /* ret == 0 means no more data available, loop will exit */
    } while (ret > 0);

    if (packet_count > 0) {
        QC_OSAL_LOG_DBG("Total processed %d data packets from ring %d", packet_count, ring_id);
    } else {
        QC_OSAL_LOG_WRN("No data available from ring %d", ring_id);
    }
}
#endif /* CONFIG_RING_SERVICE */
#endif
