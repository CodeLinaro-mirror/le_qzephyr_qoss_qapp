/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef QCC730_SPI_DEMO_H
#define QCC730_SPI_DEMO_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize SPI demo
 *
 * This function initializes the SPI demo by registering the ring service
 * callback for handling data from QCC730.
 */
void spi_demo_init(void);

/**
 * @brief Register SPI demo shell commands
 *
 * This function registers all SPI-related shell commands including:
 * - QCSPI commands (init, read, write, register access, etc.)
 * - Ring service commands (status, send, burst test, etc.)
 */
void spi_demo_register_commands(void);

#ifdef __cplusplus
}
#endif

#endif /* QCC730_SPI_DEMO_H */
