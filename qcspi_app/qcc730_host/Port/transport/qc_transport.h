/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef QC_TRANSPORT_H_
#define QC_TRANSPORT_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * @file qc_transport.h
 * @brief Qualcomm Transport Layer
 *
 * This layer provides a unified interface for different transport protocols:
 * - SPI
 * - SDIO (future)
 * - UART (future)
 *
 * The transport layer uses HAL for hardware operations and OSAL for
 * OS services (mutex, memory allocation, etc.)
 */

/* ============================================================================
 * Error Codes
 * ============================================================================ */

#define QC_TRANSPORT_EOK 0       /* Success */
#define QC_TRANSPORT_EINVAL 1    /* Invalid argument */
#define QC_TRANSPORT_ENOMEM 2    /* Out of memory */
#define QC_TRANSPORT_ENODEV 3    /* No such device */
#define QC_TRANSPORT_EIO 9       /* I/O error */
#define QC_TRANSPORT_EBUSY 10    /* Device or resource busy */
#define QC_TRANSPORT_ETIMEDOUT 5 /* Timeout */

/* ============================================================================
 * Transport Handle
 * ============================================================================ */

/**
 * @brief Opaque transport handle
 */
typedef void *qc_transport_t;

/**
 * @brief Transport configuration
 */
struct qc_transport_config {
    void *hal_ctx; /* HAL context from qc_hal_init() */
    /* Transport-specific configuration can be added here */
};

/* ============================================================================
 * Transport Operations
 * ============================================================================ */

/**
 * @brief Initialize transport layer
 *
 * This function initializes the transport layer, including:
 * - Initializing hardware interfaces (SPI, GPIO, etc.) via HAL
 * - Setting up synchronization primitives via OSAL
 * - Preparing for data transfer
 *
 * @param transport Pointer to store transport handle
 * @param config Transport configuration
 * @return 0 on success, negative error code on failure
 */
int qc_transport_init(qc_transport_t *transport, const struct qc_transport_config *config);

/**
 * @brief Deinitialize transport layer
 *
 * Releases all resources associated with the transport layer.
 *
 * @param transport Transport handle
 * @return 0 on success, negative error code on failure
 */
int qc_transport_deinit(qc_transport_t transport);

/**
 * @brief Perform a transceive operation
 *
 * This function performs a full-duplex data transfer with automatic
 * chip select control.
 *
 * @param transport Transport handle
 * @param tx_buf Transmit buffer (data to send)
 * @param rx_buf Receive buffer (data received)
 * @param len Length of data to transfer (in bytes)
 * @param hold_cs If true, keep CS asserted after transfer
 *                If false, deassert CS after transfer
 * @return 0 on success, negative error code on failure
 */
int qc_transport_transceive(qc_transport_t transport, const uint8_t *tx_buf, uint8_t *rx_buf, size_t len, bool hold_cs);

/**
 * @brief Send data (transmit only)
 *
 * @param transport Transport handle
 * @param data Data to send
 * @param len Length of data (in bytes)
 * @return 0 on success, negative error code on failure
 */
int qc_transport_send(qc_transport_t transport, const uint8_t *data, size_t len);

/**
 * @brief Receive data (receive only)
 *
 * @param transport Transport handle
 * @param data Buffer to store received data
 * @param len Length of data to receive (in bytes)
 * @return 0 on success, negative error code on failure
 */
int qc_transport_recv(qc_transport_t transport, uint8_t *data, size_t len);

/* ============================================================================
 * Transport Statistics (Optional)
 * ============================================================================ */

/**
 * @brief Transport statistics
 */
struct qc_transport_stats {
    uint32_t tx_bytes;     /* Total bytes transmitted */
    uint32_t rx_bytes;     /* Total bytes received */
    uint32_t tx_errors;    /* Transmission errors */
    uint32_t rx_errors;    /* Reception errors */
    uint32_t tx_timeouts;  /* Transmission timeouts */
    uint32_t rx_timeouts;  /* Reception timeouts */
    uint32_t transactions; /* Total transactions */
};

/**
 * @brief Get transport statistics
 *
 * @param transport Transport handle
 * @param stats Pointer to store statistics
 * @return 0 on success, negative error code on failure
 */
int qc_transport_get_stats(qc_transport_t transport, struct qc_transport_stats *stats);

/**
 * @brief Reset transport statistics
 *
 * @param transport Transport handle
 * @return 0 on success, negative error code on failure
 */
int qc_transport_reset_stats(qc_transport_t transport);

#endif /* QC_TRANSPORT_H_ */
