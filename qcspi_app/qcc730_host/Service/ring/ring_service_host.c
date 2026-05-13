/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <stdio.h>
#include <string.h>
#include "../../Port/osal/qc_osal.h"
#include "ring_service.h"
#include "ring_adapter.h"

/* Work queue for deferred processing */
static qc_osal_work_q_t ring_host_work_q;

/* Work item context for each ring */
struct ring_work_context {
    qc_osal_work_t work;
    uint8_t ring_id;
};

/* Work items for RX event processing (one per ring) */
static struct ring_work_context rx_works[MAX_RINGS];

/* Per-ring instance data */
struct ring_instance {
    qc_osal_mutex_t tx_lock;
    qc_osal_mutex_t rx_lock;
    qc_osal_sem_t tx_sem;
    qc_osal_sem_t rx_sem;
    struct ring_stats stats;
};

/* Global ring service state */
static struct {
    bool initialized;
    uint32_t num_rings;
    uint32_t ctrl_block_addr;
    const struct ring_adapter_ops *adapter;

    /* Local cache of control block */
    struct ring_control_block ctrl_cache;

    /* Per-ring instances */
    struct ring_instance rings[MAX_RINGS];

    /* Callback */
    ring_event_callback_t callback;
    void *callback_data;
} g_ring_service;

/* Forward declarations */
static void rx_work_handler(qc_osal_work_t work);

/**
 * @brief ring rx interrupt handler - called when Host Recvs data
 *
 * Polls all rings and submits work items for rings with data
 */
void ring_rx_handler(void)
{
    if (!g_ring_service.initialized) {
        return;
    }

    /*
     * Submit work items for all configured rings.
     * Cannot call ring_get_rx_available() here because it uses mutex locks
     * which are not allowed in interrupt context.
     * The work handler will check which rings actually have data.
     */
    for (uint32_t i = 0; i < g_ring_service.num_rings; i++) {
        /* Signal RX semaphore for this ring */
        qc_osal_sem_give(g_ring_service.rings[i].rx_sem);

        /* Submit work item for this ring */
        if (g_ring_service.callback) {
            qc_osal_work_submit(ring_host_work_q, rx_works[i].work);
            /* NOTE: printf is NOT safe in ISR context - do not add logging here */
        }
    }
}

/**
 * @brief Deinitialize ring service
 */
void ring_service_deinit(void)
{
    if (!g_ring_service.initialized) {
        return;
    }

    g_ring_service.initialized = false;

    /* Clear cached control block to force re-sync on next init */
    memset(&g_ring_service.ctrl_cache, 0, sizeof(g_ring_service.ctrl_cache));

    //    /* Clear callback */
    //    g_ring_service.callback = NULL;
    //    g_ring_service.callback_data = NULL;
    /* NOTE: callback is intentionally preserved across reset so the caller
     * does not need to re-register after AT+RST. */

    /* Free work items for each ring */
    for (uint32_t i = 0; i < g_ring_service.num_rings; i++) {
        qc_osal_work_deinit(&rx_works[i].work);
    }

    /* Stop work queue thread and free its resources */
    qc_osal_work_queue_deinit(&ring_host_work_q);

    /* Free per-ring synchronization objects */
    for (uint32_t i = 0; i < g_ring_service.num_rings; i++) {
        qc_osal_mutex_deinit(&g_ring_service.rings[i].tx_lock);
        qc_osal_mutex_deinit(&g_ring_service.rings[i].rx_lock);
        qc_osal_sem_deinit(&g_ring_service.rings[i].tx_sem);
        qc_osal_sem_deinit(&g_ring_service.rings[i].rx_sem);
    }

    const struct ring_adapter_ops *adapter = NULL;

    /* Get QCSPI adapter */
    adapter = ring_adapter_get_qcspi();
    if (!adapter) {
        QC_OSAL_LOG_ERR("Failed to get QCSPI adapter");
        return;
    }

    /* Initialize QCSPI adapter */
    if (adapter->deinit() < 0) {
        QC_OSAL_LOG_ERR("QCSPI adapter deinitialization failed");
    }

    QC_OSAL_LOG_INF("QCSPI adapter deinitialized successfully");

    /* Reset num_rings last — after all cleanup that depends on it */
    g_ring_service.num_rings = 0;

    QC_OSAL_LOG_INF("Host ring service deinitialized");
}

