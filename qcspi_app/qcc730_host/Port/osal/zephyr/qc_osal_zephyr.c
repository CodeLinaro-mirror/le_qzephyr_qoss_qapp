/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
#include "../../qc_port_config.h"

#if defined(QC_OS_ZEPHYR)
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <errno.h>
#include "../qc_osal.h"

LOG_MODULE_REGISTER(qc_osal, LOG_LEVEL_INF);

/**
 * @brief Convert Zephyr errno to OSAL error code
 * @param zephyr_errno Zephyr error code (positive value)
 * @return Negative OSAL error code
 */
static inline int zephyr_errno_to_osal(int zephyr_errno)
{
    if (zephyr_errno == 0) {
        return 0;
    }

    /* Map common Zephyr errno values to OSAL error codes */
    switch (zephyr_errno) {
    case EINVAL:
        return -QC_OSAL_EINVAL;
    case ENOMEM:
        return -QC_OSAL_ENOMEM;
    case ENODEV:
        return -QC_OSAL_ENODEV;
    case EAGAIN:
        return -QC_OSAL_EAGAIN;
    case ETIMEDOUT:
        return -QC_OSAL_ETIMEDOUT;
    case EMSGSIZE:
        return -QC_OSAL_EMSGSIZE;
    case EALREADY:
        return -QC_OSAL_EALREADY;
    case E2BIG:
        return -QC_OSAL_E2BIG;
    case EIO:
        return -QC_OSAL_EIO;
    case EBUSY:
        return -QC_OSAL_EBUSY;
    case ENOTSUP:
        return -QC_OSAL_ENOTSUP;
    default:
        /* For unmapped errors, return generic I/O error */
        return -QC_OSAL_EIO;
    }
}

/**
 * @brief Convert OSAL error code to Zephyr errno
 * @param osal_error OSAL error code (negative value)
 * @return Positive Zephyr errno value
 */
static inline int osal_to_zephyr_errno(int osal_error)
{
    if (osal_error == 0) {
        return 0;
    }

    /* Remove negative sign and map to Zephyr errno */
    switch (-osal_error) {
    case QC_OSAL_EINVAL:
        return EINVAL;
    case QC_OSAL_ENOMEM:
        return ENOMEM;
    case QC_OSAL_ENODEV:
        return ENODEV;
    case QC_OSAL_EAGAIN:
        return EAGAIN;
    case QC_OSAL_ETIMEDOUT:
        return ETIMEDOUT;
    case QC_OSAL_EMSGSIZE:
        return EMSGSIZE;
    case QC_OSAL_EALREADY:
        return EALREADY;
    case QC_OSAL_E2BIG:
        return E2BIG;
    case QC_OSAL_EIO:
        return EIO;
    case QC_OSAL_EBUSY:
        return EBUSY;
    case QC_OSAL_ENOTSUP:
        return ENOTSUP;
    default:
        return EIO;
    }
}

/* ============================================================================
 * Mutex Operations
 * ============================================================================ */

int qc_osal_mutex_init(qc_osal_mutex_t *mutex)
{
    int ret;
    struct k_mutex *kmutex;

    if (!mutex) {
        return -QC_OSAL_EINVAL;
    }

    kmutex = k_malloc(sizeof(struct k_mutex));
    if (!kmutex) {
        return -QC_OSAL_ENOMEM;
    }

    ret = k_mutex_init(kmutex);
    if (ret < 0) {
        k_free(kmutex);
        return zephyr_errno_to_osal(-ret);
    }

    *mutex = (qc_osal_mutex_t)kmutex;
    return 0;
}

int qc_osal_mutex_deinit(qc_osal_mutex_t *mutex)
{
    if (!mutex || !*mutex) {
        return -QC_OSAL_EINVAL;
    }

    k_free(*mutex);
    *mutex = NULL;
    return 0;
}

int qc_osal_mutex_lock(qc_osal_mutex_t mutex, int32_t timeout_ms)
{
    struct k_mutex *kmutex = (struct k_mutex *)mutex;
    k_timeout_t timeout;
    int ret;

    if (!kmutex) {
        return -QC_OSAL_EINVAL;
    }

    if (timeout_ms < 0) {
        timeout = K_FOREVER;
    } else if (timeout_ms == 0) {
        timeout = K_NO_WAIT;
    } else {
        timeout = K_MSEC(timeout_ms);
    }

    ret = k_mutex_lock(kmutex, timeout);
    if (ret < 0) {
        return zephyr_errno_to_osal(-ret);
    }
    return 0;
}

