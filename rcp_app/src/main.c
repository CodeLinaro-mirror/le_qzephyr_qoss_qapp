/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>

#include <rcp_interface_impl.h>
#include <rcp_cntrl_msgs.h>
#include <rcp_config.h>

#include "proxy_wifi_data.h"
#include "proxy_wifi_mgmt.h"

LOG_MODULE_REGISTER(rcp_app, CONFIG_QUALCOMM_RCP_APP_LOG_LEVEL);

#define RCP_INIT_PRIORITY    CONFIG_QUALCOMM_RCP_INIT_PRIORITY
#define EVT_LOOP_STACK_SIZE  2048
#define EVT_LOOP_PRIORITY    8

static int sys_init_rcp(void)
{
	(void)init_rcp();
	return 0;
}

SYS_INIT(sys_init_rcp, POST_KERNEL, RCP_INIT_PRIORITY);

static void event_loop_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	run_event_loop();

}

K_THREAD_DEFINE(event_loop_tid, EVT_LOOP_STACK_SIZE,
		event_loop_thread, NULL, NULL, NULL,
		EVT_LOOP_PRIORITY, 0, 0);

int main(void)
{
	uint8_t capabilities = 0;

	if (!proxy_wifi_mgmt_init()) {
		LOG_ERR("Failed to initialize Wi-Fi mgmt proxy");
		return 0;
	}
	if (!proxy_wifi_data_init()) {
		LOG_ERR("Failed to initialize Wi-Fi data proxy");
		return 0;
	}
	capabilities |= RCP_WIFI_CAPABILITY_FLAG;

	init_mcu_capability(capabilities);
	return 0;
}