/**
 * @brief Register event callback
 */
int ring_register_callback(ring_event_callback_t callback, void *user_data)
{
    if (!g_ring_service.initialized) {
        return -QC_OSAL_ENODEV;
    }

    g_ring_service.callback = callback;
    g_ring_service.callback_data = user_data;

    return 0;
}

/**
 * @brief Get ring statistics
 */
int ring_get_stats(uint8_t ring_id, struct ring_stats *stats)
{
    if (!g_ring_service.initialized) {
        return -QC_OSAL_ENODEV;
    }

    if (ring_id >= MAX_RINGS) {
        QC_OSAL_LOG_ERR("Invalid ring_id: %u", ring_id);
        return -QC_OSAL_EINVAL;
    }

    if (!stats) {
        return -QC_OSAL_EINVAL;
    }

    memcpy(stats, &g_ring_service.rings[ring_id].stats, sizeof(*stats));

    return 0;
}

/**
 * @brief Get available ring count
 */
int ring_get_num(void)
{
    if (!g_ring_service.initialized) {
        return -QC_OSAL_ENODEV;
    }

    return g_ring_service.num_rings;
}

/**
 * @brief Work handler for RX event processing (deferred from interrupt context)
 *
 * Runs in thread context where blocking operations (mutex, SPI read) are allowed.
 * Checks if the ring actually has data before invoking the callback.
 */
static void rx_work_handler(qc_osal_work_t work)
{
    /* Find which ring this work item belongs to by comparing work pointers */
    struct ring_work_context *ctx = NULL;

    for (uint32_t i = 0; i < g_ring_service.num_rings; i++) {
        if (rx_works[i].work == work) {
            ctx = &rx_works[i];
            break;
        }
    }

    if (!ctx) {
        QC_OSAL_LOG_ERR("rx_work_handler: failed to find ring context for work %p", work);
        return;
    }

    /*
     * Check if this ring actually has data available.
     * This is safe here because we're in thread context, not ISR context.
     */
    int rx_avail = ring_get_rx_available(ctx->ring_id);
    if (rx_avail <= 0) {
        /* No data available for this ring, skip callback */
        QC_OSAL_LOG_DBG("rx_work_handler: ring %d has no data (avail=%d)", ctx->ring_id, rx_avail);
        return;
    }

    QC_OSAL_LOG_DBG("rx_work_handler: ring %d has %d descriptors available", ctx->ring_id, rx_avail);

    /* Call the registered callback in thread context with ring_id */
    if (g_ring_service.callback) {
        g_ring_service.callback(ctx->ring_id, g_ring_service.callback_data);
    }
}

/**
 * @brief Get available TX descriptors count
 */
int ring_get_tx_available(uint8_t ring_id)
{
    uint32_t wr_idx, rd_idx, desc_count;
    int ret;

    if (!g_ring_service.initialized) {
        return -QC_OSAL_ENODEV;
    }

    if (ring_id >= MAX_RINGS) {
        QC_OSAL_LOG_ERR("Invalid ring_id: %u", ring_id);
        return -QC_OSAL_EINVAL;
    }

    if (g_ring_service.ctrl_cache.ring_status[ring_id] != RING_STATUS_VALID) {
        QC_OSAL_LOG_ERR("Unconfigure ring_id: %u", ring_id);
        return -QC_OSAL_EINVAL;
    }

    /* Lock to ensure consistent read of cached values */
    ret = qc_osal_mutex_lock(g_ring_service.rings[ring_id].tx_lock, QC_OSAL_TIMEOUT_FOREVER);
    if (ret < 0) {
        return ret;
    }

    /* Read remote read index */
    uint32_t rd_idx_addr =
        g_ring_service.ctrl_block_addr + offsetof(struct ring_control_block, tx_rd_idx) + ring_id * sizeof(uint32_t);
    ret = g_ring_service.adapter->mem_read(rd_idx_addr, &rd_idx, sizeof(rd_idx));
    if (ret < 0) {
        qc_osal_mutex_unlock(g_ring_service.rings[ring_id].tx_lock);
        return ret;
    }

    wr_idx = g_ring_service.ctrl_cache.tx_wr_idx[ring_id];
    desc_count = g_ring_service.ctrl_cache.tx_desc_count[ring_id];

    qc_osal_mutex_unlock(g_ring_service.rings[ring_id].tx_lock);

    /* Calculate available space */
    if (rd_idx > wr_idx) {
        return rd_idx - wr_idx - 1;
    } else {
        return desc_count - (wr_idx - rd_idx) - 1;
    }
}

