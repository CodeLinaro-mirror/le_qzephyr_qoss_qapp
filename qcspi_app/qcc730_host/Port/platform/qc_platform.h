/*
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef QC_PLATFORM_H_
#define QC_PLATFORM_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "qc_port_config.h"

/**
 * @brief Qualcomm Platform Hardware Abstraction Layer
 *
 * This header defines platform-agnostic interfaces for hardware operations.
 *
 * DESIGN PHILOSOPHY:
 * - Provides unified interface across different platforms (FreeRTOS, Zephyr, etc.)
 * - Does NOT initialize hardware - assumes hardware is already initialized:
 *   * FreeRTOS: By CubeMX generated code (MX_SPI_Init, MX_GPIO_Init, etc.)
 *   * Zephyr: By device tree and driver subsystem
 * - Platform-specific details (SPI config, GPIO pins, etc.) are encapsulated
 *   in the implementation files
 */

/* ============================================================================
 * Error Codes
 * ============================================================================ */

#define QC_PLATFORM_EOK 0    /* Success */
#define QC_PLATFORM_EINVAL 1 /* Invalid argument */
#define QC_PLATFORM_ENODEV 3 /* No such device */
#define QC_PLATFORM_EIO 9    /* I/O error */
#define QC_PLATFORM_EBUSY 10 /* Device or resource busy */

/* ============================================================================
 * Platform Context Structure
 * ============================================================================ */

/**
 * @brief Opaque platform context handle
 *
 * This structure encapsulates all platform-specific state including:
 * - SPI device handle
 * - SPI configuration (for platforms that need it like Zephyr)
 * - GPIO handles for CS control
 * - Any other platform-specific data
 *
 * The actual structure is defined in platform-specific implementation files.
 */
typedef void *qc_platform_ctx_t;

/* ============================================================================
 * Platform Initialization
 * ============================================================================ */

/**
 * @brief Initialize platform context
 *
 * This function creates and initializes the platform context, which includes:
 * - Getting handles to pre-initialized hardware (SPI, GPIO)
 * - Setting up internal configuration structures
 * - Preparing for hardware operations
 *
 * NOTE: This does NOT initialize the hardware itself. Hardware must be
 * initialized before calling this function:
 * - FreeRTOS: Call MX_SPI_Init(), MX_GPIO_Init() first (usually in main.c)
 * - Zephyr: Hardware is initialized by device tree and driver subsystem
 *
 * @param ctx Pointer to store platform context handle
 * @return 0 on success, negative error code on failure
 */
int qc_platform_init(qc_platform_ctx_t *ctx);

/**
 * @brief Deinitialize platform context
 *
 * Releases resources associated with the platform context.
 * Does NOT deinitialize the underlying hardware.
 *
 * @param ctx Platform context handle
 * @return 0 on success, negative error code on failure
 */
int qc_platform_deinit(qc_platform_ctx_t ctx);

/* ============================================================================
 * SPI Operations
 * ============================================================================ */

/**
 * @brief Perform SPI transceive operation
 *
 * This function performs a full-duplex SPI transfer with automatic CS control.
 *
 * CS (Chip Select) behavior:
 * - CS is asserted (active low) at the start of transfer
 * - If hold_cs is false: CS is deasserted (inactive high) after transfer
 * - If hold_cs is true: CS remains asserted for the next transfer
 *
 * Implementation details:
 * - FreeRTOS: Uses HAL_SPI_TransmitReceive with manual GPIO CS control
 * - Zephyr: Uses spi_transceive with internal spi_config
 *
 * @param ctx Platform context handle (from qc_platform_init)
 * @param tx_buf Transmit buffer (data to send)
 * @param rx_buf Receive buffer (data received)
 * @param len Length of data to transfer (in bytes)
 * @param hold_cs If true, keep CS asserted after transfer
 *                If false, deassert CS after transfer
 * @return 0 on success, negative error code on failure
 */
int qc_platform_spi_transceive(qc_platform_ctx_t ctx, uint8_t *tx_buf, uint8_t *rx_buf, size_t len, bool hold_cs);

#if defined(QC_PLATFORM_STM32)
#include "stm32u5xx_hal.h"
/*Platform UART Handle Type*/
typedef UART_HandleTypeDef QC_HAL_UART_HandleTypeDef;
#define UART_DEVICE huart1
#define QC_HAL_UART_Receive_IT(huart, pData, Size) HAL_UART_Receive_IT(huart, pData, Size)

extern QC_HAL_UART_HandleTypeDef huart1;

/*Platform send an amount of data in blocking mode*/
#define QC_HAL_UART_Transmit(huart, pData, Size, Timeout) HAL_UART_Transmit(huart, pData, Size, Timeout)
#endif

#endif /* QC_PLATFORM_H_ */
