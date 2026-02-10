/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file pm_timer_manager.c
 * @brief PM Timer Manager using Idle Hooks and System Timer Traversal.
 *
 * Architecture:
 * 1. Registers callbacks into the Kernel Idle Hooks.
 * 2. Suspends specific timers BEFORE kernel idle calculation.
 * 3. Resumes timers AFTER wakeup.
 * 4. [NEW] Analyzes system timer statistics (Count, Min, Max, Avg).
 */


#include <zephyr/kernel.h>
#include <zephyr/sys/slist.h>
#include <zephyr/sys/dlist.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>

#include <zephyr/device.h>
#include <zephyr/pm/device.h>
#include <zephyr/shell/shell.h>

#include <zephyr/sys/atomic.h>
#include <zephyr/shell/shell_uart.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/pm/policy.h>

#include "qurt_timer.h"
#include "qurt_error.h"
#include "qapi_wlan.h"

LOG_MODULE_REGISTER(pm_timer, LOG_LEVEL_DBG);

struct pm_managed_timer {
    sys_snode_t node;
    struct k_timer *timer;
    const char *name;
    bool suspendable;
    bool auto_restart;
    k_timeout_t saved_remaining;
    k_timeout_t saved_period;
    bool was_running;
};

static uint32_t pm_check_interval_ms = 180000;  //should not small than 60000

/* ============================================================================
 * [External Dependency] Zephyr Internal Symbols
 * ============================================================================ */

/* 
 * WARNING: 'timeout_list' is typically static in kernel/timeout.c.
 * You MUST remove the 'static' keyword in kernel/timeout.c for this to link.
 */
extern sys_dlist_t timeout_list;

/* 
 * We need a way to identify if a timeout belongs to a k_timer. We do this by
 * comparing its callback function to the known handler for k_timers. Since
 * z_timer_expiration_handler is static, we "capture" its address once at init.
 */
static _timeout_func_t g_timer_expiry_handler_fn = NULL;

static sys_slist_t g_pm_timer_list = SYS_SLIST_STATIC_INIT(&g_pm_timer_list);
const char shell_rx_timer_name[] = "shell_uart_rx";

static struct k_spinlock g_pm_timer_lock;
static struct idle_hook_entry my_pre_sleep_hook_entry;
static struct idle_hook_entry my_post_sleep_hook_entry;

/* A static instance to hold the management info for the shell's timer */
static struct pm_managed_timer g_shell_uart_managed_timer;

/*
 * The macro SHELL_UART_TRANSPORT_DEFINE(shell_transport_uart, ...) 
 * in shell_uart.c defines a structure named 'shell_transport_uart'.
 */
extern const struct shell_transport shell_transport_uart;

uint32_t pm_timer_stop_all_k_timers();
int pm_timer_register(struct pm_managed_timer *mt, struct k_timer *timer, bool suspendable, bool auto_restart, const char *name);
int pm_timer_unregister(struct k_timer *timer);
void pm_timer_register_internal(char *pcTimerName,TimerHandle_t k_timer_handle);
static void pm_service_timer_cb(struct k_timer *timer);



K_TIMER_DEFINE(pm_service_timer, pm_service_timer_cb, NULL);
struct k_timer dummy_timer;

static void pm_service_timer_cb(struct k_timer *timer)
{
    LOG_INF("pm_service_timer_cb");
    if(!pm_device_is_any_busy())
    {
        if(pm_policy_state_lock_is_active(PM_STATE_SUSPEND_TO_RAM,PM_ALL_SUBSTATES))
        {
            pm_policy_state_lock_put(PM_STATE_SUSPEND_TO_RAM,PM_ALL_SUBSTATES);
        }
    }
    else
    {
        //if any device still busy, restart to check at background
        k_timer_start(timer, K_MSEC(pm_check_interval_ms), K_NO_WAIT);
    }
}

void pm_timer_register_internal(char *pcTimerName,TimerHandle_t k_timer_handle)
{
    if(pcTimerName == NULL || k_timer_handle == NULL)
    {
        return;
    }
    // NOTE:use k_calloc here, some timer may be created very early when boot
    struct pm_managed_timer *pm_timer =  k_calloc(1,sizeof(struct pm_managed_timer));
    if(pm_timer == NULL)
    {
        LOG_WRN("NO MEM FOR:%s.", pcTimerName);
    }
    pm_timer_register(pm_timer,
                        k_timer_handle,
                        (bool)true,  /* suspendable = false */
                        (bool)true, /* auto_restart = false */
                        pcTimerName);
} 

