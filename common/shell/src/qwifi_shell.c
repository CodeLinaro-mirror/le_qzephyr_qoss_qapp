/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>
#include <qwifi_api.h>
#include <zephyr/net/net_if.h>
#include <qcom_wifi_mgmt.h>
#include <stdlib.h>
#include <zephyr/net/wifi_utils.h>
#include <wlan_lib_version.h>

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

static int cmd_set_aggregation(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_set_aggregation_params params = {0};

    if (argc != 3) {
        shell_error(ctx, "Usage: qwifi set_aggregation <tx_tid_mask> <rx_tid_mask>");
        return -EINVAL;
    }

    uint32_t tx = shell_strtoul(argv[1], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse <tx_tid_mask> (err %d)", err);
        return err;
    }

    uint32_t rx = shell_strtoul(argv[2], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse <rx_tid_mask> (err %d)", err);
        return err;
    }

    if (tx > 0xFF || rx > 0xFF) {
        shell_error(ctx, "The MAX value of tx_tid_mask and rx_tid_mask is 0xFF");
        return -EINVAL;
    }

    params.tx_tid_mask = (uint8_t)tx;
    params.rx_tid_mask = (uint8_t)rx;

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_SET_AGGREGATION, iface, &params, sizeof(params))) {
        shell_error(ctx, "Failed to set aggregation TIDs: tx=0x%02x rx=0x%02x", params.tx_tid_mask, params.rx_tid_mask);
        return -ENOEXEC;
    }

    shell_print(ctx, "Aggregation TIDs set: tx=0x%02x rx=0x%02x", params.tx_tid_mask, params.rx_tid_mask);
    return 0;
}

static int cmd_set_amsdu_rx(const struct shell *ctx, size_t argc, char **argv)
{
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_set_amsdu_rx_params params = {0};

    if (argc != 3) {
        shell_error(ctx, "Usage: qwifi set_amsdu rx <enable|disable>");
        return -EINVAL;
    }

    if (strcmp(argv[1], "rx") != 0) {
        shell_error(ctx, "Parameter should be 'rx'");
        return -EINVAL;
    }

    if (!strcmp(argv[2], "enable")) {
        params.enable = 1;
    } else if (!strcmp(argv[2], "disable")) {
        params.enable = 0;
    } else {
        shell_error(ctx, "Second parameter must be 'enable' or 'disable'");
        return -EINVAL;
    }

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_SET_AMSDU_RX, iface, &params, sizeof(params))) {
        shell_error(ctx, "Failed to set AMSDU RX to %s", params.enable ? "enable" : "disable");
        return -ENOEXEC;
    }

    shell_print(ctx, "AMSDU RX %s", params.enable ? "enabled" : "disabled");
    return 0;
}

static int cmd_set_phy_mode(const struct shell *ctx, size_t argc, char **argv)
{
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_set_phy_mode_params params = {0};
    const char *wmode;

    if (argc != 2) {
        shell_error(ctx, "Usage: qwifi set_phy_mode <a|b|g|ng|abgn>");
        return -EINVAL;
    }

    wmode = argv[1];

    if (!strcmp(wmode, "a")) {
        params.phy_mode = QAPI_WLAN_11A_MODE_E;
    } else if (!strcmp(wmode, "b")) {
        params.phy_mode = QAPI_WLAN_11B_MODE_E;
    } else if (!strcmp(wmode, "g")) {
        params.phy_mode = QAPI_WLAN_11G_MODE_E;
    } else if (!strcmp(wmode, "ng")) {
        params.phy_mode = QAPI_WLAN_11NG_HT20_MODE_E;
    } else if (!strcmp(wmode, "abgn")) {
        params.phy_mode = QAPI_WLAN_11ABGN_HT20_MODE_E;
    } else {
        shell_error(ctx, "Unknown mode '%s', supported: a/b/g/ng/abgn", wmode);
        return -EINVAL;
    }

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_SET_PHY_MODE, iface, &params, sizeof(params))) {
        shell_error(ctx, "Failed to set PHY mode to %s", wmode);
        return -ENOEXEC;
    }

    shell_print(ctx, "PHY mode set to %s", wmode);
    return 0;
}

