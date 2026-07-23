/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef QC_OSAL_H_
#define QC_OSAL_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @brief Qualcomm Operating System Abstraction Layer (OSAL)
 *
 * This header defines OS-agnostic interfaces for:
 * - Error codes
 * - Mutex operations
 * - Time/delay operations
 * - Logging
 * - Memory operations
 * - Work Queue Operations
 * - Message Queue Operations
 * - Thread Operations
 */

/* ============================================================================
 * Error Codes (OS-independent)
 * ============================================================================ */

#define QC_OSAL_EOK 0       /* Success */
#define QC_OSAL_EINVAL 1    /* Invalid argument */
#define QC_OSAL_ENOMEM 2    /* Out of memory */
#define QC_OSAL_ENODEV 3    /* No such device */
#define QC_OSAL_EAGAIN 4    /* Try again / Resource temporarily unavailable */
#define QC_OSAL_ETIMEDOUT 5 /* Timeout */
#define QC_OSAL_EMSGSIZE 6  /* Message too long */
#define QC_OSAL_EALREADY 7  /* Operation already in progress */
#define QC_OSAL_E2BIG 8     /* Argument list too long */
#define QC_OSAL_EIO 9       /* I/O error */
#define QC_OSAL_EBUSY 10    /* Device or resource busy */
#define QC_OSAL_ENOTSUP 11  /* Operation not supported */

/* ============================================================================
 * Mutex Operations
 * ============================================================================ */

typedef void *qc_osal_mutex_t;

/**
 * @brief Initialize a mutex
 * @param mutex Pointer to mutex handle
 * @return 0 on success, negative errno on failure
 */
int qc_osal_mutex_init(qc_osal_mutex_t *mutex);

/**
 * @brief Deinitialize a mutex and free associated resources
 * @param mutex Pointer to mutex handle (set to NULL on success)
 * @return 0 on success, negative errno on failure
 */
int qc_osal_mutex_deinit(qc_osal_mutex_t *mutex);

/**
 * @brief Lock a mutex
 * @param mutex Mutex handle
 * @param timeout_ms Timeout in milliseconds (0 = no wait, -1 = forever)
 * @return 0 on success, negative errno on failure
 */
int qc_osal_mutex_lock(qc_osal_mutex_t mutex, int32_t timeout_ms);

/**
 * @brief Unlock a mutex
 * @param mutex Mutex handle
 * @return 0 on success, negative errno on failure
 */
int qc_osal_mutex_unlock(qc_osal_mutex_t mutex);

/* ============================================================================
 * Semaphore Operations
 * ============================================================================ */

typedef void *qc_osal_sem_t;

/**
 * @brief Initialize a semaphore
 * @param sem Pointer to semaphore handle
 * @param initial_count Initial count value
 * @param max_count Maximum count value
 * @return 0 on success, negative errno on failure
 */
int qc_osal_sem_init(qc_osal_sem_t *sem, uint32_t initial_count, uint32_t max_count);

/**
 * @brief Deinitialize a semaphore and free associated resources
 * @param sem Pointer to semaphore handle (set to NULL on success)
 * @return 0 on success, negative errno on failure
 */
int qc_osal_sem_deinit(qc_osal_sem_t *sem);

/**
 * @brief Take/wait on a semaphore
 * @param sem Semaphore handle
 * @param timeout_ms Timeout in milliseconds (0 = no wait, -1 = forever)
 * @return 0 on success, negative errno on failure
 */
int qc_osal_sem_take(qc_osal_sem_t sem, int32_t timeout_ms);

/**
 * @brief Give/signal a semaphore
 * @param sem Semaphore handle
 * @return 0 on success, negative errno on failure
 */
int qc_osal_sem_give(qc_osal_sem_t sem);

/* ============================================================================
 * Time/Delay Operations
 * ============================================================================ */

/* Timeout constants */
#define QC_OSAL_TIMEOUT_NO_WAIT 0
#define QC_OSAL_TIMEOUT_FOREVER (-1)

/**
 * @brief Sleep for specified milliseconds
 * @param ms Milliseconds to sleep
 */
void qc_osal_msleep(uint32_t ms);

/**
 * @brief Sleep for specified microseconds
 * @param us Microseconds to sleep
 */
void qc_osal_usleep(uint32_t us);

/**
 * @brief Get current uptime in milliseconds
 * @return Current uptime in milliseconds
 */
uint32_t qc_osal_uptime_get_ms(void);

/* ============================================================================
 * Logging Operations
 * ============================================================================ */

typedef enum {
    QC_OSAL_LOG_LEVEL_ERR = 0,
    QC_OSAL_LOG_LEVEL_WRN = 1,
    QC_OSAL_LOG_LEVEL_INF = 2,
    QC_OSAL_LOG_LEVEL_DBG = 3
} qc_osal_log_level_t;

/**
 * @brief Log a message
 * @param level Log level
 * @param fmt Format string
 * @param ... Variable arguments
 */
void qc_osal_log(qc_osal_log_level_t level, const char *fmt, ...);

