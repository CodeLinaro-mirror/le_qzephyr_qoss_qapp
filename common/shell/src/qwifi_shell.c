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
#include <stdio.h>
#include <stdlib.h>
#include <zephyr/net/wifi_utils.h>
#include <wlan_lib_version.h>

#ifdef CONFIG_WIFI_QCOM_P2P
/* Shell-side pending state for the qwifi p2p command set.
 *
 * `cfg` mirrors struct qcom_p2p_params and is what `qwifi p2p enable` hands
 * to qcom_p2p_enable(). `qwifi p2p set <param> <value>` mutates fields in
 * place; live changes are propagated through qcom_p2p_apply_runtime_cfg /
 * qcom_p2p_apply_disc_int when P2P is already enabled.
 *
 * Fields outside cfg:
 *  - go_intent: per-connect arg; stored here so `qwifi p2p connect`
 *    inherits a default the user can override with `set go_intent <n>`.
 *    0 (always client) matches the GC focus of this FR.
 *  - disc_{min,max,max_tu}: hostap p2p_set_disc_int() inputs. Defaults
 *    mirror hostap's own (1, 3, -1). disc_set tracks whether the user
 *    has explicitly configured them — when false, we don't call into
 *    hostap so the default randomized listen window stays in effect.
 */
static struct {
    struct qcom_p2p_params cfg;
    int  go_intent;
    int  disc_min;
    int  disc_max;
    int  disc_max_tu;
    bool disc_set;
} g_p2p_shell = {
    .cfg = {
        .device_name      = "QC-IOT",
        .country          = { 'X', 'X', 0x04 },
        .listen_reg_class = 81,
        .listen_channel   = 6,
        .op_reg_class     = 81,
        .op_channel       = 6,
        .config_methods   = 0x188, /* Display + PBC + Keypad */
        /* Primary device type: WPS_DEV_NETWORK_INFRA / WPS_DEV_NETWORK_INFRA_ROUTER
         * (Android renders this as a network / IoT-router icon). */
        .pri_dev_type     = { 0x00, 0x06, 0x00, 0x50, 0xF2, 0x04, 0x00, 0x02 },
        .pbc_auto_auth    = true,
    },
    .go_intent   = 0,
    .disc_min    = 1,
    .disc_max    = 3,
    .disc_max_tu = -1,
    .disc_set    = false,
};
#endif /* CONFIG_WIFI_QCOM_P2P */

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
        set_rate_cfg.ra_ON = QAPI_WLAN_RA_ON;
    } else if (argc == 3 && !strcmp(argv[1], "htOnly")) {
        if(!strcmp(argv[2], "enable")) {
			set_rate_cfg.ra_ON = QAPI_WLAN_RA_HT_ONLY_ENABLE;
        } else if(!strcmp(argv[2], "disable")) {
			set_rate_cfg.ra_ON = QAPI_WLAN_RA_HT_ONLY_DISABLE;
        } else {
			shell_error(ctx, "Usage: qwifi set_rate htOnly <enable|disable>");
			return -EINVAL;
        }
    } else if (argc == 5) {
        set_rate_cfg.ra_ON = QAPI_WLAN_RA_OFF;

        set_rate_cfg.rate_staid = (uint32_t)shell_strtoul(argv[1], 10, &err);
        if (err) { shell_error(ctx, "Unable to parse <staid> (err %d)", err); return err; }

        set_rate_cfg.rate_p_rate = (uint32_t)shell_strtoul(argv[2], 10, &err);
        if (err) { shell_error(ctx, "Unable to parse <p_rate> (err %d)", err); return err; }

        set_rate_cfg.rate_s_rate = (uint32_t)shell_strtoul(argv[3], 10, &err);
        if (err) { shell_error(ctx, "Unable to parse <s_rate> (err %d)", err); return err; }

        set_rate_cfg.rate_t_rate = (uint32_t)shell_strtoul(argv[4], 10, &err);
        if (err) { shell_error(ctx, "Unable to parse <t_rate> (err %d)", err); return err; }
    } else {
        shell_error(ctx, "Usage: qwifi set_rate auto | htOnly <enable|disable> | <staid> <p_rate> <s_rate> <t_rate>");
        return -EINVAL;
    }

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_SET_RATE, iface, &set_rate_cfg, sizeof(set_rate_cfg))) {
        shell_error(ctx, "Failed to set rate");
        return -ENOEXEC;
    }

    shell_print(ctx, "Rate set%s", set_rate_cfg.ra_ON == QAPI_WLAN_RA_ON? " (auto)" : "");
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

static int cmd_set_ba_win_size(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    struct net_if *iface = net_if_get_wifi_sta();
	struct qcom_wifi_set_ba_win_size_params ba_win_size;

    memset(&ba_win_size, 0, sizeof(ba_win_size));

	if(argc == 3) {
		ba_win_size.tx_size = (uint16_t)shell_strtoul(argv[1], 10, &err);
		if (err) { shell_error(ctx, "Unable to parse <tx_ba_window_size> (err %d)", err); return err; }

		ba_win_size.rx_size = (uint16_t)shell_strtoul(argv[2], 10, &err);
		if (err) { shell_error(ctx, "Unable to parse <rx_ba_window_size> (err %d)", err); return err; }

		if(ba_win_size.tx_size > 64 || ba_win_size.rx_size > 64) {
			shell_error(ctx, "Tha MAX value of tx_ba_window_size and rx_ba_window_size is 64"); 
			return -EINVAL;
		}
	} else {
		shell_error(ctx, "Usage: qwifi set_ba_win_size <tx_ba_window_size> <rx_ba_window_size>");
        return -EINVAL;
	}

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_SET_BA_WIN_SIZE, iface, &ba_win_size, sizeof(ba_win_size))) {
        shell_error(ctx, "Failed to set BA WIN size");
        return -ENOEXEC;
    }
	
    return 0;
}

