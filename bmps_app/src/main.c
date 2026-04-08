/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app, CONFIG_APP_LOG_LEVEL);

int main(void)
{
    k_sleep(K_MSEC(1));
    LOG_INF("Hello World! %s", CONFIG_BOARD_TARGET);
    
    return 0;
}
