/*
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <stdbool.h>
#include <string.h>
#include "qc_osal.h"
#include "qc_platform.h"
#include "qcspi_driver.h"
#include "qcspi_protocol.h"

/* Global platform context */
static qc_platform_ctx_t g_platform_ctx;

/* Buffers for SPI transactions */
static uint8_t qcspi_tx_buffer[QCSPI_MAX_TRANSFER_SIZE];
static uint8_t qcspi_rx_buffer[QCSPI_MAX_TRANSFER_SIZE];

/* Mutex for thread-safe access */
static qc_osal_mutex_t qcspi_mutex;

/**
 * @brief Perform SPI transfer with CS held low (for scatter-gather)
 */
static int qcspi_transfer_sg(uint8_t *tx_buf, uint8_t *rx_buf, size_t len)
{
    return qc_platform_spi_transceive(g_platform_ctx, tx_buf, rx_buf, len, true);
}

/**
 * @brief Perform SPI transfer with CS released after transfer
 */
static int qcspi_transfer(uint8_t *tx_buf, uint8_t *rx_buf, size_t len)
{
    return qc_platform_spi_transceive(g_platform_ctx, tx_buf, rx_buf, len, false);
}

void qcspi_word_to_byte_big_endian(uint8_t *byte_value_buf, uint32_t word_value)
{
    byte_value_buf[0] = (word_value >> 24) & 0xFF;
    byte_value_buf[1] = (word_value >> 16) & 0xFF;
    byte_value_buf[2] = (word_value >> 8) & 0xFF;
    byte_value_buf[3] = (word_value >> 0) & 0xFF;
}

int qcspi_read_addr_align(uint32_t handle, uint16_t bytes_to_read, uint32_t addr, uint8_t *read_data_buf)
{
    int ret;
    uint32_t addr_align;
    uint32_t offset;
    uint32_t size;
    uint32_t total_size = 0;
    uint8_t data_buf[4];

    if (!read_data_buf) {
        return -QC_OSAL_EINVAL;
    }

    /* Check if already 4-byte aligned */
    if (((addr & 0x3) == 0) && ((bytes_to_read % 4) == 0)) {
        QC_OSAL_LOG_DBG("addr=0x%08X, bytes_to_read=%d are 4 bytes aligned", addr, bytes_to_read);
        return qcspi_read(handle, bytes_to_read, addr, read_data_buf);
    }

    QC_OSAL_LOG_DBG("addr=0x%08X, bytes_to_read=%d (unaligned)", addr, bytes_to_read);

    /* Read 1st DWORD */
    addr_align = addr & 0xFFFFFFFC;
    ret = qcspi_read(handle, 4, addr_align, data_buf);
    if (ret < 0) {
        return ret;
    }

    offset = addr & 0x3;
    size = ((4 - offset) > bytes_to_read) ? bytes_to_read : (4 - offset);
    memcpy(read_data_buf, &data_buf[offset], size);
    total_size += size;

    if (total_size >= bytes_to_read) {
        return 0;
    }
    addr_align += 4;

    /* Read middle DWORDs */
    if (bytes_to_read >= (total_size + 4)) {
        size = (bytes_to_read - total_size) & 0xFFFFFFFC;
        ret = qcspi_read(handle, size, addr_align, &read_data_buf[total_size]);
        if (ret < 0) {
            return ret;
        }
        total_size += size;

        if (total_size >= bytes_to_read) {
            return 0;
        }
        addr_align += size;
    }

    /* Read last DWORD */
    size = bytes_to_read - total_size;
    ret = qcspi_read(handle, 4, addr_align, data_buf);
    if (ret < 0) {
        return ret;
    }
    memcpy(&read_data_buf[total_size], data_buf, size);

    return 0;
}

