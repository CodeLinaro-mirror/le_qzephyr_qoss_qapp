/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>
#include <stdlib.h>
#include <qwifi_api.h>
#include <zephyr/net/net_if.h>
#include <qcom_wifi_mgmt.h>


static int cmd_set_tx_power(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    struct net_if *iface = net_if_get_wifi_sta();
    qapi_WLAN_Set_Txpower_Params_t set_tx_power_cfg;

    if(argc != 3) {
        shell_error(ctx, "Invalid number of arguments");
        return -EINVAL;
    }

    set_tx_power_cfg.txpower = shell_strtoul(argv[1], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse input tx_power (err %d)", err);
        return err;
    }

    set_tx_power_cfg.policy = shell_strtoul(argv[2], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse input policy (err %d)", err);
        return err;
    }

    if(set_tx_power_cfg.txpower > UINT8_MAX) {
        shell_error(ctx, "set tx power to %d fail", set_tx_power_cfg.txpower);
        return -ENOEXEC;
    }

    if(set_tx_power_cfg.policy > QAPI_WLAN_POLLICY_SECURITY_E) {
        shell_error(ctx, "policy not supported");
        return -ENOEXEC;
    }

    if(net_mgmt(NET_REQUEST_WIFI_QCOM_SET_TX_POWER, iface, &set_tx_power_cfg, sizeof(set_tx_power_cfg))) {
        shell_error(ctx, "Set tx power to %d fail", set_tx_power_cfg.txpower);
        return -ENOEXEC;
    } else {
        shell_print(ctx, "Set tx power to %d", set_tx_power_cfg.txpower);
    }

    return 0;
}

static int cmd_get_tx_power(const struct shell *ctx, size_t argc, char **argv)
{
    struct net_if *iface = net_if_get_wifi_sta();
    qapi_WLAN_Get_Power_Evt_t get_tx_power;

    if(net_mgmt(NET_REQUEST_WIFI_QCOM_GET_TX_POWER, iface, &get_tx_power, sizeof(get_tx_power))) {
        shell_error(ctx, "Failed to get tx power");
        return -ENOEXEC;
    }

    return 0;
}

static int cmd_qwifi_unit_test(const struct shell *sh, size_t argc, char **argv)
{
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_unit_test_params params = {0};
    int ret;

    if (argc < 4) {
        shell_error(sh, "Usage: wifi unit_test <vdev_id> <module_id> <num_args> <args...>");
        return -EINVAL;
    }

    params.vdev_id = (uint8_t)atoi(argv[1]);
    params.module_id = (uint8_t)atoi(argv[2]);
    params.num_args = (uint16_t)atoi(argv[3]);

    if (argc != (size_t)(4 + params.num_args)) {
        shell_error(sh, "Error: num_args (%u) expects %u actual arguments after it, but received %d. Usage: wifi unit_test <vdev_id> <module_id> <num_args> <arg1> ... <argN>",
                    params.num_args, params.num_args, (int)argc - 4);
        return -EINVAL;
    }

    for (int i = 0; i < params.num_args && i < 16; ++i) {
        params.args[i] = (uint32_t)atoi(argv[4 + i]);
    }

    ret = net_mgmt(NET_REQUEST_WIFI_QCOM_UNIT_TEST, iface, &params, sizeof(params));

    if (ret) {
        shell_warn(sh, "Unit test dispatch error: vdev=%u module=%u num=%u ret=%d",
                params.vdev_id, params.module_id, params.num_args, ret);
        return -ENOEXEC;
    }
    return 0;
}

static int cmd_set_rts_cts(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_set_rts_cts_params params = {0};

    if (argc != 2) {
        shell_error(ctx, "Invalid number of arguments");
        return -EINVAL;
    }

    params.enable = shell_strtoul(argv[1], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse <enable> (err %d)", err);
        return err;
    }
    if (params.enable != 0 && params.enable != 1) {
        shell_error(ctx, "enable must be 0 or 1");
        return -EINVAL;
    }

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_SET_RTS_CTS, iface, &params, sizeof(params))) {
        shell_error(ctx, "Failed to set RTS/CTS to %u", params.enable);
        return -ENOEXEC;
    }

    shell_print(ctx, "RTS/CTS set to %s", params.enable ? "enabled" : "disabled");
    return 0;
}

static int cmd_get_rts_cts(const struct shell *ctx, size_t argc, char **argv)
{
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_get_rts_cts_params out = {0};

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_GET_RTS_CTS, iface, &out, sizeof(out))) {
        shell_error(ctx, "Failed to get RTS/CTS");
        return -ENOEXEC;
    }

    shell_print(ctx, "RTS/CTS: %s", out.enable ? "enabled" : "disabled");
    return 0;
}

