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
#include "wmi.h"
#include <zephyr/device.h>
#include "qurt_timer.h"
#include <zephyr/kernel.h>
#include "zephyr/net/net_ip.h"

WMI_BMPS_ENABLE bmps;
WMI_BMPS_IDLE_TIME idle_time;
static void bmps_timer_cb(struct k_timer *timer);
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
extern bool (*wakeup_cb_dtim)(uint16_t type, bool bm_cast,void* pbuf,uint16_t len);

K_TIMER_DEFINE(bmps_timer, bmps_timer_cb, NULL);
static void bmps_timer_cb(struct k_timer *timer)
{
    WMI_BMPS_ENABLE *pdata = (WMI_BMPS_ENABLE *)&bmps;
    uint64_t now = hres_timer_curr_time_us();
    uint64_t delta = now - bmps_start;
    printk("BMPS timer expired. curr: %llu, delta: %llu\n", now, delta);
    memset(pdata, 0, sizeof(*pdata));
    pdata->enable = 0;
    wmi_cmd_send(WMI_BMPS_ENABLE_CMDID, pdata, sizeof(*pdata));
    k_timer_stop(&bmps_timer);
    const struct device *wifi_dev = device_get_binding("qwifi_sta");
    pm_device_busy_set(wifi_dev);
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
    wmi_cmd_send(WMI_BMPS_ENABLE_CMDID, pbmps, sizeof(*pbmps));
    if(enable)
        cmd_clear_busy(ctx, 0, NULL);
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
        wmi_cmd_send(WMI_STA_IDLE_TIMER_CMDID, pdata, sizeof(*pdata));
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
    WMI_BMPS_IGNORE_BCMC *pdata = &bmps;

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
    int err = 0;
    WMI_BMPS_ENABLE *pbmps = &bmps;
    memset(pbmps, 0, sizeof(*pbmps));
    shell_print(ctx, "dbg tsf...");
    wmi_cmd_send(WMI_DBG_TSF_CMDID, pbmps, sizeof(*pbmps));
    return 0;
}

static int cmd_clear_busy(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
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
    int err = 0;
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

        const uint8_t *llc_snap_header = wifi_frame + WIFI_MAC_HEADER_LEN;

        if (llc_snap_header[6] != 0x08 || llc_snap_header[7] != 0x00) {
            return TRUE;
        }

        ip_frame = (uint8_t *)wifi_frame + WIFI_MAC_HEADER_LEN + LLC_SNAP_HEADER_LEN;

        struct net_ipv4_hdr  *ip = (struct net_ipv4_hdr  *)(ip_frame);
        if (ip->proto != IPPROTO_UDP) {
            return TRUE;
        }

        struct net_udp_hdr  *udp = (struct udp_hdr_hdr *)(ip_frame +NET_IPV4H_LEN);
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

static  qapi_Status_t qapi_bmps_bcmc_rx_filter_cb_register(qapi_bmps_rx_filter_cb bmps_cb, qapi_bmps_rx_filter_cb net_cb)
{
    if(!bmps_cb)
    {
        return QAPI_ERR_INVALID_PARAM;
    }

    if(bmps_cb)
    {
        wakeup_cb_dtim = bmps_cb;
    }

    /** wakeup_cb_net = net_cb; */
    

   return QAPI_OK;
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
	int en, port;


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
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(qbmps, &sub_bmps_cmds, "bmps commands", NULL);