static int cmd_set_cts_to_self(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_set_cts_to_self_params params = {0};

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

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_SET_CTS_TO_SELF, iface, &params, sizeof(params))) {
        shell_error(ctx, "Failed to set CTS to SELF to %u", params.enable);
        return -ENOEXEC;
    }

    shell_print(ctx, "CTS to SELF set to %s", params.enable ? "enabled" : "disabled");
    return 0;
}

static int cmd_set_rsp_rate(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_set_rsp_rate_params params = {0};

    if (argc != 2) {
        shell_error(ctx, "Invalid number of arguments");
        return -EINVAL;
    }

    params.rate_idx = shell_strtoul(argv[1], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse <rate_idx> (err %d)", err);
        return err;
    }
    if (params.rate_idx != 8  && params.rate_idx != 16) {
        shell_error(ctx, "RspRate only support set to 8:6Mbps or 16:6.5Mbps");
        return -EINVAL;
    }

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_SET_RSP_RATE, iface, &params, sizeof(params))) {
        shell_error(ctx, "Failed to set rsp rate index to %u", params.rate_idx);
        return -ENOEXEC;
    }

    return 0;
}

static int cmd_wnm_sleep(const struct shell *ctx, size_t argc, char **argv)
{
    struct net_if *iface = net_if_get_wifi_sta();
    struct wifi_wnm_sleep_params params = {0};
    int err = 0;

    if (argc < 2) {
        shell_error(ctx, "Usage: qwifi wnm_sleep <enter [interval_ms] | exit>");
        return -EINVAL;
    }

    if (strcmp(argv[1], "enter") == 0) {
        params.action = WIFI_WNM_SLEEP_ENTER;
        if (argc >= 3) {
            params.interval_ms = shell_strtoul(argv[2], 10, &err);
            if (err) {
                shell_error(ctx, "Invalid interval_ms: %s", argv[2]);
                return err;
            }
        }
    } else if (strcmp(argv[1], "exit") == 0) {
        params.action = WIFI_WNM_SLEEP_EXIT;
    } else {
        shell_error(ctx, "Unknown action '%s'. Use 'enter' or 'exit'.", argv[1]);
        return -EINVAL;
    }

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_WNM_SLEEP, iface, &params, sizeof(params))) {
        shell_error(ctx, "WNM sleep %s failed", argv[1]);
        return -ENOEXEC;
    }

    shell_print(ctx, "WNM sleep %s requested (interval=%u ms)",
                argv[1], params.interval_ms);
    return 0;
}

static int cmd_wnm_ap_capable(const struct shell *ctx, size_t argc, char **argv)
{
    struct net_if *iface = net_if_get_wifi_sta();
    struct wifi_wnm_status status = {0};

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_WNM_STATUS, iface, &status, sizeof(status))) {
        shell_error(ctx, "Failed to query WNM status");
        return -ENOEXEC;
    }

    shell_print(ctx, "AP WNM Sleep capable: %s", status.ap_capable ? "yes" : "no");
    return 0;
}

static int cmd_wnm_stats(const struct shell *ctx, size_t argc, char **argv)
{
    struct net_if *iface = net_if_get_wifi_sta();
    struct wifi_wnm_status st = {0};

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_WNM_STATUS, iface, &st, sizeof(st))) {
        shell_error(ctx, "Failed to query WNM status");
        return -ENOEXEC;
    }

    shell_print(ctx, "WNM Sleep stats:");
    shell_print(ctx, "  enabled           : %s", st.enabled ? "yes" : "no");
    shell_print(ctx, "  sleeping          : %s", st.sleeping ? "yes" : "no");
    shell_print(ctx, "  ap_capable        : %s", st.ap_capable ? "yes" : "no");
    shell_print(ctx, "  interval_ms       : %u", st.interval_ms);
    shell_print(ctx, "  enter_req_sent    : %u", st.enter_req_sent);
    shell_print(ctx, "  enter_rsp_rcvd    : %u", st.enter_rsp_rcvd);
    shell_print(ctx, "  exit_req_sent     : %u", st.exit_req_sent);
    shell_print(ctx, "  exit_rsp_rcvd     : %u", st.exit_rsp_rcvd);
    shell_print(ctx, "  wakeup_sta_data   : %u", st.wakeup_sta_data);
    shell_print(ctx, "  wakeup_tim        : %u", st.wakeup_tim);
    shell_print(ctx, "  wakeup_bss_idle   : %u", st.wakeup_bss_idle_timer);
    return 0;
}

static int cmd_bss_max_idle(const struct shell *ctx, size_t argc, char **argv)
{
    struct net_if *iface = net_if_get_wifi_sta();
    int err = 0;
    uint32_t seconds;

    if (argc != 2) {
        shell_error(ctx, "Usage: qwifi bss_max_idle <mili seconds>");
        return -EINVAL;
    }

    seconds = shell_strtoul(argv[1], 10, &err);
    if (err) {
        shell_error(ctx, "Invalid seconds value: %s", argv[1]);
        return err;
    }

    /* Call via Zephyr net_mgmt -> driver -> propwifi */
    if (net_mgmt(NET_REQUEST_WIFI_QCOM_WNM_SET_BSS_MAX_IDLE, iface, &seconds, sizeof(seconds))) {
        shell_error(ctx, "Failed to set BSS Max Idle Period");
        return -ENOEXEC;
    }

    shell_print(ctx, "BSS Max Idle Period set to %u ms (takes effect at next association)",
                seconds);
    return 0;
}

