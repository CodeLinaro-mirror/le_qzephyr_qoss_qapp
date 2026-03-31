/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "../../qc_port_config.h"

#if defined(QC_OS_FREERTOS)
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "queue.h"
#include "timers.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../qc_osal.h"

/* FreeRTOS errno-like definitions (if not already defined) */
#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif
#ifndef ENODEV
#define ENODEV 19
#endif
#ifndef EAGAIN
#define EAGAIN 11
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif
#ifndef EMSGSIZE
#define EMSGSIZE 90
#endif
#ifndef EALREADY
#define EALREADY 114
#endif
#ifndef E2BIG
#define E2BIG 7
#endif
#ifndef EIO
#define EIO 5
#endif
#ifndef EBUSY
#define EBUSY 16
#endif
#ifndef ENOTSUP
#define ENOTSUP 95
#endif

/**
 * @brief Convert FreeRTOS error to OSAL error code
 * @param freertos_error FreeRTOS error code
 * @return Negative OSAL error code
 */
static inline int freertos_errno_to_osal(int freertos_error)
{
    if (freertos_error == 0) {
        return 0;
    }

    /* Map common errno values to OSAL error codes */
    switch (freertos_error) {
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

/* ============================================================================
 * Mutex Operations
 * ============================================================================ */

int qc_osal_mutex_init(qc_osal_mutex_t *mutex)
{
    SemaphoreHandle_t xMutex;

    if (!mutex) {
        return -QC_OSAL_EINVAL;
    }

    xMutex = xSemaphoreCreateMutex();
    if (!xMutex) {
        return -QC_OSAL_ENOMEM;
    }

    *mutex = (qc_osal_mutex_t)xMutex;
    return 0;
}

int qc_osal_mutex_lock(qc_osal_mutex_t mutex, int32_t timeout_ms)
{
    SemaphoreHandle_t xMutex = (SemaphoreHandle_t)mutex;
    TickType_t xTicksToWait;
    BaseType_t xResult;

    if (!xMutex) {
        return -QC_OSAL_EINVAL;
    }

    if (timeout_ms < 0) {
        xTicksToWait = portMAX_DELAY;
    } else if (timeout_ms == 0) {
        xTicksToWait = 0;
    } else {
        xTicksToWait = pdMS_TO_TICKS(timeout_ms);
    }

    xResult = xSemaphoreTake(xMutex, xTicksToWait);
    if (xResult != pdTRUE) {
        return -QC_OSAL_ETIMEDOUT;
    }

    return 0;
}

int qc_osal_mutex_unlock(qc_osal_mutex_t mutex)
{
    SemaphoreHandle_t xMutex = (SemaphoreHandle_t)mutex;
    BaseType_t xResult;

    if (!xMutex) {
        return -QC_OSAL_EINVAL;
    }

    xResult = xSemaphoreGive(xMutex);
    if (xResult != pdTRUE) {
        return -QC_OSAL_EIO;
    }

    return 0;
}

/* ============================================================================
 * Semaphore Operations
 * ============================================================================ */

int qc_osal_sem_init(qc_osal_sem_t *sem, uint32_t initial_count, uint32_t max_count)
{
    SemaphoreHandle_t xSem;

    if (!sem) {
        return -QC_OSAL_EINVAL;
    }

    xSem = xSemaphoreCreateCounting(max_count, initial_count);
    if (!xSem) {
        return -QC_OSAL_ENOMEM;
    }

    *sem = (qc_osal_sem_t)xSem;
    return 0;
}

int qc_osal_sem_take(qc_osal_sem_t sem, int32_t timeout_ms)
{
    SemaphoreHandle_t xSem = (SemaphoreHandle_t)sem;
    TickType_t xTicksToWait;
    BaseType_t xResult;

    if (!xSem) {
        return -QC_OSAL_EINVAL;
    }

    if (timeout_ms < 0) {
        xTicksToWait = portMAX_DELAY;
    } else if (timeout_ms == 0) {
        xTicksToWait = 0;
    } else {
        xTicksToWait = pdMS_TO_TICKS(timeout_ms);
    }

    xResult = xSemaphoreTake(xSem, xTicksToWait);
    if (xResult != pdTRUE) {
        return -QC_OSAL_ETIMEDOUT;
    }

    return 0;
}

int qc_osal_sem_give(qc_osal_sem_t sem)
{
    SemaphoreHandle_t xSem = (SemaphoreHandle_t)sem;
    BaseType_t xResult;

    if (!xSem) {
        return -QC_OSAL_EINVAL;
    }

    xResult = xSemaphoreGive(xSem);
    if (xResult != pdTRUE) {
        return -QC_OSAL_EIO;
    }

    return 0;
}

/* ============================================================================
 * Time/Delay Operations
 * ============================================================================ */

void qc_osal_msleep(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

void qc_osal_usleep(uint32_t us)
{
    /* FreeRTOS doesn't have native microsecond delay
     * Use millisecond delay for values >= 1ms
     * For sub-millisecond delays, use busy-wait (not ideal but necessary)
     */
    if (us >= 1000) {
        vTaskDelay(pdMS_TO_TICKS(us / 1000));
        us = us % 1000; // Handle remaining microseconds
    }

    if (us > 0) {
        /* Busy-wait for sub-millisecond delays
         * Cache the cycles per microsecond calculation
         */
        static const uint32_t cycles_per_us = configCPU_CLOCK_HZ / 1000000;
        volatile uint32_t count = us * cycles_per_us;

        /* Use a more efficient busy-wait with compiler optimization barrier */
        while (count--) {
            __asm__ volatile("nop");
        }
    }
}

uint32_t qc_osal_uptime_get_ms(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }

/* ============================================================================
 * Logging Operations
 * ============================================================================ */

/* Log level strings */
static const char *log_level_str[] = {"ERR", "WRN", "INF", "DBG"};

/* Current log level - can be changed at runtime */
static qc_osal_log_level_t g_log_level = QC_OSAL_LOG_LEVEL_INF;

/**
 * @brief Set the current log level
 * @param level New log level (only messages at this level or higher priority will be printed)
 */
void qc_osal_log_set_level(qc_osal_log_level_t level) { g_log_level = level; }

/**
 * @brief Get the current log level
 * @return Current log level
 */
qc_osal_log_level_t qc_osal_log_get_level(void) { return g_log_level; }

void qc_osal_log(qc_osal_log_level_t level, const char *fmt, ...)
{
    va_list args;
    char buf[256];
    int len;

    /* Filter by log level - only print if message level <= current level */
    if (level > g_log_level) {
        return;
    }

    /* Format: [level] message */
    len = snprintf(buf, sizeof(buf), "[%s] ", log_level_str[level]);

    va_start(args, fmt);
    vsnprintf(buf + len, sizeof(buf) - len, fmt, args);
    va_end(args);

    /* Output to console - platform specific
     * You may need to replace this with your platform's console output function
     */
    printf("%s\r\n", buf);
}

/* ============================================================================
 * Memory Operations
 * ============================================================================ */

void *qc_osal_malloc(size_t size) { return pvPortMalloc(size); }

void qc_osal_free(void *ptr) { vPortFree(ptr); }

/* ============================================================================
 * Work Queue Operations
 * ============================================================================ */

/* Work item structure */
struct qc_osal_work_wrapper {
    qc_osal_work_handler_t user_handler;
    void *user_data;
};

/* Work queue structure */
struct qc_osal_work_q_wrapper {
    QueueHandle_t queue;
    TaskHandle_t task;
    uint8_t running;
};

/* Work queue task function */
static void work_queue_task(void *pvParameters)
{
    struct qc_osal_work_q_wrapper *work_q = (struct qc_osal_work_q_wrapper *)pvParameters;
    struct qc_osal_work_wrapper *work;

    while (work_q->running) {
        /* Wait for work items */
        if (xQueueReceive(work_q->queue, &work, portMAX_DELAY) == pdTRUE) {
            if (work && work->user_handler) {
                work->user_handler((qc_osal_work_t)work);
            }
        }
    }

    /* Task cleanup */
    vTaskDelete(NULL);
}

int qc_osal_work_init(qc_osal_work_t *work, qc_osal_work_handler_t handler)
{
    struct qc_osal_work_wrapper *wrapper;

    if (!work || !handler) {
        return -QC_OSAL_EINVAL;
    }

    wrapper = pvPortMalloc(sizeof(struct qc_osal_work_wrapper));
    if (!wrapper) {
        return -QC_OSAL_ENOMEM;
    }

    wrapper->user_handler = handler;
    wrapper->user_data = NULL;

    *work = (qc_osal_work_t)wrapper;
    return 0;
}

int qc_osal_work_queue_init(qc_osal_work_q_t *work_q, size_t stack_size, int priority)
{
    struct qc_osal_work_q_wrapper *wrapper;
    BaseType_t xResult;

    if (!work_q) {
        return -QC_OSAL_EINVAL;
    }

    wrapper = pvPortMalloc(sizeof(struct qc_osal_work_q_wrapper));
    if (!wrapper) {
        return -QC_OSAL_ENOMEM;
    }

    /* Create queue for work items (max 16 items) */
    wrapper->queue = xQueueCreate(16, sizeof(struct qc_osal_work_wrapper *));
    if (!wrapper->queue) {
        vPortFree(wrapper);
        return -QC_OSAL_ENOMEM;
    }

    wrapper->running = 1;

    /* Create work queue task
     * Note: FreeRTOS priority is inverted compared to some RTOSes
     * Higher number = higher priority in FreeRTOS
     */
    xResult =
        xTaskCreate(work_queue_task, "work_queue", stack_size / sizeof(StackType_t), wrapper, priority, &wrapper->task);

    if (xResult != pdPASS) {
        vQueueDelete(wrapper->queue);
        vPortFree(wrapper);
        return -QC_OSAL_ENOMEM;
    }

    *work_q = (qc_osal_work_q_t)wrapper;
    return 0;
}

int qc_osal_work_submit(qc_osal_work_q_t work_q, qc_osal_work_t work)
{
    struct qc_osal_work_q_wrapper *q_wrapper = (struct qc_osal_work_q_wrapper *)work_q;
    struct qc_osal_work_wrapper *w_wrapper = (struct qc_osal_work_wrapper *)work;
    BaseType_t xResult;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    if (!q_wrapper || !w_wrapper) {
        return -QC_OSAL_EINVAL;
    }

    /* Check if called from ISR context */
    if (xPortIsInsideInterrupt()) {
        /* Send work item pointer to queue from ISR */
        xResult = xQueueSendFromISR(q_wrapper->queue, &w_wrapper, &xHigherPriorityTaskWoken);

        /* Request context switch if higher priority task was woken */
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    } else {
        /* Send work item pointer to queue from task context */
        xResult = xQueueSend(q_wrapper->queue, &w_wrapper, 0);
    }

    if (xResult != pdTRUE) {
        return -QC_OSAL_EAGAIN;
    }

    return 0;
}

/* ============================================================================
 * Message Queue Operations
 * ============================================================================ */

/* Queue wrapper structure */
struct qc_osal_queue_wrapper {
    QueueHandle_t queue_handle;
    size_t msg_size;
};

int qc_osal_queue_init(qc_osal_queue_t *queue, uint32_t max_msgs, size_t msg_size)
{
    struct qc_osal_queue_wrapper *wrapper;
    QueueHandle_t xQueue;

    if (!queue || max_msgs == 0 || msg_size == 0) {
        return -QC_OSAL_EINVAL;
    }

    /* Allocate wrapper structure */
    wrapper = pvPortMalloc(sizeof(struct qc_osal_queue_wrapper));
    if (!wrapper) {
        return -QC_OSAL_ENOMEM;
    }

    /* Create FreeRTOS queue */
    xQueue = xQueueCreate(max_msgs, msg_size);
    if (!xQueue) {
        vPortFree(wrapper);
        return -QC_OSAL_ENOMEM;
    }

    wrapper->queue_handle = xQueue;
    wrapper->msg_size = msg_size;

    *queue = (qc_osal_queue_t)wrapper;
    return 0;
}

int qc_osal_queue_send(qc_osal_queue_t queue, const void *msg, int32_t timeout_ms)
{
    struct qc_osal_queue_wrapper *wrapper = (struct qc_osal_queue_wrapper *)queue;
    TickType_t xTicksToWait;
    BaseType_t xResult;

    if (!wrapper || !wrapper->queue_handle || !msg) {
        return -QC_OSAL_EINVAL;
    }

    /* Convert timeout */
    if (timeout_ms < 0) {
        xTicksToWait = portMAX_DELAY;
    } else if (timeout_ms == 0) {
        xTicksToWait = 0;
    } else {
        xTicksToWait = pdMS_TO_TICKS(timeout_ms);
    }

    /* Send to queue */
    xResult = xQueueSend(wrapper->queue_handle, msg, xTicksToWait);
    if (xResult != pdTRUE) {
        return -QC_OSAL_ETIMEDOUT;
    }

    return 0;
}

int qc_osal_queue_recv(qc_osal_queue_t queue, void *msg, int32_t timeout_ms)
{
    struct qc_osal_queue_wrapper *wrapper = (struct qc_osal_queue_wrapper *)queue;
    TickType_t xTicksToWait;
    BaseType_t xResult;

    if (!wrapper || !wrapper->queue_handle || !msg) {
        return -QC_OSAL_EINVAL;
    }

    /* Convert timeout */
    if (timeout_ms < 0) {
        xTicksToWait = portMAX_DELAY;
    } else if (timeout_ms == 0) {
        xTicksToWait = 0;
    } else {
        xTicksToWait = pdMS_TO_TICKS(timeout_ms);
    }

    /* Receive from queue */
    xResult = xQueueReceive(wrapper->queue_handle, msg, xTicksToWait);
    if (xResult != pdTRUE) {
        return -QC_OSAL_ETIMEDOUT;
    }

    return 0;
}

int qc_osal_queue_get_count(qc_osal_queue_t queue)
{
    struct qc_osal_queue_wrapper *wrapper = (struct qc_osal_queue_wrapper *)queue;

    if (!wrapper || !wrapper->queue_handle) {
        return -QC_OSAL_EINVAL;
    }

    return (int)uxQueueMessagesWaiting(wrapper->queue_handle);
}

int qc_osal_queue_send_from_isr(qc_osal_queue_t queue, const void *msg)
{
    struct qc_osal_queue_wrapper *wrapper = (struct qc_osal_queue_wrapper *)queue;
    BaseType_t xResult;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    if (!wrapper || !wrapper->queue_handle || !msg) {
        return -QC_OSAL_EINVAL;
    }

    /* Send to queue from ISR */
    xResult = xQueueSendFromISR(wrapper->queue_handle, msg, &xHigherPriorityTaskWoken);

    /* Request context switch if a higher priority task was woken */
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);

    if (xResult != pdTRUE) {
        return -QC_OSAL_EAGAIN;
    }

    return 0;
}

/* ============================================================================
 * Thread Operations
 * ============================================================================ */

/* Thread wrapper structure to adapt OSAL interface to FreeRTOS */
struct qc_osal_thread_wrapper {
    TaskHandle_t task_handle;
    StackType_t *stack;
    qc_osal_thread_entry_t user_entry;
    void *user_arg;
};

/**
 * @brief Thread entry adapter function
 * Adapts FreeRTOS task entry signature to OSAL thread entry signature
 */
static void thread_entry_adapter(void *pvParameters)
{
    struct qc_osal_thread_wrapper *wrapper = (struct qc_osal_thread_wrapper *)pvParameters;

    if (wrapper && wrapper->user_entry) {
        /* Call user's thread entry function */
        wrapper->user_entry(wrapper->user_arg);
    }

    /* Thread finished, delete itself */
    vTaskDelete(NULL);
}

int qc_osal_thread_create(qc_osal_thread_t *thread, const struct qc_osal_thread_config *config)
{
    struct qc_osal_thread_wrapper *wrapper;
    BaseType_t xResult;

    if (!thread || !config || !config->entry) {
        return -QC_OSAL_EINVAL;
    }

    /* Allocate wrapper structure */
    wrapper = pvPortMalloc(sizeof(struct qc_osal_thread_wrapper));
    if (!wrapper) {
        return -QC_OSAL_ENOMEM;
    }

    /* Initialize wrapper */
    wrapper->user_entry = config->entry;
    wrapper->user_arg = config->arg;
    wrapper->stack = NULL; /* FreeRTOS manages stack internally with xTaskCreate */
    wrapper->task_handle = NULL;

    /* Create FreeRTOS task
     * Note: FreeRTOS priority is inverted compared to some RTOSes
     * Higher number = higher priority in FreeRTOS
     * Stack size is in words (StackType_t), not bytes
     */
    xResult = xTaskCreate(thread_entry_adapter,                     /* Task function */
                          config->name ? config->name : "thread",   /* Task name */
                          config->stack_size / sizeof(StackType_t), /* Stack size in words */
                          wrapper,                                  /* Task parameter */
                          config->priority,                         /* Task priority */
                          &wrapper->task_handle                     /* Task handle */
    );

    if (xResult != pdPASS) {
        vPortFree(wrapper);
        return -QC_OSAL_ENOMEM;
    }

    *thread = (qc_osal_thread_t)wrapper;
    return 0;
}

int qc_osal_thread_delete(qc_osal_thread_t thread)
{
    struct qc_osal_thread_wrapper *wrapper = (struct qc_osal_thread_wrapper *)thread;

    if (!wrapper || !wrapper->task_handle) {
        return -QC_OSAL_EINVAL;
    }

    /* Delete the FreeRTOS task */
    vTaskDelete(wrapper->task_handle);

    /* Free wrapper structure */
    vPortFree(wrapper);

    return 0;
}

void qc_osal_thread_yield(void)
{
    /* Yield the processor to other tasks */
    taskYIELD();
}
#endif