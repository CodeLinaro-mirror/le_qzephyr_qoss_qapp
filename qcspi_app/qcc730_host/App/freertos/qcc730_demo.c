/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
#include "../../Port/qc_port_config.h"

#if defined(QC_OS_FREERTOS) && defined(SPI_DEMO_ENABLE)
#ifdef SHELL_FEATURE
#include "shell.h"
#endif
#include "../../Port/osal/qc_osal.h"
#include "../../Service/qcspi/qcspi_adapter.h"

#ifdef CONFIG_RING_SERVICE
#include "../../Service/ring/ring_service.h"
#endif

#ifdef SHELL_FEATURE

static uint8_t send_buffer[1500];
static uint8_t recv_buffer[1500];

void hexdump_printf(const void *data, size_t len, const char *title)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t offset = 0;

    if (title) {
        printf("%s (len=%u)\r\n", title, (unsigned)len);
    }

    while (offset < len) {

        size_t line_len = (len - offset) > 16 ? 16 : (len - offset);

        printf("%08X  ", (unsigned)offset);

        for (size_t i = 0; i < 16; i++) {
            if (i < line_len) {
                printf("%02X ", p[offset + i]);
            } else {
                printf("   ");
            }
            if (i == 7)
                printf(" ");
        }

        printf(" |");
        for (size_t i = 0; i < line_len; i++) {
            uint8_t c = p[offset + i];
            printf("%c", (c >= 32 && c <= 126) ? c : '.');
        }
        printf("|\r\n");

        offset += line_len;
    }
}

/**
 * @brief Parse hex string to bytes
 */
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
        return -QC_OSAL_EINVAL;
    }

    byte_count = hex_len / 2;
    if (byte_count > max_bytes) {
        return -QC_OSAL_E2BIG;
    }

    for (size_t i = 0; i < byte_count; i++) {
        char byte_str[3] = {hex_str[i * 2], hex_str[i * 2 + 1], '\0'};
        char *endptr;
        unsigned long val = strtoul(byte_str, &endptr, 16);

        if (*endptr != '\0' || val > 0xFF) {
            return -QC_OSAL_EINVAL;
        }

        bytes[i] = (uint8_t)val;
    }

    return byte_count;
}

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

    qc_osal_msleep(5);

    if (is_printable) {
        /* Print as string */
        QC_OSAL_LOG_INF("Received string (%d bytes): %.*s", len, len, data);
    } else {
        /* Print as hex */
        hexdump_printf(data, len, "Received hex data");
    }
}

/**
 * @brief Ring service RX callback - handles data from QCC730
 */
void ring_host_rx_callback(uint8_t ring_id, void *user_data)
{
    int ret;
    int packet_count = 0;

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
        if (packet_count > 0 && packet_count % 10 == 0)
            qc_osal_msleep(3);

        /* ret == 0 means no more data available, loop will exit */
    } while (ret > 0);

    if (packet_count > 0) {
        QC_OSAL_LOG_INF("Total processed %d data packets from ring %d", packet_count, ring_id);
    } else {
        QC_OSAL_LOG_INF("No data available from ring %d", ring_id);
    }
}

/* ============================================================================
 * QCSPI Shell Commands
 * ============================================================================ */

/**
 * @brief Initialize QCSPI adapter
 * Usage: qcspi_init
 */
int cmd_qcspi_init(int argc, char **argv)
{
    int ret;

    /* Check if already initialized */
    if (qcspi_adapter_is_initialized()) {
        printf("QCSPI adapter is already initialized\r\n");
        
        /* Get QCC730 slave ID */
        uint8_t slave_id[3] = {0};
        qcspi_get_slaveid(slave_id);
        printf("QCC730 Slave ID: %02X %02X %02X\r\n", slave_id[0], slave_id[1], slave_id[2]);
        
        return 0;
    }

    /* Initialize QCSPI adapter */
    printf("Initializing QCSPI adapter...\r\n");
    ret = qcspi_adapter_init();
    if (ret < 0) {
        printf("ERROR: QCSPI adapter initialization failed: %d\r\n", ret);
        return ret;
    }

    printf("QCSPI adapter initialized successfully\r\n");

    /* Get QCC730 slave ID */
    uint8_t slave_id[3] = {0};
    qcspi_get_slaveid(slave_id);
    printf("QCC730 Slave ID: %02X %02X %02X\r\n", slave_id[0], slave_id[1], slave_id[2]);

    return 0;
}

/* QCC730 valid memory range */
#define QCC730_MEM_START 0x10000
#define QCC730_MEM_END 0x9FC00
#define QCC730_MEM_SIZE (QCC730_MEM_END - QCC730_MEM_START)

/**
 * @brief Validate memory address range
 * @param addr Start address
 * @param len Length in bytes
 * @return 0 if valid, negative error code otherwise
 */
