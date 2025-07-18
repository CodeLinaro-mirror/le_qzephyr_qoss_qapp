/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
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

//#define PM_SLEEP_BY_LATENCY

LOG_MODULE_REGISTER(app, CONFIG_APP_LOG_LEVEL);

#ifdef PM_SLEEP_BY_LATENCY
static void on_latency_changed(int32_t latency)
{
    if (latency == SYS_FOREVER_US) {
        LOG_INF("Latency constraint changed: none");
    } else {
        LOG_INF("Latency constraint changed: %" PRId32 "ms",
            latency / USEC_PER_MSEC);
    }
}
#endif

static int cmd_set_softoff(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    uint32_t slp_time_ms = shell_strtoul(argv[1], 10, &err);

    if (err) {
        shell_error(ctx, "Unable to parse input slp_time_ms (err %d)", err);
        return err;
    }

    shell_print(ctx, "Set state to SOFT_OFF...");
    qapi_power_set_parameter(__QAPI_POWER_SOFTOFF_DURATION_MS, slp_time_ms);
    pm_state_force(0u, &(struct pm_state_info){ PM_STATE_SOFT_OFF, 0, 0 });
    return 0;
}

static int cmd_set_s2ram(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    uint32_t slp_time_ms = shell_strtoul(argv[1], 10, &err);

    if (err) {
        shell_error(ctx, "Unable to parse input slp_time_ms (err %d)", err);
        return err;
    }

    shell_print(ctx, "Set state to suspend_to_ram...");
    qapi_power_set_parameter(__QAPI_POWER_SUSPEND2RAM_DURATION_MS, slp_time_ms);
    pm_state_force(0u, &(struct pm_state_info){ PM_STATE_SUSPEND_TO_RAM, 0, 0 });
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_pm_cmds,
    SHELL_CMD_ARG(softoff, NULL,
        "set pm state to soft_of\n"
        "Usage: qpm softoff <sleep_time_ms:int>\n"
        "sleep_time_ms: no timeout if 0>\n",
        cmd_set_softoff, 2, 0),
    SHELL_CMD_ARG(s2ram, NULL,
        "set pm state to suspend2ram \n"
        "Usage: qpm s2ram <sleep_time_ms:int>\n"
        "sleep_time_ms: no timeout if 0>\n",
        cmd_set_s2ram, 2, 0),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(qpm, &sub_pm_cmds, "power management commands", NULL);

int main(void)
{
#ifdef PM_SLEEP_BY_LATENCY
    //softoff: latency=500ms, residency=3000ms
    int sleep_ms = 8000;
    int sleep_pm_active_ms = 2000;
    struct pm_policy_latency_subscription subs;
    struct pm_policy_latency_request req;
#endif

    printf("Hello World! %s\n", CONFIG_BOARD_TARGET);

#ifdef PM_SLEEP_BY_LATENCY
    dead_loop_cond1();

    pm_policy_latency_changed_subscribe(&subs, on_latency_changed);
    LOG_INF("Setting latency constraint: 30ms");
    pm_policy_latency_request_add(&req, 30000);
    LOG_INF("Sleeping for %d seconds, we should stay ACTIVE due to latency constraint", sleep_ms/1000);
    k_msleep(sleep_ms);

    LOG_INF("Setting latency constraint: 1000ms");
    pm_policy_latency_request_update(&req, 1000000);
    LOG_INF("Sleeping for %d seconds, we should stay ACTIVE due to residency constraint", sleep_pm_active_ms/1000);
    k_msleep(sleep_pm_active_ms);

    LOG_INF("Sleeping for %d seconds, we should enter softoff or susmpend2ram", sleep_ms/1000);
    dead_loop_cond1();
    k_msleep(sleep_ms);

    LOG_ERR("Should not reach here. Now actually reach here because idle timeout are less than softoff or suspend2ram residence");
#endif

	return 0;
}