/**
 * @brief Get available RX descriptors count
 */
int ring_get_rx_available(uint8_t ring_id)
{
    uint32_t wr_idx, rd_idx, desc_count;
    int ret;

    if (!g_ring_service.initialized) {
        return -QC_OSAL_ENODEV;
    }

    if (ring_id >= MAX_RINGS) {
        QC_OSAL_LOG_ERR("Invalid ring_id: %u", ring_id);
        return -QC_OSAL_EINVAL;
    }

    if (g_ring_service.ctrl_cache.ring_status[ring_id] != RING_STATUS_VALID) {
        QC_OSAL_LOG_ERR("Unconfigure ring_id: %u", ring_id);
        return -QC_OSAL_EINVAL;
    }

    /* Lock to ensure consistent read of cached values */
    ret = qc_osal_mutex_lock(g_ring_service.rings[ring_id].rx_lock, QC_OSAL_TIMEOUT_FOREVER);
    if (ret < 0) {
        return ret;
    }

    /* Read remote write index */
    uint32_t wr_idx_addr =
        g_ring_service.ctrl_block_addr + offsetof(struct ring_control_block, rx_wr_idx) + ring_id * sizeof(uint32_t);
    ret = g_ring_service.adapter->mem_read(wr_idx_addr, &wr_idx, sizeof(wr_idx));
    if (ret < 0) {
        qc_osal_mutex_unlock(g_ring_service.rings[ring_id].rx_lock);
        return ret;
    }

    rd_idx = g_ring_service.ctrl_cache.rx_rd_idx[ring_id];
    desc_count = g_ring_service.ctrl_cache.rx_desc_count[ring_id];

    qc_osal_mutex_unlock(g_ring_service.rings[ring_id].rx_lock);

    /* Calculate available data */
    if (wr_idx >= rd_idx) {
        return wr_idx - rd_idx;
    } else {
        return desc_count - (rd_idx - wr_idx);
    }
}

/**
 * @brief Send data through ring buffer (Host→Slave)
 *
 * For Host, sending means writing to TX ring
 */
