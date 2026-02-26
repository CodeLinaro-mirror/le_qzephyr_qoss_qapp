/*
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "../qc_port_config.h"

#if defined(QC_TRANSPORT_SPI)

#include "qc_transport.h"
#include "../hal/qc_hal.h"
#include "../osal/qc_osal.h"

/**
 * @brief SPI Transport Context
 *
 * This structure holds all state needed for SPI transport operations.
 */
struct qc_transport_spi_ctx {
    qc_hal_spi_t spi;           /* SPI handle from HAL */
    qc_hal_gpio_t cs_gpio;      /* CS GPIO handle from HAL */
    qc_osal_mutex_t lock;       /* Mutex for thread-safe access */
    struct qc_transport_stats stats; /* Transport statistics */
};

/* ============================================================================
 * Error Code Conversion
 * ============================================================================ */

static inline int hal_errno_to_transport(int hal_errno)
{
    if (hal_errno == 0) {
        return 0;
    }

    switch (-hal_errno) {
    case QC_HAL_EINVAL:
        return -QC_TRANSPORT_EINVAL;
    case QC_HAL_ENOMEM:
        return -QC_TRANSPORT_ENOMEM;
    case QC_HAL_ENODEV:
        return -QC_TRANSPORT_ENODEV;
    case QC_HAL_EIO:
        return -QC_TRANSPORT_EIO;
    case QC_HAL_EBUSY:
        return -QC_TRANSPORT_EBUSY;
    case QC_HAL_ETIMEDOUT:
        return -QC_TRANSPORT_ETIMEDOUT;
    default:
        return -QC_TRANSPORT_EIO;
    }
}

/* ============================================================================
 * Transport Operations
 * ============================================================================ */

int qc_transport_init(qc_transport_t *transport, const struct qc_transport_config *config)
{
    struct qc_transport_spi_ctx *ctx;
    struct qc_hal_spi_config spi_cfg;
    struct qc_hal_gpio_config gpio_cfg;
    int ret;

    if (!transport || !config || !config->hal_ctx) {
        return -QC_TRANSPORT_EINVAL;
    }

    /* Allocate transport context using OSAL */
    ctx = qc_osal_malloc(sizeof(struct qc_transport_spi_ctx));
    if (!ctx) {
        QC_OSAL_LOG_ERR("Failed to allocate transport context");
        return -QC_TRANSPORT_ENOMEM;
    }

    /* Initialize statistics */
    ctx->stats.tx_bytes = 0;
    ctx->stats.rx_bytes = 0;
    ctx->stats.tx_errors = 0;
    ctx->stats.rx_errors = 0;
    ctx->stats.tx_timeouts = 0;
    ctx->stats.rx_timeouts = 0;
    ctx->stats.transactions = 0;

    /* Initialize SPI via HAL */
    spi_cfg.frequency = 5000000; /* 5 MHz */
    spi_cfg.mode = QC_HAL_SPI_MODE_0;
    spi_cfg.bits_per_word = 8;

    ret = qc_hal_spi_init(config->hal_ctx, &ctx->spi, &spi_cfg);
    if (ret < 0) {
        QC_OSAL_LOG_ERR("Failed to initialize SPI: %d", ret);
        qc_osal_free(ctx);
        return hal_errno_to_transport(ret);
    }

    /* Initialize CS GPIO via HAL */
    gpio_cfg.pin = 4; /* PA4 */
    gpio_cfg.output = true;
    gpio_cfg.active_low = true;
    gpio_cfg.initial_value = false; /* CS inactive (high) initially */

    ret = qc_hal_gpio_init(config->hal_ctx, &ctx->cs_gpio, &gpio_cfg);
    if (ret < 0) {
        QC_OSAL_LOG_ERR("Failed to initialize CS GPIO: %d", ret);
        qc_hal_spi_deinit(ctx->spi);
        qc_osal_free(ctx);
        return hal_errno_to_transport(ret);
    }

    /* Initialize mutex via OSAL */
    ret = qc_osal_mutex_init(&ctx->lock);
    if (ret < 0) {
        QC_OSAL_LOG_ERR("Failed to initialize mutex: %d", ret);
        qc_hal_gpio_deinit(ctx->cs_gpio);
        qc_hal_spi_deinit(ctx->spi);
        qc_osal_free(ctx);
        return -QC_TRANSPORT_ENOMEM;
    }

    QC_OSAL_LOG_INF("SPI transport initialized");

    *transport = (qc_transport_t)ctx;
    return 0;
}

int qc_transport_deinit(qc_transport_t transport)
{
    struct qc_transport_spi_ctx *ctx = (struct qc_transport_spi_ctx *)transport;

    if (!ctx) {
        return -QC_TRANSPORT_EINVAL;
    }

    /* Ensure CS is deasserted */
    qc_hal_gpio_set(ctx->cs_gpio, false);

    /* Deinitialize resources */
    qc_hal_gpio_deinit(ctx->cs_gpio);
    qc_hal_spi_deinit(ctx->spi);
    qc_osal_free(ctx);

    QC_OSAL_LOG_INF("SPI transport deinitialized");

    return 0;
}