int qc_osal_mutex_unlock(qc_osal_mutex_t mutex)
{
    struct k_mutex *kmutex = (struct k_mutex *)mutex;
    int ret;

    if (!kmutex) {
        return -QC_OSAL_EINVAL;
    }

    ret = k_mutex_unlock(kmutex);
    if (ret < 0) {
        return zephyr_errno_to_osal(-ret);
    }
    return 0;
}

/* ============================================================================
 * Semaphore Operations
 * ============================================================================ */

int qc_osal_sem_init(qc_osal_sem_t *sem, uint32_t initial_count, uint32_t max_count)
{
    struct k_sem *ksem;
    int ret;

    if (!sem) {
        return -QC_OSAL_EINVAL;
    }

    ksem = k_malloc(sizeof(struct k_sem));
    if (!ksem) {
        return -QC_OSAL_ENOMEM;
    }

    ret = k_sem_init(ksem, initial_count, max_count);
    if (ret < 0) {
        k_free(ksem);
        return zephyr_errno_to_osal(-ret);
    }

    *sem = (qc_osal_sem_t)ksem;
    return 0;
}

int qc_osal_sem_deinit(qc_osal_sem_t *sem)
{
    if (!sem || !*sem) {
        return -QC_OSAL_EINVAL;
    }

    k_free(*sem);
    *sem = NULL;
    return 0;
}

int qc_osal_sem_take(qc_osal_sem_t sem, int32_t timeout_ms)
{
    struct k_sem *ksem = (struct k_sem *)sem;
    k_timeout_t timeout;
    int ret;

    if (!ksem) {
        return -QC_OSAL_EINVAL;
    }

    if (timeout_ms < 0) {
        timeout = K_FOREVER;
    } else if (timeout_ms == 0) {
        timeout = K_NO_WAIT;
    } else {
        timeout = K_MSEC(timeout_ms);
    }

    ret = k_sem_take(ksem, timeout);
    if (ret < 0) {
        return zephyr_errno_to_osal(-ret);
    }
    return 0;
}

int qc_osal_sem_give(qc_osal_sem_t sem)
{
    struct k_sem *ksem = (struct k_sem *)sem;

    if (!ksem) {
        return -QC_OSAL_EINVAL;
    }

    k_sem_give(ksem);
    return 0;
}

/* ============================================================================
 * Time/Delay Operations
 * ============================================================================ */

void qc_osal_msleep(uint32_t ms) { k_msleep(ms); }

void qc_osal_usleep(uint32_t us) { k_usleep(us); }

uint32_t qc_osal_uptime_get_ms(void) { return k_uptime_get_32(); }

/* ============================================================================
 * Logging Operations
 * ============================================================================ */

void qc_osal_log(qc_osal_log_level_t level, const char *fmt, ...)
{
    va_list args;
    char buf[256];

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    switch (level) {
    case QC_OSAL_LOG_LEVEL_ERR:
        LOG_ERR("%s", buf);
        break;
    case QC_OSAL_LOG_LEVEL_WRN:
        LOG_WRN("%s", buf);
        break;
    case QC_OSAL_LOG_LEVEL_INF:
        LOG_INF("%s", buf);
        break;
    case QC_OSAL_LOG_LEVEL_DBG:
        LOG_DBG("%s", buf);
        break;
    default:
        break;
    }
}

/* ============================================================================
 * Memory Operations
 * ============================================================================ */

void *qc_osal_malloc(size_t size) { return k_malloc(size); }

void qc_osal_free(void *ptr) { k_free(ptr); }

/* ============================================================================
 * Message Queue Operations
 * ============================================================================ */

/* Queue wrapper structure */
struct qc_osal_queue_wrapper {
    struct k_msgq msgq;
    char *buffer;
    size_t msg_size;
    uint32_t max_msgs;
};

int qc_osal_queue_init(qc_osal_queue_t *queue, uint32_t max_msgs, size_t msg_size)
{
    struct qc_osal_queue_wrapper *wrapper;

    if (!queue || max_msgs == 0 || msg_size == 0) {
        return -QC_OSAL_EINVAL;
    }

    /* Allocate wrapper structure */
    wrapper = k_malloc(sizeof(struct qc_osal_queue_wrapper));
    if (!wrapper) {
        return -QC_OSAL_ENOMEM;
    }

    /* Allocate buffer for queue messages */
    wrapper->buffer = k_malloc(max_msgs * msg_size);
    if (!wrapper->buffer) {
        k_free(wrapper);
        return -QC_OSAL_ENOMEM;
    }

    wrapper->msg_size = msg_size;
    wrapper->max_msgs = max_msgs;

    /* Initialize Zephyr message queue */
    k_msgq_init(&wrapper->msgq, wrapper->buffer, msg_size, max_msgs);

    *queue = (qc_osal_queue_t)wrapper;
    return 0;
}

