/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>
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


SHELL_STATIC_SUBCMD_SET_CREATE(sub_qwifi_commands,
                               SHELL_CMD_ARG(SetTxPower, NULL,
                                             "Set the transmit power in dbm.\n" 
                                             "Usage: <txPower> [<policy = 0:SAFETY>].\n"
                                             "The default policy is SAFETY(SAFETY is the minimum value among reg domain, CTL and target power). Set value to 100 to restore default settings. Tx power range, xpa: 10-SAFETY; ipa:3-SAFETY. Due to limited range in DAC gain with one designated Tx gain index, need to change PowerMode in BDF while setting power\n",
                                             cmd_set_tx_power, 3, 0),
                               SHELL_CMD_ARG(GetTxPower, NULL,
                                             "Get the transmit power, reg_power, target power and CTL power\n",
                                             cmd_get_tx_power, 1, 0),
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(qwifi, &sub_qwifi_commands, "qwifi commands", NULL);