void pm_timer_unregister_internal(TimerHandle_t k_timer_handle)
{
    if(k_timer_handle == NULL)
    {
        return;
    }

    pm_timer_unregister(k_timer_handle);
} 

/**
 * @brief Dump all timers currently registered in the managed list.
 * 
 * This iterates through g_pm_timer_list and prints the configuration
 * and status of each timer.
 */
void pm_timer_dump_managed_list(void)
{
    struct pm_managed_timer *mt;
    uint32_t count = 0;

    LOG_INF("==========================================================================");
    LOG_INF("                       Managed PM Timers List                             ");
    LOG_INF("--------------------------------------------------------------------------");
    LOG_INF("%-20s | %-10s | %-10s | %-10s | %-10s", 
            "Name", "Timer Ptr", "Suspendable", "AutoRestart", "WasRunning");
    LOG_INF("--------------------------------------------------------------------------");

    /* Lock the list to ensure consistent traversal */
    k_spinlock_key_t key = k_spin_lock(&g_pm_timer_lock);

    SYS_SLIST_FOR_EACH_CONTAINER(&g_pm_timer_list, mt, node) {
        LOG_INF("%-20s | %-10p | %-11s | %-11s | %-10s",
                mt->name ? mt->name : "unnamed",
                mt->timer,
                mt->suspendable ? "Yes" : "No",
                mt->auto_restart ? "Yes" : "No",
                mt->was_running ? "Yes" : "No");
        
        count++;
    }

    k_spin_unlock(&g_pm_timer_lock, key);

    LOG_INF("--------------------------------------------------------------------------");
    LOG_INF("Total managed timers: %u", count);
    LOG_INF("==========================================================================");
}

/* 
 * @brief Registers the shell's internal RX timer to be suspendable.
 *
 * This function demonstrates the correct, robust way to access an internal
 * object of another subsystem without modifying kernel source code. It relies
 * on public APIs and publicly defined (though internal-use) structures.
 *
 * @return 0 on success, negative errno code on failure.
 */
int pm_register_shell_uart_timer(void)
{
    /* Step 1: Get a pointer to the default shell instance using a public API. */
    const struct shell_transport *transport = &shell_transport_uart;


    /* Step 3: Access the context pointer (void *). */
    void *ctx = transport->ctx;
    if (!ctx) {
        LOG_WRN("Transport backend has no context data. Cannot register timer.");
        return -EINVAL;
    }

    // Validate that this is actually a polling transport
    // Check if the transport API matches what we expect
    if (transport->api == NULL) {
        LOG_WRN("Transport has no API. Cannot register timer.");
        return -EINVAL;
    }

    // Add runtime check or compile-time assertion
    #if !defined(CONFIG_SHELL_BACKEND_SERIAL_API_POLLING)
        LOG_WRN("Shell not configured for polling mode. Timer registration skipped.");
        return -ENOTSUP;
    #endif

    /*
     * Step 4: SAFE & CORRECT CAST.
     * We cast the generic 'void *' context to the specific struct type that
     * is defined in the public <zephyr/shell/shell_uart.h> header.
     * This is NOT a hacky re-definition; it is using a known, defined type.
     */
    struct shell_uart_polling *sh_uart_ctx = (struct shell_uart_polling *)ctx;

    /* Step 5: Now we can safely access the timer member of the defined struct. */
    struct k_timer *rx_timer_ptr = &sh_uart_ctx->rx_timer;
    g_shell_uart_managed_timer.saved_period.ticks = (sh_uart_ctx->rx_timer).period.ticks;

    LOG_INF("Successfully located shell_uart rx_timer at address: %p", rx_timer_ptr);

    /* 
     * Step 6: Register it with our PM manager.
     * It's crucial to set auto_restart to 'false'. The shell backend's logic
     * is responsible for starting/stopping its own timer when it needs to.
     * We just suspend it during idle.
     */
    return pm_timer_register(&g_shell_uart_managed_timer,
                             rx_timer_ptr,
                             (bool)true,  /* suspendable = no */
                             (bool)true, /* auto_restart = no */
                             shell_rx_timer_name);
}

