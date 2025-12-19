/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <ftm_uart_polling.h>


#if CONFIG_FTM_MODE
#include "halphy_api.h"
#endif


#if CONFIG_FTM_MODE
    app_mode_id_t app_mode = APP_MODE_FTM; 
#else
    app_mode_id_t app_mode = APP_MODE_MM; 
#endif

#if !IS_ENABLED(CONFIG_SHELL)
    uint8_t min_loglvl = LOG_LEVEL_INF;
#endif

extern void SEGGER_RTT_Init (void);

void main(void)
{
    printk("Hello World! %s\n", CONFIG_BOARD_TARGET);

    SEGGER_RTT_Init();

#if CONFIG_FTM_MODE
    printk("Build FTM image date and time: %s - %s\n", __DATE__, __TIME__);
#endif

    qwifi_init();

}