static int cmd_get_phy_mode(const struct shell *ctx, size_t argc, char **argv)
{
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_get_phy_mode_params out = {0};
    const char *mode_str = "unknown";

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_GET_PHY_MODE, iface, &out, sizeof(out))) {
        shell_error(ctx, "Failed to get PHY mode");
        return -ENOEXEC;
    }

    switch (out.phy_mode) {
    case QAPI_WLAN_11A_MODE_E:            mode_str = "a";    break;
    case QAPI_WLAN_11B_MODE_E:            mode_str = "b";    break;
    case QAPI_WLAN_11G_MODE_E:            mode_str = "g";    break;
    case QAPI_WLAN_11NG_HT20_MODE_E:      mode_str = "ng";   break;
    case QAPI_WLAN_11ABGN_HT20_MODE_E:    mode_str = "abgn"; break;
    default:
        mode_str = "unknown";
        break;
    }

    shell_print(ctx, "PHY mode: %s (enum=%u)", mode_str, out.phy_mode);
    return 0;
}

static int cmd_set_rate(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_set_rate_params set_rate_cfg;

    memset(&set_rate_cfg, 0, sizeof(set_rate_cfg));

    if (argc == 2 && !strcmp(argv[1], "auto")) {
        set_rate_cfg.ra_ON = 1;
    } else if (argc == 5) {
        set_rate_cfg.ra_ON = 0;

        set_rate_cfg.rate_staid = (uint32_t)shell_strtoul(argv[1], 10, &err);
        if (err) { shell_error(ctx, "Unable to parse <staid> (err %d)", err); return err; }

        set_rate_cfg.rate_p_rate = (uint32_t)shell_strtoul(argv[2], 10, &err);
        if (err) { shell_error(ctx, "Unable to parse <p_rate> (err %d)", err); return err; }

        set_rate_cfg.rate_s_rate = (uint32_t)shell_strtoul(argv[3], 10, &err);
        if (err) { shell_error(ctx, "Unable to parse <s_rate> (err %d)", err); return err; }

        set_rate_cfg.rate_t_rate = (uint32_t)shell_strtoul(argv[4], 10, &err);
        if (err) { shell_error(ctx, "Unable to parse <t_rate> (err %d)", err); return err; }
    } else {
        shell_error(ctx, "Usage: qwifi set_rate auto | <staid> <p_rate> <s_rate> <t_rate>");
        return -EINVAL;
    }

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_SET_RATE, iface, &set_rate_cfg, sizeof(set_rate_cfg))) {
        shell_error(ctx, "Failed to set rate");
        return -ENOEXEC;
    }

    shell_print(ctx, "Rate set%s", set_rate_cfg.ra_ON ? " (auto)" : "");
    return 0;
}

static int cmd_get_rate(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_set_rate_params set_rate_cfg;

    memset(&set_rate_cfg, 0, sizeof(set_rate_cfg));

    if (argc != 2) {
        shell_error(ctx, "Usage: qwifi get_rate <staid>");
        return -EINVAL;
    }

    set_rate_cfg.rate_staid = (uint32_t)shell_strtoul(argv[1], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse <staid> (err %d)", err);
        return err;
    }

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_GET_RATE, iface, &set_rate_cfg, sizeof(set_rate_cfg))) {
        shell_error(ctx, "Failed to get rate");
        return -ENOEXEC;
    }

    shell_print(ctx, "Rate: p_rate=%d, s_rate=%d, t_rate=%d",
                set_rate_cfg.rate_p_rate,
                set_rate_cfg.rate_s_rate,
                set_rate_cfg.rate_t_rate);
    return 0;
}