static int my_app_init(void)
{
    // do not enter sleep at first
    pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_RAM,PM_ALL_SUBSTATES);
    /* 
     * During application initialization, attempt to register the shell timer.
     * This ensures it happens after the shell subsystem itself is initialized.
     */
    // NOTE: only used when use polling_init in shell_uart.c
    if (pm_register_shell_uart_timer() != 0) {
        LOG_WRN("Failed to register Shell UART RX timer for power saving.");
    }
    
    // start backend activity service

    return 0;
}

/* Run this after the shell is initialized, but before the app main loop starts */
SYS_INIT(my_app_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);



/* ============================================================================
 * [Infrastructure] Idle Hook Mechanism Definition
 * ============================================================================ */

typedef void (*idle_hook_t)(void);

struct idle_hook_entry {
    sys_snode_t node;
    idle_hook_t hook_fn;
};

static sys_slist_t g_suspend_hooks = SYS_SLIST_STATIC_INIT(&g_suspend_hooks);
static sys_slist_t g_resume_hooks = SYS_SLIST_STATIC_INIT(&g_resume_hooks);
static struct k_spinlock g_hook_lock;

void idle_hook_register_suspend(struct idle_hook_entry *entry, idle_hook_t fn)
{
    k_spinlock_key_t key = k_spin_lock(&g_hook_lock);
    entry->hook_fn = fn;
    sys_slist_append(&g_suspend_hooks, &entry->node);
    k_spin_unlock(&g_hook_lock, key);
}

void idle_hook_register_resume(struct idle_hook_entry *entry, idle_hook_t fn)
{
    k_spinlock_key_t key = k_spin_lock(&g_hook_lock);
    entry->hook_fn = fn;
    sys_slist_append(&g_resume_hooks, &entry->node);
    k_spin_unlock(&g_hook_lock, key);
}

/* Called by kernel/idle.c */
void z_idle_hooks_run_suspend(void)
{
    struct idle_hook_entry *entry;
    SYS_SLIST_FOR_EACH_CONTAINER(&g_suspend_hooks, entry, node) {
        if (entry->hook_fn) entry->hook_fn();
    }
}

void z_idle_hooks_run_resume(void)
{
    struct idle_hook_entry *entry;
    SYS_SLIST_FOR_EACH_CONTAINER(&g_resume_hooks, entry, node) {
        if (entry->hook_fn) entry->hook_fn();
    }
}

/* ============================================================================
 * [Logic] PM Timer Manager Implementation
 * ============================================================================ */




/* ============================================================================
 *  System Timer Statistics
 * ============================================================================ */

/**
 * @brief Structure to hold system timer statistics
 */
struct pm_system_timer_stats {
    uint32_t active_count;      /* Number of currently running timers */
    k_ticks_t min_timeout;      /* The nearest timeout (ticks from now) */
    k_ticks_t max_timeout;      /* The furthest timeout (ticks from now) */
    k_ticks_t avg_timeout;      /* Average timeout duration */
    uint64_t total_ticks_sum;   /* Sum of all timeouts (for avg calculation) */
};


/**
 * @brief Iterates through all static devices and prints their PM status.
 *
 * This function provides a snapshot of the power management configuration
 * for every device in the system, including its flags and current busy state.
 */
#if defined(CONFIG_PM_DEVICE)
void pm_device_dump_all_status(void)
{
    /* Get the head of the array of all static devices in the system */
    const struct device *devs;
    size_t devc;
    devc = z_device_get_all_static(&devs);
    size_t dev_count = 0;

    LOG_INF("==============================================================================");
    LOG_INF("                           Device PM Status Dump                              ");
    LOG_INF("------------------------------------------------------------------------------");
    LOG_INF("%-24s | %-12s | %-12s | %s", "Device Name", "PM Flags", "Busy State", "PM state");
    LOG_INF("------------------------------------------------------------------------------");

    if (devs == NULL) {
        LOG_WRN("Could not get device list.");
        return;
    }

    
	for (const struct device *dev = devs; dev < (devs + devc); dev++) {
        char flags_str[32] = "N/A";
        char pm_state_str[32] = "N/A";
        
        /* Check if the device has PM configured */
        if (dev->pm_base != NULL) {
            /* Format the PM flags for printing */
            snprintk(flags_str, sizeof(flags_str), "0x%02x", dev->pm_base->flags);

            snprintk(pm_state_str, sizeof(pm_state_str),"%d", dev->pm_base->state);
            
        }

        /* Use the public API to check the device's busy status */
        const char *busy_str = pm_device_is_busy(dev) ? "Busy" : "Idle";

        LOG_INF("%-24s | %-12s | %-12s | %s", 
                dev->name,
                flags_str,
                busy_str,
                pm_state_str);
        
        dev_count++;
	}

    LOG_INF("------------------------------------------------------------------------------");
    LOG_INF("Total devices found: %u", dev_count);
    LOG_INF("==============================================================================");
}
#endif

