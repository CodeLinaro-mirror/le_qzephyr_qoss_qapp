/*
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef RING_SERVICE_H_
#define RING_SERVICE_H_

#include <stdint.h>
#include <stdbool.h>
#include <sys/_stdint.h>

/* Maximum number of rings supported */
#define MAX_RINGS 8

/* Ring status */
#define RING_STATUS_VALID (1)    /* Descriptor is valid */
#define RING_STATUS_INVALID (-1) /* Descriptor is invalid */

#define CONFIG_RING_CTRL_BLOCK_ADDR 0x9fc00

/**
 * @brief Ring buffer descriptor
 *
 * Each descriptor describes a fixed-size buffer (1500 bytes)
 */
struct ring_descriptor {
    uint32_t buffer_addr; /* Buffer physical address (in Slave memory space) */
    uint16_t length;      /* Actual data length (0-1500) */
    uint16_t flags;       /* Status flags */
} __packed __aligned(4);

/* Descriptor flags */
#define RING_DESC_FLAG_VALID (1 << 0)  /* Descriptor is valid */
#define RING_DESC_FLAG_WRAP (1 << 1)   /* Index wrap flag */
#define RING_DESC_FLAG_ERROR (1 << 15) /* Error flag */

/**
 * @brief Ring configuration
 */
struct ring_config {
    uint8_t ring_id;     /* Ring ID (0-7) */
    uint32_t desc_count; /* Descriptor count (e.g., 16) */
    uint32_t buf_size;   /* Buffer size (e.g., 1500) */
};

/**
 * @brief Ring control block (supports multiple rings)
 *
 * Fixed at specific address via linker script, known to both Host and Slave
 */
struct ring_control_block {
    /* Header */
    uint32_t magic;     /* 0x52494E47 ("RING") */
    uint32_t version;   /* 0x00020000 (Version 2.0) */
    uint32_t num_rings; /* Number of configured rings (1-8) */

    /* TX Ring arrays (Host→Slave) - independent per ring */
    uint32_t tx_desc_base[MAX_RINGS];       /* TX descriptor array address */
    uint32_t tx_desc_count[MAX_RINGS];      /* Descriptor count */
    volatile uint32_t tx_wr_idx[MAX_RINGS]; /* Write index (updated by Host) */
    volatile uint32_t tx_rd_idx[MAX_RINGS]; /* Read index (updated by Slave) */
    uint32_t tx_buf_size[MAX_RINGS];        /* Each buffer size */
    uint32_t tx_buf_base[MAX_RINGS];        /* Buffer pool address */

    /* RX Ring arrays (Slave→Host) - independent per ring */
    uint32_t rx_desc_base[MAX_RINGS];       /* RX descriptor array address */
    uint32_t rx_desc_count[MAX_RINGS];      /* Descriptor count */
    volatile uint32_t rx_wr_idx[MAX_RINGS]; /* Write index (updated by Slave) */
    volatile uint32_t rx_rd_idx[MAX_RINGS]; /* Read index (updated by Host) */
    uint32_t rx_buf_size[MAX_RINGS];        /* Each buffer size */
    uint32_t rx_buf_base[MAX_RINGS];        /* Buffer pool address */

    /* Per Ring status */
    uint8_t ring_status[MAX_RINGS];

    /* Status */
    uint32_t status;      /* Status flags */
    uint32_t reserved[8]; /* Reserved for future use */
} __packed __aligned(4);

/* Magic number */
#define RING_MAGIC_NUMBER 0x52494E47 /* "RING" */
#define RING_VERSION 0x00020000      /* Version 2.0.0 */

/* Status flags */
#define RING_STATUS_INITIALIZED (1 << 0) /* Ring initialized */
#define RING_STATUS_TX_OVERFLOW (1 << 1) /* TX overflow */
#define RING_STATUS_RX_OVERFLOW (1 << 2) /* RX overflow */
#define RING_STATUS_ERROR (1 << 31)      /* Error */

/**
 * @brief Ring types
 */
enum ring_type {
    RING_CONFIG, /* RX data available */
    RING_DATA,   /* TX buffer available */
    RING_LOOPBACK,
    RING_ERROR, /* Error occurred */
};

/**
 * @brief Ring event callback (with ring_id parameter)
 */
typedef void (*ring_event_callback_t)(uint8_t ring_id, void *user_data);

/**
 * @brief Ring statistics
 */
struct ring_stats {
    uint32_t tx_count;     /* Total TX packets */
    uint32_t rx_count;     /* Total RX packets */
    uint32_t tx_errors;    /* TX errors */
    uint32_t rx_errors;    /* RX errors */
    uint32_t tx_overflows; /* TX overflows */
    uint32_t rx_overflows; /* RX overflows */
};

/* ========== Common API (Both Sides) ========== */

/**
 * @brief Send data through ring buffer
 *
 * @param ring_id Ring ID (0-7)
 * @param data Data to send
 * @param len Data length (max 1500 bytes)
 * @param timeout Timeout
 * @return 0 on success, negative errno on failure
 */
int ring_send(uint8_t ring_id, const uint8_t *data, size_t len, uint32_t timeout);

/**
 * @brief Receive data from ring buffer
 *
 * @param ring_id Ring ID (0-7)
 * @param data Buffer to store received data
 * @param max_len Maximum buffer length
 * @param timeout Timeout
 * @return Number of bytes received, or negative errno on failure
 */
int ring_recv(uint8_t ring_id, uint8_t *data, size_t max_len, uint32_t timeout);

/**
 * @brief Get number of configured rings
 *
 * @param ring_id Ring ID (0-7)
 * @return Number of configured rings
 */
int ring_get_num(void);

/**
 * @brief Get available TX descriptors count
 *
 * @return Number of available TX descriptors
 */
int ring_get_tx_available(uint8_t ring_id);

/**
 * @brief Get available RX descriptors count
 *
 * @param ring_id Ring ID (0-7)
 * @return Number of available RX descriptors
 */
int ring_get_rx_available(uint8_t ring_id);

/**
 * @brief Register event callback
 *
 * @param callback Callback function
 * @param user_data User data passed to callback
 * @return 0 on success, negative errno on failure
 */
int ring_register_callback(ring_event_callback_t callback, void *user_data);

/**
 * @brief Get ring statistics
 *
 * @param ring_id Ring ID (0-7)
 * @param stats Pointer to statistics structure
 * @return 0 on success, negative errno on failure
 */
int ring_get_stats(uint8_t ring_id, struct ring_stats *stats);

/**
 * @brief Deinitialize ring service
 */
void ring_service_deinit(void);

/**
 * @brief Trigger ring service RX processing
 *
 */
void ring_rx_handler(void);

/**
 * @brief init_qring
 */
int init_qring(void);

#endif /* RING_SERVICE_H_ */