static int cmd_wnm_enable(const struct shell *ctx, size_t argc, char **argv)
{
    struct net_if *iface = net_if_get_wifi_sta();
    int err = 0;
    uint32_t enable;

    if (argc != 2) {
        shell_error(ctx, "Usage: qwifi wnm_enable <0|1>");
        return -EINVAL;
    }

    enable = shell_strtoul(argv[1], 10, &err);
    if (err || enable > 1) {
        shell_error(ctx, "Invalid value '%s'; use 0 or 1", argv[1]);
        return -EINVAL;
    }

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_WNM_SET_ENABLE, iface, &enable, sizeof(enable))) {
        shell_error(ctx, "Failed to set WNM enable");
        return -ENOEXEC;
    }

    shell_print(ctx, "WNM sleep mode %s (takes effect at next association)",
                enable ? "enabled" : "disabled");
    return 0;
}

#ifdef CONFIG_WIFI_QCOM_P2P
/* Parse a hex byte sequence (with or without colons) into an output buffer
 * of exact length `out_len`. Returns 0 on success, -1 on length / format
 * mismatch. Accepts "00:06:00:50:F2:04:00:02" or "0006005050F20402" style.
 */
static int parse_hex_bytes(const char *s, uint8_t *out, size_t out_len)
{
    size_t got = 0;
    while (*s && got < out_len) {
        while (*s == ':' || *s == ' ') s++;
        if (!*s) break;
        unsigned int v;
        int n = 0;
        if (sscanf(s, "%2x%n", &v, &n) != 1 || n != 2) return -1;
        out[got++] = (uint8_t)v;
        s += n;
    }
    while (*s == ':' || *s == ' ') s++;
    return (got == out_len && *s == '\0') ? 0 : -1;
}

static int cmd_p2p_set(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;

    if (argc < 3) {
        shell_error(ctx, "Usage: qwifi p2p set <param> <value>");
        shell_print(ctx, "Params: device_name | listen_channel | op_channel"
                         " | country | config_methods | pri_dev_type | go_intent"
                         " | disc_int | pbc_auto_auth");
        return -EINVAL;
    }

    const char *p = argv[1];
    const char *v = argv[2];

    /* disc_int has its own arg layout: <min> <max> [<max_tu>]
     * (3 or 4 tokens after "set"). Handle it before the single-value
     * dispatch below. */
    if (strcmp(p, "disc_int") == 0) {
        if (argc < 4 || argc > 5) {
            shell_error(ctx,
                "Usage: qwifi p2p set disc_int <min> <max> [<max_tu>]");
            shell_print(ctx,
                "  min/max: discoverable interval in units of 100 TU (defaults 1, 3)");
            shell_print(ctx,
                "  max_tu : optional cap in raw TUs; -1 (default) = no cap");
            return -EINVAL;
        }
        int mn = (int)shell_strtol(argv[2], 10, &err);
        if (err || mn < 0) { shell_error(ctx, "Invalid min"); return -EINVAL; }
        int mx = (int)shell_strtol(argv[3], 10, &err);
        if (err || mx < mn) {
            shell_error(ctx, "Invalid max (must be >= min)");
            return -EINVAL;
        }
        int mt = -1;
        if (argc == 5) {
            mt = (int)shell_strtol(argv[4], 10, &err);
            if (err) { shell_error(ctx, "Invalid max_tu"); return -EINVAL; }
        }
        g_p2p_shell.disc_min = mn;
        g_p2p_shell.disc_max = mx;
        g_p2p_shell.disc_max_tu  = mt;
        g_p2p_shell.disc_set     = true;

        struct net_if *iface = net_if_get_wifi_sta();
        struct qcom_wifi_p2p_params dp = {
            .subcmd = P2P_SUBCMD_APPLY_DISC_INT,
            .apply_disc_int = { .min_disc_int = mn, .max_disc_int = mx,
                                .max_disc_tu = mt },
        };
        int rc = net_mgmt(NET_REQUEST_WIFI_QCOM_P2P, iface, &dp, sizeof(dp));
        if (rc == 0) {
            shell_print(ctx, "OK (applied live)");
        } else {
            shell_print(ctx,
                "OK (pending — will apply on next 'qwifi p2p enable')");
        }
        return 0;
    }

    if (strcmp(p, "device_name") == 0) {
        if (strlen(v) >= QCOM_P2P_MAX_DEV_NAME_LEN) {
            shell_error(ctx, "device_name too long (max %d)",
                        QCOM_P2P_MAX_DEV_NAME_LEN - 1);
            return -EINVAL;
        }
        memset(g_p2p_shell.cfg.device_name, 0, QCOM_P2P_MAX_DEV_NAME_LEN);
        snprintf(g_p2p_shell.cfg.device_name, QCOM_P2P_MAX_DEV_NAME_LEN,
                 "%s", v);
    } else if (strcmp(p, "listen_channel") == 0) {
        int ch = shell_strtoul(v, 10, &err);
        /* P2P social channels are 2.4 GHz only by spec — reg_class 81
         * covers 1..13. Listen window is always on 2.4 GHz so peers can
         * find us regardless of operating-channel preference. */
        if (err || ch < 1 || ch > 13) {
            shell_error(ctx, "Invalid listen_channel (1..13, 2.4 GHz only)");
            return -EINVAL;
        }
        g_p2p_shell.cfg.listen_reg_class = 81;
        g_p2p_shell.cfg.listen_channel   = (uint8_t)ch;
    } else if (strcmp(p, "op_channel") == 0) {
        int ch = shell_strtoul(v, 10, &err);
        /* Operating channel can be 2.4 GHz (reg_class 81: 1..13) or
         * 5 GHz UNII-1 (reg_class 115: 36/40/44/48). UNII-2/2e/3 need
         * DFS or country-specific allow-lists not yet implemented. */
        if (err) {
            shell_error(ctx, "Invalid op_channel");
            return -EINVAL;
        }
        if (ch >= 1 && ch <= 13) {
            g_p2p_shell.cfg.op_reg_class = 81;
        } else if (ch == 36 || ch == 40 || ch == 44 || ch == 48) {
            g_p2p_shell.cfg.op_reg_class = 115;
        } else {
            shell_error(ctx, "Invalid op_channel "
                "(2.4 GHz: 1..13 / 5 GHz UNII-1: 36/40/44/48)");
            return -EINVAL;
        }
        g_p2p_shell.cfg.op_channel = (uint8_t)ch;
    } else if (strcmp(p, "country") == 0) {
        if (strlen(v) != 2) {
            shell_error(ctx, "country must be 2 chars (e.g. CN, US, XX)");
            return -EINVAL;
        }
        g_p2p_shell.cfg.country[0] = v[0];
        g_p2p_shell.cfg.country[1] = v[1];
        /* country[2] keeps the regulatory environment byte (0x04 = "all"). */
    } else if (strcmp(p, "config_methods") == 0) {
        unsigned long cm = shell_strtoul(v, 0, &err); /* base 0 -> 0x prefix OK */
        if (err || cm > 0xFFFF) {
            shell_error(ctx, "Invalid config_methods (16-bit hex/dec)");
            return -EINVAL;
        }
        g_p2p_shell.cfg.config_methods = (uint16_t)cm;
    } else if (strcmp(p, "pri_dev_type") == 0) {
        uint8_t buf[QCOM_P2P_DEV_TYPE_LEN];
        if (parse_hex_bytes(v, buf, QCOM_P2P_DEV_TYPE_LEN) != 0) {
            shell_error(ctx, "Invalid pri_dev_type — expect 8 hex bytes "
                             "(e.g. 00:06:00:50:F2:04:00:02)");
            return -EINVAL;
        }
        memcpy(g_p2p_shell.cfg.pri_dev_type, buf, QCOM_P2P_DEV_TYPE_LEN);
    } else if (strcmp(p, "go_intent") == 0) {
        int gi = shell_strtoul(v, 10, &err);
        if (err || gi < 0 || gi > 15) {
            shell_error(ctx, "Invalid go_intent (0..15)");
            return -EINVAL;
        }
        g_p2p_shell.go_intent = gi;
        /* go_intent is a per-connect parameter, no hostap state to push. */
        shell_print(ctx, "OK");
        return 0;
    } else if (strcmp(p, "pbc_auto_auth") == 0) {
        int en = shell_strtoul(v, 10, &err);
        if (err || (en != 0 && en != 1)) {
            shell_error(ctx, "Invalid pbc_auto_auth (0 or 1)");
            return -EINVAL;
        }
        g_p2p_shell.cfg.pbc_auto_auth = (bool)en;
        /* Falls through to the generic apply_cfg push below — reuses
         * struct qcom_p2p_params's existing tail padding (see its doc
         * comment) instead of a dedicated subcmd, so this never changes
         * the wire size of struct qcom_wifi_p2p_params. */
    } else {
        shell_error(ctx, "Unknown param '%s'", p);
        return -EINVAL;
    }

    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_p2p_params ap = {
        .subcmd = P2P_SUBCMD_APPLY_CFG,
        .apply_cfg = { .cfg = g_p2p_shell.cfg },
    };
    int rc = net_mgmt(NET_REQUEST_WIFI_QCOM_P2P, iface, &ap, sizeof(ap));
    if (rc == 0) {
        shell_print(ctx, "OK (applied live)");
    } else {
        shell_print(ctx, "OK (pending — will apply on next 'qwifi p2p enable')");
    }
    return 0;
}