static int validate_memory_range(uint32_t addr, uint32_t len)
{
    /* Check for zero length */
    if (len == 0) {
        printf("ERROR: Length cannot be zero\r\n");
        return -QC_OSAL_EINVAL;
    }

    /* Check for overflow */
    if (addr + len < addr) {
        printf("ERROR: Address overflow\r\n");
        return -QC_OSAL_EINVAL;
    }

    /* Check if address is within valid range */
    if (addr < QCC730_MEM_START || addr >= QCC730_MEM_END) {
        printf("ERROR: Address 0x%08X out of range (0x%08X - 0x%08X)\r\n", addr, QCC730_MEM_START, QCC730_MEM_END -1);
        return -QC_OSAL_EINVAL;
    }

    /* Check if end address is within valid range */
    if (addr + len > QCC730_MEM_END) {
        printf("ERROR: Access range 0x%08X-0x%08X exceeds memory limit 0x%08X\r\n", addr, addr + len, QCC730_MEM_END - 1);
        return -QC_OSAL_EINVAL;
    }

    return 0;
}

/**
 * @brief Read QCC730 memory via QCSPI adapter
 * Usage: qcspi_read <address> <length>
 */
int cmd_qcspi_read(int argc, char **argv)
{
    int ret = 0;
    if (argc != 3) {
        printf("Usage: qcspi_read <address> <length>\r\n");
        printf("Example: qcspi_read 0x8fc00 4\r\n");
        printf("Valid range: 0x0 - 0x9FFFF\r\n");
        return -QC_OSAL_EINVAL;
    }

    uint32_t addr = strtoul(argv[1], NULL, 0);
    uint32_t len = strtoul(argv[2], NULL, 0);

    /* Validate length */
    if (len > 1500) {
        printf("ERROR: Length too large (max 1500 bytes)\r\n");
        return -QC_OSAL_EINVAL;
    }

    /* Validate memory range */
    if (addr + len - 1 > 0x9FFFF) {
        printf("ERROR: Access range 0x%08X-0x%08X exceeds memory limit 0x9FFFF\r\n", addr, addr + len - 1);
        return -QC_OSAL_EINVAL;
    }

    ret = qcspi_adapter_mem_read(addr, recv_buffer, len);
    if (ret < 0) {
        printf("ERROR: QCSPI read failed: %d\r\n", ret);
        return ret;
    }

    printf("Read %u bytes from 0x%08X:\r\n", len, addr);
    for (uint32_t i = 0; i < len; i++) {
        if (i % 16 == 0) {
            printf("%08X: ", addr + i);
        }
        printf("%02X ", recv_buffer[i]);
        if ((i + 1) % 16 == 0) {
            printf("\r\n");
        }
    }
    if (len % 16 != 0) {
        printf("\r\n");
    }

    return 0;
}

/**
 * @brief Write QCC730 memory via QCSPI adapter
 * Usage: qcspi_write <address> <hex_data>
 */
int cmd_qcspi_write(int argc, char **argv)
{
    if (argc != 3) {
        printf("Usage: qcspi_write <address> <hex_data>\r\n");
        printf("Example: qcspi_write 0x8fc00 AABBCCDD\r\n");
        printf("Valid range: 0x%08X - 0x%08X\r\n", QCC730_MEM_START, QCC730_MEM_END);
        return -QC_OSAL_EINVAL;
    }

    uint32_t addr = strtoul(argv[1], NULL, 0);

    int len = hex_string_to_bytes(argv[2], send_buffer, 1500);
    if (len < 0) {
        printf("ERROR: Invalid hex data format\r\n");
        return len;
    }

    /* Validate memory range */
    int ret = validate_memory_range(addr, len);
    if (ret < 0) {
        return ret;
    }

    ret = qcspi_adapter_mem_write(addr, send_buffer, len);
    if (ret < 0) {
        printf("ERROR: QCSPI write failed: %d\r\n", ret);
        return ret;
    }

    printf("Wrote %d bytes to 0x%08X\r\n", len, addr);

    return 0;
}

/* QCC730 register range */
#define QCC730_REG_START 0x00
#define QCC730_REG_END 0x60
#define QCC730_REG_ALIGN 4

/**
 * @brief Validate register address
 * @param reg_addr Register address/offset
 * @return 0 if valid, negative error code otherwise
 */
static int validate_register_addr(uint8_t reg_addr)
{
    /* Check if register address is within valid range */
    if (reg_addr < QCC730_REG_START || reg_addr > QCC730_REG_END) {
        printf("ERROR: Register address 0x%02X out of range (0x%02X - 0x%02X)\r\n", reg_addr, QCC730_REG_START,
               QCC730_REG_END);
        return -QC_OSAL_EINVAL;
    }

    /* Check if register address is 4-byte aligned */
    if (reg_addr % QCC730_REG_ALIGN != 0) {
        printf("ERROR: Register address 0x%02X not aligned to %d bytes\r\n", reg_addr, QCC730_REG_ALIGN);
        return -QC_OSAL_EINVAL;
    }

    return 0;
}

