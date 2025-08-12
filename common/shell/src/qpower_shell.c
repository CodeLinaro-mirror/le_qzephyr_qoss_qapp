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
    pm_state_force(0u, &(struct pm_state_info){PM_STATE_SOFT_OFF, 0, 0});
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
    pm_state_force(0u, &(struct pm_state_info){PM_STATE_SUSPEND_TO_RAM, 0, 0});
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_pm_cmds,
                               SHELL_CMD_ARG(softoff, NULL,
                                             "set pm state to soft_of\n"
                                             "Usage: softoff <sleep_time_ms:int>\n"
                                             "sleep_time_ms: no timeout if 0>\n",
                                             cmd_set_softoff, 2, 0),
                               SHELL_CMD_ARG(s2ram, NULL,
                                             "set pm state to suspend2ram \n"
                                             "Usage: s2ram <sleep_time_ms:int>\n"
                                             "sleep_time_ms: no timeout if 0>\n",
                                             cmd_set_s2ram, 2, 0),
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(qpm, &sub_pm_cmds, "power management commands", NULL);