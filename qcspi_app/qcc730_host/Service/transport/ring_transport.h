/*
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef RING_TRANSPORT_H_
#define RING_TRANSPORT_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <errno.h>

/* OS-agnostic transport device handle */
typedef void *ring_transport_dev_t;

/**
 * @brief Transport layer API for ring service
 *
 * This abstraction allows ring service to work with any transport
 * (SPI, UART, SDIO shared memory, etc.)
 */
struct ring_transport_api {
    /**
     * @brief Initialize transport layer
     *
     * @param dev Transport device handle
     * @return 0 on success, negative errno on failure
     */
    int (*init)(ring_transport_dev_t dev);

    /**
     * @brief Deinitialize transport layer
     *
     * @return 0 on success, negative errno on failure
     */
    int (*deinit)(ring_transport_dev_t dev);

    /**
     * @brief Read data from remote device memory
     *
     * @param dev Transport device handle
     * @param addr Remote memory address to read from
     * @param data Buffer to store read data
     * @param len Number of bytes to read
     * @return 0 on success, negative errno on failure
     */
    int (*read)(ring_transport_dev_t dev, uint32_t addr, uint8_t *data, size_t len);

    /**
     * @brief Write data to remote device memory
     *
     * @param dev Transport device handle
     * @param addr Remote memory address to write to
     * @param data Buffer containing data to write
     * @param len Number of bytes to write
     * @return 0 on success, negative errno on failure
     */
    int (*write)(ring_transport_dev_t dev, uint32_t addr, const uint8_t *data, size_t len);

    /**
     * @brief Trigger interrupt on remote device
     *
     * @param dev Transport device handle
     * @return 0 on success, negative errno on failure
     */
    int (*interrupt)(ring_transport_dev_t dev);

    /**
     * @brief Read remote device register
     *
     * @param dev Transport device handle
     * @param reg_addr Register address
     * @param value Pointer to store register value
     * @return 0 on success, negative errno on failure
     */
    int (*reg_read)(ring_transport_dev_t dev, uint8_t reg_addr, uint32_t *value);

    /**
     * @brief Write remote device register
     *
     * @param dev Transport device handle
     * @param reg_addr Register address
     * @param value Value to write to register
     * @return 0 on success, negative errno on failure
     */
    int (*reg_write)(ring_transport_dev_t dev, uint8_t reg_addr, uint32_t value);

    /**
     * @brief Reset remote device
     *
     * @param dev Transport device handle
     * @return 0 on success, negative errno on failure
     */
    int (*reset)(ring_transport_dev_t dev);
};

/**
 * @brief Get transport API from device handle
 */
const struct ring_transport_api *ring_transport_get_api(ring_transport_dev_t dev);

/**
 * @brief Helper function to read from transport
 */
static inline int ring_transport_read(ring_transport_dev_t dev, uint32_t addr, uint8_t *data, size_t len)
{
    const struct ring_transport_api *api = ring_transport_get_api(dev);

    if (!api || !api->read) {
        return -ENOTSUP;
    }

    return api->read(dev, addr, data, len);
}

/**
 * @brief Helper function to write to transport
 */
static inline int ring_transport_write(ring_transport_dev_t dev, uint32_t addr, const uint8_t *data, size_t len)
{
    const struct ring_transport_api *api = ring_transport_get_api(dev);

    if (!api || !api->write) {
        return -ENOTSUP;
    }

    return api->write(dev, addr, data, len);
}

/**
 * @brief Helper function to trigger interrupt
 */
static inline int ring_transport_interrupt(ring_transport_dev_t dev)
{
    const struct ring_transport_api *api = ring_transport_get_api(dev);

    if (!api || !api->interrupt) {
        return -ENOTSUP;
    }

    return api->interrupt(dev);
}

/**
 * @brief Helper function to read register
 */
static inline int ring_transport_reg_read(ring_transport_dev_t dev, uint8_t reg_addr, uint32_t *value)
{
    const struct ring_transport_api *api = ring_transport_get_api(dev);

    if (!api || !api->reg_read) {
        return -ENOTSUP;
    }

    return api->reg_read(dev, reg_addr, value);
}

/**
 * @brief Helper function to write register
 */
static inline int ring_transport_reg_write(ring_transport_dev_t dev, uint8_t reg_addr, uint32_t value)
{
    const struct ring_transport_api *api = ring_transport_get_api(dev);

    if (!api || !api->reg_write) {
        return -ENOTSUP;
    }

    return api->reg_write(dev, reg_addr, value);
}

/**
 * @brief Helper function to reset device
 */
static inline int ring_transport_reset(ring_transport_dev_t dev)
{
    const struct ring_transport_api *api = ring_transport_get_api(dev);

    if (!api || !api->reset) {
        return -ENOTSUP;
    }

    return api->reset(dev);
}

/**
 * @brief Helper function to init device
 */
static inline int ring_transport_init(ring_transport_dev_t dev)
{
    const struct ring_transport_api *api = ring_transport_get_api(dev);

    if (!api || !api->init) {
        return -ENOTSUP;
    }

    return api->init(dev);
}

/**
 * @brief Helper function to deinit device
 */
static inline int ring_transport_deinit(ring_transport_dev_t dev)
{
    const struct ring_transport_api *api = ring_transport_get_api(dev);

    if (!api || !api->deinit) {
        return -ENOTSUP;
    }

    return api->deinit(dev);
}
#endif /* RING_TRANSPORT_H_ */
