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
#include "qapi_lowpower.h"
#include "wmi.h"
#include <zephyr/device.h>
#include "qurt_timer.h"
#include <zephyr/kernel.h>
#include "zephyr/net/net_ip.h"
#include <zephyr/pm/policy.h>
#include <zephyr/pm/device.h>
#include <zephyr/net/wifi_mgmt.h>
#include "pm_timer.h"

WMI_BMPS_ENABLE bmps;
WMI_BMPS_IDLE_TIME idle_time;
static void bmps_timer_cb(struct k_timer *timer);
static void period_wakeup_timer_cb(struct k_timer *timer);
extern void wmi_ignore_bcmc_in_bmps(void *, uint8_t data);
static int cmd_clear_busy(const struct shell *ctx, size_t argc, char **argv);
static int cmd_set_busy(const struct shell *ctx, size_t argc, char **argv);
extern uint64_t bmps_duration;
uint64_t bmps_start = 0;

#define WIFI_MAC_HEADER_LEN 24
#define LLC_SNAP_HEADER_LEN 8
#define UDP_WHITELIST_LEN     4
uint32_t udp_whitelist_arr[UDP_WHITELIST_LEN] = {7777, 0, 0, 0};
typedef bool (*qapi_bmps_rx_filter_cb)(uint16_t type, bool bm_cast, void *pbuf, uint16_t len);

void pm_timer_debug_dump(void);
uint32_t pm_timer_stop_all_k_timers(void);

K_TIMER_DEFINE(bmps_timer, bmps_timer_cb, NULL);
K_TIMER_DEFINE(period_wakeup_timer, period_wakeup_timer_cb, NULL);

static void period_wakeup_timer_cb(struct k_timer *timer)
{
    /*printk("TICK\n");*/
}

static void bmps_timer_cb(struct k_timer *timer)
{
    WMI_BMPS_ENABLE *pdata = (WMI_BMPS_ENABLE *)&bmps;
    uint64_t now = hres_timer_curr_time_us();
    uint64_t delta = now - bmps_start;
    printk("BMPS timer expired. curr: %llu, delta: %llu\n", now, delta);
    memset(pdata, 0, sizeof(*pdata));
    pdata->enable = 0;
    wmi_cmd_send(WMI_BMPS_ENABLE_CMDID, pdata, sizeof(*pdata));
    pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_RAM,PM_ALL_SUBSTATES);
    k_timer_stop(&bmps_timer);
}

static int cmd_bmps_enable(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    WMI_BMPS_ENABLE *pbmps = &bmps;
    memset(pbmps, 0, sizeof(*pbmps));
    uint8_t enable = shell_strtoul(argv[1], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse enable (err %d)", err);
        return err;
    }

    uint32_t time= shell_strtoul(argv[2], 10, &err);

    if (err) {
        shell_error(ctx, "Unable to parse enable (err %d)", err);
        return err;
    }
    if(time!= 0)
    {
        k_timer_start(&bmps_timer, K_MSEC(time), K_NO_WAIT);
        bmps_start = hres_timer_curr_time_us();
        bmps_duration = bmps_start + (uint64_t)time*1000;
        shell_print(ctx, "%s duration:%llu ms:%d\r\n", __func__, bmps_duration, time);
    }

    pbmps->enable = enable;
    shell_print(ctx, "Set bmps enable...");
    // wmi_cmd_send(WMI_BMPS_RX_FILTER_ENABLE_CMDID, pdata, sizeof(*pdata));
    qapi_bmps_rx_filter_enable(enable);

    qapi_bmps_cfg(pbmps->enable, 0);
    if(enable && pm_policy_state_lock_is_active(PM_STATE_SUSPEND_TO_RAM,PM_ALL_SUBSTATES))
    {
        pm_policy_state_lock_put(PM_STATE_SUSPEND_TO_RAM,PM_ALL_SUBSTATES);
    }
    return 0;
}

static int cmd_bmps_idle_time(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    uint32_t idle_timeout = shell_strtoul(argv[1], 10, &err);

    if (err) {
        shell_error(ctx, "Unable to parse idle time (err %d)", err);
        return err;
    }

    if (idle_timeout) {
        WMI_BMPS_IDLE_TIME *pdata = &idle_time;
        shell_print(ctx, "Set bmps idle_timeout to %d ms", idle_timeout);
        memset(pdata, 0, sizeof(*pdata));
        pdata->time = idle_timeout;
        qapi_bmps_cfg(2, pdata->time);
    }
    else
    {
        shell_error(ctx, "bmps idle_timeout can't set to 0");
    }

    return 0;
}