/**
 * @brief Collects statistics about all active timers in the system.
 * 
 * This iterates Zephyr's internal `timeout_list`. Since the list is 
 * delta-encoded (each node stores ticks relative to the previous node),
 * we accumulate ticks to find the absolute time relative to "now".
 * 
 * @param stats Pointer to the stats structure to populate.
 */
void pm_timer_collect_system_stats(struct pm_system_timer_stats *stats)
{
    struct _timeout *to;
    sys_dnode_t *node;
    k_ticks_t accumulated_ticks = 0;
    
    /* Use local variables for calculation to minimize struct access */
    uint32_t count = 0;
    k_ticks_t min_t = INT64_MAX; /* Max value */
    k_ticks_t max_t = 0;
    uint64_t sum_t = 0;

    /* Use the same lock to protect access to the shared list (approximation) */
    unsigned int key = irq_lock();

    SYS_DLIST_FOR_EACH_NODE(&timeout_list, node) {
        to = CONTAINER_OF(node, struct _timeout, node);
        
        /* 
         * DECODE DELTA ENCODING:
         * Node 1: dticks = 10 (Expires at +10)
         * Node 2: dticks = 5  (Expires at +10 + 5 = +15)
         * Node 3: dticks = 20 (Expires at +15 + 20 = +35)
         */
        accumulated_ticks += to->dticks;
        
        /* Update Stats */
        count++;
        
        if (accumulated_ticks < min_t) {
            min_t = accumulated_ticks;
        }
        
        if (accumulated_ticks > max_t) {
            max_t = accumulated_ticks;
        }
        
        sum_t += accumulated_ticks;
    }
    
    irq_unlock(key);

    /* Populate result */
    stats->active_count = count;
    stats->total_ticks_sum = sum_t;
    
    if (count > 0) {
        stats->min_timeout = min_t;
        stats->max_timeout = max_t;
        stats->avg_timeout = (k_ticks_t)(sum_t / count);
    } else {
        stats->min_timeout = 0;
        stats->max_timeout = 0;
        stats->avg_timeout = 0;
    }
}

/**
 * @brief Generic traversal function (for printing/debugging)
 */
void pm_timer_traverse_system_timeouts(void (*callback)(struct _timeout *, k_ticks_t, void *), void *user_data)
{
    struct _timeout *to;
    sys_dnode_t *node;
    k_ticks_t accumulated_ticks = 0;

    k_spinlock_key_t key = k_spin_lock(&g_pm_timer_lock);

    SYS_DLIST_FOR_EACH_NODE(&timeout_list, node) {
        to = CONTAINER_OF(node, struct _timeout, node);
        accumulated_ticks += to->dticks;

        if (callback) {
            callback(to, accumulated_ticks, user_data);
        }
    }

    k_spin_unlock(&g_pm_timer_lock, key);
}

/* ============================================================================
 * [Hooks] Idle Suspend/Resume Implementation
 * ============================================================================ */