int qc_osal_queue_send(qc_osal_queue_t queue, const void *msg, int32_t timeout_ms)
{
    struct qc_osal_queue_wrapper *wrapper = (struct qc_osal_queue_wrapper *)queue;
    k_timeout_t timeout;
    int ret;

    if (!wrapper || !msg) {
        return -QC_OSAL_EINVAL;
    }

    /* Convert timeout */
    if (timeout_ms < 0) {
        timeout = K_FOREVER;
    } else if (timeout_ms == 0) {
        timeout = K_NO_WAIT;
    } else {
        timeout = K_MSEC(timeout_ms);
    }

    /* Send to queue */
    ret = k_msgq_put(&wrapper->msgq, msg, timeout);
    if (ret != 0) {
        return -QC_OSAL_ETIMEDOUT;
    }

    return 0;
}

int qc_osal_queue_recv(qc_osal_queue_t queue, void *msg, int32_t timeout_ms)
{
    struct qc_osal_queue_wrapper *wrapper = (struct qc_osal_queue_wrapper *)queue;
    k_timeout_t timeout;
    int ret;

    if (!wrapper || !msg) {
        return -QC_OSAL_EINVAL;
    }

    /* Convert timeout */
    if (timeout_ms < 0) {
        timeout = K_FOREVER;
    } else if (timeout_ms == 0) {
        timeout = K_NO_WAIT;
    } else {
        timeout = K_MSEC(timeout_ms);
    }

    /* Receive from queue */
    ret = k_msgq_get(&wrapper->msgq, msg, timeout);
    if (ret != 0) {
        return -QC_OSAL_ETIMEDOUT;
    }

    return 0;
}

int qc_osal_queue_get_count(qc_osal_queue_t queue)
{
    struct qc_osal_queue_wrapper *wrapper = (struct qc_osal_queue_wrapper *)queue;

    if (!wrapper) {
        return -QC_OSAL_EINVAL;
    }

    return (int)k_msgq_num_used_get(&wrapper->msgq);
}

int qc_osal_queue_send_from_isr(qc_osal_queue_t queue, const void *msg)
{
    struct qc_osal_queue_wrapper *wrapper = (struct qc_osal_queue_wrapper *)queue;
    int ret;

    if (!wrapper || !msg) {
        return -QC_OSAL_EINVAL;
    }

    /* In Zephyr, k_msgq_put can be called from ISR context with K_NO_WAIT */
    ret = k_msgq_put(&wrapper->msgq, msg, K_NO_WAIT);
    if (ret != 0) {
        return -QC_OSAL_EAGAIN;
    }

    return 0;
}

/* ============================================================================
 * Thread Operations
 * ============================================================================ */

/* Thread wrapper structure */
struct qc_osal_thread_wrapper {
    struct k_thread thread;
    k_thread_stack_t *stack;
    qc_osal_thread_entry_t user_entry;
    void *user_arg;
};

/* Thread entry adapter - converts OSAL entry to Zephyr entry */
static void thread_entry_adapter(void *p1, void *p2, void *p3)
{
    struct qc_osal_thread_wrapper *wrapper = (struct qc_osal_thread_wrapper *)p1;

    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    if (wrapper && wrapper->user_entry) {
        wrapper->user_entry(wrapper->user_arg);
    }
}

int qc_osal_thread_create(qc_osal_thread_t *thread, const struct qc_osal_thread_config *config)
{
    struct qc_osal_thread_wrapper *wrapper;
    k_tid_t tid;

    if (!thread || !config || !config->entry) {
        return -QC_OSAL_EINVAL;
    }

    if (config->stack_size == 0) {
        return -QC_OSAL_EINVAL;
    }

    wrapper = k_malloc(sizeof(struct qc_osal_thread_wrapper));
    if (!wrapper) {
        return -QC_OSAL_ENOMEM;
    }

    /* Allocate stack */
    wrapper->stack = k_malloc(config->stack_size);
    if (!wrapper->stack) {
        k_free(wrapper);
        return -QC_OSAL_ENOMEM;
    }

    wrapper->user_entry = config->entry;
    wrapper->user_arg = config->arg;

    /* Create and start thread */
    tid = k_thread_create(&wrapper->thread, wrapper->stack, config->stack_size, thread_entry_adapter, wrapper, NULL,
                          NULL, config->priority, 0, K_NO_WAIT);

    if (!tid) {
        k_free(wrapper->stack);
        k_free(wrapper);
        return -QC_OSAL_ENOMEM;
    }

    /* Set thread name if provided */
    if (config->name) {
        k_thread_name_set(tid, config->name);
    }

    *thread = (qc_osal_thread_t)wrapper;
    return 0;
}

