/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>
#include <zephyr/pm/pm.h>
#include <zephyr/kernel.h>
#include <stdio.h>
#include "qpower.h"
#include "wifi_fw_cpr_driver.h"
#include <zephyr/pm/policy.h>

static void s2ram_timer_cb(struct k_timer *timer);
K_TIMER_DEFINE(s2ram_timer, s2ram_timer_cb, NULL);

static int slp_count = 1;
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
    return 0;
}

static void s2ram_timer_cb(struct k_timer *timer)
{
   early_printk("s2ram_timer_cb\r\n");
   if(slp_count>0)
   {
        slp_count--;
   }
   if(slp_count == 0)
   {
        k_timer_stop(&s2ram_timer);
        pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_RAM,PM_ALL_SUBSTATES);
   }
}


static int cmd_set_s2ram(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    uint32_t slp_time_ms = shell_strtoul(argv[1], 10, &err);
    slp_count = shell_strtoul(argv[2], 10, &err);

    if (err) {
        shell_error(ctx, "Unable to parse input slp_time_ms (err %d)", err);
        return err;
    }

    shell_print(ctx, "allow state to suspend_to_ram %u ms",slp_time_ms);

    if(slp_time_ms)
    {
        k_timer_start(&s2ram_timer, K_MSEC(slp_time_ms), K_MSEC(slp_time_ms));
    }
    pm_policy_state_lock_put(PM_STATE_SUSPEND_TO_RAM,PM_ALL_SUBSTATES);
    
    return 0;
}

#ifdef CONFIG_CPR_ENABLE
static int cmd_set_cpr(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    uint32_t enable = shell_strtoul(argv[1], 10, &err);

    if (err) {
        shell_error(ctx, "Unable to parse input slp_time_ms (err %d)", err);
        return err;
    }

    shell_print(ctx, "%s cpr ...", enable?"Enable":"Disable");
    if (enable) {
        wifi_fw_cpr_reenable();
    } else {
        wifi_fw_cpr_disable();
    }
    return 0;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(sub_pm_cmds,
                               SHELL_CMD_ARG(softoff, NULL,
                                             "set pm state to soft_of\n"
                                             "Usage: softoff <sleep_time_ms:int>\n"
                                             "sleep_time_ms: no timeout if 0>\n",
                                             cmd_set_softoff, 2, 0),
                               SHELL_CMD_ARG(s2ram, NULL,
                                             "set pm state to suspend2ram \n"
                                             "Usage: s2ram <sleep_time_ms:int> <sleep_count:int>\n"
                                             "sleep_time_ms: no timeout if 0>\n sleep_count: the count of sleep-exist loop\n",
                                             cmd_set_s2ram, 2, 1),
#ifdef CONFIG_CPR_ENABLE
                               SHELL_CMD_ARG(cpr_set, NULL,
                                             "enable or disable cpr\n"
                                             "Usage: cpr_set <set:int>\n"
                                             "set: enable if 1 disable if 0>\n",
                                             cmd_set_cpr, 2, 0),
#endif
                                
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(qpm, &sub_pm_cmds, "power management commands", NULL);