/* Convenience macros */
#define QC_OSAL_LOG_ERR(fmt, ...) qc_osal_log(QC_OSAL_LOG_LEVEL_ERR, fmt, ##__VA_ARGS__)
#define QC_OSAL_LOG_WRN(fmt, ...) qc_osal_log(QC_OSAL_LOG_LEVEL_WRN, fmt, ##__VA_ARGS__)
#define QC_OSAL_LOG_INF(fmt, ...) qc_osal_log(QC_OSAL_LOG_LEVEL_INF, fmt, ##__VA_ARGS__)
#define QC_OSAL_LOG_DBG(fmt, ...) qc_osal_log(QC_OSAL_LOG_LEVEL_DBG, fmt, ##__VA_ARGS__)

/* ============================================================================
 * Memory Operations
 * ============================================================================ */

/**
 * @brief Allocate memory
 * @param size Size in bytes
 * @return Pointer to allocated memory, NULL on failure
 */
void *qc_osal_malloc(size_t size);

/**
 * @brief Free memory
 * @param ptr Pointer to memory to free
 */
void qc_osal_free(void *ptr);

/* ============================================================================
 * Work Queue Operations
 * ============================================================================ */

typedef void *qc_osal_work_t;
typedef void *qc_osal_work_q_t;

/**
 * @brief Work handler function type
 * @param work Work item handle
 */
typedef void (*qc_osal_work_handler_t)(qc_osal_work_t work);

/**
 * @brief Initialize a work item
 * @param work Pointer to work item handle
 * @param handler Work handler function
 * @return 0 on success, negative errno on failure
 */
int qc_osal_work_init(qc_osal_work_t *work, qc_osal_work_handler_t handler);

/**
 * @brief Deinitialize a work item and free associated resources
 * @param work Pointer to work item handle (set to NULL on success)
 * @return 0 on success, negative errno on failure
 */
int qc_osal_work_deinit(qc_osal_work_t *work);

/**
 * @brief Initialize a work queue
 * @param work_q Pointer to work queue handle
 * @param stack_size Stack size for work queue thread
 * @param priority Thread priority
 * @return 0 on success, negative errno on failure
 */
int qc_osal_work_queue_init(qc_osal_work_q_t *work_q, size_t stack_size, int priority);

/**
 * @brief Deinitialize a work queue, stopping its thread and freeing all resources
 * @param work_q Pointer to work queue handle (set to NULL on success)
 * @return 0 on success, negative errno on failure
 */
int qc_osal_work_queue_deinit(qc_osal_work_q_t *work_q);

/**
 * @brief Submit work to a work queue
 * @param work_q Work queue handle
 * @param work Work item handle
 * @return 0 on success, negative errno on failure
 */
int qc_osal_work_submit(qc_osal_work_q_t work_q, qc_osal_work_t work);

/* ============================================================================
 * Message Queue Operations
 * ============================================================================ */

typedef void *qc_osal_queue_t;

/**
 * @brief Initialize a message queue
 * @param queue Pointer to queue handle
 * @param max_msgs Maximum number of messages in queue
 * @param msg_size Size of each message in bytes
 * @return 0 on success, negative errno on failure
 */
int qc_osal_queue_init(qc_osal_queue_t *queue, uint32_t max_msgs, size_t msg_size);

/**
 * @brief Send a message to queue
 * @param queue Queue handle
 * @param msg Pointer to message data
 * @param timeout_ms Timeout in milliseconds (0 = no wait, -1 = forever)
 * @return 0 on success, negative errno on failure
 */
int qc_osal_queue_send(qc_osal_queue_t queue, const void *msg, int32_t timeout_ms);

/**
 * @brief Receive a message from queue
 * @param queue Queue handle
 * @param msg Pointer to buffer for received message
 * @param timeout_ms Timeout in milliseconds (0 = no wait, -1 = forever)
 * @return 0 on success, negative errno on failure
 */
int qc_osal_queue_recv(qc_osal_queue_t queue, void *msg, int32_t timeout_ms);

/**
 * @brief Get number of messages in queue
 * @param queue Queue handle
 * @return Number of messages, or negative errno on failure
 */
int qc_osal_queue_get_count(qc_osal_queue_t queue);

/**
 * @brief Send a message to queue from ISR context
 * @param queue Queue handle
 * @param msg Pointer to message data
 * @return 0 on success, negative errno on failure
 * @note This function is safe to call from interrupt context
 */
int qc_osal_queue_send_from_isr(qc_osal_queue_t queue, const void *msg);

/* ============================================================================
 * Thread Operations
 * ============================================================================ */

typedef void *qc_osal_thread_t;

/**
 * @brief Thread entry function type
 * @param arg Thread argument
 */
typedef void (*qc_osal_thread_entry_t)(void *arg);

/**
 * @brief Thread configuration structure
 */
struct qc_osal_thread_config {
    const char *name;             /* Thread name (optional, can be NULL) */
    size_t stack_size;            /* Stack size in bytes */
    int priority;                 /* Thread priority */
    qc_osal_thread_entry_t entry; /* Thread entry function */
    void *arg;                    /* Thread argument */
};

/**
 * @brief Create a new thread
 * @param thread Pointer to thread handle
 * @param config Thread configuration
 * @return 0 on success, negative errno on failure
 */
int qc_osal_thread_create(qc_osal_thread_t *thread, const struct qc_osal_thread_config *config);

/**
 * @brief Abort/terminate a thread
 * @param thread Thread handle
 * @return 0 on success, negative errno on failure
 */
int qc_osal_thread_delete(qc_osal_thread_t thread);

/**
 * @brief Yield the processor to other threads
 */
void qc_osal_thread_yield(void);

#endif /* QC_OSAL_H_ */