static int cmd_set_rts_rate(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_set_rts_rate_params params = {0};

    if (argc != 2) {
        shell_error(ctx, "Invalid number of arguments");
        return -EINVAL;
    }

    params.rts_rate = shell_strtoul(argv[1], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse <rate_index> (err %d)", err);
        return err;
    }
    if (params.rts_rate > 2) {
        shell_error(ctx, "rate_index must be 0: 1Mbps, 1: 6Mbps, or 2: 12Mbps");
        return -EINVAL;
    }

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_SET_RTS_RATE, iface, &params, sizeof(params))) {
        shell_error(ctx, "Failed to set RTS rate to %u", params.rts_rate);
        return -ENOEXEC;
    }

    shell_print(ctx, "RTS rate set to %u (%s)", params.rts_rate,
                params.rts_rate == 0 ? "1 Mbps" : (params.rts_rate == 1 ? "6 Mbps" : "12 Mbps"));
    return 0;
}

static int cmd_get_rts_rate(const struct shell *ctx, size_t argc, char **argv)
{
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_get_rts_rate_params out = {0};
	uint32_t rts_rate;

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_GET_RTS_RATE, iface, &out, sizeof(out))) {
        shell_error(ctx, "Failed to get RTS rate");
        return -ENOEXEC;
    }

    rts_rate = out.rts_rate == 1 ? 0 : (out.rts_rate == 6 ? 1 : (out.rts_rate == 12 ? 2 : 0xff));
    shell_print(ctx, "RTS rate: %u (%s)", rts_rate,
                rts_rate == 0 ? "1 Mbps" : (rts_rate == 1 ? "6 Mbps" : (rts_rate == 2 ? "12 Mbps" : "unknown")));
    return 0;
}

static int cmd_set_edca_param_cfg(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_set_edca_param_cfg_params params = {0};

    if (argc != 6) {
        shell_error(ctx, "Usage: qwifi set_edca_param <qid> <aifsn> <cw_min> <cw_max> <txop_limit>");
        return -EINVAL;
    }

    params.qid = (uint8_t)shell_strtoul(argv[1], 10, &err);
    if (err) { shell_error(ctx, "parse qid error %d", err); return err; }
    if ((params.qid >= 8) && (params.qid != 0xFF)) {
        shell_error(ctx, "qid must be 0..7 or 255 (0xFF)");
        return -EINVAL;
    }

    params.aifsn = (uint8_t)shell_strtoul(argv[2], 10, &err);
    if (err) { shell_error(ctx, "parse aifsn error %d", err); return err; }

    params.cw_min = (uint16_t)shell_strtoul(argv[3], 10, &err);
    if (err) { shell_error(ctx, "parse cw_min error %d", err); return err; }

    params.cw_max = (uint16_t)shell_strtoul(argv[4], 10, &err);
    if (err) { shell_error(ctx, "parse cw_max error %d", err); return err; }

    params.txop_limit = (uint16_t)shell_strtoul(argv[5], 10, &err);
    if (err) { shell_error(ctx, "parse txop_limit error %d", err); return err; }

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_SET_EDCA_PARAM_CFG, iface, &params, sizeof(params))) {
        shell_error(ctx, "Failed to set EDCA params");
        return -ENOEXEC;
    }

    shell_print(ctx, "EDCA set: qid=%u aifsn=%u cw_min=%u cw_max=%u txop=%u",
                params.qid, params.aifsn, params.cw_min, params.cw_max, params.txop_limit);
    return 0;
}

static int cmd_get_edca_param_cfg(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_get_edca_param_cfg_params out = {0};

    /* Optional qid selector */
    if (argc == 2) {
        out.qid = (uint8_t)shell_strtoul(argv[1], 10, &err);
        if (err) { shell_error(ctx, "parse qid error %d", err); return err; }
    } else {
        out.qid = 0xFF; /* default: all queues when supported */
    }

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_GET_EDCA_PARAM_CFG, iface, &out, sizeof(out))) {
        shell_error(ctx, "Failed to get EDCA params");
        return -ENOEXEC;
    }

    shell_print(ctx, "EDCA: qid=%u aifsn=%u cw_min=%u cw_max=%u txop=%u",
                out.qid, out.aifsn, out.cw_min, out.cw_max, out.txop_limit);
    return 0;
}

static int cmd_set_threshold(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_set_threshold_params params = {0};

    if (argc != 2) {
        shell_error(ctx, "Invalid number of arguments");
        return -EINVAL;
    }

    params.threshold = shell_strtoul(argv[1], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse <threshold> (err %d)", err);
        return err;
    }
    if (params.threshold > 100) {
        shell_error(ctx, "threshold must be <= 100");
        return -EINVAL;
    }

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_SET_THRESHOLD, iface, &params, sizeof(params))) {
        shell_error(ctx, "Failed to set PER threshold to %u", params.threshold);
        return -ENOEXEC;
    }

    shell_print(ctx, "PER threshold set to %u", params.threshold);
    return 0;
}

