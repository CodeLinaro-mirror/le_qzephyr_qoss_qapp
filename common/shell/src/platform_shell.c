/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#include <qlib_util.h>

#if 0
#define NT_LOG_LVL_INFO 0
/*! @warning condition priority. */
#define NT_LOG_LVL_WARN 1
/*! @error condition priority. */
#define NT_LOG_LVL_ERR 2
/*! @critical condition priority. */
#define NT_LOG_LVL_CRIT 3
#endif
uint8_t min_loglvl = 1; // warn

static int cmd_set_logger_lvl(const struct shell *ctx, size_t argc, char **argv)
{
    if (argc >= 2) {
        int err = 0;
        long rts_val = shell_strtol(argv[1], 10, &err);
        if (err) {
            shell_error(ctx, "Unable to parse input (err %d)", err);
            return err;
        }
        shell_print(ctx, "min_loglvl=%d=>%d for nt_logger", min_loglvl, rts_val);
        min_loglvl = (uint8_t)rts_val;
    }
    return 0;
}

static int cmd_info(const struct shell *ctx, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    shell_print(ctx, "min_loglvl=%d for nt_logger", min_loglvl);
    shell_print(ctx, "g32_dead_loop_1=%d for dead_loop_cond1(), generally used before sleep", g32_dead_loop_1);
    shell_print(ctx, "g32_dead_loop_2=%d for dead_loop_cond2(), generally used after sleep", g32_dead_loop_2);
    return 0;
}

static int cmd_version(const struct shell *ctx, size_t argc, char **argv)
{
    shell_print(ctx, "CRM Number: %s", CONFIG_QCC730_CRM_NUMBER);

    return 0;
}

static int cmd_reboot(const struct shell *ctx, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    shell_print(ctx, "Reboot...");
    nt_system_sw_reset();
    return 0;
}

static int cmd_setdbg(const struct shell *ctx, size_t argc, char **argv)
{
    if (argc != 3) {
        shell_error(ctx, "parameters count not right (cnt %d), should be 3", argc);
        return -EINVAL;
    }

    int err = 0;
    uint32_t dbg_type = shell_strtoul(argv[1], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse input dbg_type (err %d)", err);
        return err;
    }

    uint32_t dbg_value = shell_strtoul(argv[2], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse input dbg_value (err %d)", err);
        return err;
    }

    shell_print(ctx, "dbg_type=%d", dbg_type);
    shell_print(ctx, "dbg_value=%d", dbg_value);

    switch (dbg_type) {
    case 1:
        shell_print(ctx, "set g32_dead_loop_1 %d=>%d", g32_dead_loop_1, dbg_value);
        g32_dead_loop_1 = dbg_value;
        break;
    case 2:
        shell_print(ctx, "set g32_dead_loop_2 %d=>%d", g32_dead_loop_2, dbg_value);
        g32_dead_loop_2 = dbg_value;
        break;
    default:
        shell_warn(ctx, "dbg_type=%d not supported yet", dbg_type);
    }

    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_uart_cmds,
    SHELL_CMD_ARG(version, NULL,
                  "version\n"
                  "Usage: version\n",
                  cmd_version, 1, 0),
    SHELL_CMD_ARG(setloglvl, NULL,
                  "setloglvl\n"
                  "Usage: setloglvl [n], n=0/1/2/3 for info/warn/err/crit for nt_logger\n",
                  cmd_set_logger_lvl, 2, 0),
    SHELL_CMD_ARG(info, NULL,
                  "info\n"
                  "Usage: info\n",
                  cmd_info, 1, 0),
    SHELL_CMD_ARG(reboot, NULL,
                  "reboot\n"
                  "Usage: reboot\n",
                  cmd_reboot, 1, 0),
    SHELL_CMD_ARG(setdbg, NULL,
                  "setdbg\n"
                  "Usage: setdbg [dbg_type:uint32_t] [dbg_value: uint32_t]\n",
                  cmd_setdbg, 3, 0),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(platform, &sub_uart_cmds, "platform commands", NULL);