static int cmd_p2p_show(const struct shell *ctx, size_t argc, char **argv)
{
    (void)argc; (void)argv;
    const struct qcom_p2p_params *c = &g_p2p_shell.cfg;
    shell_print(ctx, "device_name    : %s", c->device_name);
    shell_print(ctx, "country        : %c%c", c->country[0], c->country[1]);
    shell_print(ctx, "listen_channel : %u", c->listen_channel);
    shell_print(ctx, "op_channel     : %u", c->op_channel);
    shell_print(ctx, "config_methods : 0x%04x", c->config_methods);
    shell_print(ctx, "pri_dev_type   : %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                c->pri_dev_type[0], c->pri_dev_type[1],
                c->pri_dev_type[2], c->pri_dev_type[3],
                c->pri_dev_type[4], c->pri_dev_type[5],
                c->pri_dev_type[6], c->pri_dev_type[7]);
    shell_print(ctx, "go_intent      : %d", g_p2p_shell.go_intent);
    if (g_p2p_shell.disc_set) {
        shell_print(ctx, "disc_int       : min=%d max=%d max_tu=%d",
                    g_p2p_shell.disc_min, g_p2p_shell.disc_max,
                    g_p2p_shell.disc_max_tu);
    } else {
        shell_print(ctx, "disc_int       : (hostap default)");
    }
    shell_print(ctx, "pbc_auto_auth  : %d", c->pbc_auto_auth);
    return 0;
}

static int cmd_p2p_enable(const struct shell *ctx, size_t argc, char **argv)
{
    (void)argc; (void)argv;
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_get_mac_address_params mac = {0};
    struct qcom_wifi_p2p_params params = {
        .subcmd = P2P_SUBCMD_ENABLE,
        .enable = { .cfg = g_p2p_shell.cfg },
    };

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_GET_MAC_ADDRESS, iface, &mac, sizeof(mac)) == 0) {
        memcpy(params.enable.cfg.dev_addr, mac.mac, QCOM_P2P_MAC_LEN);
    }

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_P2P, iface, &params, sizeof(params))) {
        shell_error(ctx, "P2P init failed");
        return -ENOEXEC;
    }

    if (g_p2p_shell.disc_set) {
        struct qcom_wifi_p2p_params dp = {
            .subcmd = P2P_SUBCMD_APPLY_DISC_INT,
            .apply_disc_int = { .min_disc_int = g_p2p_shell.disc_min,
                                .max_disc_int = g_p2p_shell.disc_max,
                                .max_disc_tu = g_p2p_shell.disc_max_tu },
        };
        (void)net_mgmt(NET_REQUEST_WIFI_QCOM_P2P, iface, &dp, sizeof(dp));
    }

    shell_print(ctx, "P2P enabled, listen=%u op=%u",
                g_p2p_shell.cfg.listen_channel, g_p2p_shell.cfg.op_channel);
    return 0;
}