int qcspi_write_addr_align(uint32_t handle, uint16_t bytes_to_write, uint32_t addr, uint8_t *write_data_buf)
{
    int ret;
    uint32_t addr_align;
    uint32_t offset;
    uint32_t size;
    uint32_t total_size = 0;
    uint8_t data_buf[4] = {0};
    uint8_t nop_wbuf = 0;
    uint8_t nop_rbuf = 0;

    if (!write_data_buf) {
        return -QC_OSAL_EINVAL;
    }

    /* Check if already 4-byte aligned */
    if (((addr & 0x3) == 0) && ((bytes_to_write % 4) == 0)) {
        QC_OSAL_LOG_DBG("addr=0x%08X, bytes_to_write=%d are 4 bytes aligned", addr, bytes_to_write);
        return qcspi_write(handle, bytes_to_write, addr, write_data_buf);
    }

    QC_OSAL_LOG_DBG("addr=0x%08X, bytes_to_write=%d (unaligned)", addr, bytes_to_write);

    /* Write 1st DWORD */
    addr_align = addr & 0xFFFFFFFC;
    ret = qcspi_read(handle, 4, addr_align, data_buf);
    if (ret < 0) {
        return ret;
    }

    offset = addr & 0x3;
    size = ((4 - offset) > bytes_to_write) ? bytes_to_write : (4 - offset);
    memcpy(&data_buf[offset], write_data_buf, size);

    /* NOP to avoid timing issue */
    ret = qcspi_transfer(&nop_wbuf, &nop_rbuf, 1);
    if (ret < 0) {
        QC_OSAL_LOG_WRN("NOP failed: %d", ret);
    }

    ret = qcspi_write(handle, 4, addr_align, data_buf);
    if (ret < 0) {
        return ret;
    }
    total_size += size;

    if (total_size >= bytes_to_write) {
        return 0;
    }
    addr_align += 4;

    /* Write middle DWORDs */
    if (bytes_to_write >= (total_size + 4)) {
        size = (bytes_to_write - total_size) & 0xFFFFFFFC;
        ret = qcspi_write(handle, size, addr_align, &write_data_buf[total_size]);
        if (ret < 0) {
            return ret;
        }
        total_size += size;

        if (total_size >= bytes_to_write) {
            return 0;
        }
        addr_align += size;
    }

    /* Write last DWORD */
    size = bytes_to_write - total_size;
    ret = qcspi_read(handle, 4, addr_align, data_buf);
    if (ret < 0) {
        return ret;
    }
    memcpy(data_buf, &write_data_buf[total_size], size);
    ret = qcspi_write(handle, 4, addr_align, data_buf);

    return ret;
}

int qcspi_interrupt(uint32_t handle)
{
    uint32_t config_value = 0;
    int ret;

    QC_OSAL_LOG_DBG("Triggering Slave interrupt");

    /* Read current config */
    ret = qcspi_IRR(handle, SPI_SLAVE_CONFIG, &config_value);
    if (ret < 0) {
        QC_OSAL_LOG_ERR("Failed to read SPI_SLAVE_CONFIG: %d", ret);
        return ret;
    }

    /* Check if interrupt already set */
    if (config_value & QCSPI_CONFIG_HOST_IRQ_INT0_EN(1)) {
        qc_osal_msleep(10);
        ret = qcspi_IRR(handle, SPI_SLAVE_CONFIG, &config_value);
        if (ret == 0 && (config_value & QCSPI_CONFIG_HOST_IRQ_INT0_EN(1))) {
            QC_OSAL_LOG_DBG("Interrupt already set, config=0x%x", config_value);
            return 0;
        }
    }

    /* Set interrupt bit */
    config_value |= QCSPI_CONFIG_HOST_IRQ_INT0_EN(1);
    ret = qcspi_IRW(handle, SPI_SLAVE_CONFIG, config_value);
    if (ret < 0) {
        QC_OSAL_LOG_ERR("Failed to write SPI_SLAVE_CONFIG: %d", ret);
        return ret;
    }

    return 0;
}

