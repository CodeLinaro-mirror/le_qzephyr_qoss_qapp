/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#include <qlib_util.h>
#include <qwifi_api.h>
#include <qcom_wifi_mgmt.h>

#if CONFIG_MBEDTLS_USE_PKA
#include "self_test.h"
#endif
#if 0
#define NT_LOG_LVL_INFO 0
/*! @warning condition priority. */
#define NT_LOG_LVL_WARN 1
/*! @error condition priority. */
#define NT_LOG_LVL_ERR 2
/*! @critical condition priority. */
#define NT_LOG_LVL_CRIT 3
#endif
extern uint8_t min_loglvl; // warn

static int cmd_set_logger_lvl(const struct shell *ctx, size_t argc, char **argv)
{
    if (argc < 2) {
        shell_print(ctx, "Usage: setloglvl [n]");
        shell_print(ctx, "n=0/1/2/3 for INFO/WARN/ERR/CRIT for nt_logger");
        shell_print(ctx, "Current min_loglvl=%d", min_loglvl);
        return -EINVAL;
    }

    int err = 0;
    long rts_val = shell_strtol(argv[1], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse input (err %d)", err);
        return err;
    }

    if (rts_val < 0 || rts_val > 3) {
        shell_error(ctx, "Invalid level: %ld. Allowed values: 0=INFO, 1=WARN, 2=ERR, 3=CRIT", rts_val);
        return -EINVAL;
    }

    static const char * const lvl_names[] = {"INFO", "WARN", "ERR", "CRIT"};
    shell_print(ctx, "nt_logger level: %s (%ld)", lvl_names[rts_val], rts_val);
    shell_print(ctx, "min_loglvl=%d=>%ld", min_loglvl, rts_val);
    min_loglvl = (uint8_t)rts_val;

    return 0;
}

static int32_t get_boot_reason(const struct shell *ctx)
{
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_get_boot_reason_params out =
	    (struct qcom_wifi_get_boot_reason_params){0};
    char data[65] = {0};
    size_t pos = 0;

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_GET_BOOT_REASON, iface, &out, sizeof(out))) {
        shell_error(ctx, "Failed to get boot reason");
        return -ENOEXEC;
    }

    if (out.boot_reason == QAPI_BOOT_REASON_COLD_BOOT) {
        pos += snprintk(data + pos, sizeof(data) - pos, "Boot from cold boot");
    } else if (out.boot_reason == QAPI_BOOT_REASON_DTIM_SLEEP) {
        pos += snprintk(data + pos, sizeof(data) - pos, "Boot from dtim sleep");
    } else if (out.boot_reason == QAPI_BOOT_REASON_DEEP_SLEEP) {
        pos += snprintk(data + pos, sizeof(data) - pos, "Boot from deep sleep");
    } else {
        pos += snprintk(data + pos, sizeof(data) - pos, "Unknown status 0x%08x", out.boot_reason);
    }

    shell_print(ctx, "Status: %s", data);
    return 0;
}

#if K_HEAP_MEM_POOL_SIZE > 0 && defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
extern struct sys_heap _system_heap;
#endif

static int get_kernel_heap(const struct shell *ctx)
{
#if K_HEAP_MEM_POOL_SIZE > 0 && defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
	int err;
	struct sys_memory_stats stats;

	err = sys_heap_runtime_stats_get(&_system_heap, &stats);
	if (err) {
		shell_error(ctx, "Failed to read kernel system heap statistics (err %d)", err);
		return -ENOEXEC;
	}

	shell_print(ctx, "sys heap:");
	shell_print(ctx, "free bytes:           %zu", stats.free_bytes);
	shell_print(ctx, "allocated bytes:      %zu", stats.allocated_bytes);
	shell_print(ctx, "max allocated bytes:  %zu", stats.max_allocated_bytes);
#endif /* K_HEAP_MEM_POOL_SIZE > 0 */

	return 0;
}

static int32_t get_z_malloc_heap_statistics(const struct shell *ctx)
{
#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
	struct sys_memory_stats stats;
	struct sys_heap * malloc_heap = NULL;
	int err;

	malloc_heap = get_malloc_heap_address();
	if (!malloc_heap) {
		shell_error(ctx, "Failed to get malloc heap address");
		return -ENOEXEC;
	}

	err = sys_heap_runtime_stats_get(malloc_heap, &stats);
	if (err) {
		shell_error(ctx, "Failed to read malloc heap statistics (err %d)", err);
		return -ENOEXEC;
	}

	shell_print(ctx, "c-library heap:");
	shell_print(ctx, "free bytes:           %zu", stats.free_bytes);
	shell_print(ctx, "allocated bytes:      %zu", stats.allocated_bytes);
	shell_print(ctx, "max allocated bytes:  %zu", stats.max_allocated_bytes);

	return 0;
#else
	shell_print(ctx, "Heap runtime stats disabled or unsupported in this configuration");
	return -ENOTSUP;
#endif
}

static int cmd_info(const struct shell *ctx, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_print(ctx, "==== Platform Info ====");
    shell_print(ctx, "min_loglvl=%d for nt_logger", min_loglvl);
    shell_print(ctx, "g32_dead_loop_1=%d for dead_loop_cond1(), generally used before sleep", g32_dead_loop_1);
    shell_print(ctx, "g32_dead_loop_2=%d for dead_loop_cond2(), generally used after sleep", g32_dead_loop_2);

    (void)get_kernel_heap(ctx);
    (void)get_z_malloc_heap_statistics(ctx);
    (void)get_boot_reason(ctx);

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
        shell_print(ctx, "Usage: setdbg [dbg_type:uint32_t] [dbg_value:uint32_t]");
        shell_print(ctx, "Supported dbg_type: 1=g32_dead_loop_1, 2=g32_dead_loop_2");
        shell_print(ctx, "Examples:");
        shell_print(ctx, "  setdbg 1 0   # disable dead_loop_cond1()");
        shell_print(ctx, "  setdbg 2 1   # enable dead_loop_cond2()");
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
        shell_print(ctx, "Supported dbg_type: 1=g32_dead_loop_1, 2=g32_dead_loop_2");
    }

    return 0;
}

