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

WMI_BMPS_ENABLE bmps;
WMI_BMPS_IDLE_TIME idle_time;
static void bmps_timer_cb(struct k_timer *timer);
extern void wmi_ignore_bcmc_in_bmps(void *, uint8_t data);
extern uint64_t bmps_duration = 0;
uint64_t bmps_start = 0;

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
        k_timer_init(&bmps_timer, bmps_timer_cb, NULL);
        k_timer_start(&bmps_timer, K_MSEC(time), K_NO_WAIT);
        bmps_start = hres_timer_curr_time_us();
        bmps_duration = bmps_start + (uint64_t)time*1000;
        shell_print(ctx, "%s duration:%llu ms:%d\r\n", __func__, bmps_duration, time);
    }

    pbmps->enable = enable;
    shell_print(ctx, "Set bmps enable...");
    wmi_cmd_send(WMI_BMPS_ENABLE_CMDID, pbmps, sizeof(*pbmps));
    return 0;
}

static int cmd_bmps_idle_time(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    uint8_t idle_timeout = shell_strtoul(argv[1], 10, &err);

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

    if (err) {
        shell_error(ctx, "Unable to parse enable (err %d)", err);
        return err;
    }
    wmi_ignore_bcmc_in_bmps(NULL, enable);
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
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(qbmps, &sub_bmps_cmds, "bmps commands", NULL);
