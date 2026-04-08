/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef QC_PORT_CONFIG_H_
#define QC_PORT_CONFIG_H_

/**
 * @file qc_port_config.h
 * @brief Porting layer configuration
 *
 * This file defines the high-level configuration for the porting layer:
 * - Operating System selection
 * - Platform/Hardware selection
 * - Transport interface selection (SPI, SDIO, UART, etc.)
 *
 * Hardware details (GPIO pins, frequencies, etc.) are configured in the
 * platform-specific implementation files, not here.
 */

/* ============================================================================
 * Operating System Selection
 * ============================================================================ */

/**
 * @brief Operating System Options
 *
 * Define ONE of the following to select the target OS:
 */

/* Uncomment ONE of the following: */
//#define QC_OS_ZEPHYR        1
#define QC_OS_FREERTOS 1
/* #define QC_OS_LINUX         1 */ // not support now

/* ============================================================================
 * Platform/Hardware Selection
 * ============================================================================ */

/**
 * @brief Platform Options
 *
 * Define ONE of the following to select the target platform:
 */

/* Uncomment ONE of the following: */
#define QC_PLATFORM_STM32 1
/* #define QC_PLATFORM_RPI     1 */ // not support now

/* ============================================================================
 * Transport Interface Selection
 * ============================================================================ */

/**
 * @brief Transport Interface Options
 *
 * Define ONE of the following to select the transport interface:
 * - QC_TRANSPORT_SPI    : SPI interface
 * - QC_TRANSPORT_SDIO   : SDIO interface
 */

/* Uncomment ONE of the following: */
#define QC_TRANSPORT_SPI 1
/* #define QC_TRANSPORT_SDIO   1 */ // not support now

/* User Macro */
#define CONFIG_RING_SERVICE
#define SHELL_FEATURE
//#define QCC730_SPI_ENABLE
#define QCC730_ATCMD_ENABLE

/* ============================================================================
 * Configuration Validation
 * ============================================================================ */

/* Validate OS selection */
#if !defined(QC_OS_ZEPHYR) && !defined(QC_OS_FREERTOS) && !defined(QC_OS_LINUX)
#error "No operating system selected! Please define one of: QC_OS_ZEPHYR, QC_OS_FREERTOS, QC_OS_LINUX"
#endif

/* Check for multiple OS selections */
#if (defined(QC_OS_ZEPHYR) + defined(QC_OS_FREERTOS) + defined(QC_OS_LINUX)) > 1
#error "Multiple operating systems selected! Please define only ONE OS."
#endif

/* Validate Platform selection */
#if !defined(QC_PLATFORM_STM32) && !defined(QC_PLATFORM_RPI)
#error "No platform selected! Please define one of: QC_PLATFORM_STM32, QC_PLATFORM_RPI"
#endif

/* Check for multiple Platform selections */
#if (defined(QC_PLATFORM_STM32) + defined(QC_PLATFORM_RPI)) > 1
#error "Multiple platforms selected! Please define only ONE platform."
#endif

/* Validate Transport selection */
#if !defined(QC_TRANSPORT_SPI) && !defined(QC_TRANSPORT_SDIO)
#error "No transport interface selected! Please define one of: QC_TRANSPORT_SPI, QC_TRANSPORT_SDIO"
#endif

/* Check for multiple Transport selections */
#if (defined(QC_TRANSPORT_SPI) + defined(QC_TRANSPORT_SDIO)) > 1
#error "Multiple transport interfaces selected! Please define only ONE transport."
#endif

/* Validate Demo selection */
#if !defined(QCC730_SPI_ENABLE) && !defined(QCC730_ATCMD_ENABLE)
#error "No demo selected! Please define one of: QCC730_SPI_ENABLE, QCC730_ATCMD_ENABLE"
#endif

/* Check for multiple Demo selections */
#if (defined(QCC730_SPI_ENABLE) + defined(QCC730_ATCMD_ENABLE)) > 1
#error "Multiple demo selected! QCC730_SPI_ENABLE and QCC730_ATCMD_ENABLE cannot be enabled at the same time."
#endif

/* ============================================================================
 * Configuration Summary Strings (for logging/debugging)
 * ============================================================================ */

#if defined(QC_OS_ZEPHYR)
#define QC_OS_NAME "Zephyr"
#elif defined(QC_OS_FREERTOS)
#define QC_OS_NAME "FreeRTOS"
#elif defined(QC_OS_LINUX)
#define QC_OS_NAME "Linux"
#endif

#if defined(QC_PLATFORM_STM32)
#define QC_PLATFORM_NAME "STM32"
#elif defined(QC_PLATFORM_RPI)
#define QC_PLATFORM_NAME "Raspberry Pi"
#endif

#if defined(QC_TRANSPORT_SPI)
#define QC_TRANSPORT_NAME "SPI"
#elif defined(QC_TRANSPORT_SDIO)
#define QC_TRANSPORT_NAME "SDIO"
#endif

#endif /* QC_PORT_CONFIG_H_ */
