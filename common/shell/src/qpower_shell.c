/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>
#include <zephyr/pm/pm.h>
#include <zephyr/kernel.h>
#include <stdio.h>
#include "qpower.h"
#include "wifi_fw_cpr_driver.h"
#include <zephyr/pm/policy.h>

#define TEMPERATUREC_MIN    (-40)
#define TEMPERATUREC_MAX    (125)
#define TEMPERATUREC_ERR_N  (-5)
#define TEMPERATUREC_ERR_P  (5)
#define TEMPERATUREC_GOLDEN (40)

#define VBATMV_MIN   (1600)
#define VBATMV_MAX   (3600)
#define VBATMV_ERR_N (-50)
#define VBATMV_ERR_P (10)

#define CX_ONESHOT_GOLDEN (3)  // for temperature<=40C
#define CX_ONESHOT_MIN    (0)
#define CX_ONESHOT_MAX    (31)

static void s2ram_timer_cb(struct k_timer *timer);
K_TIMER_DEFINE(s2ram_timer, s2ram_timer_cb, NULL);

static int slp_count = 1;
static int cmd_set_softoff(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    uint32_t slp_time_ms = shell_strtoul(argv[1], 10, &err);

    if (err) {
        shell_error(ctx, "Unable to parse input slp_time_ms (err %d)", err);
        return err;
    }

    shell_print(ctx, "Set state to SOFT_OFF...");
    qapi_power_set_parameter(__QAPI_POWER_SOFTOFF_DURATION_MS, slp_time_ms);
    return 0;
}

static void s2ram_timer_cb(struct k_timer *timer)
{
   early_printk("s2ram_timer_cb\r\n");
   if(slp_count>0)
   {
        slp_count--;
   }
   if(slp_count == 0)
   {
        k_timer_stop(&s2ram_timer);
        pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_RAM,PM_ALL_SUBSTATES);
   }
}


static int cmd_set_s2ram(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    uint32_t slp_time_ms = shell_strtoul(argv[1], 10, &err);
    slp_count = shell_strtoul(argv[2], 10, &err);

    if (err) {
        shell_error(ctx, "Unable to parse input slp_time_ms (err %d)", err);
        return err;
    }

    shell_print(ctx, "allow state to suspend_to_ram %u ms",slp_time_ms);

    if(slp_time_ms)
    {
        k_timer_start(&s2ram_timer, K_MSEC(slp_time_ms), K_MSEC(slp_time_ms));
    }
    pm_policy_state_lock_put(PM_STATE_SUSPEND_TO_RAM,PM_ALL_SUBSTATES);
    
    return 0;
}

#ifdef CONFIG_CPR_ENABLE
static int cmd_set_cpr(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    uint32_t enable = shell_strtoul(argv[1], 10, &err);

    if (err) {
        shell_error(ctx, "Unable to parse input slp_time_ms (err %d)", err);
        return err;
    }

    shell_print(ctx, "%s cpr ...", enable?"Enable":"Disable");
    if (enable) {
        wifi_fw_cpr_reenable();
    } else {
        wifi_fw_cpr_disable();
    }
    return 0;
}
#endif

static int cmd_getcx(const struct shell *ctx, size_t argc, char **argv)
{
    shell_print(ctx, "Show cx(ULP-SMPS2) related information\n");
    uint32_t curr_oneshot = ulpsmps2_get_oneshot();
    uint32_t otp_oneshot = ulpsmps2_get_OTP_oneshot();

    shell_print(ctx, "current oneshot code:%d, otp oneshot code:%d\n", curr_oneshot, otp_oneshot);

    tv_monitor_dump("getcx");
    dtim_tv_monitor_dump("getcx");
    hkadc_drv_dump("getcx");

    return 0;
}

static int cmd_calcxoneshot(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    int tempc = shell_strtol(argv[1], 10, &err);
    uint32_t otp_oneshot = 0;
    uint32_t optmized_oneshot = 0;
    uint32_t t_one_shot_ns = 0;

    if (err) {
        shell_error(ctx, "Unable to parse tempC (err %d)", err);
        return err;
    }

    uint32_t vbatmv = shell_strtoul(argv[2], 10, &err);

    if (err) {
        shell_error(ctx, "Unable to parse vbatmV (err %d)", err);
        return err;
    }

    if ((tempc < TEMPERATUREC_MIN) || (tempc > TEMPERATUREC_MAX)) {
        shell_print(ctx, "temperature %d not supported, should be in [%d, %d]\n", tempc, TEMPERATUREC_MIN, TEMPERATUREC_MAX);
        return -EINVAL;
    }


    if ((vbatmv < VBATMV_MIN) || (vbatmv > VBATMV_MAX)) {
        shell_print(ctx, "vbatmV %d not supported, should be in [%d, %d]\n", vbatmv,  VBATMV_MIN, VBATMV_MAX);
        return -EINVAL;
    }

    optmized_oneshot = ulpsmps2_get_optimized_oneshot(vbatmv, tempc, &otp_oneshot, &t_one_shot_ns);

    shell_print(ctx, "vbat=%dmV T=%dC otp_oneshot=%d t_one_shot_ns=%dns optmized_oneshot=%d\n", vbatmv, tempc, otp_oneshot, t_one_shot_ns, optmized_oneshot);

    return 0;
}

