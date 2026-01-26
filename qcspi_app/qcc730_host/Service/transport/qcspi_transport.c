/*
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "../../Port/qc_port_config.h"

#if defined(QC_TRANSPORT_SPI)
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <errno.h>
#include "qc_osal.h"
#include "qcspi_driver.h"
#include "qcspi_protocol.h"
#include "ring_transport.h"

/* Transport instance data */
static struct {
    bool initialized;
} g_transport_inst = {0};

/**
 * @brief Read from remote device memory via QCSPI
 */
static int qcspi_transport_read(ring_transport_dev_t dev, uint32_t addr, uint8_t *data, size_t len)
{
    int ret;

    (void)dev; /* Unused in current implementation */

    if (!data || len == 0) {
        return -EINVAL;
    }

    if (!g_transport_inst.initialized) {
        QC_OSAL_LOG_ERR("Transport not initialized");
        return -ENODEV;
    }

    QC_OSAL_LOG_DBG("Read: addr=0x%08X len=%zu", addr, len);

    ret = qcspi_read_addr_align(0, (uint16_t)len, addr, data);
    if (ret < 0) {
        QC_OSAL_LOG_ERR("QCSPI read failed: %d", ret);
        return ret;
    }

    return 0;
}

/**
 * @brief Write to remote device memory via QCSPI
 */
static int qcspi_transport_write(ring_transport_dev_t dev, uint32_t addr, const uint8_t *data, size_t len)
{
    int ret;

    (void)dev; /* Unused in current implementation */

    if (!data || len == 0) {
        return -EINVAL;
    }

    if (!g_transport_inst.initialized) {
        QC_OSAL_LOG_ERR("Transport not initialized");
        return -ENODEV;
    }

    QC_OSAL_LOG_DBG("Write: addr=0x%08X len=%zu", addr, len);

    ret = qcspi_write_addr_align(0, (uint16_t)len, addr, (uint8_t *)data);
    if (ret < 0) {
        QC_OSAL_LOG_ERR("QCSPI write failed: %d", ret);
        return ret;
    }

    return 0;
}

/**
 * @brief Trigger interrupt on remote device
 */
static int qcspi_transport_interrupt(ring_transport_dev_t dev)
{
    int ret;

    (void)dev; /* Unused in current implementation */

    if (!g_transport_inst.initialized) {
        QC_OSAL_LOG_ERR("Transport not initialized");
        return -ENODEV;
    }

    QC_OSAL_LOG_DBG("Triggering interrupt");

    ret = qcspi_interrupt(0);
    if (ret < 0) {
        QC_OSAL_LOG_ERR("QCSPI interrupt failed: %d", ret);
        return ret;
    }

    return 0;
}

/**
 * @brief Read remote device register
 */
static int qcspi_transport_reg_read(ring_transport_dev_t dev, uint8_t reg_addr, uint32_t *value)
{
    int ret;

    (void)dev; /* Unused in current implementation */

    if (!value) {
        return -EINVAL;
    }

    if (!g_transport_inst.initialized) {
        QC_OSAL_LOG_ERR("Transport not initialized");
        return -ENODEV;
    }

    ret = qcspi_IRR(0, reg_addr, value);
    if (ret < 0) {
        QC_OSAL_LOG_ERR("QCSPI IRR failed: %d", ret);
        return ret;
    }

    return 0;
}

/**
 * @brief Write remote device register
 */
static int qcspi_transport_reg_write(ring_transport_dev_t dev, uint8_t reg_addr, uint32_t value)
{
    int ret;

    (void)dev; /* Unused in current implementation */

    if (!g_transport_inst.initialized) {
        QC_OSAL_LOG_ERR("Transport not initialized");
        return -ENODEV;
    }

    ret = qcspi_IRW(0, reg_addr, value);
    if (ret < 0) {
        QC_OSAL_LOG_ERR("QCSPI IRW failed: %d", ret);
        return ret;
    }

    return 0;
}

/**
 * @brief Reset remote device via QCSPI
 */
static int qcspi_transport_reset(ring_transport_dev_t dev)
{
    (void)dev; /* Unused in current implementation */

    if (!g_transport_inst.initialized) {
        QC_OSAL_LOG_ERR("Transport not initialized");
        return -ENODEV;
    }

    QC_OSAL_LOG_INF("Resetting remote device via QCSPI");

    /* Call qcspi_reset from qcspi_driver */
    qcspi_reset();

    QC_OSAL_LOG_INF("Remote device reset completed");

    return 0;
}

/**
 * @brief Initialize QCSPI transport
 */
int qcspi_transport_init(ring_transport_dev_t dev)
{

    int ret;

    QC_OSAL_LOG_INF("Initializing QCSPI transport");

    if (g_transport_inst.initialized) {
        QC_OSAL_LOG_WRN("Transport already initialized");
        return 0;
    }

    /* Initialize QCSPI driver (platform layer handles hardware initialization) */
    ret = qcspi_init();
    if (ret < 0) {
        QC_OSAL_LOG_ERR("QCSPI driver init failed: %d", ret);
        return ret;
    }

    /* Get and verify slave ID */
    uint8_t slave_id[3] = {0};
    qcspi_get_slaveid(slave_id);

    if (slave_id[1] == QCSPI_ID1 && slave_id[2] == QCSPI_ID2) {
        QC_OSAL_LOG_INF("QCC730 slave detected: ID=%02X %02X %02X", slave_id[0], slave_id[1], slave_id[2]);
    } else {
        QC_OSAL_LOG_WRN("Unexpected slave ID: %02X %02X %02X", slave_id[0], slave_id[1], slave_id[2]);
        return -ENODEV;
    }

    g_transport_inst.initialized = true;
    dev = (ring_transport_dev_t)&g_transport_inst;

    QC_OSAL_LOG_INF("QCSPI transport initialized successfully");

    return 0;
}

/**
 * @brief Deinitialize QCSPI transport
 */
static int qcspi_transport_deinit(ring_transport_dev_t dev)
{
    (void)dev; /* Unused in current implementation */

    if (!g_transport_inst.initialized) {
        return 0;
    }

    QC_OSAL_LOG_INF("Deinitializing QCSPI transport");
    g_transport_inst.initialized = false;

    return 0;
}

/* Transport API implementation */
static const struct ring_transport_api qcspi_transport_api = {
    .init = qcspi_transport_init,
    .deinit = qcspi_transport_deinit,
    .read = qcspi_transport_read,
    .write = qcspi_transport_write,
    .interrupt = qcspi_transport_interrupt,
    .reg_read = qcspi_transport_reg_read,
    .reg_write = qcspi_transport_reg_write,
    .reset = qcspi_transport_reset,
};

/**
 * @brief Get transport API
 */
const struct ring_transport_api *ring_transport_get_api(ring_transport_dev_t dev)
{
    (void)dev; /* Unused in current implementation */
    return &qcspi_transport_api;
}

/**
 * @brief Check if transport is initialized
 */
bool qcspi_transport_is_initialized(void) { return g_transport_inst.initialized; }

/**
 * @brief Get transport device handle (for ring layer)
 */
ring_transport_dev_t qcspi_transport_get_device(void)
{
    if (!g_transport_inst.initialized) {
        return NULL;
    }
    /* Return a non-NULL handle to indicate transport is available */
    return (ring_transport_dev_t)&g_transport_inst;
}
#endif