static int cmd_info(const struct shell *ctx, size_t argc, char **argv)
{
    static const struct {
        struct net_if *(*get_iface)(void);
        const char *label;
        const char *devname;
    } ifaces[] = {
        { net_if_get_wifi_sta, "STA", "wlan0" },
        { net_if_get_wifi_sap, "SAP", "wlan1" },
    };
    bool sta_on = net_if_is_carrier_ok(net_if_get_wifi_sta());
    bool sap_on = net_if_is_carrier_ok(net_if_get_wifi_sap());
    bool any_printed = false;

    for (int i = 0; i < ARRAY_SIZE(ifaces); i++) {
        struct net_if *iface = ifaces[i].get_iface();

        if (!net_if_is_carrier_ok(iface)) {
            continue;
        }

        if (any_printed) {
            shell_print(ctx, "");
        }
        shell_print(ctx, "Interface %s (%s)", ifaces[i].devname, ifaces[i].label);
        shell_print(ctx, "==============================");

        /* MAC address */
        struct qcom_wifi_get_mac_address_params mac = {0};

        if (net_mgmt(NET_REQUEST_WIFI_QCOM_GET_MAC_ADDRESS, iface, &mac, sizeof(mac))) {
            shell_error(ctx, "Failed to get %s MAC address", ifaces[i].label);
        } else {
            shell_print(ctx, "  MAC addr  : %02x:%02x:%02x:%02x:%02x:%02x",
                        mac.mac[0], mac.mac[1], mac.mac[2],
                        mac.mac[3], mac.mac[4], mac.mac[5]);
        }

        /* PHY mode */
        struct qcom_wifi_get_phy_mode_params phy = {0};

        if (net_mgmt(NET_REQUEST_WIFI_QCOM_GET_PHY_MODE, iface, &phy, sizeof(phy))) {
            shell_error(ctx, "Failed to get %s PHY mode", ifaces[i].label);
        } else {
            const char *mode_str;

            switch (phy.phy_mode) {
            case QAPI_WLAN_11A_MODE_E:         mode_str = "a";       break;
            case QAPI_WLAN_11B_MODE_E:         mode_str = "b";       break;
            case QAPI_WLAN_11G_MODE_E:         mode_str = "g";       break;
            case QAPI_WLAN_11NG_HT20_MODE_E:   mode_str = "ng";      break;
            case QAPI_WLAN_11ABGN_HT20_MODE_E: mode_str = "abgn";    break;
            default:                           mode_str = "unknown";  break;
            }
            shell_print(ctx, "  PHY mode  : %s (enum=%u)", mode_str, phy.phy_mode);
        }

        /* Power mode */
        struct qcom_wifi_get_power_mode_params pwr = {0};

        if (net_mgmt(NET_REQUEST_WIFI_QCOM_GET_POWER_MODE, iface, &pwr, sizeof(pwr))) {
            shell_error(ctx, "Failed to get %s power mode", ifaces[i].label);
        } else {
            char data[65] = {0};
            size_t pos = 0;

            if (pwr.power_mode == 0) {
                pos += snprintk(data + pos, sizeof(data) - pos, "Max Perf");
            } else {
                pos += snprintk(data + pos, sizeof(data) - pos, "Power Save");
                if (pwr.power_mode & 1) {
                    pos += snprintk(data + pos, sizeof(data) - pos, " (bmps)");
                }
                if (pwr.power_mode & 2) {
                    pos += snprintk(data + pos, sizeof(data) - pos, " (IMPS)");
                }
                if (pwr.power_mode & 4) {
                    pos += snprintk(data + pos, sizeof(data) - pos, " (WUR)");
                }
                if (pwr.power_mode & 8) {
                    pos += snprintk(data + pos, sizeof(data) - pos, " (WNM)");
                }
            }
            shell_print(ctx, "  Power mode: %s", data);
        }

        /* Operation mode */
        if (sta_on && sap_on) {
            shell_print(ctx, "  Op mode   : concurrency (AP+STA)");
        } else {
            struct qcom_wifi_get_operation_mode_params op = {0};

            if (net_mgmt(NET_REQUEST_WIFI_QCOM_GET_OPERATION_MODE, iface, &op, sizeof(op))) {
                shell_error(ctx, "Failed to get %s operation mode", ifaces[i].label);
            } else if (op.opmode == DEV_MODE_STATION_E) {
                shell_print(ctx, "  Op mode   : station");
            } else if (op.opmode == DEV_MODE_AP_E) {
                shell_print(ctx, "  Op mode   : softap");
            } else {
                shell_print(ctx, "  Op mode   : unknown (0x%x)", op.opmode);
            }
        }

        any_printed = true;
    }

    return 0;
}