extern bool g_presleep_update_ulpsmps2_oneshot_enable;
static int cmd_setcxoneshot(const struct shell *ctx, size_t argc, char **argv)
{
    int err = 0;
    uint32_t set_oneshot = shell_strtoul(argv[1], 10, &err);

    if (err) {
        shell_error(ctx, "Unable to parse request oneshot code (err %d)", err);
        return err;
    }

    if (set_oneshot >= CX_ONESHOT_MAX) {
            shell_print(ctx, "requested_oneshot not supported, should be in (%d, %d]\n", CX_ONESHOT_MIN, CX_ONESHOT_MAX);
            if (set_oneshot == 255) {
                // if requested_oneshot==255, enable update oneshot in sleep
                g_presleep_update_ulpsmps2_oneshot_enable = true;
                shell_print(ctx, "Magic code match, enable update oneshot in sleep\n");
                return 0;
            } 
            return -EINVAL;
    }

    if (set_oneshot) {
        shell_print(ctx, "do set oneshot=%d=>%d and disable update oneshot in sleep\n", ulpsmps2_get_oneshot(),  set_oneshot);
        dtim_tv_set_ulpsmps2_oneshot(set_oneshot);
        g_presleep_update_ulpsmps2_oneshot_enable = false;
        return 0;
    }

    uint32_t cal_oneshot , vbat;
    int temp;
    uint32_t otp_oneshot = 0;
    uint32_t t_one_shot_ns = 0;

    temp = shell_strtol(argv[2], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse request temperature(err %d)", err);
        return err;
    }

    if ((temp < TEMPERATUREC_MIN) || (temp > TEMPERATUREC_MAX)) {
        shell_print(ctx, "temperature %d not supported, should be in [%d, %d]\n", temp, TEMPERATUREC_MIN, TEMPERATUREC_MAX);
        return -EINVAL;
    }

    vbat = shell_strtoul(argv[3], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse request vbat(err %d)", err);
        return err;
    }

    if ((vbat < VBATMV_MIN) || (vbat > VBATMV_MAX)) {
        shell_print(ctx, "vbatmV %d not supported, should be in [%d, %d]\n", vbat,  VBATMV_MIN, VBATMV_MAX);
        return -EINVAL;
    }

    cal_oneshot = ulpsmps2_get_optimized_oneshot(vbat, temp, &otp_oneshot, &t_one_shot_ns);

    shell_print(ctx, "vbat=%dmV T=%dC otp_oneshot=%d t_one_shot_ns=%dns optmized_oneshot=%d\n", vbat, temp, otp_oneshot, t_one_shot_ns, cal_oneshot);

    shell_print(ctx, "do set oneshot=%d=>%d and disable update oneshot in sleep\n", ulpsmps2_get_oneshot(), cal_oneshot);
    dtim_tv_set_ulpsmps2_oneshot(cal_oneshot);
    g_presleep_update_ulpsmps2_oneshot_enable = false;
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_pm_cmds,
                               SHELL_CMD_ARG(softoff, NULL,
                                             "set pm state to soft_of\n"
                                             "Usage: softoff <sleep_time_ms:int>\n"
                                             "sleep_time_ms: no timeout if 0>\n",
                                             cmd_set_softoff, 2, 0),
                               SHELL_CMD_ARG(s2ram, NULL,
                                             "set pm state to suspend2ram \n"
                                             "Usage: s2ram <sleep_time_ms:int> <sleep_count:int>\n"
                                             "sleep_time_ms: no timeout if 0>\n sleep_count: the count of sleep-exist loop\n",
                                             cmd_set_s2ram, 2, 1),
#ifdef CONFIG_CPR_ENABLE
                               SHELL_CMD_ARG(cpr_set, NULL,
                                             "enable or disable cpr\n"
                                             "Usage: cpr_set <set:int>\n"
                                             "set: enable if 1 disable if 0>\n",
                                             cmd_set_cpr, 2, 0),
#endif
                                
                               SHELL_CMD_ARG(getcx, NULL,
                                             "show all ULP-SMPS2 related information, including temp/vbat/otp_oneshot/cur_oneshot\n"
                                             "Usage: getcx \n",
                                             cmd_getcx, 1, 0),
                               SHELL_CMD_ARG(calcxoneshot, NULL,
                                             "Show calculated oneshot_code based on tempC/vbatmV\n"
                                             "Usage: calcxoneshot <tempC> <vbatmV>\n",
                                             cmd_calcxoneshot, 3, 0),
                               SHELL_CMD_ARG(setcxoneshot, NULL,
                                             "Set oneshot_code based on tempC/vbatmV\n"
		                                     "<oneshot_code> : directly oneshot code, if 0 use tempc/vbatmv"
		                                     "[tempc] : tempC for oneshot input.\n"
		                                     "[vbatmv] :  vbat mV for oneshot input.\n"
                                             "Usage: setcxoneshot <tempC> <vbatmV>\n",
                                             cmd_setcxoneshot, 2, 2),
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(qpm, &sub_pm_cmds, "power management commands", NULL);
