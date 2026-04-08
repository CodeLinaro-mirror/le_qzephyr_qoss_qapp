/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef RING_ADAPTER_H_
#define RING_ADAPTER_H_

#include <stdint.h>
#include <stddef.h>

/**
 * @file ring_adapter.h
 * @brief Ring service internal adapter interface
 *
 * This is an internal interface used by ring_service_host.c to communicate
 * with different transport adapters (QCSPI, UART, SDIO, etc.).
 *
 * NOTE: This is NOT a public API. Applications should use ring_service.h
 */

/**
 * @brief Ring adapter operations
 *
 * Each adapter (qcspi, uart, sdio) implements these operations
 */
struct ring_adapter_ops {
    /**
     * @brief Initialize adapter
     * @return 0 on success, negative errno on failure
     */
    int (*init)(void);

    /**
     * @brief Deinitialize adapter
     * @return 0 on success, negative errno on failure
     */
    int (*deinit)(void);

    /**
     * @brief Read data from remote device memory
     *
     * @param addr Remote memory address
     * @param buf Buffer to store read data
     * @param len Number of bytes to read
     * @return 0 on success, negative errno on failure
     */
    int (*mem_read)(uint32_t addr, void *buf, size_t len);

    /**
     * @brief Write data to remote device memory
     *
     * @param addr Remote memory address
     * @param buf Buffer containing data to write
     * @param len Number of bytes to write
     * @return 0 on success, negative errno on failure
     */
    int (*mem_write)(uint32_t addr, const void *buf, size_t len);

    /**
     * @brief Trigger interrupt on remote device
     * @return 0 on success, negative errno on failure
     */
    int (*trigger_irq)(void);
};

/**
 * @brief Get QCSPI adapter operations
 *
 * @return Pointer to QCSPI adapter operations
 */
const struct ring_adapter_ops *ring_adapter_get_qcspi(void);

/* Future adapters can be added here:
 * const struct ring_adapter_ops *ring_adapter_get_uart(void);
 * const struct ring_adapter_ops *ring_adapter_get_sdio(void);
 */

#endif /* RING_ADAPTER_H_ */