/**
 * @brief Read QCC730 register via QCSPI
 * Usage: qcspi_reg_read <reg_addr>
 */
int cmd_qcspi_reg_read(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage: qcspi_reg_read <reg_addr>\r\n");
        printf("Example: qcspi_reg_read 0x0C\r\n");
        printf("Valid range: 0x%02X - 0x%02X (4-byte aligned)\r\n", QCC730_REG_START, QCC730_REG_END);
        return -QC_OSAL_EINVAL;
    }

    uint8_t reg_addr = (uint8_t)strtoul(argv[1], NULL, 0);

    /* Validate register address */
    int ret = validate_register_addr(reg_addr);
    if (ret < 0) {
        return ret;
    }

    uint32_t reg_val;

    ret = qcspi_IRR(reg_addr, &reg_val);
    if (ret < 0) {
        printf("ERROR: QCSPI register read failed: %d\r\n", ret);
        return ret;
    }

    printf("Register 0x%02X = 0x%08X\r\n", reg_addr, reg_val);

    return 0;
}

/**
 * @brief Write QCC730 register via QCSPI
 * Usage: qcspi_reg_write <reg_addr> <value>
 */
int cmd_qcspi_reg_write(int argc, char **argv)
{
    if (argc != 3) {
        printf("Usage: qcspi_reg_write <reg_addr> <value>\r\n");
        printf("Example: qcspi_reg_write 0x0C 0x12345678\r\n");
        printf("Valid range: 0x%02X - 0x%02X (4-byte aligned)\r\n", QCC730_REG_START, QCC730_REG_END);
        return -QC_OSAL_EINVAL;
    }

    uint8_t reg_addr = (uint8_t)strtoul(argv[1], NULL, 0);
    uint32_t reg_val = strtoul(argv[2], NULL, 0);

    /* Validate register address */
    int ret = validate_register_addr(reg_addr);
    if (ret < 0) {
        return ret;
    }

    ret = qcspi_IRW(reg_addr, reg_val);
    if (ret < 0) {
        printf("ERROR: QCSPI register write failed: %d\r\n", ret);
        return ret;
    }

    printf("Wrote 0x%08X to register 0x%02X\r\n", reg_val, reg_addr);

    return 0;
}
/**
 * @brief Trigger interrupt on QCC730
 * Usage: qcspi_interrupt
 */
int cmd_qcspi_interrupt(int argc, char **argv)
{
    int ret = qcspi_adapter_trigger_irq();
    if (ret < 0) {
        printf("ERROR: QCSPI interrupt failed: %d\r\n", ret);
        return ret;
    }

    printf("A2F interrupt triggered successfully\r\n");

    return 0;
}

void qcc730_reset()
{
#define DELAY_TIMING 300
    int ret = -1;

    HAL_NVIC_DisableIRQ(EXTI12_IRQn);

    while (ret < 0) {
        /* host toggle */
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_7, GPIO_PIN_RESET); // chip_on test
        HAL_Delay(DELAY_TIMING);
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_7, GPIO_PIN_SET); // chip_on test
        HAL_Delay(DELAY_TIMING);
        HAL_Delay(1000);
        
        /* Deinitialize ring service and adapter */
        ring_service_deinit();
        qcspi_adapter_deinit();

        /* Initialize QCSPI adapter and ring service */
        ret = init_qring();
    }

    HAL_NVIC_EnableIRQ(EXTI12_IRQn);
}

/**
 * @brief Reset QCSPI
 * Usage: qcspi_reset
 */
int cmd_qcspi_reset(int argc, char **argv)
{
    qcc730_reset();

    return 0;
}

