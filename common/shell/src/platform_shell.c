/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>
#include <stdlib.h>
#include <stdbool.h>
#include <nt_sys_monitoring.h>
#include <qlib_util.h>
#if CONFIG_WIFI
#include <qwifi_api.h>
#include <qcom_wifi_mgmt.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#endif
#include <qapi_rram.h>
#include "smps2_low_vbat.h"

#define OTP_MAC_ADDR                    0x1a01c0
#define OTP_MANUFACTURING_YEAR_WEEK     0x1a0260
#define OTP_MODULE_PART_NUMBER          0x1a0264

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

#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
extern struct sys_heap *get_malloc_heap_address(void);
#endif

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

#if CONFIG_WIFI
static bool wifi_is_connected(struct net_if *iface)
{
    if (!iface) {
        return false;
    }
    struct wifi_iface_status status = (struct wifi_iface_status){0};
    int ret = net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &status, sizeof(status));
    if (ret) {
        return false;
    }
    return status.state == WIFI_STATE_ASSOCIATED || status.state == WIFI_STATE_COMPLETED;
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
#endif

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
#if CONFIG_WIFI
    (void)get_boot_reason(ctx);
#endif

    return 0;
}

static int cmd_version(const struct shell *ctx, size_t argc, char **argv)
{
    volatile unsigned char *mac_addr = (unsigned char *)OTP_MAC_ADDR;
    unsigned int manufacturing_year_week = *(unsigned int*)OTP_MANUFACTURING_YEAR_WEEK;
    volatile unsigned char *module_part_number = (unsigned char*)OTP_MODULE_PART_NUMBER;

    shell_print(ctx, "CRM Number: %s", CONFIG_QCC730_CRM_NUMBER);
    shell_print(ctx, "MAC address: %02x:%02x:%02x:%02x:%02x:%02x", mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    shell_fprintf_normal(ctx, "Module Part Number: ");
    uint8_t i = 0;
    while (module_part_number[i] && module_part_number[i] != 0x03) {
        shell_fprintf_normal(ctx, "%c", module_part_number[i]);
        i++;
    }
    shell_print(ctx, "");
    shell_print(ctx, "Manufacturing year: %d, week: %d", (manufacturing_year_week >> 8) & 0xff, manufacturing_year_week & 0xff);
    shell_print(ctx, "BOM configuration: %d", (manufacturing_year_week >> 16) & 0xffff);

    return 0;
}

static int cmd_reboot(const struct shell *ctx, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

#if CONFIG_WIFI
    struct net_if *iface = net_if_get_wifi_sta();
    if (iface != NULL) {
        if (wifi_is_connected(iface)) {
            shell_print(ctx, "Wi-Fi is connected, disconnecting before reboot...");
            int ret = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
            if (ret) {
                shell_warn(ctx, "Wi-Fi disconnect failed (err %d)", ret);
            } else {
                shell_print(ctx, "Wi-Fi disconnected");
		k_sleep(K_MSEC(100)); // Allow disconnect to complete
            }
        } else {
            shell_print(ctx, "Wi-Fi not connected");
        }
    }
#endif

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

static int cmd_rram_read(const struct shell *ctx, size_t argc, char **argv)
{
    int err = -ENOTSUP;

#if CONFIG_WIFI
    err = 0;
    uint32_t partition_id = shell_strtoul(argv[1], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse partition_id (err %d)", err);
        return err;
    }

    uint32_t address = shell_strtoul(argv[2], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse input address (err %d)", err);
        return err;
    }

    uint32_t length = shell_strtoul(argv[3], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse input length (err %d)", err);
        return err;
    }

    uint8_t *buffer = calloc(sizeof(uint8_t), length);
    if (!buffer) {
        shell_error(ctx, "calloc fail. (err %d)", err);
        return -ENOMEM;
    }

    qapi_Status_t ret = qapi_rram_read(partition_id, address, buffer, length);
    if (ret != QAPI_OK) {
        err = -EINVAL;
        shell_error(ctx, "qapi_rram_read. (ret %d)", ret);
        goto free_buffer;
    }

    shell_hexdump(ctx, buffer, length);

free_buffer:
    free(buffer);

#endif /* CONFIG_WIFI */

    return err;
}

static int cmd_rram_write(const struct shell *ctx, size_t argc, char **argv)
{
    int err = -ENOTSUP;

#if CONFIG_WIFI
    err = 0;
    uint32_t partition_id = shell_strtoul(argv[1], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse partition_id (err %d)", err);
        return err;
    }

    uint32_t address = shell_strtoul(argv[2], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse input address (err %d)", err);
        return err;
    }

    char *buf = argv[3];
    size_t buf_length = strlen(buf);
    size_t hex_length = (buf_length + 1) / 2;

    uint8_t *buffer = calloc(sizeof(uint8_t), hex_length);
    if (!buffer) {
        shell_error(ctx, "calloc fail. (err %d)", err);
        return -ENOMEM;
    }

    size_t n = hex2bin(buf, buf_length, buffer, hex_length);
    if (n == 0) {
        shell_error(ctx, "invalid contents");
        err = -EINVAL;
        goto free_buffer;
    }

    qapi_Status_t ret = qapi_rram_write(partition_id, address, buffer, hex_length);
    if (ret != QAPI_OK) {
        err = -EINVAL;
        shell_error(ctx, "qapi_rram_write. (ret %d)", ret);
        goto free_buffer;
    }

free_buffer:
    free(buffer);

#endif /* CONFIG_WIFI */

    return err;
}

static int cmd_smps2_set_pfm_temp(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    int32_t t0 = (int32_t)shell_strtol(argv[1], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse t0 (err %d)", err);
        return err;
    }
    int32_t t1 = (int32_t)shell_strtol(argv[2], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse t1 (err %d)", err);
        return err;
    }
    int32_t t2 = (int32_t)shell_strtol(argv[3], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse t2 (err %d)", err);
        return err;
    }
    int32_t t3 = (int32_t)shell_strtol(argv[4], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse t3 (err %d)", err);
        return err;
    }

    if (!smps2_set_pfm_temp_thresholds(t0, t1, t2, t3)) {
        shell_error(ctx,
                    "smps2_set_pfm_temp: invalid thresholds [%d %d %d %d] "
                    "— must be strictly increasing",
                    t0, t1, t2, t3);
        return -EINVAL;
    }

    shell_print(ctx, "smps2 PFM temp thresholds set to [%d, %d, %d, %d] C",
                t0, t1, t2, t3);
    return 0;
}

static int cmd_smps2_get_pfm_temp(const struct shell *ctx, size_t argc, char **argv)
{
    int32_t thresholds[PFM_TEMP_COLS];

    smps2_get_pfm_temp_thresholds(thresholds);

    shell_print(ctx,
                "smps2 PFM temp thresholds (upper-bound per column):\n"
                "  col0 (<=t0): %d C\n"
                "  col1 (<=t1): %d C\n"
                "  col2 (<=t2): %d C\n"
                "  col3 (<=t3): %d C",
                thresholds[0], thresholds[1], thresholds[2], thresholds[3]);
    return 0;
}

static int cmd_smps2_set_guardband(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    int32_t  temp_gb = (int32_t)shell_strtol(argv[1], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse temp_gb (err %d)", err);
        return err;
    }
    uint32_t vbat_gb = (uint32_t)shell_strtoul(argv[2], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse vbat_gb (err %d)", err);
        return err;
    }

    smps2_set_pfm_guardband(temp_gb, vbat_gb);
    shell_print(ctx, "smps2 PFM guard-band set: temp_gb=%d C, vbat_gb=%u mV",
                temp_gb, vbat_gb);
    return 0;
}

static int cmd_smps2_get_guardband(const struct shell *ctx, size_t argc, char **argv)
{
    int32_t  temp_gb = 0;
    uint32_t vbat_gb = 0;

    smps2_get_pfm_guardband(&temp_gb, &vbat_gb);
    shell_print(ctx,
                "smps2 PFM guard-band:\n"
                "  temp_gb: %d C  (added to measured temp before lookup)\n"
                "  vbat_gb: %u mV (subtracted from measured Vbat before lookup)",
                temp_gb, vbat_gb);
    return 0;
}

static int cmd_smps2_set_verbose(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    uint32_t verbose = (uint32_t)shell_strtoul(argv[1], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse verbose (err %d)", err);
        return err;
    }

    smps2_set_log_verbose(verbose ? true : false);
    shell_print(ctx, "smps2 verbose logging: %s",
                verbose ? "ON (ERR level)" : "OFF (INFO level)");
    return 0;
}

static int cmd_smps2_get_verbose(const struct shell *ctx, size_t argc, char **argv)
{
    shell_print(ctx, "smps2 verbose logging: %s)",
                smps2_get_log_verbose() ? "ON (ERR level)" : "OFF (INFO level)");
    return 0;
}

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
    SHELL_CMD_ARG(rram_read, NULL,
                  "rram_read\n"
                  "Usage: rram_read [partition_id:uint32_t] [address:uint32_t] [length:uint32_t]\n"
                  "Examples:\n"
                  "\trram_read 4 0 4\n",
                  cmd_rram_read, 4, 0),
    SHELL_CMD_ARG(rram_write, NULL,
                  "rram_write\n"
                  "Usage: rram_write [partition_id:uint32_t] [address:uint32_t] [contents:hex format string]\n"
                  "Examples:\n"
                  "\trram_write 4 0 \"deadbeef\"\n",
                  cmd_rram_write, 4, 0),
    SHELL_CMD_ARG(smps2_set_pfm_temp, NULL,
                    "Set SMPS2 PFM temperature thresholds (upper-bound per column)\n"
                    "Usage: smps2_set_pfm_temp <t0> <t1> <t2> <t3>\n"
                    "  t0/t1/t2/t3: temperature upper-bounds in °C, must be strictly increasing\n"
                    "  Default: 30 50 70 90\n"
                    "  Example: smps2_set_pfm_temp 30 50 70 90\n",
                    cmd_smps2_set_pfm_temp, 5, 0),
    SHELL_CMD_ARG(smps2_get_pfm_temp, NULL,
                    "Get current SMPS2 PFM temperature thresholds\n"
                    "Usage: smps2_get_pfm_temp\n",
                    cmd_smps2_get_pfm_temp, 1, 0),
    SHELL_CMD_ARG(smps2_set_guardband, NULL,
                    "Set SMPS2 PFM guard-band values\n"
                    "Usage: smps2_set_guardband <temp_gb_c> <vbat_gb_mv>\n"
                    "  temp_gb_c : temperature guard-band in C (added to measured temp; can be negative)\n"
                    "  vbat_gb_mv: Vbatt guard-band in mV (subtracted from measured Vbat; unsigned)\n"
                    "  Default: temp_gb_c=5, vbat_gb_mv=50\n"
                    "  Example: smps2_set_guardband 5 50\n"
                    "  Disable guard-band: smps2_set_guardband 0 0\n",
                    cmd_smps2_set_guardband, 3, 0),
    SHELL_CMD_ARG(smps2_get_guardband, NULL,
                    "Get current SMPS2 PFM guard-band values\n"
                    "Usage: smps2_get_guardband\n",
                    cmd_smps2_get_guardband, 1, 0),
    SHELL_CMD_ARG(smps2_set_verbose, NULL,
                    "Enable/disable SMPS2 verbose (ERR-level) logging\n"
                    "Usage: smps2_set_verbose <0|1>\n"
                    "  0: quiet — decision/state-change logs use INFO (default)\n"
                    "  1: verbose — decision/state-change logs use ERR\n"
                    "  Example: smps2_set_verbose 1\n",
                    cmd_smps2_set_verbose, 2, 0),
    SHELL_CMD_ARG(smps2_get_verbose, NULL,
                    "Get current SMPS2 verbose logging state\n"
                    "Usage: smps2_get_verbose\n",
                    cmd_smps2_get_verbose, 1, 0),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(platform, &sub_uart_cmds, "platform commands", NULL);