static int cmd_csa(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    struct net_if *iface;
    struct qcom_wifi_csa_params csa = {0};
    enum wifi_frequency_bands check_bands[] = { WIFI_FREQ_BAND_2_4_GHZ, WIFI_FREQ_BAND_5_GHZ };

    unsigned long v = shell_strtoul(argv[1], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse <mode> (err %d)", err);
        return -EINVAL;
    }
    if (v != 0 && v != 1) {
        shell_error(ctx, "<mode> should be 0 or 1");
        return -EINVAL;
    }
    csa.switch_mode = v;

    v = shell_strtoul(argv[2], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse <channel> (err %d)", err);
        return -EINVAL;
    }

    bool valid_channel = false;
    for (int i = 0; i < sizeof(check_bands) / sizeof(check_bands[0]); i++) {
        if (wifi_utils_validate_chan(check_bands[i], v)) {
            valid_channel = true;
            break;
        }
    }

    if (!valid_channel) {
        shell_error(ctx, "<channel> should be valid in 2.4g or 5g.");
        return -EINVAL;
    }
    csa.new_channel = v;

    v = shell_strtoul(argv[3], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse <count> (err %d)", err);
        return -EINVAL;
    }

    if (v > 255) {
        shell_error(ctx, "<count> should be less than 255.");
        return -EINVAL;
    }
    csa.switch_count = v;

    if (argc < 5) {
        iface = net_if_get_wifi_sap();
    } else {
         unsigned long iface_index = shell_strtoul(argv[4], 10, &err);
        if (err) {
            shell_error(ctx, "Unable to parse iface index (err %d)", err);
            return -EINVAL;
        }
        iface = net_if_get_by_index(iface_index);
    }

    if (!iface) {
        shell_error(ctx, "Get SAP iface fail.");
        return -EINVAL;
    }

    return net_mgmt(NET_REQUEST_WIFI_QCOM_SET_SAP_CSA, iface, &csa, sizeof(csa));
}

static int cmd_wifi_set_operation_mode(const struct shell *ctx, size_t argc, char **argv)
{
    struct net_if *iface;
    struct qcom_wifi_set_op_mode_params set_op_mode_cfg;

    if(argc < 1) {
        shell_error(ctx, "Invalid number of arguments");
        return -EINVAL;
    }

    if (argc >= 3) {
        set_op_mode_cfg.hidden_ssid = argv[2];
    }
    else {
        set_op_mode_cfg.hidden_ssid = "0";
    }
    set_op_mode_cfg.opmode = argv[1];
    if (strcmp(argv[1], "station") == 0) {
        iface = net_if_get_wifi_sta();
    } else {
        iface = net_if_get_wifi_sap();
    }

    if (!iface) {
        shell_error(ctx, "Failed to get wifi iface for mode %s", argv[1]);
        return -ENODEV;
    }

    if(net_mgmt(NET_REQUEST_WIFI_QCOM_SET_OPERATION_MODE, iface, &set_op_mode_cfg, sizeof(set_op_mode_cfg))) {
        shell_error(ctx, "Set op mode to %s fail", set_op_mode_cfg.opmode);
        return -ENOEXEC;
    } else {
        shell_print(ctx, "Set op mode to %s", set_op_mode_cfg.opmode);
    }

    return 0;

}

static int cmd_wifi_set_active_device(const struct shell *ctx, size_t argc, char **argv)
{
    uint16_t deviceId;
    int err = 0;
    struct net_if *iface = net_if_get_wifi_sta();

    if(argc != 2) {
        shell_error(ctx, "Invalid number of arguments");
        return -EINVAL;
    }

    deviceId = shell_strtoul(argv[1], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse input deviceId (err %d)", err);
        return err;
    }

    if (deviceId != 0 && deviceId != 1) {
        shell_error(ctx, "Invaild device id");
        return EINVAL;
    }

    if(net_mgmt(NET_REQUEST_WIFI_QCOM_SET_DEVICE_ID, iface, &deviceId, sizeof(uint16_t))) {
        shell_error(ctx, "Set device id to %s fail", deviceId == 0? "softap":"station");
        return -ENOEXEC;
    } else {
        shell_print(ctx, "Set device id to %s", deviceId == 0? "softap":"station");
    }

    return 0;
}