static int cmd_bmps_ignore_bcmc(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    uint8_t enable = shell_strtoul(argv[1], 10, &err);
    WMI_BMPS_IGNORE_BCMC *pdata = (WMI_BMPS_IGNORE_BCMC *)&bmps;

    pdata->enable = enable;

    if (err) {
        shell_error(ctx, "Unable to parse enable (err %d)", err);
        return err;
    }
    /** wmi_ignore_bcmc_in_bmps(NULL, enable); */
    wmi_cmd_send(WMI_BMPS_IGNORE_BCMC_CMDID, pdata, sizeof(*pdata));
    return 0;
}

static int cmd_dbg_tsf(const struct shell *ctx, size_t argc, char **argv)
{
    WMI_BMPS_ENABLE *pbmps = &bmps;
    memset(pbmps, 0, sizeof(*pbmps));
    shell_print(ctx, "dbg tsf...");
    wmi_cmd_send(WMI_DBG_TSF_CMDID, pbmps, sizeof(*pbmps));
    return 0;
}

static int cmd_clear_busy(const struct shell *ctx, size_t argc, char **argv)
{
    const struct device *wifi_dev = device_get_binding("qwifi_sta");

    if (!wifi_dev) {
        shell_print(ctx, "qwifi_sta not found\r\n");
        return -ENODEV;
    }
    if (!device_is_ready(wifi_dev)) {
        shell_print(ctx, "WiFi device not ready\r\n");
        return -ENODEV;
    }
    shell_print(ctx, "wifi busy clear\r\n");
    pm_device_busy_clear(wifi_dev);
    return 0;
}


static int cmd_set_busy(const struct shell *ctx, size_t argc, char **argv)
{
    const struct device *wifi_dev = device_get_binding("qwifi_sta");

    if (!wifi_dev) {
        shell_print(ctx, "qwifi_sta not found\n");
        return -ENODEV;
    }
    if (!device_is_ready(wifi_dev)) {
        shell_print(ctx, "WiFi device not ready\n");
        return -ENODEV;
    }
    shell_print(ctx, "wifi busy set\r\n");
    pm_device_busy_set(wifi_dev);
    return 0;
}

bool wakeup_cb_bcmc_filter_dtim(uint16_t type, bool bm_cast, void *wifi_frame, uint16_t len)
{
    uint8_t *ip_frame;
    if (bm_cast) {
        if (len < (WIFI_MAC_HEADER_LEN + LLC_SNAP_HEADER_LEN)) {
            return FALSE;

        }

        const uint8_t *llc_snap_header =(uint8_t *) wifi_frame + WIFI_MAC_HEADER_LEN;

        if (llc_snap_header[6] != 0x08 || llc_snap_header[7] != 0x00) {
            return TRUE;
        }

        ip_frame = (uint8_t *)wifi_frame + WIFI_MAC_HEADER_LEN + LLC_SNAP_HEADER_LEN;

        struct net_ipv4_hdr  *ip = (struct net_ipv4_hdr  *)(ip_frame);
        if (ip->proto != IPPROTO_UDP) {
            return TRUE;
        }

        struct net_udp_hdr  *udp = (struct net_udp_hdr *)(ip_frame +NET_IPV4H_LEN);
        uint16_t src_port = ntohs(udp->src_port);
        uint16_t dst_port = ntohs(udp->dst_port);

        // whitelist for UDP dst port
        for (uint16_t i = 0; i < UDP_WHITELIST_LEN; i++) {
            if (udp_whitelist_arr[i] && dst_port == udp_whitelist_arr[i]) {
                return TRUE;
            }
        }

        return FALSE;
    }
    return TRUE;
}