static void on_idle_pre_sleep(void)
{
    struct pm_managed_timer *mt;

    if(!pm_policy_state_lock_is_active(PM_STATE_SUSPEND_TO_RAM,PM_ALL_SUBSTATES) && !pm_device_is_any_busy())
    {
        k_spinlock_key_t key = k_spin_lock(&g_pm_timer_lock);

        SYS_SLIST_FOR_EACH_CONTAINER(&g_pm_timer_list, mt, node) {
            if (!mt->suspendable || !mt->timer) continue;
    
            mt->saved_remaining.ticks = k_timer_remaining_ticks(mt->timer);
            mt->saved_period.ticks = mt->timer->period.ticks;

            /*
             * Check if timer is running by verifying if it is in the kernel's timeout queue.
             * True periodic timers (like shell_uart_rx) are re-added to the queue atomically
             * within the expiry ISR, so node.next is always non-NULL while active.
             * This check correctly identifies running timers and ignores intentionally
             * stopped timers (which are removed from the queue).
             */
            if (mt->timer->timeout.node.next != NULL) {
                 k_timer_stop(mt->timer);
                 mt->was_running = true;
            } else {
                mt->was_running = false;
            }
        }
        k_spin_unlock(&g_pm_timer_lock, key);
        if(qurt_timer_Is_Active(&pm_service_timer))
        {
            k_timer_stop(&pm_service_timer);
        }
    }
    else
    {
#ifdef CONFIG_QPM_AUTO_SLEEP
        if (!qurt_timer_Is_Active(&pm_service_timer)) {
            k_timer_start(&pm_service_timer, K_MSEC(pm_check_interval_ms), K_NO_WAIT);
        }
#endif
    }
}

static void on_idle_post_sleep(void)
{
    struct pm_managed_timer *mt;
    k_spinlock_key_t key = k_spin_lock(&g_pm_timer_lock);

    SYS_SLIST_FOR_EACH_CONTAINER(&g_pm_timer_list, mt, node) {
        if (!mt->was_running || !mt->timer) continue;

        if (mt->auto_restart) {
            k_timer_start(mt->timer, mt->saved_remaining, mt->saved_period);
        }
        mt->was_running = false;
    }
    k_spin_unlock(&g_pm_timer_lock, key);
}

/* ============================================================================
 * [API] Registration & Init
 * ============================================================================ */

int pm_timer_register(struct pm_managed_timer *mt, struct k_timer *timer, bool suspendable, bool auto_restart, const char *name)
{
    if (!mt || !timer) return -EINVAL;

    mt->timer = timer;
    mt->name = name;
    mt->suspendable = suspendable;
    mt->auto_restart = auto_restart;
    mt->was_running = false;
    

    k_spinlock_key_t key = k_spin_lock(&g_pm_timer_lock);
    sys_slist_append(&g_pm_timer_list, &mt->node);
    k_spin_unlock(&g_pm_timer_lock, key);
    return 0;
}

int pm_timer_unregister(struct k_timer *timer)
{
    struct pm_managed_timer *mt, *next;
    k_spinlock_key_t key = k_spin_lock(&g_pm_timer_lock);

    SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&g_pm_timer_list, mt, next, node) {
        if (mt->timer == timer) {
            sys_slist_find_and_remove(&g_pm_timer_list, &mt->node);
            k_free(mt); 
            break;
        }
    }
    k_spin_unlock(&g_pm_timer_lock, key);
    return 0;
}

/**
 * @brief Stops all active k_timers currently scheduled in the system.
 *
 * This function safely traverses the kernel's internal timeout queue, identifies
 * timeouts belonging to k_timers, and stops them. This is a powerful debugging
 * tool to quiesce a system.
 *
 * @return The number of timers that were stopped.
 */
uint32_t pm_timer_stop_all_k_timers(void)
{
    struct _timeout *to;
    sys_dnode_t *node, *next_node; /* 'next_node' is crucial for safe iteration */
    uint32_t stop_count = 0;

    if (g_timer_expiry_handler_fn == NULL) {
        LOG_WRN("Timer handler address not captured yet. Cannot stop timers.");
        return 0;
    }

    LOG_INF("Attempting to stop all active k_timers...");

    /* Lock to ensure thread-safe traversal and modification of the list */
    k_spinlock_key_t key = k_spin_lock(&g_pm_timer_lock);

    /* 
     * CRITICAL: Use SYS_DLIST_FOR_EACH_NODE_SAFE.
     * This macro safely gets the next node BEFORE the loop body executes,
     * so even if k_timer_stop() removes 'node' from the list, 'next_node'
     * is still valid for the next iteration.
     */
    SYS_DLIST_FOR_EACH_NODE_SAFE(&timeout_list, node, next_node) {
        to = CONTAINER_OF(node, struct _timeout, node);

        /* Identify if this timeout belongs to a k_timer */
        if (to->fn == g_timer_expiry_handler_fn) {
            struct k_timer *parent_timer = CONTAINER_OF(to, struct k_timer, timeout);
            
            /* Stop the timer. This removes the 'node' from timeout_list. */
            k_timer_stop(parent_timer);
            stop_count++;
            
            /* 
             * We can't safely get the 'name' here if the timer was on a different
             * thread's stack and that thread is gone. But for debugging, it's often ok.
             */
            LOG_DBG("  Stopped timer at address %p", parent_timer);
        }
    }

    k_spin_unlock(&g_pm_timer_lock, key);

    LOG_INF("Finished: %u k_timers were stopped.", stop_count);
    return stop_count;
}