static int cmd_get_threshold(const struct shell *ctx, size_t argc, char **argv)
{
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_get_threshold_params out = {0};

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_GET_THRESHOLD, iface, &out, sizeof(out))) {
        shell_error(ctx, "Failed to get PER threshold");
        return -ENOEXEC;
    }

    shell_print(ctx, "PER threshold: %u", out.threshold);
    return 0;
}

static int cmd_set_ba_win_timing(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_set_ba_win_timing_params params = {0};

    if (argc != 3) {
        shell_error(ctx, "Usage: qwifi set_ba_win <ack_timeout_us> <delay_cycles>");
        return -EINVAL;
    }

    params.ack_timeout = (uint16_t)shell_strtoul(argv[1], 10, &err);
    if (err) { shell_error(ctx, "parse ack_timeout error %d", err); return err; }
    params.delay = (uint16_t)shell_strtoul(argv[2], 10, &err);
    if (err) { shell_error(ctx, "parse delay error %d", err); return err; }

    if (params.ack_timeout > 4096) {
        shell_error(ctx, "ack_timeout must be <= 4096");
        return -EINVAL;
    }
    if (params.delay > 64) {
        shell_error(ctx, "delay must be <= 64");
        return -EINVAL;
    }

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_SET_BA_WIN_TIMING, iface, &params, sizeof(params))) {
        shell_error(ctx, "Failed to set BA window");
        return -ENOEXEC;
    }

    shell_print(ctx, "BA window set: ack_timeout=%u us, delay=%u", params.ack_timeout, params.delay);
    return 0;
}

static int cmd_get_ba_win_timing(const struct shell *ctx, size_t argc, char **argv)
{
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_get_ba_win_timing_params out = {0};

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_GET_BA_WIN_TIMING, iface, &out, sizeof(out))) {
        shell_error(ctx, "Failed to get BA window");
        return -ENOEXEC;
    }

    shell_print(ctx, "BA window: ack_timeout=%u us, delay=%u", out.ack_timeout, out.delay);
    return 0;
}

static int cmd_set_slot_time(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_set_slot_time_params params = {0};

    if (argc != 2) {
        shell_error(ctx, "Invalid number of arguments");
        return -EINVAL;
    }

    params.slot_time = shell_strtoul(argv[1], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse <slot_time_us> (err %d)", err);
        return err;
    }

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_SET_SLOT_TIME, iface, &params, sizeof(params))) {
        shell_error(ctx, "Failed to set slot time to %u us", params.slot_time);
        return -ENOEXEC;
    }

    shell_print(ctx, "Slot time set to %u us", params.slot_time);
    return 0;
}

static int cmd_get_slot_time(const struct shell *ctx, size_t argc, char **argv)
{
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_get_slot_time_params out = {0};

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_GET_SLOT_TIME, iface, &out, sizeof(out))) {
        shell_error(ctx, "Failed to get slot time");
        return -ENOEXEC;
    }

    shell_print(ctx, "Slot time: %u us", out.slot_time);
    return 0;
}


static int cmd_set_bmiss_threshold(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_set_bmiss_threshold_params params = {0};

    if (argc != 2) {
        shell_error(ctx, "Invalid number of arguments");
        return -EINVAL;
    }

    params.threshold = shell_strtoul(argv[1], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse <threshold> (err %d)", err);
        return err;
    }

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_SET_BMISS_THRESHOLD, iface, &params, sizeof(params))) {
        shell_error(ctx, "Failed to set BMISS threshold to %u", params.threshold);
        return -ENOEXEC;
    }

    shell_print(ctx, "BMISS threshold set to %u", params.threshold);
    return 0;
}