static int cmd_bcmc_enable(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    WMI_BMPS_ENABLE *pdata = (WMI_BMPS_ENABLE *)&bmps;
    uint8_t enable = shell_strtoul(argv[1], 10, &err);

    if (err) {
        shell_error(ctx, "Unable to parse enable (err %d)", err);
        return err;
    }

    memset(pdata, 0, sizeof(*pdata));
    pdata->enable = enable;
    wmi_cmd_send(WMI_BMPS_RX_FILTER_ENABLE_CMDID, pdata, sizeof(*pdata));

    if (enable) {
        qapi_bmps_bcmc_rx_filter_cb_register(wakeup_cb_bcmc_filter_dtim, NULL);

    }



    return 0;

}

static int cmd_bmps_power_optimization_enable(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    uint8_t enable = shell_strtoul(argv[1], 10, &err);

    if (err) {
        shell_error(ctx, "Unable to parse enable (err %d)", err);
        return err;
    }

    if(QAPI_OK !=qapi_bmps_power_optimization_enable(enable?1:0)){
        shell_error(ctx, "qapi_bmps_power_optimization_enable error");
    }

    return 0;
}

static int cmd_compress_qos_null_enable(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    uint8_t enable = shell_strtoul(argv[1], 10, &err);

    if (err) {
        shell_error(ctx, "Unable to parse enable (err %d)", err);
        return err;
    }

    if(QAPI_OK !=qapi_bmps_compress_qos_null_enable(enable?1:0)){
        shell_error(ctx, "qapi_bmps_compress_qos_null_enable error");
    }

    return 0;
}
static int cmd_set_bcmc_filter(const struct shell *ctx, size_t argc, char **argv)
{
	int opt;
	int opt_index = 0;
	struct getopt_state *state;
	static const struct option long_options[] = {
		{"adddel", required_argument, 0, 'a'},
		{"udpport", required_argument, 0, 'u'},
		{"querry", no_argument, 0, 'q'},
		{0, 0, 0, 0}};
	int en=0, port=0;


	while ((opt = getopt_long(argc, argv, "a:u:q",
				  long_options, &opt_index)) != -1) {
		state = getopt_state_get();
		switch (opt) {
		case 'a':
            en = atoi(state->optarg);
            break;
        case 'u':
            port = atoi(state->optarg);
            break;
        case 'q':
            shell_print(ctx, "udp whitelist port:\n");
            for (int i = 0; i < UDP_WHITELIST_LEN; i++) {
                shell_print(ctx, "[%d]: %d\n", i, udp_whitelist_arr[i]);
            }
            break;
		case '?':
		default:
			shell_error(ctx, "Invalid option or option usage: %s\n",
				 argv[opt_index + 1]);
			return -ENOEXEC;
        }
    }

        if (en) {
            for (int i = 0; i < UDP_WHITELIST_LEN; i++) {
                if (udp_whitelist_arr[i] == 0) {
                    udp_whitelist_arr[i] = port;
                    break;
                }
            }

        } else {
            for (int i = 0; i < UDP_WHITELIST_LEN; i++) {
                if (udp_whitelist_arr[i] == port)
                    udp_whitelist_arr[i] = 0;
            }
        }

    return 0;
}
#ifdef CONFIG_PM
static int cmd_bmps_get_hres(const struct shell *ctx, size_t argc, char **argv)
{
    uint64_t time = 0;
    time = hres_timer_curr_time_us();
    shell_print(ctx, "curr_time_us: %u\r\n",(uint32_t)time);
    return 0;
}

static int cmd_get_kt_stats(const struct shell *ctx, size_t argc, char **argv)
{
    pm_timer_debug_dump();
    return 0;
}

static int cmd_kill_all_kt(const struct shell *ctx, size_t argc, char **argv)
{
    pm_timer_stop_all_k_timers();
    pm_timer_debug_dump();
    return 0;
}

static int cmd_list_all_dev(const struct shell *ctx, size_t argc, char **argv)
{
    pm_device_dump_all_status();
    return 0;
}

static int cmd_get_pm_kt(const struct shell *ctx, size_t argc, char **argv)
{
    pm_timer_dump_managed_list();
    return 0;
}
#endif