int ring_send(uint8_t ring_id, const uint8_t *data, size_t len, uint32_t timeout)
{
    struct ring_descriptor desc;
    uint32_t wr_idx, rd_idx, next_wr_idx;
    uint32_t avail;
    uint32_t desc_addr, buf_addr, wr_idx_addr, rd_idx_addr;
    int ret;

    if (!g_ring_service.initialized) {
        QC_OSAL_LOG_ERR("Ring service not initialized");
        return -QC_OSAL_ENODEV;
    }

    if (ring_id >= MAX_RINGS) {
        QC_OSAL_LOG_ERR("Invalid ring_id: %u", ring_id);
        return -QC_OSAL_EINVAL;
    }

    if (g_ring_service.ctrl_cache.ring_status[ring_id] != RING_STATUS_VALID) {
        QC_OSAL_LOG_ERR("Unconfigure ring_id: %u", ring_id);
        return -QC_OSAL_EINVAL;
    }

    if (!data || len == 0) {
        QC_OSAL_LOG_ERR("Invalid parameters");
        return -QC_OSAL_EINVAL;
    }

    if (len > g_ring_service.ctrl_cache.tx_buf_size[ring_id]) {
        QC_OSAL_LOG_ERR("Data too large: %zu > %u", len, g_ring_service.ctrl_cache.tx_buf_size[ring_id]);
        return -QC_OSAL_EMSGSIZE;
    }

    /* Lock TX */
    ret = qc_osal_mutex_lock(g_ring_service.rings[ring_id].tx_lock, timeout);
    if (ret < 0) {
        QC_OSAL_LOG_ERR("Failed to lock TX mutex: %d", ret);
        return ret;
    }

    /* Get local read index */
    rd_idx = g_ring_service.ctrl_cache.tx_rd_idx[ring_id];
    /* Get local write index */
    wr_idx = g_ring_service.ctrl_cache.tx_wr_idx[ring_id];

    /* Calculate available space */
    if (rd_idx > wr_idx) {
        avail = rd_idx - wr_idx - 1;
    } else {
        avail = g_ring_service.ctrl_cache.tx_desc_count[ring_id] - (wr_idx - rd_idx) - 1;
    }

    if (avail == 0) {
        /* Read remote read index (updated by Slave) */
        rd_idx_addr = g_ring_service.ctrl_block_addr + offsetof(struct ring_control_block, tx_rd_idx) +
                      ring_id * sizeof(uint32_t);
        ret = g_ring_service.adapter->mem_read(rd_idx_addr, &rd_idx, sizeof(rd_idx));
        if (ret < 0) {
            QC_OSAL_LOG_ERR("Failed to read remote rd_idx: %d", ret);
            qc_osal_mutex_unlock(g_ring_service.rings[ring_id].tx_lock);
            return ret;
        }
        g_ring_service.ctrl_cache.tx_rd_idx[ring_id] = rd_idx;
    }

    /* Recalculate available space */
    if (rd_idx > wr_idx) {
        avail = rd_idx - wr_idx - 1;
    } else {
        avail = g_ring_service.ctrl_cache.tx_desc_count[ring_id] - (wr_idx - rd_idx) - 1;
    }

    /* Check if space available */
    if (avail == 0) {
        qc_osal_mutex_unlock(g_ring_service.rings[ring_id].tx_lock);
        return -QC_OSAL_EAGAIN; /* No space available, return immediately */
    }

    /* Calculate buffer address */

    buf_addr = g_ring_service.ctrl_cache.tx_buf_base[ring_id] + wr_idx * g_ring_service.ctrl_cache.tx_buf_size[ring_id];

    /* Write data to remote buffer */
    ret = g_ring_service.adapter->mem_write(buf_addr, data, len);
    if (ret < 0) {
        QC_OSAL_LOG_ERR("Failed to write data: %d", ret);
        qc_osal_mutex_unlock(g_ring_service.rings[ring_id].tx_lock);
        return ret;
    }

    /* Update descriptor */
    desc.buffer_addr = buf_addr;
    desc.length = len;
    desc.flags = RING_DESC_FLAG_VALID;

    /* Calculate descriptor address */
    desc_addr = g_ring_service.ctrl_cache.tx_desc_base[ring_id] + wr_idx * sizeof(struct ring_descriptor);

    /* Write descriptor back */
    ret = g_ring_service.adapter->mem_write(desc_addr, &desc, sizeof(desc));
    if (ret < 0) {
        QC_OSAL_LOG_ERR("Failed to write descriptor: %d", ret);
        qc_osal_mutex_unlock(g_ring_service.rings[ring_id].tx_lock);
        return ret;
    }

    /* Update write index */
    next_wr_idx = (wr_idx + 1) % g_ring_service.ctrl_cache.tx_desc_count[ring_id];
    g_ring_service.ctrl_cache.tx_wr_idx[ring_id] = next_wr_idx;

    /* Write back remote write index */
    wr_idx_addr =
        g_ring_service.ctrl_block_addr + offsetof(struct ring_control_block, tx_wr_idx) + ring_id * sizeof(uint32_t);
    ret = g_ring_service.adapter->mem_write(wr_idx_addr, &next_wr_idx, sizeof(next_wr_idx));
    if (ret < 0) {
        QC_OSAL_LOG_ERR("Failed to write wr_idx: %d", ret);
        qc_osal_mutex_unlock(g_ring_service.rings[ring_id].tx_lock);
        return ret;
    }

    /* Update statistics */
    g_ring_service.rings[ring_id].stats.tx_count++;

    qc_osal_mutex_unlock(g_ring_service.rings[ring_id].tx_lock);

    /* Trigger Slave interrupt */
    ret = g_ring_service.adapter->notify_tx_done();
    if (ret < 0) {
        QC_OSAL_LOG_ERR("Failed to trigger interrupt: %d", ret);
    }

    QC_OSAL_LOG_DBG("Sent %zu bytes on ring %u (wr_idx: %u -> %u)", len, ring_id, wr_idx, next_wr_idx);

    return 0;
}

/**
 * @brief Receive data from ring buffer (Slave→Host)
 *
 * For Host, receiving means reading from RX ring
 */