static int cmd_p2p_disable(const struct shell *ctx, size_t argc, char **argv)
{
    (void)argc; (void)argv;
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_p2p_params params = { .subcmd = P2P_SUBCMD_DISABLE };
    if (net_mgmt(NET_REQUEST_WIFI_QCOM_P2P, iface, &params, sizeof(params))) {
        shell_error(ctx, "P2P disable failed");
        return -ENOEXEC;
    }
    shell_print(ctx, "P2P disabled");
    return 0;
}

static int cmd_p2p_find(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_p2p_params params = { .subcmd = P2P_SUBCMD_FIND };

    if (argc >= 2) {
        params.find.timeout = shell_strtoul(argv[1], 10, &err);
        if (err) {
            shell_error(ctx, "Unable to parse <timeout> (err %d)", err);
            return err;
        }
    }

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_P2P, iface, &params, sizeof(params))) {
        shell_error(ctx, "P2P find failed (not enabled?)");
        return -ENOEXEC;
    }

    shell_print(ctx, "P2P find started (timeout=%us)", params.find.timeout);
    return 0;
}

static void p2p_shell_print(void *cb_ctx, const char *fmt, ...)
{
    const struct shell *ctx = cb_ctx;
    va_list ap;
    char buf[160];

    va_start(ap, fmt);
    vsnprintk(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    shell_print(ctx, "%s", buf);
}

static int cmd_p2p_peers(const struct shell *ctx, size_t argc, char **argv)
{
    (void)argc; (void)argv;
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_p2p_params params = {
        .subcmd = P2P_SUBCMD_PEERS_DUMP,
        .peers_dump = { .cb = p2p_shell_print, .cb_ctx = (void *)ctx },
    };

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_P2P, iface, &params, sizeof(params))) {
        shell_error(ctx, "P2P peer list unavailable (not enabled?)");
        return -ENOEXEC;
    }
    if (params.peers_dump.n == 0) {
        shell_print(ctx, "(no peers found)");
    } else {
        shell_print(ctx, "%d peer(s)", params.peers_dump.n);
    }
    return 0;
}

static int cmd_p2p_peer(const struct shell *ctx, size_t argc, char **argv)
{
    unsigned int v[QCOM_P2P_MAC_LEN];
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_p2p_params params = {
        .subcmd = P2P_SUBCMD_PEER_DUMP,
        .peer_dump = { .cb = p2p_shell_print, .cb_ctx = (void *)ctx },
    };

    if (argc != 2) {
        shell_error(ctx, "Usage: qwifi p2p peer <xx:xx:xx:xx:xx:xx>");
        return -EINVAL;
    }
    if (sscanf(argv[1], "%x:%x:%x:%x:%x:%x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        shell_error(ctx, "Invalid MAC address: %s", argv[1]);
        return -EINVAL;
    }
    for (int i = 0; i < QCOM_P2P_MAC_LEN; i++) {
        params.peer_dump.mac[i] = (uint8_t)v[i];
    }

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_P2P, iface, &params, sizeof(params))) {
        shell_error(ctx, "Peer not found");
        return -ENOEXEC;
    }
    return 0;
}

static int cmd_p2p_connect(const struct shell *ctx, size_t argc, char **argv)
{
    unsigned int v[QCOM_P2P_MAC_LEN];
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_p2p_params params = {
        .subcmd = P2P_SUBCMD_CONNECT,
        .connect = { .wps_method = QCOM_P2P_WPS_PBC,
                     .go_intent = g_p2p_shell.go_intent },
    };
    int err = 0;
    int next_arg;

    if (argc < 3) {
        shell_error(ctx, "Usage: qwifi p2p connect <xx:xx:xx:xx:xx:xx>"
                         " pbc|pin <PIN>|display [<go_intent>] [auth]");
        return -EINVAL;
    }

    if (sscanf(argv[1], "%x:%x:%x:%x:%x:%x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        shell_error(ctx, "Invalid peer MAC: %s", argv[1]);
        return -EINVAL;
    }
    for (int i = 0; i < QCOM_P2P_MAC_LEN; i++) {
        params.connect.mac[i] = (uint8_t)v[i];
    }

    if (strcmp(argv[2], "pbc") == 0) {
        params.connect.wps_method = QCOM_P2P_WPS_PBC;
        next_arg = 3;
    } else if (strcmp(argv[2], "display") == 0) {
        params.connect.wps_method = QCOM_P2P_WPS_PIN_DISPLAY;
        next_arg = 3;
    } else if (strcmp(argv[2], "pin") == 0) {
        if (argc < 4) {
            shell_error(ctx, "pin method requires an 8-digit PIN argument");
            return -EINVAL;
        }
        params.connect.wps_method = QCOM_P2P_WPS_PIN_KEYPAD;
        next_arg = 4;
        (void)argv[3];
    } else {
        shell_error(ctx, "Unknown WPS method '%s' (expected pbc|pin|display)",
                    argv[2]);
        return -EINVAL;
    }

    for (size_t i = (size_t)next_arg; i < argc; i++) {
        if (strcmp(argv[i], "auth") == 0) {
            params.connect.auth = 1;
        } else {
            int tmp = shell_strtoul(argv[i], 10, &err);
            if (err) {
                shell_error(ctx, "Unknown argument '%s' (expected go_intent or 'auth')",
                            argv[i]);
                return err;
            }
            params.connect.go_intent = tmp;
        }
    }

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_P2P, iface, &params, sizeof(params))) {
        shell_error(ctx, "P2P connect rejected");
        return -ENOEXEC;
    }
    if (params.connect.auth) {
        shell_print(ctx, "P2P authorize issued (peer=%s method=%d go_intent=%d)"
                         " — waiting for peer to retry GO Neg",
                    argv[1], params.connect.wps_method, params.connect.go_intent);
    } else {
        shell_print(ctx, "P2P connect issued (peer=%s method=%d go_intent=%d)",
                    argv[1], params.connect.wps_method, params.connect.go_intent);
    }
    return 0;
}

