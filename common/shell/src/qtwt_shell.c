/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <zephyr/shell/shell.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/pm.h>
#include <zephyr/pm/policy.h>

#include <qapi_wlan_base.h>

extern int32_t g_twt_s2w_extra_compensation_us;
extern void nt_twt_dbg_set_tpe_pm(uint8_t pm);
extern volatile uint32_t g_dbg_sp_start_crash_arm;
extern volatile uint32_t g_dbg_sp_end_pm1_crash_arm;
extern volatile uint32_t g_twt_sp_end_log_enable;

static int cmd_ext_wakeup(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    qapi_Status_t status;
    uint8_t enable = shell_strtoul(argv[1], 10, &err);

    (void)argc;
    if (err || (enable > 1)) {
        shell_error(ctx, "Enable must be 0 or 1");
        return -EINVAL;
    }

    status = qapi_TWT_Ext_Wakeup(enable);
    if (status != QAPI_OK) {
        shell_error(ctx, "TWT external wake request failed: %d", status);
        return -EIO;
    }

    shell_print(ctx, "TWT external wake request queued: %s",
                enable ? "enable" : "disable");
    return 0;
}

static int cmd_s2w_comp(const struct shell *ctx, size_t argc, char **argv)
{
    if (argc < 2) {
        shell_print(ctx, "twt_s2w_comp = %d us", g_twt_s2w_extra_compensation_us);
        return 0;
    }

    int err = 0;
    int32_t val = (int32_t)shell_strtol(argv[1], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse signed us (err %d)", err);
        return err;
    }

    g_twt_s2w_extra_compensation_us = val;
    shell_print(ctx, "twt_s2w_comp set to %d us (takes effect next SP sleep calc)", val);
    return 0;
}

static int cmd_mcu_sleep_enable(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    uint8_t enable = shell_strtoul(argv[1], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse enable (err %d)", err);
        return err;
    }
    qapi_TWT_Mcu_Sleep_Enable(enable);
    shell_print(ctx, "TWT MCU sleep %s", enable ? "enabled (S2RAM lock released)"
                                                : "disabled (S2RAM lock acquired)");
    return 0;
}

static int cmd_tpe_pm_set(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    uint8_t pm = shell_strtoul(argv[1], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse pm (err %d)", err);
        return err;
    }

    shell_print(ctx, "Setting TPE PM bit reg to %u (no TWT/SP involved)", pm);
    nt_twt_dbg_set_tpe_pm(pm);
    return 0;
}

static int cmd_force_crash(const struct shell *ctx, size_t argc, char **argv)
{
    (void)argc; (void)argv;
    shell_print(ctx, "Triggering hard fault for CSR dump...");
    k_busy_wait(200000);
    __asm__ volatile("udf #0");
    return 0;
}

static int cmd_arm_sp_crash(const struct shell *ctx, size_t argc, char **argv)
{
    (void)argc; (void)argv;
    g_dbg_sp_start_crash_arm = 1;
    shell_print(ctx, "Armed: next TWT SP-start (post desc->pm=1) will hard fault");
    return 0;
}

static int cmd_arm_sp_end_pm1_crash(const struct shell *ctx, size_t argc, char **argv)
{
    (void)argc; (void)argv;
    g_dbg_sp_end_pm1_crash_arm = 1;
    shell_print(ctx, "Armed: next TWT SP-end (pre desc->pm=0 write) will hard fault");
    return 0;
}

static int cmd_sp_end_log(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    uint8_t enable = shell_strtoul(argv[1], 10, &err);

    (void)argc;
    if (err || (enable > 1)) {
        shell_error(ctx, "Enable must be 0 or 1");
        return -EINVAL;
    }

    g_twt_sp_end_log_enable = enable;
    shell_print(ctx, "TWT SP-end diagnostic log %s",
                enable ? "enabled" : "disabled");
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(qtwt_cmds,
    SHELL_CMD_ARG(ext_wakeup, NULL,
                  "Enable or disable TWT external wake\n"
                  "Usage: qtwt ext_wakeup <0|1>\n",
                  cmd_ext_wakeup, 2, 0),
    SHELL_CMD_ARG(s2w_comp, NULL,
                  "Get/set extra TWT sleep-to-wake compensation (signed us)\n"
                  "Usage: qtwt s2w_comp [<signed_us>]\n",
                  cmd_s2w_comp, 1, 1),
    SHELL_CMD_ARG(mcu_sleep_enable, NULL,
                  "Enable/disable MCU sleep during TWT SP sleep\n"
                  "Usage: qtwt mcu_sleep_enable 1/0\n",
                  cmd_mcu_sleep_enable, 2, 0),
    SHELL_CMD_ARG(tpe_pm_set, NULL,
                  "Manually set TPE PM bit reg for the connected BSS\n"
                  "Usage: qtwt tpe_pm_set 0/1\n",
                  cmd_tpe_pm_set, 2, 0),
    SHELL_CMD_ARG(force_crash, NULL,
                  "Trigger hard fault for JTAG CSR dump\n"
                  "Usage: qtwt force_crash\n",
                  cmd_force_crash, 1, 0),
    SHELL_CMD_ARG(arm_sp_crash, NULL,
                  "Arm next TWT SP-start to hard fault\n"
                  "Usage: qtwt arm_sp_crash\n",
                  cmd_arm_sp_crash, 1, 0),
    SHELL_CMD_ARG(arm_sp_end_pm1_crash, NULL,
                  "Arm next TWT SP-end (pre pm=0) to hard fault\n"
                  "Usage: qtwt arm_sp_end_pm1_crash\n",
                  cmd_arm_sp_end_pm1_crash, 1, 0),
    SHELL_CMD_ARG(sp_end_log, NULL,
                  "Enable or disable TWT SP-end diagnostic logging\n"
                  "Usage: qtwt sp_end_log <0|1>\n",
                  cmd_sp_end_log, 2, 0),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(qtwt, &qtwt_cmds, "TWT commands", NULL);