int ring_recv(uint8_t ring_id, uint8_t *data, size_t max_len, uint32_t timeout)
{
    struct ring_descriptor desc;
    uint32_t wr_idx, rd_idx, next_rd_idx;
    uint32_t avail;
    uint32_t desc_addr, buf_addr, wr_idx_addr, rd_idx_addr;
    uint16_t data_len;
    int ret;

    if (!g_ring_service.initialized) {
        QC_OSAL_LOG_ERR("Ring service not initialized");
        return -QC_OSAL_ENODEV;
    }

    if (ring_id >= MAX_RINGS) {
        QC_OSAL_LOG_ERR("Invalid ring_id: %u", ring_id);
        return -QC_OSAL_EINVAL;
    }

    if (g_ring_service.ctrl_cache.ring_status[ring_id] != RING_STATUS_VALID) {
        QC_OSAL_LOG_ERR("Unconfigure ring_id: %u", ring_id);
        return -QC_OSAL_EINVAL;
    }

    if (!data || max_len == 0) {
        QC_OSAL_LOG_ERR("Invalid parameters");
        return -QC_OSAL_EINVAL;
    }

    /* Lock RX */
    ret = qc_osal_mutex_lock(g_ring_service.rings[ring_id].rx_lock, timeout);
    if (ret < 0) {
        QC_OSAL_LOG_ERR("Failed to lock RX mutex: %d", ret);
        return ret;
    }

    /* Read remote write index (updated by Slave) */
    wr_idx_addr =
        g_ring_service.ctrl_block_addr + offsetof(struct ring_control_block, rx_wr_idx) + ring_id * sizeof(uint32_t);
    ret = g_ring_service.adapter->mem_read(wr_idx_addr, &wr_idx, sizeof(wr_idx));
    if (ret < 0) {
        QC_OSAL_LOG_ERR("Failed to read remote wr_idx: %d", ret);
        qc_osal_mutex_unlock(g_ring_service.rings[ring_id].rx_lock);
        return ret;
    }

    /* Get local read index */
    rd_idx = g_ring_service.ctrl_cache.rx_rd_idx[ring_id];

    /* Calculate available data */
    if (wr_idx >= rd_idx) {
        avail = wr_idx - rd_idx;
    } else {
        avail = g_ring_service.ctrl_cache.rx_desc_count[ring_id] - (rd_idx - wr_idx);
    }

    /* Check if data available */
    if (avail == 0) {
        if (timeout == QC_OSAL_TIMEOUT_NO_WAIT) {
            qc_osal_mutex_unlock(g_ring_service.rings[ring_id].rx_lock);
            return 0;
        }

        /* Wait for data */
        qc_osal_mutex_unlock(g_ring_service.rings[ring_id].rx_lock);
        ret = qc_osal_sem_take(g_ring_service.rings[ring_id].rx_sem, timeout);
        if (ret < 0) {
            QC_OSAL_LOG_WRN("Timeout waiting for RX data");
            return 0;
        }

        /* Re-acquire lock and re-read indices */
        ret = qc_osal_mutex_lock(g_ring_service.rings[ring_id].rx_lock, QC_OSAL_TIMEOUT_FOREVER);
        if (ret < 0) {
            return ret;
        }

        ret = g_ring_service.adapter->mem_read(wr_idx_addr, &wr_idx, sizeof(wr_idx));
        if (ret < 0) {
            qc_osal_mutex_unlock(g_ring_service.rings[ring_id].rx_lock);
            return ret;
        }

        rd_idx = g_ring_service.ctrl_cache.rx_rd_idx[ring_id];
    }

    /* Calculate descriptor address */
    desc_addr = g_ring_service.ctrl_cache.rx_desc_base[ring_id] + rd_idx * sizeof(struct ring_descriptor);

    /* Read descriptor */
    ret = g_ring_service.adapter->mem_read(desc_addr, &desc, sizeof(desc));
    if (ret < 0) {
        QC_OSAL_LOG_ERR("Failed to read descriptor: %d", ret);
        qc_osal_mutex_unlock(g_ring_service.rings[ring_id].rx_lock);
        return ret;
    }

    /* Check descriptor validity */
    if (!(desc.flags & RING_DESC_FLAG_VALID)) {
        QC_OSAL_LOG_ERR("Invalid descriptor at index %u", rd_idx);
        g_ring_service.rings[ring_id].stats.rx_errors++;
        qc_osal_mutex_unlock(g_ring_service.rings[ring_id].rx_lock);
        return -QC_OSAL_EINVAL;
    }

    /* Get data length */
    data_len = desc.length;
    if (data_len > max_len) {
        QC_OSAL_LOG_ERR("Buffer too small: %u > %u", data_len, max_len);
        g_ring_service.rings[ring_id].stats.rx_errors++;
        qc_osal_mutex_unlock(g_ring_service.rings[ring_id].rx_lock);
        return -QC_OSAL_EMSGSIZE;
    }

    /* Get buffer address */
    buf_addr = desc.buffer_addr;

    /* Read data from remote buffer */
    ret = g_ring_service.adapter->mem_read(buf_addr, data, data_len);
    if (ret < 0) {
        QC_OSAL_LOG_ERR("Failed to read data: %d", ret);
        qc_osal_mutex_unlock(g_ring_service.rings[ring_id].rx_lock);
        return ret;
    }

    /* Clear descriptor */
    desc.flags = 0;
    desc.length = 0;

    /* Write descriptor back */
    ret = g_ring_service.adapter->mem_write(desc_addr, &desc, sizeof(desc));
    if (ret < 0) {
        QC_OSAL_LOG_ERR("Failed to write descriptor: %d", ret);
        qc_osal_mutex_unlock(g_ring_service.rings[ring_id].rx_lock);
        return ret;
    }

    /* Update read index */
    next_rd_idx = (rd_idx + 1) % g_ring_service.ctrl_cache.rx_desc_count[ring_id];
    g_ring_service.ctrl_cache.rx_rd_idx[ring_id] = next_rd_idx;

    /* Write back remote read index */
    rd_idx_addr =
        g_ring_service.ctrl_block_addr + offsetof(struct ring_control_block, rx_rd_idx) + ring_id * sizeof(uint32_t);
    ret = g_ring_service.adapter->mem_write(rd_idx_addr, &next_rd_idx, sizeof(next_rd_idx));
    if (ret < 0) {
        QC_OSAL_LOG_ERR("Failed to write rd_idx: %d", ret);
        qc_osal_mutex_unlock(g_ring_service.rings[ring_id].rx_lock);
        return ret;
    }

    /* Update statistics */
    g_ring_service.rings[ring_id].stats.rx_count++;

    qc_osal_mutex_unlock(g_ring_service.rings[ring_id].rx_lock);

    /* Notify slave that host has read data (HOST_INT1) */
    if (g_ring_service.adapter->notify_rx_done) {
        g_ring_service.adapter->notify_rx_done();
    }

    return data_len;
}

