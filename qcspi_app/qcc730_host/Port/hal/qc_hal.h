/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef QC_HAL_H_
#define QC_HAL_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * @file qc_hal.h
 * @brief Qualcomm Hardware Abstraction Layer (HAL)
 *
 * This layer provides OS-independent hardware interfaces for:
 * - GPIO operations
 * - SPI operations
 * - Platform initialization
 *
 * The HAL layer is completely independent of the OS and only deals with
 * hardware-specific operations. It uses the OSAL layer for memory allocation
 * and other OS services when needed.
 */

/* ============================================================================
 * Error Codes
 * ============================================================================ */

#define QC_HAL_EOK 0       /* Success */
#define QC_HAL_EINVAL 1    /* Invalid argument */
#define QC_HAL_ENOMEM 2    /* Out of memory */
#define QC_HAL_ENODEV 3    /* No such device */
#define QC_HAL_EIO 9       /* I/O error */
#define QC_HAL_EBUSY 10    /* Device or resource busy */
#define QC_HAL_ETIMEDOUT 5 /* Timeout */

/* ============================================================================
 * HAL Context
 * ============================================================================ */

/**
 * @brief Opaque HAL context handle
 *
 * This structure encapsulates all platform-specific hardware state.
 * The actual structure is defined in platform-specific implementation files.
 */
typedef void *qc_hal_ctx_t;

/**
 * @brief Initialize HAL context
 *
 * This function initializes the HAL context and prepares hardware for use.
 * It gets handles to pre-initialized hardware devices.
 *
 * @param ctx Pointer to store HAL context handle
 * @return 0 on success, negative error code on failure
 */
int qc_hal_init(qc_hal_ctx_t *ctx);

/**
 * @brief Deinitialize HAL context
 *
 * Releases resources associated with the HAL context.
 *
 * @param ctx HAL context handle
 * @return 0 on success, negative error code on failure
 */
int qc_hal_deinit(qc_hal_ctx_t ctx);

/* ============================================================================
 * GPIO Operations
 * ============================================================================ */

/**
 * @brief Opaque GPIO handle
 */
typedef void *qc_hal_gpio_t;

/**
 * @brief GPIO configuration
 */
struct qc_hal_gpio_config {
    uint32_t pin;       /* GPIO pin number */
    bool output;        /* true = output, false = input */
    bool active_low;    /* true = active low, false = active high */
    bool initial_value; /* Initial value for output pins */
};

/**
 * @brief Initialize a GPIO pin
 *
 * @param ctx HAL context handle
 * @param gpio Pointer to store GPIO handle
 * @param config GPIO configuration
 * @return 0 on success, negative error code on failure
 */
int qc_hal_gpio_init(qc_hal_ctx_t ctx, qc_hal_gpio_t *gpio, const struct qc_hal_gpio_config *config);

/**
 * @brief Set GPIO output value
 *
 * @param gpio GPIO handle
 * @param value Output value (true = high/active, false = low/inactive)
 * @return 0 on success, negative error code on failure
 */
int qc_hal_gpio_set(qc_hal_gpio_t gpio, bool value);

/**
 * @brief Get GPIO input value
 *
 * @param gpio GPIO handle
 * @param value Pointer to store input value
 * @return 0 on success, negative error code on failure
 */
int qc_hal_gpio_get(qc_hal_gpio_t gpio, bool *value);

/**
 * @brief Deinitialize a GPIO pin
 *
 * @param gpio GPIO handle
 * @return 0 on success, negative error code on failure
 */
int qc_hal_gpio_deinit(qc_hal_gpio_t gpio);

/* ============================================================================
 * SPI Operations
 * ============================================================================ */

/**
 * @brief Opaque SPI handle
 */
typedef void *qc_hal_spi_t;

/**
 * @brief SPI mode definitions
 */
#define QC_HAL_SPI_MODE_0 0 /* CPOL=0, CPHA=0 */
#define QC_HAL_SPI_MODE_1 1 /* CPOL=0, CPHA=1 */
#define QC_HAL_SPI_MODE_2 2 /* CPOL=1, CPHA=0 */
#define QC_HAL_SPI_MODE_3 3 /* CPOL=1, CPHA=1 */

/**
 * @brief SPI configuration
 */
struct qc_hal_spi_config {
    uint32_t frequency;    /* SPI frequency in Hz */
    uint8_t mode;          /* SPI mode (0-3) */
    uint8_t bits_per_word; /* Bits per word (typically 8) */
};

/**
 * @brief Initialize SPI interface
 *
 * @param ctx HAL context handle
 * @param spi Pointer to store SPI handle
 * @param config SPI configuration
 * @return 0 on success, negative error code on failure
 */
int qc_hal_spi_init(qc_hal_ctx_t ctx, qc_hal_spi_t *spi, const struct qc_hal_spi_config *config);

/**
 * @brief Perform SPI transfer (full-duplex)
 *
 * This function performs a full-duplex SPI transfer.
 * CS control is handled separately via GPIO operations.
 *
 * @param spi SPI handle
 * @param tx_buf Transmit buffer (data to send)
 * @param rx_buf Receive buffer (data received)
 * @param len Length of data to transfer (in bytes)
 * @return 0 on success, negative error code on failure
 */
int qc_hal_spi_transfer(qc_hal_spi_t spi, const uint8_t *tx_buf, uint8_t *rx_buf, size_t len);

/**
 * @brief Deinitialize SPI interface
 *
 * @param spi SPI handle
 * @return 0 on success, negative error code on failure
 */
int qc_hal_spi_deinit(qc_hal_spi_t spi);

#endif /* QC_HAL_H_ */