/* Shell command: Test qcspi write/read performance */
static int cmd_qcspi_test_transfer(int argc, char **argv)
{
    if (argc != 5) {
        printf("Usage: qcspi_test_transfer <tx|rx> <address> <size> <count>\r\n");
        printf("Example: qcspi_test_transfer tx 0x8fc00 1452 1000\r\n");
        printf("Example: qcspi_test_transfer rx 0x8fc00 1452 1000\r\n");
        printf("  tx|rx: transfer direction (tx=write, rx=read)\r\n");
        printf("  address: target memory address (0x%08X - 0x%08X)\r\n", QCC730_MEM_START, QCC730_MEM_END);
        printf("  size: bytes per transfer (1-1500)\r\n");
        printf("  count: number of transfer iterations (1-100000)\r\n");
        return -QC_OSAL_EINVAL;
    }

    /* Parse direction */
    const char *direction = argv[1];
    bool is_tx = false;

    if (strcmp(direction, "tx") == 0) {
        is_tx = true;
    } else if (strcmp(direction, "rx") == 0) {
        is_tx = false;
    } else {
        printf("ERROR: Invalid direction '%s' (use 'tx' or 'rx')\r\n", direction);
        return -QC_OSAL_EINVAL;
    }

    uint32_t addr = strtoul(argv[2], NULL, 0);
    uint32_t size = strtoul(argv[3], NULL, 0);
    uint32_t count = strtoul(argv[4], NULL, 0);

    /* Validate size parameter */
    if (size == 0 || size > 1500) {
        printf("ERROR: Invalid size (1-1500 bytes)\r\n");
        return -QC_OSAL_EINVAL;
    }

    /* Validate count parameter */
    if (count == 0 || count > 100000) {
        printf("ERROR: Invalid count (1-100000)\r\n");
        return -QC_OSAL_EINVAL;
    }

    /* Validate memory range */
    int ret = validate_memory_range(addr, size);
    if (ret < 0) {
        return ret;
    }

    /* Allocate test buffer */
    static uint8_t test_buffer[1500];

    /* Fill buffer with test pattern for TX */
    if (is_tx) {
        for (uint32_t i = 0; i < size; i++) {
            test_buffer[i] = i & 0xFF;
        }
    }

    printf("=== QCSPI Transport %s Performance Test ===\r\n", is_tx ? "Write (TX)" : "Read (RX)");
    printf("Target Address: 0x%08X\r\n", addr);
    printf("Transfer Size: %u bytes\r\n", size);
    printf("Iterations: %u\r\n", count);
    printf("Starting test...\r\n");

    /* Record start time */
    uint32_t start_time = qc_osal_uptime_get_ms();

    uint32_t success_count = 0;
    uint32_t fail_count = 0;

    /* Perform transfer test */
    for (uint32_t i = 0; i < count; i++) {
        if (is_tx) {
            /* TX: use qcspi_adapter_mem_write */
            ret = qcspi_adapter_mem_write(addr, test_buffer, size);
        } else {
            /* RX: use qcspi_adapter_mem_read */
            ret = qcspi_adapter_mem_read(addr, test_buffer, size);
        }

        if (ret < 0) {
            fail_count++;
            if (fail_count <= 10) { /* Only print first 10 errors */
                printf("%s %u failed: %d\r\n", is_tx ? "Write" : "Read", i, ret);
            }
        } else {
            success_count++;
        }

        /* Print progress every 1000 iterations */
        if ((i + 1) % 1000 == 0) {
            printf("Progress: %u/%u %s completed\r\n", i + 1, count, is_tx ? "writes" : "reads");
        }
    }

    /* Record end time */
    uint32_t elapsed_time = qc_osal_uptime_get_ms() - start_time;

    /* Calculate statistics */
    uint64_t total_bytes = (uint64_t)success_count * size;

    printf("\r\n");
    printf("=== Test Results ===\r\n");
    printf("Success: %u %s\r\n", success_count, is_tx ? "writes" : "reads");
    printf("Failed: %u %s\r\n", fail_count, is_tx ? "writes" : "reads");
    printf("Total Bytes %s: %llu bytes\r\n", is_tx ? "Written" : "Read", (unsigned long long)total_bytes);
    printf("Elapsed Time: %u ms\r\n", elapsed_time);

    if (elapsed_time > 0) {
        uint64_t throughput_bps = (total_bytes * 8 * 1000) / elapsed_time; /* bps */

        /* Bytes per second */
        uint32_t bytes_per_sec = (uint32_t)((total_bytes * 1000) / elapsed_time);
        uint32_t kb_per_sec = bytes_per_sec / 1024;
        uint32_t kb_per_sec_frac = ((bytes_per_sec % 1024) * 100) / 1024;

        /* Kbps */
        uint32_t throughput_kbps = (uint32_t)(throughput_bps / 1000);

        /* Mbps with 2 decimal places */
        uint32_t throughput_mbps = (uint32_t)(throughput_bps / 1000000);
        uint32_t throughput_mbps_frac = (uint32_t)((throughput_bps % 1000000) / 10000);

        /* Transfers per second */
        uint32_t transfers_per_sec = (success_count * 1000) / elapsed_time;

        /* Average time per transfer in microseconds */
        uint32_t avg_time_us = (elapsed_time * 1000) / success_count;

        printf("\r\n");
        printf("=== Performance Metrics ===\r\n");

        printf("Throughput:\r\n");
        printf("  %u bytes/sec (%u.%02u KB/sec)\r\n", bytes_per_sec, kb_per_sec, kb_per_sec_frac);
        printf("  %u Kbps (%u.%02u Mbps)\r\n", throughput_kbps, throughput_mbps, throughput_mbps_frac);

        printf("Transfer Rate: %u %s/sec\r\n", transfers_per_sec, is_tx ? "writes" : "reads");
        printf("Average Time per Transfer: %u us\r\n", avg_time_us);
    }

    if (fail_count > 0) {
        printf("Test completed with %u failures\r\n", fail_count);
        return -QC_OSAL_EIO;
    }

    printf("Test completed successfully\r\n");
    return 0;
}

