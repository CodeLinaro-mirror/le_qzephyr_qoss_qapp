/*
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "../../qc_port_config.h"

#if defined(QC_PLATFORM_STM32) && defined(QC_OS_FREERTOS)

#include "FreeRTOS.h"
#include "stm32u5xx_hal.h"
#include <stddef.h>
#include <stdio.h>
#include "../qc_platform.h"

#if defined(QC_TRANSPORT_SPI)
/* External SPI handle from CubeMX generated code */
extern SPI_HandleTypeDef hspi1;

/* Software CS pin configuration - GPIOA Pin 4 */
#define SPI_CS_GPIO_PORT GPIOA
#define SPI_CS_GPIO_PIN GPIO_PIN_4

/* CS control macros */
#define SPI_CS_LOW() HAL_GPIO_WritePin(SPI_CS_GPIO_PORT, SPI_CS_GPIO_PIN, GPIO_PIN_RESET)
#define SPI_CS_HIGH() HAL_GPIO_WritePin(SPI_CS_GPIO_PORT, SPI_CS_GPIO_PIN, GPIO_PIN_SET)
#endif /* QC_TRANSPORT_SPI */

/**
 * @brief STM32 platform context structure for FreeRTOS
 *
 * This structure holds references to STM32 HAL handles that are
 * initialized by CubeMX generated code.
 */
struct qc_platform_stm32_ctx {
#if defined(QC_TRANSPORT_SPI)
    SPI_HandleTypeDef *hspi; /* SPI handle from CubeMX */
#endif
    bool initialized; /* Initialization flag */
                      /* Future: Add SDIO, UART, USB specific fields here */
};

/**
 * @brief Convert HAL status to platform error code
 */
static inline int hal_status_to_platform(HAL_StatusTypeDef status)
{
    switch (status) {
    case HAL_OK:
        return 0;
    case HAL_ERROR:
        return -QC_PLATFORM_EIO;
    case HAL_BUSY:
        return -QC_PLATFORM_EBUSY;
    case HAL_TIMEOUT:
        return -QC_PLATFORM_EIO;
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

    if (!ctx) {
        return -QC_PLATFORM_EINVAL;
    }

    /* Allocate platform context */
    stm32_ctx = pvPortMalloc(sizeof(struct qc_platform_stm32_ctx));
    if (!stm32_ctx) {
        printf("[Platform] Failed to allocate platform context\r\n");
        return -QC_PLATFORM_EIO;
    }

#if defined(QC_TRANSPORT_SPI)
    /* Get reference to SPI handle initialized by CubeMX
     * NOTE: MX_SPI1_Init() and MX_GPIO_Init() must be called before this
     */
    stm32_ctx->hspi = &hspi1;

    /* Verify SPI is initialized */
    if (stm32_ctx->hspi->Instance == NULL) {
        printf("[Platform] SPI not initialized by CubeMX\r\n");
        vPortFree(stm32_ctx);
        return -QC_PLATFORM_ENODEV;
    }

    /* Ensure CS is deasserted (high) initially */
    SPI_CS_HIGH();
#endif /* QC_TRANSPORT_SPI */

    stm32_ctx->initialized = true;
    printf("[Platform] %s platform initialized on %s with %s transport\r\n", QC_PLATFORM_NAME, QC_OS_NAME,
           QC_TRANSPORT_NAME);

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
    SPI_CS_HIGH();
#endif /* QC_TRANSPORT_SPI */

    vPortFree(stm32_ctx);
    printf("[Platform] %s platform deinitialized\r\n", QC_PLATFORM_NAME);

    return 0;
}

/* ============================================================================
 * Transport-Specific Operations
 * ============================================================================ */

#if defined(QC_TRANSPORT_SPI)
/**
 * @brief SPI transceive operation for STM32 on FreeRTOS
 *
 * This function is only compiled when SPI transport is selected.
 * Future transports (SDIO, UART, USB) will have their own sections.
 */
int qc_platform_spi_transceive(qc_platform_ctx_t ctx, uint8_t *tx_buf, uint8_t *rx_buf, size_t len, bool hold_cs)
{
    struct qc_platform_stm32_ctx *stm32_ctx = (struct qc_platform_stm32_ctx *)ctx;
    HAL_StatusTypeDef status;
    int ret = 0;

    /* Validate context and parameters */
    if (!stm32_ctx || !stm32_ctx->initialized) {
        return -QC_PLATFORM_EINVAL;
    }

    if (!tx_buf || !rx_buf || len == 0) {
        return -QC_PLATFORM_EINVAL;
    }

    /* Assert CS (pull low) before transfer */
    SPI_CS_LOW();

    if (len <= 16) {
        /* Small data: use polling mode */
        status = HAL_SPI_TransmitReceive(stm32_ctx->hspi, tx_buf, rx_buf, len, HAL_MAX_DELAY);

        if (status != HAL_OK) {
            ret = hal_status_to_platform(status);
            goto cleanup;
        }
    } else {
        /* Large data: use DMA mode */
        status = HAL_SPI_TransmitReceive_DMA(stm32_ctx->hspi, tx_buf, rx_buf, len);

        if (status != HAL_OK) {
            ret = hal_status_to_platform(status);
            goto cleanup;
        }

        /* Wait for DMA transfer completion */
        uint32_t timeout = HAL_GetTick() + 1000; /* 1 second timeout */

        while (HAL_SPI_GetState(stm32_ctx->hspi) != HAL_SPI_STATE_READY) {
            if (HAL_GetTick() > timeout) {
                ret = hal_status_to_platform(HAL_TIMEOUT);
                goto cleanup;
            }
        }
    }

cleanup:
    /* Deassert CS (pull high) on error or if not holding */
    if (ret != 0 || !hold_cs) {
        SPI_CS_HIGH();
    }

    return ret;
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

#endif /* QC_PLATFORM_STM32 && QC_OS_FREERTOS */