/**
 * @brief Host ring service initialization
 */
static int ring_service_host_init(uint32_t ctrl_block_addr, const struct ring_adapter_ops *adapter)
{
    struct ring_control_block ctrl;
    int ret;

    if (g_ring_service.initialized) {
        QC_OSAL_LOG_WRN("Ring service already initialized");
        return -QC_OSAL_EALREADY;
    }

    QC_OSAL_LOG_INF("Initializing host ring service");
    QC_OSAL_LOG_INF("  Control block addr: 0x%08x", ctrl_block_addr);

    /* Validate parameters */
    if (!adapter) {
        QC_OSAL_LOG_ERR("Invalid adapter");
        return -QC_OSAL_EINVAL;
    }

    /* 1. Read control block from Slave */
    ret = adapter->mem_read(ctrl_block_addr, &ctrl, sizeof(ctrl));
    if (ret < 0) {
        QC_OSAL_LOG_ERR("Failed to read control block: %d", ret);
        return ret;
    }

    /* 2. Validate magic number */
    if (ctrl.magic != RING_MAGIC_NUMBER) {
        QC_OSAL_LOG_ERR("Invalid magic number: 0x%08x (expected 0x%08x)", ctrl.magic, RING_MAGIC_NUMBER);
        return -QC_OSAL_EINVAL;
    }

    /* 3. Check version */
    if (ctrl.version != RING_VERSION) {
        QC_OSAL_LOG_WRN("Version mismatch: 0x%08x (expected 0x%08x)", ctrl.version, RING_VERSION);
    }

    /* 4. Check status */
    if (!(ctrl.status & RING_STATUS_INITIALIZED)) {
        QC_OSAL_LOG_ERR("Ring not initialized on Slave side");
        return -QC_OSAL_ENODEV;
    }

    /* 5. Save configuration */
    g_ring_service.ctrl_block_addr = ctrl_block_addr;
    g_ring_service.adapter = adapter;
    g_ring_service.num_rings = ctrl.num_rings;
    memcpy(&g_ring_service.ctrl_cache, &ctrl, sizeof(ctrl));

    QC_OSAL_LOG_INF("Number of rings: %u", ctrl.num_rings);

    /* 6. Initialize synchronization objects for each ring */
    for (uint32_t i = 0; i < ctrl.num_rings; i++) {
        ret = qc_osal_mutex_init(&g_ring_service.rings[i].tx_lock);
        if (ret < 0) {
            QC_OSAL_LOG_ERR("Failed to initialize TX mutex for ring %u: %d", i, ret);
            return ret;
        }

        ret = qc_osal_mutex_init(&g_ring_service.rings[i].rx_lock);
        if (ret < 0) {
            QC_OSAL_LOG_ERR("Failed to initialize RX mutex for ring %u: %d", i, ret);
            return ret;
        }

        ret = qc_osal_sem_init(&g_ring_service.rings[i].tx_sem, 0, ctrl.tx_desc_count[i]);
        if (ret < 0) {
            QC_OSAL_LOG_ERR("Failed to initialize TX semaphore for ring %u: %d", i, ret);
            return ret;
        }

        ret = qc_osal_sem_init(&g_ring_service.rings[i].rx_sem, 0, ctrl.rx_desc_count[i]);
        if (ret < 0) {
            QC_OSAL_LOG_ERR("Failed to initialize RX semaphore for ring %u: %d", i, ret);
            return ret;
        }

        /* Initialize statistics */
        memset(&g_ring_service.rings[i].stats, 0, sizeof(g_ring_service.rings[i].stats));

        QC_OSAL_LOG_INF("Ring %u initialized: TX desc=%u, RX desc=%u", i, ctrl.tx_desc_count[i], ctrl.rx_desc_count[i]);
    }

    /* 7. Initialize work queue and work item */
    ret = qc_osal_work_queue_init(&ring_host_work_q, 2048, 6); /* Stack size 2048, priority 6 */
    if (ret < 0) {
        QC_OSAL_LOG_ERR("Failed to initialize work queue: %d", ret);
        return ret;
    }

    /* 8. Initialize work items for each ring */
    for (uint32_t i = 0; i < ctrl.num_rings; i++) {
        rx_works[i].ring_id = i;
        ret = qc_osal_work_init(&rx_works[i].work, rx_work_handler);
        if (ret < 0) {
            QC_OSAL_LOG_ERR("Failed to initialize work item for ring %u: %d", i, ret);
            return ret;
        }
    }

    g_ring_service.initialized = true;

    QC_OSAL_LOG_INF("Host ring service initialized successfully");
    for (uint32_t i = 0; i < ctrl.num_rings; i++) {
        QC_OSAL_LOG_INF("  Ring %u: TX desc=0x%08x (count=%u), RX desc=0x%08x (count=%u)", i, ctrl.tx_desc_base[i],
                        ctrl.tx_desc_count[i], ctrl.rx_desc_base[i], ctrl.rx_desc_count[i]);
    }

    return 0;
}

int init_qring(void)
{
    int ret = 0;
    const struct ring_adapter_ops *adapter = NULL;

    /* Get QCSPI adapter */
    adapter = ring_adapter_get_qcspi();
    if (!adapter) {
        QC_OSAL_LOG_ERR("Failed to get QCSPI adapter");
        return -QC_OSAL_ENODEV;
    }

    /* Initialize QCSPI adapter */
    ret = adapter->init();
    if (ret < 0) {
        QC_OSAL_LOG_ERR("QCSPI adapter initialization failed: %d", ret);
        return ret;
    }

    QC_OSAL_LOG_INF("QCSPI adapter initialized successfully");

    /* Initialize ring service */
    ret = ring_service_host_init(CONFIG_RING_CTRL_BLOCK_ADDR, adapter);
    if (ret == 0) {
        QC_OSAL_LOG_INF("Ring service initialized successfully");
    } else {
        QC_OSAL_LOG_ERR("Ring service initialization failed: %d", ret);
    }

    return ret;
}