#ifdef CONFIG_RING_SERVICE
/* ============================================================================
 * Ring Service Shell Commands
 * ============================================================================ */

int ring_status(int argc, char **argv)
{
    int ring_num = ring_get_num();
    printf("=== Ring Service Status ===\r\n");

    /* Show status for all configured rings */
    for (uint8_t ring_id = 0; ring_id < ring_num; ring_id++) {
        int tx_avail = ring_get_tx_available(ring_id);
        int rx_avail = ring_get_rx_available(ring_id);
        struct ring_stats stats;

        /* Skip unconfigured rings */
        if (tx_avail == -QC_OSAL_EINVAL || rx_avail == -QC_OSAL_EINVAL) {
            continue;
        }

        printf("\r\n--- Ring %u ---\r\n", ring_id);

        if (tx_avail >= 0) {
            printf("TX Available: %d descriptors\r\n", tx_avail);
        } else {
            printf("TX Available: Error %d\r\n", tx_avail);
        }

        if (rx_avail >= 0) {
            printf("RX Available: %d descriptors\r\n", rx_avail);
        } else {
            printf("RX Available: Error %d\r\n", rx_avail);
        }

        if (ring_get_stats(ring_id, &stats) == 0) {
            printf("TX Count: %u\r\n", stats.tx_count);
            printf("RX Count: %u\r\n", stats.rx_count);
            printf("TX Errors: %u\r\n", stats.tx_errors);
            printf("RX Errors: %u\r\n", stats.rx_errors);
        }
    }

    return 0;
}

/**
 * @brief Send string data via ring service
 * Usage: ring_send <ring_id> <string>
 */
int ring_send_cmd(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: ring_send <ring_id> <string>\r\n");
        printf("Example: ring_send 0 Hello\r\n");
        return -QC_OSAL_EINVAL;
    }

    uint8_t ring_id = (uint8_t)strtoul(argv[1], NULL, 0);
    if (ring_id >= MAX_RINGS) {
        printf("ERROR: Invalid ring_id (0-%d)\r\n", MAX_RINGS - 1);
        return -QC_OSAL_EINVAL;
    }

    /* Concatenate all arguments into one string (starting from argv[2]) */
    size_t offset = 0;

    for (size_t i = 2; i < argc && offset < 1500 - 1; i++) {
        size_t arg_len = strlen(argv[i]);
        if (offset + arg_len + 1 > 1500 - 1) {
            break;
        }

        if (i > 2) {
            send_buffer[offset++] = ' ';
        }

        memcpy(send_buffer + offset, argv[i], arg_len);
        offset += arg_len;
    }
    send_buffer[offset] = '\0';

    // printf("Sending %u bytes on ring %u: \"%s\"\r\n", (unsigned int)offset, ring_id, send_buffer);

    int ret = ring_send(ring_id, (const uint8_t *)send_buffer, offset, 1000);
    if (ret < 0) {
        printf("ERROR: Ring send failed: %d\r\n", ret);
        return ret;
    }

    // printf("Data sent successfully\r\n");
    return 0;
}

/**
 * @brief Send hex data via ring service
 * Usage: ring_send_hex <ring_id> <hex_string>
 */
int ring_send_hex(int argc, char **argv)
{
    if (argc != 3) {
        printf("Usage: ring_send_hex <ring_id> <hex_string>\r\n");
        printf("Example: ring_send_hex 0 AABBCCDD\r\n");
        return -QC_OSAL_EINVAL;
    }

    uint8_t ring_id = (uint8_t)strtoul(argv[1], NULL, 0);
    if (ring_id >= MAX_RINGS) {
        printf("ERROR: Invalid ring_id (0-%d)\r\n", MAX_RINGS - 1);
        return -QC_OSAL_EINVAL;
    }

    int byte_count = hex_string_to_bytes(argv[2], send_buffer, 1500);
    if (byte_count < 0) {
        printf("ERROR: Invalid hex string format\r\n");
        return byte_count;
    }

    // printf("Sending %d bytes (hex) on ring %u:\r\n", byte_count, ring_id);
    //	for (int i = 0; i < byte_count; i++) {
    //		printf("%02X ", send_buffer[i]);
    //		if ((i + 1) % 16 == 0) {
    //			printf("\r\n");
    //		}
    //	}
    //	if (byte_count % 16 != 0) {
    //		printf("\r\n");
    //	}

    int ret = ring_send(ring_id, send_buffer, byte_count, 1000);
    if (ret < 0) {
        printf("ERROR: Ring send failed: %d\r\n", ret);
        return ret;
    }

    // printf("Data sent successfully\r\n");
    return 0;
}

/**
 * @brief Send test pattern via ring service
 * Usage: ring_pattern <ring_id> <length>
 */
