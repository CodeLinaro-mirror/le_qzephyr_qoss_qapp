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

LOG_MODULE_REGISTER(app, CONFIG_APP_LOG_LEVEL);

static void on_latency_changed(int32_t latency)
{
    if (latency == SYS_FOREVER_US) {
        LOG_INF("Latency constraint changed: none");
    } else {
        LOG_INF("Latency constraint changed: %" PRId32 "ms",
            latency / USEC_PER_MSEC);
    }
}

static int cmd_set_softoff(const struct shell *shell, size_t argc, char **argv)
{
    shell_print(shell, "Set state to SOFT_OFF...");
    int wkup_src = (int)strtol(argv[1], NULL, 10);
    int slp_time_ms = (int)strtol(argv[2], NULL, 10);
    config_deepsleep(wkup_src, slp_time_ms);

    pm_state_force(0u, &(struct pm_state_info){ PM_STATE_SOFT_OFF, 0, 0 });
    return 0;
}

static int cmd_set_s2ram(const struct shell *shell, size_t argc, char **argv)
{
    shell_print(shell, "Set state to suspend_to_ram...");
    int wkup_src = (int)strtol(argv[1], NULL, 10);
    int slp_time_ms = (int)strtol(argv[2], NULL, 10);
    config_deepsleep(wkup_src, slp_time_ms);

    pm_state_force(0u, &(struct pm_state_info){ PM_STATE_SUSPEND_TO_RAM, 0, 0 });
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_pm_cmds,
    SHELL_CMD_ARG(softoff, NULL,
        "set pm state to soft_of\n"
        "Usage: pm softoff <wkup_src:int> <sleep_time_ms:int>\n"
        "wkup_src: 1:WKUP_AON_TIMER, 2:WKUP_EXT_PIN\n"
        "sleep_time_ms: sleep duration ms when WKUP_AON_TIMER>\n",
        cmd_set_softoff, 3, 0),
    SHELL_CMD_ARG(s2ram, NULL,
        "set pm state to suspend2ram \n"
        "Usage: pm s2ram <wkup_src:int> <sleep_time_ms:int>\n"
        "wkup_src: 1:WKUP_AON_TIMER, 2:WKUP_EXT_PIN\n"
        "sleep_time_ms: sleep duration ms when WKUP_AON_TIMER>\n",
        cmd_set_s2ram, 3, 0),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(pm, &sub_pm_cmds, "power management commands", NULL);

//#define PM_SLEEP_BY_LATENCY

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
    //g32_dead_loop_1 = 1;
    //dead_loop_cond1();

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
    //g32_dead_loop_1 = 1;
    //dead_loop_cond1();
    k_msleep(sleep_ms);

    LOG_ERR("Should not reach here. Now actually reach here because idle timeout are less than softoff or suspend2ram residence");
#endif

	return 0;
}