#if CONFIG_MBEDTLS_USE_PKA
static int cmd_pka_test(const struct shell *ctx, size_t argc, char **argv)
{
    if (argc != 3) {
        shell_error(ctx, "parameters count not right (cnt %d), should be 3", argc);
        return -EINVAL;
    }

    int err = 0;
    int verbose = shell_strtoul(argv[2], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse input verbose (err %d)", err);
        return err;
    }

    if (strcmp(argv[1], "all") == 0) {
        /* MPI Test */
        shell_print(ctx, "MPI Test");
        if (mbedtls_mpi_pka_self_test(verbose) != 0)
            shell_error(ctx, "Test Fail");
        else
            shell_print(ctx, "Test Pass");

        /* RSA Test */
        shell_print(ctx, "RSA Test");

        if (mbedtls_rsa_pka_self_test(verbose) != 0)
            shell_error(ctx, "Test Fail");
        else
            shell_print(ctx, "Test Pass");

        /* DH Test */
        shell_print(ctx, "DH Test");

        if (mbedtls_dhm_pka_self_test(verbose) != 0)
            shell_error(ctx, "Test Fail");
        else
            shell_print(ctx, "Test Pass");

        /* ECP Test */
        shell_print(ctx, "ECP Test");

        if (mbedtls_ecp_pka_self_test(verbose) != 0)
            shell_error(ctx, "Test Fail");
        else
            shell_print(ctx, "Test Pass");

        /* ECDSA Test */
        shell_print(ctx, "ECDSA Test");

        if (mbedtls_ecdsa_pka_self_test(verbose) != 0)
            shell_error(ctx, "Test Fail");
        else
            shell_print(ctx, "Test Pass");

        return 0;
    } else if (strcmp(argv[1], "mpi") == 0) {
        shell_print(ctx, "MPI Test");

        if (mbedtls_mpi_pka_self_test(verbose) != 0)
            shell_error(ctx, "Test Fail");
        else
            shell_print(ctx, "Test Pass");

        return 0;
    } else if (strcmp(argv[1], "rsa") == 0) {
        shell_print(ctx, "RSA Test");

        if (mbedtls_rsa_pka_self_test(verbose) != 0)
            shell_error(ctx, "Test Fail");
        else
            shell_print(ctx, "Test Pass");

        return 0;
    } else if (strcmp(argv[1], "dh") == 0) {
        shell_print(ctx, "DH Test");

        if (mbedtls_dhm_pka_self_test(verbose) != 0)
            shell_error(ctx, "Test Fail");
        else
            shell_print(ctx, "Test Pass");

        return 0;

    } else if (strcmp(argv[1], "ecp") == 0) {
        shell_print(ctx, "ECP Test");

        if (mbedtls_ecp_pka_self_test(verbose) != 0)
            shell_error(ctx, "Test Fail");
        else
            shell_print(ctx, "Test Pass");

        return 0;
    } else if (strcmp(argv[1], "ecdsa") == 0) {
        shell_print(ctx, "ECDSA Test");

        if (mbedtls_ecdsa_pka_self_test(verbose) != 0)
            shell_error(ctx, "Test Fail");
        else
            shell_print(ctx, "Test Pass");

        return 0;
    } else {
        shell_error(ctx, "Invalid test type '%s'. Valid types: mpi, rsa, dh, ecp, ecdsa", argv[1]);
        return -EINVAL;
    }
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_uart_cmds,
    SHELL_CMD_ARG(version, NULL,
                  "version\n"
                  "Usage: version\n"
                  "Description: Print CRM build number\n",
                  cmd_version, 1, 0),
    SHELL_CMD_ARG(setloglvl, NULL,
                  "setloglvl\n"
                  "Usage: setloglvl [n], n=0/1/2/3 for info/warn/err/crit for nt_logger\n"
                  "Examples:\n"
		  "setloglvl 0  # INFO\n"
		  "setloglvl 1  # WARN\n"
		  "setloglvl 2  # ERR\n"
		  "setloglvl 3  # CRIT\n",
		  cmd_set_logger_lvl, 2, 0),
    SHELL_CMD_ARG(info, NULL,
                  "info\n"
                  "Usage: info\n"
                  "Description: Show platform runtime info (logger level, debug flags, heap stats, boot reason)\n",
                  cmd_info, 1, 0),
    SHELL_CMD_ARG(reboot, NULL,
                  "reboot\n"
                  "Usage: reboot\n"
                  "Description: Reboot the system immediately\n",
                  cmd_reboot, 1, 0),
    SHELL_CMD_ARG(setdbg, NULL,
                  "setdbg\n"
                  "Usage: setdbg [dbg_type:uint32_t] [dbg_value:uint32_t]\n"
                  "Supported dbg_type: 1=g32_dead_loop_1, 2=g32_dead_loop_2\n"
                  "Examples:\n"
                  "setdbg 1 0   # disable dead_loop_cond1()\n"
                  "setdbg 2 1   # enable dead_loop_cond2()\n",
                  cmd_setdbg, 3, 0),
#if CONFIG_MBEDTLS_USE_PKA
    SHELL_CMD_ARG(pka_test, NULL,
                  "pka_test\n"
                  "Usage: pka_test\n",
                  cmd_pka_test, 3, 0),
#endif
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(platform, &sub_uart_cmds, "platform commands", NULL);