int ring_send_pattern(int argc, char **argv)
{
    if (argc != 3) {
        printf("Usage: ring_pattern <ring_id> <length>\r\n");
        printf("Example: ring_pattern 0 100\r\n");
        return -QC_OSAL_EINVAL;
    }

    uint8_t ring_id = (uint8_t)strtoul(argv[1], NULL, 0);
    if (ring_id >= MAX_RINGS) {
        printf("ERROR: Invalid ring_id (0-%d)\r\n", MAX_RINGS - 1);
        return -QC_OSAL_EINVAL;
    }

    uint32_t len = strtoul(argv[2], NULL, 0);
    if (len == 0 || len > 1500) {
        printf("ERROR: Invalid length (1-1500)\r\n");
        return -QC_OSAL_EINVAL;
    }

    for (uint32_t i = 0; i < len; i++) {
        send_buffer[i] = i & 0xFF;
    }

    // printf("Sending %u bytes test pattern on ring %u\r\n", len, ring_id);

    int ret = ring_send(ring_id, send_buffer, len, 1000);
    if (ret < 0) {
        printf("ERROR: Ring send failed: %d\r\n", ret);
        return ret;
    }

    // printf("Test pattern sent successfully\r\n");
    return 0;
}

/**
 * @brief Burst test mode
 */
enum burst_test_mode {
    BURST_MODE_COUNT, /* Test by packet count */
    BURST_MODE_TIME,  /* Test by duration (seconds) */
    BURST_MODE_DATA   /* Test by total data size (MB) */
};

/**
 * @brief Burst test parameters
 */
struct burst_test_params {
    uint8_t ring_id;
    enum burst_test_mode mode;
    union {
        uint32_t count;        /* Packet count for BURST_MODE_COUNT */
        uint32_t duration_sec; /* Duration in seconds for BURST_MODE_TIME */
        uint32_t data_mb;      /* Total data in MB for BURST_MODE_DATA */
    };
    uint32_t size; /* Packet size in bytes */
    bool running;
};

static struct burst_test_params burst_params = {0};
static qc_osal_thread_t burst_thread;

/**
 * @brief Burst test thread function
 */
static void burst_test_thread(void *arg)
{
    struct burst_test_params *params = (struct burst_test_params *)arg;
    uint8_t ring_id = params->ring_id;
    uint32_t size = params->size;

    /* Allocate local buffer for this thread */
    uint8_t *test_buffer = qc_osal_malloc(size);
    if (!test_buffer) {
        printf("ERROR: Failed to allocate test buffer\r\n");
        params->running = false;
        return;
    }

    /* Fill buffer with test pattern */
    for (uint32_t i = 0; i < size; i++) {
        test_buffer[i] = i & 0xFF;
    }

    /* Print test configuration */
    printf("[BURST_THREAD] Starting burst test on ring %u\r\n", ring_id);
    printf("  Packet size: %u bytes\r\n", size);

    switch (params->mode) {
    case BURST_MODE_COUNT:
        printf("  Mode: Packet count (%u packets)\r\n", params->count);
        break;
    case BURST_MODE_TIME:
        printf("  Mode: Duration (%u seconds)\r\n", params->duration_sec);
        break;
    case BURST_MODE_DATA:
        printf("  Mode: Data size (%u MB)\r\n", params->data_mb);
        break;
    }

    uint32_t start_time = qc_osal_uptime_get_ms();
    uint32_t success_count = 0;
    uint32_t fail_count = 0;
    uint32_t packet_index = 0;

    /* Calculate limits based on mode */
    uint32_t end_time = 0;
    uint64_t target_bytes = 0;
    uint64_t total_bytes = 0;

    switch (params->mode) {
    case BURST_MODE_TIME:
        end_time = start_time + (params->duration_sec * 1000);
        break;
    case BURST_MODE_DATA:
        target_bytes = (uint64_t)params->data_mb * 1024 * 1024;
        break;
    case BURST_MODE_COUNT:
        /* No additional calculation needed */
        break;
    }

    /* Main test loop */
    while (params->running) {
        /* Check termination conditions based on mode */
        switch (params->mode) {
        case BURST_MODE_COUNT:
            if (packet_index >= params->count) {
                goto test_complete;
            }
            break;
        case BURST_MODE_TIME:
            if (qc_osal_uptime_get_ms() >= end_time) {
                goto test_complete;
            }
            break;
        case BURST_MODE_DATA:
            if (total_bytes >= target_bytes) {
                goto test_complete;
            }
            break;
        }

        /* Send packet */
        int ret = ring_send(ring_id, test_buffer, size, 10);
        if (ret < 0) {
            fail_count++;
            if (fail_count <= 10) {
                printf("[BURST_THREAD] Packet %u failed: %d\r\n", packet_index, ret);
            }
        } else {
            success_count++;
            total_bytes += size;
        }

        packet_index++;

        /* Print progress based on mode */
        if (params->mode == BURST_MODE_COUNT) {
            if (packet_index % 100 == 0) {
                printf("[BURST_THREAD] Progress: %u/%u packets\r\n", packet_index, params->count);
            }
        } else if (params->mode == BURST_MODE_TIME) {
            if (packet_index % 100 == 0) {
                uint32_t elapsed = qc_osal_uptime_get_ms() - start_time;
                uint32_t remaining =
                    (end_time > qc_osal_uptime_get_ms()) ? (end_time - qc_osal_uptime_get_ms()) / 1000 : 0;
                printf("[BURST_THREAD] Progress: %u packets, %u/%u sec remaining\r\n", packet_index, remaining,
                       params->duration_sec);
            }
        } else if (params->mode == BURST_MODE_DATA) {
            if (packet_index % 100 == 0) {
                uint32_t sent_mb = (uint32_t)(total_bytes / (1024 * 1024));
                printf("[BURST_THREAD] Progress: %u/%u MB sent\r\n", sent_mb, params->data_mb);
            }
        }
        qc_osal_thread_yield();
    }

test_complete:
    uint32_t elapsed_time = qc_osal_uptime_get_ms() - start_time;

    printf("\r\n=== Burst Test Results ===\r\n");
    printf("Total Packets: %u (Success: %u, Failed: %u)\r\n", packet_index, success_count, fail_count);
    printf("Elapsed Time: %u ms\r\n", elapsed_time);

    if (elapsed_time > 0) {
        /* Calculate statistics */
        uint32_t total_kb = (uint32_t)(total_bytes / 1024);
        uint32_t total_mb = (uint32_t)(total_bytes / (1024 * 1024));

        uint64_t throughput_bps = (total_bytes * 8 * 1000) / elapsed_time; /* bps */
        uint32_t throughput_kbps = (uint32_t)(throughput_bps / 1000);      /* Kbps */
        uint32_t throughput_mbps = (uint32_t)(throughput_bps / 1000000);   /* Mbps */
        uint32_t throughput_mbps_frac = (uint32_t)((throughput_bps % 1000000) / 10000);

        printf("Total Data: %u KB (%u MB)\r\n", total_kb, total_mb);

        printf("Throughput: %u Kbps (%u.%02u Mbps)\r\n", throughput_kbps, throughput_mbps, throughput_mbps_frac);

        uint32_t pps = (success_count * 1000) / elapsed_time;
        printf("Packet Rate: %u pps\r\n", pps);

        if (success_count > 0) {
            uint32_t avg_latency_us = (elapsed_time * 1000) / success_count;
            printf("Average Latency: %u us/packet\r\n", avg_latency_us);
        }
    }

    qc_osal_free(test_buffer);
    params->running = false;

    printf("[BURST_THREAD] Burst test completed\r\n");
}