static int cmd_version(const struct shell *ctx, size_t argc, char **argv)
{
    struct qwifi_wlan_lib_version ver;

    if (qwifi_get_wlan_lib_version(&ver) != 0) {
        shell_error(ctx, "Failed to get WLAN lib version");
        return -ENOEXEC;
    }

    shell_print(ctx, "WLAN lib version: %u.%u.%u.%u",
                ver.major, ver.minor, ver.patch, ver.build);
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
                                             "Note: Ensure the count in <num_args> exactly matches the number of <arg>s provided.\n", cmd_qwifi_unit_test, 4, 20),
                               SHELL_CMD_ARG(set_operation_mode, NULL,
                                             "Set operation mode.\n"
                                             "Usage: qwifi set_operation_mode <ap|station|ap_sta> [<hidden|0>] \n"
                                             "Example: Set operation mode to station\n"
                                             "  qwifi set_operation_mode station \n"
                                             "Example: Set operation mode to soft ap \n"
                                             "  qwifi set_operation_mode ap \n"
                                             "Example: Enable ap+sta concurrency mode\n"
                                             "  qwifi set_operation_mode ap_sta \n"
                                             "Example: To hide ssid \n"
                                             "  qwifi set_operation_mode ap hidden \n",
                                             cmd_wifi_set_operation_mode, 2, 1),
                               SHELL_CMD_ARG(set_device, NULL,
                                             "Set Active Device.\n"
                                             "Usage: qwifi set_device [0 : soft ap | 1: station] \n",
                                             cmd_wifi_set_active_device, 2, 0),
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
                               SHELL_CMD_ARG(set_aggregation, NULL,
                                             "Set TX/RX aggregation TID bitmasks.\n"
					     "Usage: qwifi set_aggregation <tx_tid_mask> <rx_tid_mask>\n"
                                             "Each mask is 8-bit (0..0xFF); bit i enables aggregation for TID i (0..7).\n",
                                             cmd_set_aggregation, 3, 0),
                               SHELL_CMD_ARG(set_amsdu, NULL,
                                             "Enable/disable AMSDU RX.\n"
					     "Usage: qwifi set_amsdu rx <enable|disable>\n",
                                             cmd_set_amsdu_rx, 3, 0),
                               SHELL_CMD_ARG(set_phy_mode, NULL,
                                             "Set PHY mode.\n"
					     "Usage: qwifi set_phy_mode <a|b|g|ng|abgn>\n",
                                             cmd_set_phy_mode, 2, 0),
                               SHELL_CMD_ARG(get_phy_mode, NULL,
                                             "Get PHY mode.\n"
					     "Usage: qwifi get_phy_mode\n",
                                             cmd_get_phy_mode, 1, 0),
                               SHELL_CMD_ARG(set_rate, NULL,
                                             "Set data rate.\n"
					     "Usage: qwifi set_rate auto | <staid> <p_rate> <s_rate> <t_rate>\n",
                                             cmd_set_rate, 2, 3),
                               SHELL_CMD_ARG(get_rate, NULL,
                                             "Get data rate for station.\n"
					     "Usage: qwifi get_rate <staid>\n",
                                             cmd_get_rate, 2, 0),
                               SHELL_CMD_ARG(info, NULL,
                                             "Show WLAN information: PHY mode, power mode, MAC address, and operation mode.\n"
					     "Usage: qwifi info\n",
                                             cmd_info, 1, 0),
                               SHELL_CMD_ARG(set_csa, NULL,
                                             "Channel Switch Announcement.\n"
					     "Usage: qwifi set_csa | <mode> <channel> <count> [<iface index>: default is sap iface index.]\n",
                                             cmd_csa, 4, 1),
                               SHELL_CMD_ARG(version, NULL,
                                             "Show WLAN lib version.\n"
					     "Usage: qwifi version\n",
                                             cmd_version, 1, 0),
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(qwifi, &sub_qwifi_commands, "qwifi commands", NULL);