static int cmd_p2p_stop_find(const struct shell *ctx, size_t argc, char **argv)
{
    (void)argc; (void)argv;
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_p2p_params params = { .subcmd = P2P_SUBCMD_STOP_FIND };
    if (net_mgmt(NET_REQUEST_WIFI_QCOM_P2P, iface, &params, sizeof(params))) {
        shell_error(ctx, "P2P stop_find failed (not enabled?)");
        return -ENOEXEC;
    }
    shell_print(ctx, "P2P find stopped");
    return 0;
}

static int cmd_p2p_listen(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_p2p_params params = { .subcmd = P2P_SUBCMD_LISTEN };

    if (argc >= 2) {
        params.listen.timeout = shell_strtoul(argv[1], 10, &err);
        if (err) {
            shell_error(ctx, "Invalid timeout '%s'", argv[1]);
            return err;
        }
    }
    if (net_mgmt(NET_REQUEST_WIFI_QCOM_P2P, iface, &params, sizeof(params))) {
        shell_error(ctx, "P2P listen rejected");
        return -ENOEXEC;
    }
    shell_print(ctx, "P2P listen started%s%u%s",
                params.listen.timeout ? " (timeout=" : "",
                params.listen.timeout, params.listen.timeout ? " sec)" : "");
    return 0;
}

static int cmd_p2p_cancel(const struct shell *ctx, size_t argc, char **argv)
{
    (void)argc; (void)argv;
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_p2p_params params = { .subcmd = P2P_SUBCMD_CANCEL };
    if (net_mgmt(NET_REQUEST_WIFI_QCOM_P2P, iface, &params, sizeof(params))) {
        shell_error(ctx, "P2P cancel failed (not enabled?)");
        return -ENOEXEC;
    }
    shell_print(ctx, "P2P cancel issued");
    return 0;
}

static int cmd_p2p_flush(const struct shell *ctx, size_t argc, char **argv)
{
    (void)argc; (void)argv;
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_p2p_params params = { .subcmd = P2P_SUBCMD_FLUSH };
    if (net_mgmt(NET_REQUEST_WIFI_QCOM_P2P, iface, &params, sizeof(params))) {
        shell_error(ctx, "P2P flush failed (not enabled?)");
        return -ENOEXEC;
    }
    shell_print(ctx, "P2P device table flushed");
    return 0;
}