/**
 * @brief Send multiple packets via ring service (in separate thread)
 * Usage:
 *   ring_burst count <ring_id> <count> <size>
 *   ring_burst time <ring_id> <seconds> <size>
 *   ring_burst data <ring_id> <MB> <size>
 */
int ring_send_burst(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage:\r\n");
        printf("  ring_burst count <ring_id> <count> <size>\r\n");
        printf("    Example: ring_burst count 0 1000 1500\r\n");
        printf("    Send specified number of packets\r\n\r\n");
        printf("  ring_burst time <ring_id> <seconds> <size>\r\n");
        printf("    Example: ring_burst time 0 60 1500\r\n");
        printf("    Send packets for specified duration\r\n\r\n");
        printf("  ring_burst data <ring_id> <MB> <size>\r\n");
        printf("    Example: ring_burst data 0 100 1500\r\n");
        printf("    Send until specified data amount is reached\r\n");
        printf("  ring_burst stop\r\n");
        printf("    Stop running burst test\r\n");
        return -QC_OSAL_EINVAL;
    }

    const char *subcmd = argv[1];

    /* Handle 'stop' subcommand */
    if (strcmp(subcmd, "stop") == 0) {
        if (!burst_params.running) {
            printf("No burst test is running\r\n");
            return 0;
        }

        printf("Stopping burst test...\r\n");
        burst_params.running = false;

        /* Wait for thread to finish (with timeout) */
        uint32_t timeout = qc_osal_uptime_get_ms() + 5000;
        while (burst_params.running && qc_osal_uptime_get_ms() < timeout) {
            qc_osal_msleep(100);
        }

        if (burst_params.running) {
            printf("WARNING: Burst test did not stop gracefully\r\n");
        } else {
            printf("Burst test stopped\r\n");
        }

        return 0;
    }

    /* Check if burst test is already running */
    if (burst_params.running) {
        printf("ERROR: Burst test already running\r\n");
        return -QC_OSAL_EBUSY;
    }

    /* Parse mode */
    const char *mode_str = argv[1];
    enum burst_test_mode mode;

    if (strcmp(mode_str, "count") == 0) {
        mode = BURST_MODE_COUNT;
    } else if (strcmp(mode_str, "time") == 0) {
        mode = BURST_MODE_TIME;
    } else if (strcmp(mode_str, "data") == 0) {
        mode = BURST_MODE_DATA;
    } else {
        printf("ERROR: Invalid mode '%s' (use: count, time, or data)\r\n", mode_str);
        return -QC_OSAL_EINVAL;
    }

    /* Parse common parameters */
    uint8_t ring_id = (uint8_t)strtoul(argv[2], NULL, 0);
    if (ring_id >= MAX_RINGS) {
        printf("ERROR: Invalid ring_id (0-%d)\r\n", MAX_RINGS - 1);
        return -QC_OSAL_EINVAL;
    }

    uint32_t param_value = strtoul(argv[3], NULL, 0);
    uint32_t size = strtoul(argv[4], NULL, 0);

    /* Validate size */
    if (size == 0 || size > 1500) {
        printf("ERROR: Invalid size (1-1500 bytes)\r\n");
        return -QC_OSAL_EINVAL;
    }

    /* Validate mode-specific parameters */
    switch (mode) {
    case BURST_MODE_COUNT:
        if (param_value == 0 || param_value > 1000000) {
            printf("ERROR: Invalid count (1-1000000)\r\n");
            return -QC_OSAL_EINVAL;
        }
        burst_params.count = param_value;
        break;

    case BURST_MODE_TIME:
        if (param_value == 0 || param_value > 360000) {
            printf("ERROR: Invalid duration (1-360000 seconds)\r\n");
            return -QC_OSAL_EINVAL;
        }
        burst_params.duration_sec = param_value;
        break;

    case BURST_MODE_DATA:
        if (param_value == 0 || param_value > 10240) {
            printf("ERROR: Invalid data size (1-10240 MB)\r\n");
            return -QC_OSAL_EINVAL;
        }
        burst_params.data_mb = param_value;
        break;
    }

    /* Initialize burst test parameters */
    burst_params.ring_id = ring_id;
    burst_params.mode = mode;
    burst_params.size = size;
    burst_params.running = true;

    /* Create burst test thread */
    struct qc_osal_thread_config config = {.name = "burst_test",
                                           .stack_size = 4096, /* Increased stack size */
                                           .priority = 5,
                                           .entry = burst_test_thread,
                                           .arg = &burst_params};

    int ret = qc_osal_thread_create(&burst_thread, &config);
    if (ret != 0) {
        printf("ERROR: Failed to create burst test thread: %d\r\n", ret);
        burst_params.running = false;
        return ret;
    }

    printf("Burst test started in background thread\r\n");

    return 0;
}