static int cmd_set_period_wakeup(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    uint32_t slp_time_ms = shell_strtoul(argv[1], 10, &err);

    if (err) {
        shell_error(ctx, "Unable to parse input slp_time_ms (err %d)", err);
        return err;
    }

    shell_print(ctx, "allow state to suspend_to_ram %u ms",slp_time_ms);

    if(slp_time_ms)
    {
        k_timer_start(&period_wakeup_timer, K_MSEC(slp_time_ms), K_MSEC(slp_time_ms));
    }

    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_bmps_cmds,
                               SHELL_CMD_ARG(enable, NULL,
                                             "set bmps enable \n"
                                             "Usage: bmps <1/0> <timeout in ms, 0 means never timeout>, 1:enable, 0:disable\n",
                                             cmd_bmps_enable, 3, 0),
                               SHELL_CMD_ARG(idle_time, NULL,
                                             "Cfg max idle time prior entering into BMPS(DTIM) sleep\n"
                                             "Usage: idle_time <Idle time in ms>\n",
                                             cmd_bmps_idle_time, 2, 0),
                               SHELL_CMD_ARG(ignore_bcmc, NULL,
                                             "set ignore_bcmc\n"
                                             "Usage: ignore_gcmc 1/0, 1:ignore, 0: dont ignore\n",
                                             cmd_bmps_ignore_bcmc, 2, 0),
                               SHELL_CMD_ARG(dbgtsf, NULL,
                                             "dbg tsf\n",
                                             cmd_dbg_tsf, 1, 0),
                               SHELL_CMD_ARG(clear_busy, NULL,
                                             "clear wifi busy\n",
                                             cmd_clear_busy, 1, 0),
                               SHELL_CMD_ARG(set_busy, NULL,
                                             "set wifi busy\n",
                                             cmd_set_busy, 1, 0),
                               SHELL_CMD_ARG(bcmc_enable, NULL,
                                             "enable/disable bcmc filter\n"
                                             "Usage: bcmc_enable 1/0, 1:enable, 0: disable\n",
                                             cmd_bcmc_enable, 2, 0),
                               SHELL_CMD_ARG(bcmc_list, NULL,
                                             "Add/delete udp port to whitelist\n"
                                             "Usage: bcmc_list [-a 1/0] [-u dst udp port] -q\n"
                                             "[-a 1/0] 1: add udp port to the whitelist, 0: delte udp port from whitelist\n"
                                             "[-u dst udp port]: the dst udp port like 7777\n"
                                             "-q : query the whitelist\n",
                                             cmd_set_bcmc_filter, 2, 3),
                                SHELL_CMD_ARG(bmps_power_optimization_enable, NULL,
                                             "enable/disable BMPS when in active mode\n"
                                             "Usage: bmps_power_optimization_enable 1/0, 1:enable, 0: disable\n",
                                             cmd_bmps_power_optimization_enable, 2, 0),
                                SHELL_CMD_ARG(compress_qos_null_enable, NULL,
                                             "enable/disable compress qos null frame sending\n"
                                             "Usage: compress_qos_null_enable 1/0, 1:enable, 0: disable\n",
                                             cmd_compress_qos_null_enable, 2, 0),
#ifdef CONFIG_PM
                               SHELL_CMD_ARG(get_hres, NULL,
                                             "get high resoluation time\n"
                                             "Usage: get_hres\n",
                                             cmd_bmps_get_hres, 1, 0),
                               SHELL_CMD_ARG(get_kt_stats, NULL,
                                            "get kernel timer stats\n"
                                            "Usage: get_kt_stats\n",
                                            cmd_get_kt_stats, 1, 0),
                               SHELL_CMD_ARG(kill_all_kt, NULL,
                                            "kill all kernel timer\n"
                                            "Usage: kill_all_kt\n",
                                            cmd_kill_all_kt, 1, 0),
                               SHELL_CMD_ARG(list_all_dev, NULL,
                                            "list all device inclue driver status busy or idle\n"
                                            "Usage: list_all_dev\n",
                                            cmd_list_all_dev, 1, 0),          
                               SHELL_CMD_ARG(get_pm_kt, NULL,
                                            "get kernel timers manager by power module\n"
                                            "Usage: get_pm_kt\n",
                                            cmd_get_pm_kt, 1, 0),
#endif
                                SHELL_CMD_ARG(set_period_wakeup, NULL,
                                            "set period wakeup\n"
                                            "Usage: set_period_wakeup <period(ms)>\n",
                                            cmd_set_period_wakeup, 2, 0),
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(qbmps, &sub_bmps_cmds, "bmps commands", NULL);