int qc_osal_thread_delete(qc_osal_thread_t thread)
{
    struct qc_osal_thread_wrapper *wrapper = (struct qc_osal_thread_wrapper *)thread;

    if (!wrapper) {
        return -QC_OSAL_EINVAL;
    }

    k_thread_abort(&wrapper->thread);

    /* Free resources */
    k_free(wrapper->stack);
    k_free(wrapper);

    return 0;
}

void qc_osal_thread_yield(void) { k_yield(); }

/* ============================================================================
 * Work Queue Operations
 * ============================================================================ */

/* Wrapper structure for work item to store handler and Zephyr work */
struct qc_osal_work_wrapper {
    struct k_work zephyr_work;
    qc_osal_work_handler_t user_handler;
};

/* Wrapper structure for work queue to store Zephyr work queue and stack */
struct qc_osal_work_q_wrapper {
    struct k_work_q zephyr_work_q;
    k_thread_stack_t *stack;
};

/* Work handler adapter - converts Zephyr work handler to OSAL handler */
static void work_handler_adapter(struct k_work *work)
{
    struct qc_osal_work_wrapper *wrapper = CONTAINER_OF(work, struct qc_osal_work_wrapper, zephyr_work);

    if (wrapper->user_handler) {
        wrapper->user_handler((qc_osal_work_t)wrapper);
    }
}

int qc_osal_work_init(qc_osal_work_t *work, qc_osal_work_handler_t handler)
{
    struct qc_osal_work_wrapper *wrapper;

    if (!work || !handler) {
        return -QC_OSAL_EINVAL;
    }

    wrapper = k_malloc(sizeof(struct qc_osal_work_wrapper));
    if (!wrapper) {
        return -QC_OSAL_ENOMEM;
    }

    wrapper->user_handler = handler;
    k_work_init(&wrapper->zephyr_work, work_handler_adapter);

    *work = (qc_osal_work_t)wrapper;
    return 0;
}

int qc_osal_work_queue_init(qc_osal_work_q_t *work_q, size_t stack_size, int priority)
{
    struct qc_osal_work_q_wrapper *wrapper;
    k_thread_stack_t *stack;

    if (!work_q) {
        return -QC_OSAL_EINVAL;
    }

    wrapper = k_malloc(sizeof(struct qc_osal_work_q_wrapper));
    if (!wrapper) {
        return -QC_OSAL_ENOMEM;
    }

    /* Allocate stack for work queue thread */
    stack = k_malloc(stack_size);
    if (!stack) {
        k_free(wrapper);
        return -QC_OSAL_ENOMEM;
    }

    wrapper->stack = stack;

    /* Initialize and start work queue */
    k_work_queue_init(&wrapper->zephyr_work_q);
    k_work_queue_start(&wrapper->zephyr_work_q, stack, stack_size, priority, NULL);

    *work_q = (qc_osal_work_q_t)wrapper;
    return 0;
}

int qc_osal_work_deinit(qc_osal_work_t *work)
{
    struct qc_osal_work_wrapper *wrapper;

    if (!work || !*work) {
        return -QC_OSAL_EINVAL;
    }

    wrapper = (struct qc_osal_work_wrapper *)*work;
    /* k_work_cancel_sync blocks until any in-progress handler finishes,
     * ensuring the wrapper is not freed while the handler is still running. */
    struct k_work_sync sync;
    k_work_cancel_sync(&wrapper->zephyr_work, &sync);
    k_free(wrapper);
    *work = NULL;
    return 0;
}

int qc_osal_work_queue_deinit(qc_osal_work_q_t *work_q)
{
    struct qc_osal_work_q_wrapper *wrapper;

    if (!work_q || !*work_q) {
        return -QC_OSAL_EINVAL;
    }

    wrapper = (struct qc_osal_work_q_wrapper *)*work_q;
    k_thread_abort(&wrapper->zephyr_work_q.thread);
    k_free(wrapper->stack);
    k_free(wrapper);
    *work_q = NULL;
    return 0;
}

int qc_osal_work_submit(qc_osal_work_q_t work_q, qc_osal_work_t work)
{
    struct qc_osal_work_q_wrapper *q_wrapper = (struct qc_osal_work_q_wrapper *)work_q;
    struct qc_osal_work_wrapper *w_wrapper = (struct qc_osal_work_wrapper *)work;
    int ret;

    if (!q_wrapper || !w_wrapper) {
        return -QC_OSAL_EINVAL;
    }

    ret = k_work_submit_to_queue(&q_wrapper->zephyr_work_q, &w_wrapper->zephyr_work);
    if (ret < 0) {
        return zephyr_errno_to_osal(-ret);
    }
    return 0;
}
#endif
