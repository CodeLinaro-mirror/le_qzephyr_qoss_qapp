/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>
#include <zephyr/pm/pm.h>
#include <stdio.h>
#include "qpower.h"
#include <stdlib.h>
#include <stdbool.h>
#include <qlib_util.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/policy.h>
#include <zephyr/sys_clock.h>

// #define PM_SLEEP_BY_LATENCY

LOG_MODULE_REGISTER(app, CONFIG_APP_LOG_LEVEL);

#ifdef PM_SLEEP_BY_LATENCY
static void on_latency_changed(int32_t latency)
{
    if (latency == SYS_FOREVER_US) {
        LOG_INF("Latency constraint changed: none");
    } else {
        LOG_INF("Latency constraint changed: %" PRId32 "ms", latency / USEC_PER_MSEC);
    }
}
#endif

int main(void)
{
#ifdef PM_SLEEP_BY_LATENCY
    // softoff: latency=500ms, residency=3000ms
    int sleep_ms = 8000;
    int sleep_pm_active_ms = 2000;
    struct pm_policy_latency_subscription subs;
    struct pm_policy_latency_request req;
#endif

    k_sleep(K_MSEC(1));
    LOG_INF("Hello World! %s", CONFIG_BOARD_TARGET);

#ifdef PM_SLEEP_BY_LATENCY
    dead_loop_cond1();

    pm_policy_latency_changed_subscribe(&subs, on_latency_changed);
    LOG_INF("Setting latency constraint: 30ms");
    pm_policy_latency_request_add(&req, 30000);
    LOG_INF("Sleeping for %d seconds, we should stay ACTIVE due to latency constraint", sleep_ms / 1000);
    k_msleep(sleep_ms);

    LOG_INF("Setting latency constraint: 1000ms");
    pm_policy_latency_request_update(&req, 1000000);
    LOG_INF("Sleeping for %d seconds, we should stay ACTIVE due to residency constraint", sleep_pm_active_ms / 1000);
    k_msleep(sleep_pm_active_ms);

    LOG_INF("Sleeping for %d seconds, we should enter softoff or susmpend2ram", sleep_ms / 1000);
    dead_loop_cond1();
    k_msleep(sleep_ms);

    LOG_ERR("Should not reach here. Now actually reach here because idle timeout are less than softoff or suspend2ram "
            "residence");
#endif

    return 0;
}