static int cmd_get_bmiss_threshold(const struct shell *ctx, size_t argc, char **argv)
{
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_get_bmiss_threshold_params out = {0};

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_GET_BMISS_THRESHOLD, iface, &out, sizeof(out))) {
        shell_error(ctx, "Failed to get BMISS threshold");
        return -ENOEXEC;
    }

    shell_print(ctx, "BMISS threshold: %u", out.threshold);
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_qwifi_commands,
                               SHELL_CMD_ARG(set_tx_power, NULL,
                                             "Set the transmit power in dbm.\n"
                                             "Usage: <txPower> [<policy = 0:SAFETY>].\n"
                                             "The default policy is SAFETY(SAFETY is the minimum value among reg domain, CTL and target power). Set value to 100 to restore default settings. Tx power range, xpa: 10-SAFETY; ipa:3-SAFETY. Due to limited range in DAC gain with one designated Tx gain index, need to change PowerMode in BDF while setting power\n",
                                             cmd_set_tx_power, 3, 0),
                               SHELL_CMD_ARG(get_tx_power, NULL,
                                             "Get the transmit power, reg_power, target power and CTL power\n",
                                             cmd_get_tx_power, 1, 0),
                               SHELL_CMD_ARG(unit_test, NULL, "Perform Wi-Fi unit test commands.\n"
                                             "Usage: wifi unit_test <vdev_id> <module_id> <num_args> <arg1> ... <argN>\n"
                                             "<vdev_id>: Virtual device ID (e.g., 1 for STA).\n"
                                             "<module_id>: Test module ID (e.g., 4 for ANI).\n"
                                             "<num_args>: Number of arguments that follow this parameter (N).\n"
                                             "<arg1> ... <argN>: The N actual arguments for the test command.\n"
                                             "Example: To enable ANI\n"
                                             "  qwifi unit_test 1 4 2 3 1\n"
                                             "Example: To disable ANI\n"
                                             "  qwifi unit_test 1 4 1 0\n"
                                             "Example: To perform HW readouts\n"
                                             "  qwifi unit_test 1 4 1 12\n"
                                             "Note: Ensure the count in <num_args> exactly matches the number of <arg>s provided.", cmd_qwifi_unit_test, 4, 20),
                               SHELL_CMD_ARG(set_rts, NULL,
                                             "Enable/disable RTS/CTS protection.\n"
					     "Usage: qwifi set_rts <0 | 1>\n",
                                             cmd_set_rts_cts, 2, 0),
                               SHELL_CMD_ARG(get_rts, NULL,
                                             "Get RTS/CTS protection status.\n"
					     "Usage: qwifi get_rts\n",
                                             cmd_get_rts_cts, 1, 0),
                               SHELL_CMD_ARG(set_rts_rate, NULL,
                                             "Set RTS control frame rate (2.4 GHz).\n"
					     "Usage: qwifi set_rts_rate <0: 1Mbps | 1: 6Mbps | 2: 12Mbps>\n",
                                             cmd_set_rts_rate, 2, 0),
                               SHELL_CMD_ARG(get_rts_rate, NULL,
                                             "Get RTS control frame rate (2.4 GHz).\n"
					     "Usage: qwifi get_rts_rate\n",
                                             cmd_get_rts_rate, 1, 0),
                               SHELL_CMD_ARG(set_edca_param, NULL,
                                             "Set EDCA parameters.\n"
					     "Usage: qwifi set_edca_param <qid> <aifsn> <cw_min> <cw_max> <txop_limit>\n",
                                             cmd_set_edca_param_cfg, 6, 0),
                               SHELL_CMD_ARG(get_edca_param, NULL,
                                             "Get EDCA parameters.\n"
					     "Usage: qwifi get_edca_param [<qid>]\n",
                                             cmd_get_edca_param_cfg, 1, 1),
                               SHELL_CMD_ARG(set_threshold, NULL,
                                             "Set PER upper threshold.\n"
					     "Usage: qwifi set_threshold <value>\n",
                                             cmd_set_threshold, 2, 0),
                               SHELL_CMD_ARG(get_threshold, NULL,
                                             "Get PER upper threshold.\n"
					     "Usage: qwifi get_threshold\n",
                                             cmd_get_threshold, 1, 0),
                               SHELL_CMD_ARG(set_ba_timing, NULL,
                                             "Set BA timing parameters.\n"
					     "Usage: qwifi set_ba_timing <ack_timeout_us> <delay_cycles>\n",
                                             cmd_set_ba_win_timing, 3, 0),
                               SHELL_CMD_ARG(get_ba_timing, NULL,
                                             "Get BA timing parameters.\n"
					     "Usage: qwifi get_ba_timing\n",
                                             cmd_get_ba_win_timing, 1, 0),
                               SHELL_CMD_ARG(set_slot_time, NULL,
                                             "Set PHY slot time (e.g. 9 or 20).\n"
					     "Usage: qwifi set_slot_time <slot_time_us>\n",
                                             cmd_set_slot_time, 2, 0),
                               SHELL_CMD_ARG(get_slot_time, NULL,
                                             "Get PHY slot time.\n"
					     "Usage: qwifi get_slot_time\n",
                                             cmd_get_slot_time, 1, 0),
                               SHELL_CMD_ARG(set_bmiss_threshold, NULL,
                                             "Set BMISS threshold.\n"
					     "Usage: qwifi set_bmiss_threshold <value>\n",
                                             cmd_set_bmiss_threshold, 2, 0),
                               SHELL_CMD_ARG(get_bmiss_threshold, NULL,
                                             "Get BMISS threshold.\n"
					     "Usage: qwifi get_bmiss_threshold\n",
                                             cmd_get_bmiss_threshold, 1, 0),
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(qwifi, &sub_qwifi_commands, "qwifi commands", NULL);