/**
 * @brief Initialize the module, register hooks, and capture the timer handler address.
 */
int pm_timer_manager_init(void)
{
    LOG_INF("Initializing PM Timer Manager...");

    /* 
     * CAPTURE THE HANDLER ADDRESS:
     * To identify k_timers in the generic timeout list, we need to know the
     * address of the internal 'z_timer_expiration_handler'. We can get this
     * by creating a dummy timer and reading the 'fn' pointer from its 
     * internal _timeout struct.
     */
    if (g_timer_expiry_handler_fn == NULL) {
   
        k_timer_init(&dummy_timer, NULL, NULL);
        /* Start and immediately stop the timer. This is enough for the kernel
         * to populate the internal timeout struct. Using K_MSEC(1) is safe. */
        k_timer_start(&dummy_timer, K_MSEC(1), K_NO_WAIT);
        
        g_timer_expiry_handler_fn = dummy_timer.timeout.fn;
        
        /* Stop the timer to remove it from the kernel's timeout queue. */
        k_timer_stop(&dummy_timer);

        LOG_DBG("Captured k_timer handler function address: %p", g_timer_expiry_handler_fn);
    }

    idle_hook_register_suspend(&my_pre_sleep_hook_entry, on_idle_pre_sleep);
    idle_hook_register_resume(&my_post_sleep_hook_entry, on_idle_post_sleep);

    // some known timer need to be suspend before enter sleep, they should not as wakeup source
    // pm_timer_register();

    return 0;
}
SYS_INIT(pm_timer_manager_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);



/**
 * @brief The core debug function, now enhanced to show the user expiry_fn.
 */
static void _debug_print_timeout(struct _timeout *to, k_ticks_t ticks, void *ud)
{
    ARG_UNUSED(ud);

    /* Check if the callback is the one used by k_timer */
    if (g_timer_expiry_handler_fn != NULL && to->fn == g_timer_expiry_handler_fn) {
        /* 
         * This is a k_timer! We can safely get the parent k_timer struct.
         * The 'timeout' member is the name of the _timeout struct inside k_timer.
         */
        struct k_timer *parent_timer = CONTAINER_OF(to, struct k_timer, timeout);
        
        LOG_INF("  -> [k_timer] Expires @ +%lld ticks. User Fn: %p", 
                ticks, 
                parent_timer->expiry_fn);
    } else {
        /* This timeout belongs to another kernel object (e.g., k_sleep, k_sem) */
        LOG_INF("  -> [Kernel Obj] Expires @ +%lld ticks. Handler: %p", ticks, to->fn);
    }
}
void pm_timer_debug_dump(void)
{
    struct pm_system_timer_stats stats;
    
    LOG_INF("========================================");
    LOG_INF("      System Timer Analysis             ");
    LOG_INF("========================================");
    
    /* 1. Collect Stats */
    pm_timer_collect_system_stats(&stats);
    
    LOG_INF("Active Timers : %u", stats.active_count);
    if (stats.active_count > 0) {
        LOG_INF("Min Timeout   : %lld ticks", stats.min_timeout);
        LOG_INF("Max Timeout   : %lld ticks", stats.max_timeout);
        LOG_INF("Avg Timeout   : %lld ticks", stats.avg_timeout);
    } else {
        LOG_INF("No active timers.");
    }
    
    LOG_INF("----------------------------------------");
    LOG_INF("Detailed List:");
    
    /* 2. Traverse and Print Details */
    pm_timer_traverse_system_timeouts(_debug_print_timeout, NULL);
    
    LOG_INF("========================================");
}