#endif

void vTaskShell(void *p)
{

    cmd_shell_init(&(UART_DEVICE));
    qcc730_reset();
    /* Register callback to handle data from QCC730 */
    ring_register_callback(ring_host_rx_callback, NULL);

    cmd_shell_add("qcspi_init", (void *)cmd_qcspi_init, "Init QCSPI transport");
    cmd_shell_add("qcspi_read", (void *)cmd_qcspi_read, "Read QCC730 memory");
    cmd_shell_add("qcspi_write", (void *)cmd_qcspi_write, "Write QCC730 memory");
    cmd_shell_add("qcspi_reg_read", (void *)cmd_qcspi_reg_read, "Read QCC730 register");
    cmd_shell_add("qcspi_reg_write", (void *)cmd_qcspi_reg_write, "Write QCC730 register");
    cmd_shell_add("qcspi_interrupt", (void *)cmd_qcspi_interrupt, "Trigger A2F interrupt");
    cmd_shell_add("qcspi_reset", (void *)cmd_qcspi_reset, "Reset QCSPI");
    cmd_shell_add("qcspi_test_transfer", (void *)cmd_qcspi_test_transfer, "Test qcspi_transfer performance");
#ifdef CONFIG_RING_SERVICE
    /* Register Ring service commands */
    cmd_shell_add("ring_status", (void *)ring_status, "Show ring status");
    cmd_shell_add("ring_send", (void *)ring_send_cmd, "Send string data");
    cmd_shell_add("ring_send_hex", (void *)ring_send_hex, "Send hex data");
    cmd_shell_add("ring_pattern", (void *)ring_send_pattern, "Send test pattern");
    cmd_shell_add("ring_burst", (void *)ring_send_burst, "Send burst packets");
#endif
    while (1) {
        cmd_shell_char_received();
    }
}

void task_init_all(void)
{
    int ret;
    qc_osal_thread_t xHandle = NULL;
    struct qc_osal_thread_config config = {
        .name = "Shell", .stack_size = 2048, .priority = 5, .entry = vTaskShell, .arg = NULL};

    ret = qc_osal_thread_create(&xHandle, &config);
    if (ret != 0) {
        printf("Task Shell creation error: %d\r\n", ret);
    }
}
#endif
#endif
