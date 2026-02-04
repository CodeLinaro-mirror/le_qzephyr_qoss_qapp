/*
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "../../qc_port_config.h"

#if defined(QC_PLATFORM_STM32) && defined(QC_OS_ZEPHYR)

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include "../qc_platform.h"

#if defined(QC_TRANSPORT_SPI)
#include <zephyr/drivers/spi.h>
#endif

LOG_MODULE_REGISTER(qc_platform_stm32_zephyr, LOG_LEVEL_INF);

/**
 * @brief STM32 platform context structure for Zephyr
 *
 * This structure holds all STM32-specific hardware handles and configuration
 * needed for platform operations under Zephyr RTOS.
 */
struct qc_platform_stm32_ctx {
#if defined(QC_TRANSPORT_SPI)
    const struct device *spi_dev; /* SPI device from device tree */
    struct spi_config spi_cfg;    /* SPI configuration */
    struct gpio_dt_spec cs_gpio;  /* CS GPIO specification */
    bool cs_initialized;          /* CS GPIO initialization flag */
#endif
    /* Future: Add SDIO, UART, USB specific fields here */
};

/**
 * @brief Convert Zephyr errno to platform error code
 */
static inline int zephyr_errno_to_platform(int zephyr_errno)
{
    if (zephyr_errno == 0) {
        return 0;
    }

    switch (zephyr_errno) {
    case EINVAL:
        return -QC_PLATFORM_EINVAL;
    case ENODEV:
        return -QC_PLATFORM_ENODEV;
    case EIO:
        return -QC_PLATFORM_EIO;
    case EBUSY:
        return -QC_PLATFORM_EBUSY;
    default:
        return -QC_PLATFORM_EIO;
    }
}

/* ============================================================================
 * Platform Initialization
 * ============================================================================ */

int qc_platform_init(qc_platform_ctx_t *ctx)
{
    struct qc_platform_stm32_ctx *stm32_ctx;
    int ret;

    if (!ctx) {
        return -QC_PLATFORM_EINVAL;
    }

    /* Allocate platform context */
    stm32_ctx = k_malloc(sizeof(struct qc_platform_stm32_ctx));
    if (!stm32_ctx) {
        LOG_ERR("Failed to allocate platform context");
        return -QC_PLATFORM_EIO;
    }

#if defined(QC_TRANSPORT_SPI)
    /* Get SPI device from device tree */
    stm32_ctx->spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi1));
    if (!device_is_ready(stm32_ctx->spi_dev)) {
        LOG_ERR("SPI device not ready");
        k_free(stm32_ctx);
        return -QC_PLATFORM_ENODEV;
    }

    /* Configure SPI parameters */
    stm32_ctx->spi_cfg.frequency = 5000000; /* 5 MHz */
    stm32_ctx->spi_cfg.operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB;
    stm32_ctx->spi_cfg.slave = 0;
    /* Manual CS control - set cs.gpio.port to NULL to disable automatic CS */
    stm32_ctx->spi_cfg.cs.gpio.port = NULL;
    stm32_ctx->spi_cfg.cs.delay = 0;

    /* Initialize CS GPIO - GPIOA pin 4 */
    stm32_ctx->cs_gpio.port = DEVICE_DT_GET(DT_NODELABEL(gpioa));
    stm32_ctx->cs_gpio.pin = 4;
    stm32_ctx->cs_gpio.dt_flags = GPIO_ACTIVE_LOW;

    if (!device_is_ready(stm32_ctx->cs_gpio.port)) {
        LOG_ERR("CS GPIO device not ready");
        k_free(stm32_ctx);
        return -QC_PLATFORM_ENODEV;
    }

    /* Configure CS as output, initially high (inactive) */
    ret = gpio_pin_configure_dt(&stm32_ctx->cs_gpio, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        LOG_ERR("Failed to configure CS GPIO: %d", ret);
        k_free(stm32_ctx);
        return zephyr_errno_to_platform(-ret);
    }

    stm32_ctx->cs_initialized = true;
#endif /* QC_TRANSPORT_SPI */

    LOG_INF("%s platform initialized on %s with %s transport", QC_PLATFORM_NAME, QC_OS_NAME, QC_TRANSPORT_NAME);

    *ctx = (qc_platform_ctx_t)stm32_ctx;
    return 0;
}

int qc_platform_deinit(qc_platform_ctx_t ctx)
{
    struct qc_platform_stm32_ctx *stm32_ctx = (struct qc_platform_stm32_ctx *)ctx;

    if (!stm32_ctx) {
        return -QC_PLATFORM_EINVAL;
    }

#if defined(QC_TRANSPORT_SPI)
    /* Ensure CS is deasserted before cleanup */
    if (stm32_ctx->cs_initialized) {
        gpio_pin_set_dt(&stm32_ctx->cs_gpio, 0); /* CS inactive high */
    }
#endif /* QC_TRANSPORT_SPI */

    k_free(stm32_ctx);
    LOG_INF("%s platform deinitialized", QC_PLATFORM_NAME);

    return 0;
}

/* ============================================================================
 * Transport-Specific Operations
 * ============================================================================ */

#if defined(QC_TRANSPORT_SPI)
/**
 * @brief SPI transceive operation for STM32 on Zephyr
 *
 * This function is only compiled when SPI transport is selected.
 * Future transports (SDIO, UART, USB) will have their own sections.
 */
int qc_platform_spi_transceive(qc_platform_ctx_t ctx, uint8_t *tx_buf, uint8_t *rx_buf, size_t len, bool hold_cs)
{
    struct qc_platform_stm32_ctx *stm32_ctx = (struct qc_platform_stm32_ctx *)ctx;
    int ret;

    if (!stm32_ctx || !tx_buf || !rx_buf || len == 0) {
        return -QC_PLATFORM_EINVAL;
    }

    if (!stm32_ctx->cs_initialized) {
        LOG_ERR("CS GPIO not initialized");
        return -QC_PLATFORM_ENODEV;
    }

    /* Prepare SPI buffer sets */
    struct spi_buf tx_spi_buf = {.buf = tx_buf, .len = len};
    struct spi_buf rx_spi_buf = {.buf = rx_buf, .len = len};
    struct spi_buf_set tx_buf_set = {.buffers = &tx_spi_buf, .count = 1};
    struct spi_buf_set rx_buf_set = {.buffers = &rx_spi_buf, .count = 1};

    /* Assert CS (active low) before transaction */
    gpio_pin_set_dt(&stm32_ctx->cs_gpio, 1);

    /* Perform SPI transceive */
    ret = spi_transceive(stm32_ctx->spi_dev, &stm32_ctx->spi_cfg, &tx_buf_set, &rx_buf_set);

    /* Deassert CS (inactive high) after transaction if not holding */
    if (!hold_cs) {
        gpio_pin_set_dt(&stm32_ctx->cs_gpio, 0);
    }

    if (ret < 0) {
        LOG_ERR("SPI transceive failed: %d", ret);
        /* Ensure CS is deasserted on error */
        gpio_pin_set_dt(&stm32_ctx->cs_gpio, 0);
        return zephyr_errno_to_platform(-ret);
    }

    return 0;
}
#endif /* QC_TRANSPORT_SPI */

#if defined(QC_TRANSPORT_SDIO)
/* Future: SDIO transport implementation */
#endif

#if defined(QC_TRANSPORT_UART)
/* Future: UART transport implementation */
#endif

#if defined(QC_TRANSPORT_USB)
/* Future: USB transport implementation */
#endif

#endif /* QC_PLATFORM_STM32 && QC_OS_ZEPHYR */