int qc_transport_transceive(qc_transport_t transport, const uint8_t *tx_buf, uint8_t *rx_buf, size_t len,
                             bool hold_cs)
{
    struct qc_transport_spi_ctx *ctx = (struct qc_transport_spi_ctx *)transport;
    int ret = 0;

    if (!ctx || !tx_buf || !rx_buf || len == 0) {
        return -QC_TRANSPORT_EINVAL;
    }

    /* Lock for thread-safe access */
    //QC_OSAL_LOG_ERR("lock qc_transport_transceive");
    //ret = qc_osal_mutex_lock(ctx->lock, QC_OSAL_TIMEOUT_FOREVER);
    if (ret < 0) {
        QC_OSAL_LOG_ERR("Failed to lock mutex");
        return -QC_TRANSPORT_EBUSY;
    }

    /* Assert CS (active) */
    ret = qc_hal_gpio_set(ctx->cs_gpio, true);
    if (ret < 0) {
        QC_OSAL_LOG_ERR("Failed to assert CS");
        //qc_osal_mutex_unlock(ctx->lock);
        ctx->stats.tx_errors++;
        return hal_errno_to_transport(ret);
    }

    /* Perform SPI transfer */
    ret = qc_hal_spi_transfer(ctx->spi, tx_buf, rx_buf, len);
    if (ret < 0) {
        QC_OSAL_LOG_ERR("SPI transfer failed: %d", ret);
        /* Ensure CS is deasserted on error */
        qc_hal_gpio_set(ctx->cs_gpio, false);
        //qc_osal_mutex_unlock(ctx->lock);
        ctx->stats.tx_errors++;
        return hal_errno_to_transport(ret);
    }

    /* Deassert CS if not holding */
    if (!hold_cs) {
        ret = qc_hal_gpio_set(ctx->cs_gpio, false);
        if (ret < 0) {
            QC_OSAL_LOG_ERR("Failed to deassert CS");
            //qc_osal_mutex_unlock(ctx->lock);
            return hal_errno_to_transport(ret);
        }
    }

    /* Update statistics */
    ctx->stats.tx_bytes += len;
    ctx->stats.rx_bytes += len;
    ctx->stats.transactions++;
    //QC_OSAL_LOG_ERR("unlock qc_transport_transceive");
    //qc_osal_mutex_unlock(ctx->lock);

    return 0;
}

int qc_transport_send(qc_transport_t transport, const uint8_t *data, size_t len)
{
    uint8_t *rx_buf;
    int ret;

    if (!transport || !data || len == 0) {
        return -QC_TRANSPORT_EINVAL;
    }

    /* Allocate temporary receive buffer */
    rx_buf = qc_osal_malloc(len);
    if (!rx_buf) {
        return -QC_TRANSPORT_ENOMEM;
    }

    /* Perform transceive (discard received data) */
    ret = qc_transport_transceive(transport, data, rx_buf, len, false);

    qc_osal_free(rx_buf);
    return ret;
}

int qc_transport_recv(qc_transport_t transport, uint8_t *data, size_t len)
{
    uint8_t *tx_buf;
    int ret;

    if (!transport || !data || len == 0) {
        return -QC_TRANSPORT_EINVAL;
    }

    /* Allocate temporary transmit buffer (filled with dummy data) */
    tx_buf = qc_osal_malloc(len);
    if (!tx_buf) {
        return -QC_TRANSPORT_ENOMEM;
    }

    /* Fill with dummy data (0xFF is common for SPI) */
    for (size_t i = 0; i < len; i++) {
        tx_buf[i] = 0xFF;
    }

    /* Perform transceive */
    ret = qc_transport_transceive(transport, tx_buf, data, len, false);

    qc_osal_free(tx_buf);
    return ret;
}

/* ============================================================================
 * Transport Statistics
 * ============================================================================ */

int qc_transport_get_stats(qc_transport_t transport, struct qc_transport_stats *stats)
{
    struct qc_transport_spi_ctx *ctx = (struct qc_transport_spi_ctx *)transport;

    if (!ctx || !stats) {
        return -QC_TRANSPORT_EINVAL;
    }

    /* Copy statistics */
    *stats = ctx->stats;

    return 0;
}

int qc_transport_reset_stats(qc_transport_t transport)
{
    struct qc_transport_spi_ctx *ctx = (struct qc_transport_spi_ctx *)transport;

    if (!ctx) {
        return -QC_TRANSPORT_EINVAL;
    }

    /* Reset all statistics */
    ctx->stats.tx_bytes = 0;
    ctx->stats.rx_bytes = 0;
    ctx->stats.tx_errors = 0;
    ctx->stats.rx_errors = 0;
    ctx->stats.tx_timeouts = 0;
    ctx->stats.rx_timeouts = 0;
    ctx->stats.transactions = 0;

    return 0;
}

#endif /* QC_TRANSPORT_SPI */
