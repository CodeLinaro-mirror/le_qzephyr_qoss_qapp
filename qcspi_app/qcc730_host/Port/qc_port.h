/*
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef QC_PORT_H_
#define QC_PORT_H_

/**
 * @file qc_port.h
 * @brief Qualcomm Port Layer - Unified Interface
 *
 * This is the main header file for the Qualcomm Port Layer.
 * It provides a unified interface that combines:
 * - OSAL (Operating System Abstraction Layer)
 * - HAL (Hardware Abstraction Layer)
 * - Transport Layer
 *
 * Applications should include this file to access all port layer functionality.
 */

/* Include all port layer components */
#include "qc_port_config.h"
#include "osal/qc_osal.h"
#include "hal/qc_hal.h"
#include "transport/qc_transport.h"

/**
 * @brief Port layer version information
 */
#define QC_PORT_VERSION_MAJOR 1
#define QC_PORT_VERSION_MINOR 0
#define QC_PORT_VERSION_PATCH 0

/**
 * @brief Port layer context
 *
 * This structure holds all port layer contexts for easy management.
 */
struct qc_port_ctx {
    qc_hal_ctx_t hal_ctx;           /* HAL context */
    qc_transport_t transport;       /* Transport handle */
};

/**
 * @brief Initialize the entire port layer
 *
 * This is a convenience function that initializes all port layer components
 * in the correct order:
 * 1. HAL (Hardware Abstraction Layer)
 * 2. Transport Layer
 *
 * Note: OSAL functions don't require initialization as they are stateless.
 *
 * @param port_ctx Pointer to store port context
 * @return 0 on success, negative error code on failure
 */
static inline int qc_port_init(struct qc_port_ctx *port_ctx)
{
    struct qc_transport_config transport_cfg;
    int ret;

    if (!port_ctx) {
        return -1;
    }

    /* Initialize HAL */
    ret = qc_hal_init(&port_ctx->hal_ctx);
    if (ret < 0) {
        QC_OSAL_LOG_ERR("Failed to initialize HAL: %d", ret);
        return ret;
    }

    /* Initialize Transport */
    transport_cfg.hal_ctx = port_ctx->hal_ctx;
    ret = qc_transport_init(&port_ctx->transport, &transport_cfg);
    if (ret < 0) {
        QC_OSAL_LOG_ERR("Failed to initialize transport: %d", ret);
        qc_hal_deinit(port_ctx->hal_ctx);
        return ret;
    }

    QC_OSAL_LOG_INF("Port layer initialized successfully");
    QC_OSAL_LOG_INF("  Platform: %s", QC_PLATFORM_NAME);
    QC_OSAL_LOG_INF("  OS: %s", QC_OS_NAME);
    QC_OSAL_LOG_INF("  Transport: %s", QC_TRANSPORT_NAME);

    return 0;
}

/**
 * @brief Deinitialize the entire port layer
 *
 * This function deinitializes all port layer components in reverse order.
 *
 * @param port_ctx Port context
 * @return 0 on success, negative error code on failure
 */
static inline int qc_port_deinit(struct qc_port_ctx *port_ctx)
{
    if (!port_ctx) {
        return -1;
    }

    /* Deinitialize in reverse order */
    qc_transport_deinit(port_ctx->transport);
    qc_hal_deinit(port_ctx->hal_ctx);

    QC_OSAL_LOG_INF("Port layer deinitialized");

    return 0;
}

#endif /* QC_PORT_H_ */