static int cmd_p2p_reject(const struct shell *ctx, size_t argc, char **argv)
{
    unsigned int v[QCOM_P2P_MAC_LEN];
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_p2p_params params = { .subcmd = P2P_SUBCMD_REJECT };

    if (argc < 2) {
        shell_error(ctx, "Usage: qwifi p2p reject <xx:xx:xx:xx:xx:xx>");
        return -EINVAL;
    }
    if (sscanf(argv[1], "%x:%x:%x:%x:%x:%x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        shell_error(ctx, "Invalid peer MAC: %s", argv[1]);
        return -EINVAL;
    }
    for (int i = 0; i < QCOM_P2P_MAC_LEN; i++) {
        params.reject.mac[i] = (uint8_t)v[i];
    }
    if (net_mgmt(NET_REQUEST_WIFI_QCOM_P2P, iface, &params, sizeof(params))) {
        shell_error(ctx, "P2P reject failed (peer not in table?)");
        return -ENOEXEC;
    }
    shell_print(ctx, "P2P peer %s rejected", argv[1]);
    return 0;
}

static int cmd_p2p_auth_invite(const struct shell *ctx, size_t argc, char **argv)
{
    unsigned int v[QCOM_P2P_MAC_LEN];
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_p2p_params params = { .subcmd = P2P_SUBCMD_AUTH_INVITE };

    if (argc == 1 || (argc == 2 && strcmp(argv[1], "clear") == 0)) {
        params.auth_invite.clear = true;
        if (net_mgmt(NET_REQUEST_WIFI_QCOM_P2P, iface, &params, sizeof(params))) {
            shell_error(ctx, "auth_invite clear failed (P2P not enabled?)");
            return -ENOEXEC;
        }
        shell_print(ctx, "P2P invitation pre-auth cleared");
        return 0;
    }
    if (sscanf(argv[1], "%x:%x:%x:%x:%x:%x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        shell_error(ctx, "Invalid peer MAC: %s", argv[1]);
        return -EINVAL;
    }
    for (int i = 0; i < QCOM_P2P_MAC_LEN; i++) {
        params.auth_invite.mac[i] = (uint8_t)v[i];
    }
    if (net_mgmt(NET_REQUEST_WIFI_QCOM_P2P, iface, &params, sizeof(params))) {
        shell_error(ctx, "auth_invite failed (P2P not enabled?)");
        return -ENOEXEC;
    }
    shell_print(ctx, "P2P invitation pre-auth set: %s", argv[1]);
    return 0;
}

static int cmd_p2p_invite(const struct shell *ctx, size_t argc, char **argv)
{
    unsigned int v[QCOM_P2P_MAC_LEN];
    struct net_if *iface = net_if_get_wifi_sta();
    struct qcom_wifi_p2p_params params = {
        .subcmd = P2P_SUBCMD_INVITE,
        .invite = { .role = QCOM_P2P_INVITE_ROLE_GO },
    };
    int err = 0;
    size_t i;

    if (argc < 3) {
        shell_error(ctx, "Usage: qwifi p2p invite <peer_mac> <ssid>"
                         " [role=go|active_go|client] [freq=<MHz>] [persistent]");
        return -EINVAL;
    }
    if (sscanf(argv[1], "%x:%x:%x:%x:%x:%x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        shell_error(ctx, "Invalid peer MAC: %s", argv[1]);
        return -EINVAL;
    }
    for (int j = 0; j < QCOM_P2P_MAC_LEN; j++) {
        params.invite.mac[j] = (uint8_t)v[j];
    }

    params.invite.ssid     = (const uint8_t *)argv[2];
    params.invite.ssid_len = strlen(argv[2]);
    if (params.invite.ssid_len == 0) {
        shell_error(ctx, "Empty SSID");
        return -EINVAL;
    }

    for (i = 3; i < argc; i++) {
        if (strncmp(argv[i], "role=", 5) == 0) {
            const char *r = argv[i] + 5;
            if (strcmp(r, "go") == 0)              params.invite.role = QCOM_P2P_INVITE_ROLE_GO;
            else if (strcmp(r, "active_go") == 0)  params.invite.role = QCOM_P2P_INVITE_ROLE_ACTIVE_GO;
            else if (strcmp(r, "client") == 0)     params.invite.role = QCOM_P2P_INVITE_ROLE_CLIENT;
            else {
                shell_error(ctx, "Invalid role '%s' (expected go|active_go|client)", r);
                return -EINVAL;
            }
        } else if (strncmp(argv[i], "freq=", 5) == 0) {
            params.invite.freq = shell_strtoul(argv[i] + 5, 10, &err);
            if (err) {
                shell_error(ctx, "Invalid freq '%s'", argv[i] + 5);
                return err;
            }
        } else if (strcmp(argv[i], "persistent") == 0) {
            params.invite.persistent_group = 1;
        } else {
            shell_error(ctx, "Unknown argument '%s'", argv[i]);
            return -EINVAL;
        }
    }

    if (net_mgmt(NET_REQUEST_WIFI_QCOM_P2P, iface, &params, sizeof(params))) {
        shell_error(ctx, "P2P invite rejected");
        return -ENOEXEC;
    }
    shell_print(ctx, "P2P invite issued (peer=%s ssid=\"%s\" role=%d freq=%u%s)",
                argv[1], argv[2], params.invite.role, params.invite.freq,
                params.invite.persistent_group ? " persistent" : "");
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_qwifi_p2p_commands,
                               SHELL_CMD_ARG(set, NULL,
                                             "Set a P2P parameter. Live if P2P enabled, else pending.\n"
                                             "Usage: qwifi p2p set <param> <value>\n"
                                             "Params:\n"
                                             "  device_name    <string up to 31 chars>\n"
                                             "  listen_channel <1..13>          (2.4 GHz only — P2P social channels are 2.4 GHz by spec)\n"
                                             "  op_channel     <1..13|36|40|44|48> (2.4 GHz reg_class 81 or 5 GHz UNII-1 reg_class 115)\n"
                                             "  country        <XX>\n"
                                             "  config_methods <hex16, e.g. 0x188>\n"
                                             "  pri_dev_type   <8 hex bytes, e.g. 00:06:00:50:F2:04:00:02>\n"
                                             "  go_intent      <0..15>\n"
                                             "  disc_int       <min> <max> [<max_tu>]\n"
                                             "                  (min/max in 100 TU units; max_tu raw TUs, -1=no cap)\n"
                                             "  pbc_auto_auth  <0|1> (default 1 — auto-authorize PBC GO Neg requests)\n",
                                             cmd_p2p_set, 3, 2),
                               SHELL_CMD_ARG(show, NULL,
                                             "Show pending P2P configuration.\n"
                                             "Usage: qwifi p2p show\n",
                                             cmd_p2p_show, 1, 0),
                               SHELL_CMD_ARG(enable, NULL,
                                             "Enable P2P.\n"
                                             "Usage: qwifi p2p enable\n",
                                             cmd_p2p_enable, 1, 0),
                               SHELL_CMD_ARG(disable, NULL,
                                             "Disable P2P (tears down hostap p2p, releases eloop).\n"
                                             "Usage: qwifi p2p disable\n",
                                             cmd_p2p_disable, 1, 0),
                               SHELL_CMD_ARG(find, NULL,
                                             "Start P2P device discovery.\n"
                                             "Usage: qwifi p2p find [<timeout_sec>]\n",
                                             cmd_p2p_find, 1, 1),
                               SHELL_CMD_ARG(stop_find, NULL,
                                             "Stop an ongoing P2P device discovery.\n"
                                             "Usage: qwifi p2p stop_find\n",
                                             cmd_p2p_stop_find, 1, 0),
                               SHELL_CMD_ARG(peers, NULL,
                                             "List all discovered P2P peers.\n"
                                             "Usage: qwifi p2p peers\n",
                                             cmd_p2p_peers, 1, 0),
                               SHELL_CMD_ARG(peer, NULL,
                                             "Dump details of one P2P peer.\n"
                                             "Usage: qwifi p2p peer <xx:xx:xx:xx:xx:xx>\n",
                                             cmd_p2p_peer, 2, 0),
                               SHELL_CMD_ARG(connect, NULL,
                                             "Initiate or accept P2P group formation.\n"
                                             "Usage: qwifi p2p connect <peer_mac> pbc|display [<go_intent>] [auth]\n"
                                             "       qwifi p2p connect <peer_mac> pin <PIN> [<go_intent>] [auth]\n"
                                             "Add the 'auth' keyword to accept an incoming P2P-GO-NEG-REQUEST\n"
                                             "(authorize-only; we wait for the peer to retry GO Neg).\n",
                                             cmd_p2p_connect, 3, 3),
                               SHELL_CMD_ARG(listen, NULL,
                                             "Enter listen-only state for [<timeout_sec>] seconds.\n"
                                             "Usage: qwifi p2p listen [<timeout_sec>]\n"
                                             "0 or omitted = use hostap default (5 s).\n",
                                             cmd_p2p_listen, 1, 1),
                               SHELL_CMD_ARG(cancel, NULL,
                                             "Cancel pending GO negotiation and stop find.\n"
                                             "Usage: qwifi p2p cancel\n",
                                             cmd_p2p_cancel, 1, 0),
                               SHELL_CMD_ARG(flush, NULL,
                                             "Drop all known P2P peers.\n"
                                             "Usage: qwifi p2p flush\n",
                                             cmd_p2p_flush, 1, 0),
                               SHELL_CMD_ARG(reject, NULL,
                                             "Reject any further connection attempts from a peer.\n"
                                             "Usage: qwifi p2p reject <xx:xx:xx:xx:xx:xx>\n",
                                             cmd_p2p_reject, 2, 0),
                               SHELL_CMD_ARG(auth_invite, NULL,
                                             "Pre-authorize a peer to invite us into a group.\n"
                                             "Usage: qwifi p2p auth_invite <xx:xx:xx:xx:xx:xx>\n"
                                             "       qwifi p2p auth_invite clear   (or no arg)\n"
                                             "Authorization is one-shot; consumed on the next matching\n"
                                             "Invitation Request from the peer (matched against either\n"
                                             "the source MAC or the GO Device Address).\n",
                                             cmd_p2p_auth_invite, 1, 1),
                               SHELL_CMD_ARG(invite, NULL,
                                             "Send a P2P Invitation Request to a peer.\n"
                                             "Usage: qwifi p2p invite <peer_mac> <ssid>"
                                             " [role=go|active_go|client] [freq=<MHz>] [persistent]\n"
                                             "Skeleton — wire-level Invitation Request is sent, but the full\n"
                                             "lifecycle (GO bring-up / persistent group reuse) is not yet\n"
                                             "implemented. Useful for protocol testing.\n",
                                             cmd_p2p_invite, 3, 3),
                               SHELL_SUBCMD_SET_END);
#endif /* CONFIG_WIFI_QCOM_P2P */

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
					     "Usage: qwifi set_rate auto | htOnly <enable|disable> | <staid> <p_rate> <s_rate> <t_rate>\n",
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
                               SHELL_CMD_ARG(set_ba_win_size, NULL,
                                             "Set BA Window size.\n"
					     "Usage: qwifi set_ba_win_size | <tx_ba_window_size> <rx_ba_window_size>\n",
                                             cmd_set_ba_win_size, 3, 0),
                               SHELL_CMD_ARG(set_cts_to_self, NULL,
                                             "Set CTS to SELF enable or disable.\n"
					     "Usage: qwifi set_cts_to_self | <1: enable| 0: disable>\n",
                                             cmd_set_cts_to_self, 2, 0),
                               SHELL_CMD_ARG(set_rsp_rate, NULL,
                                             "Set Rsp rate to 6Mbps or 6.5Mbps.\n"
					     "Usage: qwifi set_rsp_rate | <rate_idx = 8:6Mbps or 16:6.5Mbps>\n",
                                             cmd_set_rsp_rate, 2, 0),
                               SHELL_CMD_ARG(wnm_sleep, NULL,
                                             "Enter or exit WNM Sleep Mode.\n"
                                             "Usage: qwifi wnm_sleep enter [interval_ms]\n"
                                             "       qwifi wnm_sleep exit\n",
                                             cmd_wnm_sleep, 2, 1),
                               SHELL_CMD_ARG(wnm_ap_capable, NULL,
                                             "Query whether connected AP supports WNM Sleep Mode.\n"
                                             "Usage: qwifi wnm_ap_capable\n",
                                             cmd_wnm_ap_capable, 1, 0),
                               SHELL_CMD_ARG(wnm_stats, NULL,
                                             "Show WNM Sleep Mode statistics.\n"
                                             "Usage: qwifi wnm_stats\n",
                                             cmd_wnm_stats, 1, 0),
                               SHELL_CMD_ARG(bss_max_idle, NULL,
                                             "Set BSS Max Idle Period. Takes effect at next association.\n"
                                             "Usage: qwifi bss_max_idle <mili seconds>\n",
                                             cmd_bss_max_idle, 2, 0),
                               SHELL_CMD_ARG(wnm_enable, NULL,
                                             "Enable or disable WNM Sleep Mode. Must be set before association.\n"
                                             "Usage: qwifi wnm_enable <0|1>\n",
                                             cmd_wnm_enable, 2, 0),
#ifdef CONFIG_WIFI_QCOM_P2P
                               SHELL_CMD(p2p, &sub_qwifi_p2p_commands,
                                         "Wi-Fi Direct (P2P) command set.\n"
                                         "Usage: qwifi p2p <subcommand> [args]\n"
                                         "Typical flow:\n"
                                         "  qwifi p2p set <param> <value>     (optional, before enable)\n"
                                         "  qwifi p2p enable\n"
                                         "  qwifi p2p find [<timeout_sec>]    (or 'listen' to be discoverable)\n"
                                         "  qwifi p2p peers                   (list discovered peers)\n"
                                         "  qwifi p2p connect <mac> pbc [<go_intent>] [auth]\n"
                                         "Subcommands:\n"
                                         "  set / show           : configure / inspect P2P parameters\n"
                                         "  enable / disable     : bring P2P up or tear it down\n"
                                         "  find / stop_find     : start / stop device discovery\n"
                                         "  listen / cancel      : enter listen state / cancel ongoing op\n"
                                         "  peers / peer         : list peers / dump one peer\n"
                                         "  connect / reject     : initiate GO neg / reject a peer\n"
                                         "  flush                : drop all known peers\n"
                                         "  auth_invite / invite : pre-authorize / send Invitation Request\n"
                                         "Run 'qwifi p2p <subcommand>' with no further args to see its usage.\n",
                                         NULL),
#endif /* CONFIG_WIFI_QCOM_P2P */
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(qwifi, &sub_qwifi_commands, "qwifi commands", NULL);