int qcspi_read(uint32_t handle, uint16_t size, uint32_t address, uint8_t *rcv_buf)
{
    uint8_t header_tx[15] = {0};
    uint8_t header_rx[15] = {0};
    uint32_t retry = 0;
    uint32_t status = 0;
    int ret;

    if (!rcv_buf) {
        return -QC_OSAL_EINVAL;
    }

    if (size > QCSPI_MAX_TRANSFER_SIZE) {
        QC_OSAL_LOG_ERR("read size too long, size=%d", size);
        return -QC_OSAL_EINVAL;
    }

    /* Update TRNS_LEN register */
    ret = qcspi_IRW(0, SPI_SLAVE_TRNS_LEN, size << 16);
    if (ret < 0) {
        QC_OSAL_LOG_ERR("Failed to update TRNS_LEN: %d", ret);
    }

    qc_osal_mutex_lock(qcspi_mutex, QC_OSAL_TIMEOUT_FOREVER);

    /* Build FREAD command */
    header_tx[0] = SPI_CMDCODE_FREAD;
    qcspi_word_to_byte_big_endian(&header_tx[1], address);

    /* Add dummy bytes */
    for (uint32_t i = 0; i < QCSPI_READ_DUMMY_BYTES; i++) {
        header_tx[5 + i] = 0xA5;
    }

    /* Send header first (keep CS low) */
    ret = qcspi_transfer_sg(header_tx, header_rx, 15);
    if (ret < 0) {
        qc_osal_mutex_unlock(qcspi_mutex);
        QC_OSAL_LOG_ERR("SPI header transfer failed: %d", ret);
        return ret;
    }

    /* Receive data directly to user buffer (release CS after) */
    ret = qcspi_transfer(qcspi_tx_buffer, rcv_buf, size);

    qc_osal_mutex_unlock(qcspi_mutex);

    if (ret < 0) {
        QC_OSAL_LOG_ERR("SPI data transfer failed: %d", ret);
        return ret;
    }

    /* Wait until READ bulk is completed */
    while (retry++ < QCSPI_MAX_INIT_TRY_TIMES) {
        ret = qcspi_RDSR(0, &status);
        if (ret == 0 && !BIT_CHECK(status, QCSPI_STATUS_BUSY_BIT)) {
            break;
        }

        if (retry == QCSPI_MAX_INIT_TRY_TIMES - 1) {
            QC_OSAL_LOG_ERR("Read retry timeout, size=%d status=0x%x", size, status);
            return -QC_OSAL_ETIMEDOUT;
        }
        qc_osal_usleep(10);
    }

    return 0;
}
int qcspi_write(uint32_t handle, uint16_t size, uint32_t address, uint8_t *snd_buf)
{
    static uint8_t header[5];
    uint32_t retry = 0;
    uint32_t status = 0;
    int ret;
    if (!snd_buf) {
        return -QC_OSAL_EINVAL;
    }

    if (size > QCSPI_MAX_TRANSFER_SIZE) {
        QC_OSAL_LOG_ERR("write size too long, size=%d", size);
        return -QC_OSAL_EINVAL;
    }

    qc_osal_mutex_lock(qcspi_mutex, QC_OSAL_TIMEOUT_FOREVER);

    /* Build WRITE command */
    header[0] = SPI_CMDCODE_WRITE;
    qcspi_word_to_byte_big_endian(&header[1], address);

    /* Send header first (keep CS low) */
    ret = qcspi_transfer_sg(header, qcspi_rx_buffer, 5);
    if (ret < 0) {
        qc_osal_mutex_unlock(qcspi_mutex);
        return ret;
    }
    /* Send data (release CS after) */
    ret = qcspi_transfer(snd_buf, qcspi_rx_buffer, size);

    qc_osal_mutex_unlock(qcspi_mutex);

    if (ret < 0) {
        return ret;
    }

    /* Wait until write is completed */
    while (retry++ < QCSPI_MAX_INIT_TRY_TIMES) {
        ret = qcspi_RDSR(0, &status);
        if (ret == 0) {
            if (BIT_CHECK(status, QCSPI_STATUS_TXUERR_BIT)) {
                // QC_OSAL_LOG_WRN("TXUERR error: 0x%x", status);
            }
            if (!BIT_CHECK(status, QCSPI_STATUS_BUSY_BIT)) {
                break;
            }
        }

        if (retry == QCSPI_MAX_INIT_TRY_TIMES - 1) {
            QC_OSAL_LOG_ERR("Write retry timeout, size=%d status=0x%x", size, status);
            return -QC_OSAL_ETIMEDOUT;
        }
        qc_osal_usleep(10);
    }

    return 0;
}

void qcspi_reset(void)
{
    uint8_t write_buf[4] = {0};
    uint8_t read_buf[4] = {0};
    uint32_t status = 0;

    /* Send reset command */
    write_buf[0] = SPI_CMDCODE_RESET_REQUEST_BYTE0;
    write_buf[1] = SPI_CMDCODE_RESET_REQUEST_BYTE1;
    write_buf[2] = SPI_CMDCODE_REQUEST_BYTE2;
    write_buf[3] = 0x00;

    if (qcspi_transfer(write_buf, read_buf, 4) < 0) {
        QC_OSAL_LOG_ERR("Reset command failed");
    }
    qc_osal_msleep(1);

    qcspi_RDSR(0, &status);
    QC_OSAL_LOG_INF("Status after reset: 0x%x", status);
    qc_osal_msleep(1);
}

int qcspi_init(void)
{
    uint8_t write_buf[5] = {0};
    uint8_t read_buf[5] = {0};
    int ret;

    /* Initialize platform layer */
    ret = qc_platform_init(&g_platform_ctx);
    if (ret < 0) {
        QC_OSAL_LOG_ERR("Failed to initialize platform: %d", ret);
        return ret;
    }

    /* Initialize mutex */
    qc_osal_mutex_init(&qcspi_mutex);

    /* Send NOP */
    write_buf[0] = SPI_CMDCODE_NOP;
    ret = qcspi_transfer(write_buf, read_buf, 1);
    if (ret < 0) {
        QC_OSAL_LOG_ERR("NOP failed: %d", ret);
    }

    /* Send WREN */
    write_buf[0] = SPI_CMDCODE_WREN;
    ret = qcspi_transfer(write_buf, read_buf, 1);
    if (ret < 0) {
        QC_OSAL_LOG_ERR("WREN failed: %d", ret);
    }

    /* Read status */
    write_buf[0] = SPI_CMDCODE_RDSR;
    write_buf[1] = 0;
    write_buf[2] = 0;
    write_buf[3] = 0;
    write_buf[4] = 0;

    ret = qcspi_transfer(write_buf, read_buf, 3);
    if (ret == 0) {
        QC_OSAL_LOG_INF("RDSR: %02x %02x %02x", read_buf[1], read_buf[2], read_buf[3]);
    } else {
        QC_OSAL_LOG_ERR("RDSR failed: %d", ret);
    }

    QC_OSAL_LOG_INF("QCSPI driver initialized");

    return 0;
}

void qcspi_get_slaveid(uint8_t *id_array)
{
    uint8_t write_buf[7] = {0};
    uint8_t read_buf[7] = {0};

    if (!id_array) {
        return;
    }

    /* Send NOP */
    write_buf[0] = SPI_CMDCODE_NOP;
    qcspi_transfer(write_buf, read_buf, 1);
    qc_osal_msleep(1);

    /* Send WREN */
    write_buf[0] = SPI_CMDCODE_WREN;
    qcspi_transfer(write_buf, read_buf, 1);
    qc_osal_msleep(1);

    /* Read ID */
    write_buf[0] = SPI_CMDCODE_RDID;
    write_buf[1] = 0;
    write_buf[2] = 0;
    write_buf[3] = 0;
    write_buf[4] = 0;
    write_buf[5] = 0;
    write_buf[6] = 0;

    if (qcspi_transfer(write_buf, read_buf, 7) == 0) {
        id_array[0] = read_buf[0];
        id_array[1] = read_buf[1];
        id_array[2] = read_buf[2];
        QC_OSAL_LOG_INF("Slave ID: %02x %02x %02x", id_array[0], id_array[1], id_array[2]);
    }
}

int qcspi_RDSR(uint32_t handle, uint32_t *status)
{
    uint8_t write_buf[5] = {0};
    uint8_t read_buf[5] = {0};
    int ret;

    if (!status) {
        return -QC_OSAL_EINVAL;
    }

    qc_osal_mutex_lock(qcspi_mutex, QC_OSAL_TIMEOUT_FOREVER);

    write_buf[0] = SPI_SLAVE_RDSR;
    write_buf[1] = 0;
    write_buf[2] = 0;
    write_buf[3] = 0;
    write_buf[4] = 0;

    ret = qcspi_transfer(write_buf, read_buf, sizeof(read_buf));

    qc_osal_mutex_unlock(qcspi_mutex);

    if (ret < 0) {
        return ret;
    }

    *status = (read_buf[1] << 24) | (read_buf[2] << 16) | (read_buf[3] << 8) | (read_buf[4] << 0);

    return 0;
}

int qcspi_IRR(uint32_t handle, uint8_t reg_addr, uint32_t *reg_val)
{
    uint8_t op_buf[8] = {0};
    uint8_t read_buf[8] = {0};
    int ret;

    if (!reg_val) {
        return -QC_OSAL_EINVAL;
    }

    qc_osal_mutex_lock(qcspi_mutex, QC_OSAL_TIMEOUT_FOREVER);

    op_buf[0] = SPI_CMDCODE_IRR;
    op_buf[1] = reg_addr;

    ret = qcspi_transfer(op_buf, read_buf, IRR_CMD_LENGTH);

    qc_osal_mutex_unlock(qcspi_mutex);

    if (ret < 0) {
        QC_OSAL_LOG_ERR("IRR failed: %d", ret);
        return ret;
    }

    *reg_val = (read_buf[4] << 24) | (read_buf[5] << 16) | (read_buf[6] << 8) | (read_buf[7] << 0);

    QC_OSAL_LOG_DBG("IRR reg=0x%x val=0x%x", reg_addr, *reg_val);

    return 0;
}

int qcspi_IRW(uint32_t handle, uint8_t reg_addr, uint32_t reg_val)
{
    uint8_t op_buf[6] = {0};
    uint8_t read_buf[8] = {0};
    int ret;

    qc_osal_mutex_lock(qcspi_mutex, QC_OSAL_TIMEOUT_FOREVER);

    op_buf[0] = SPI_CMDCODE_IRW;
    op_buf[1] = reg_addr;
    qcspi_word_to_byte_big_endian(&op_buf[2], reg_val);

    ret = qcspi_transfer(op_buf, read_buf, IRW_CMD_LENGTH);

    qc_osal_mutex_unlock(qcspi_mutex);

    if (ret < 0) {
        QC_OSAL_LOG_ERR("IRW failed: reg=0x%x val=0x%x ret=%d", reg_addr, reg_val, ret);
        return ret;
    }

    QC_OSAL_LOG_DBG("IRW reg=0x%x val=0x%x", reg_addr, reg_val);

    return 0;
}
