/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/wifi_mgmt.h>

LOG_MODULE_REGISTER(MAIN);

#define MACSTR "%02X:%02X:%02X:%02X:%02X:%02X"

#define NET_EVENT_WIFI_MASK                                                                        \
	(NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT |                        \
	 NET_EVENT_WIFI_SCAN_RESULT | NET_EVENT_WIFI_SCAN_DONE | NET_EVENT_WIFI_RAW_SCAN_RESULT)

struct scan_context {
	int scan_result;
};

static struct scan_context scan_context;
static K_SEM_DEFINE(scan_sem, 0, 1);

static struct wifi_connect_req_params sta_config;
static struct net_mgmt_event_callback wifi_event_cb;

/* Check necessary definitions */
BUILD_ASSERT(sizeof(CONFIG_WIFI_SAMPLE_SSID) > 1,
	     "CONFIG_WIFI_SAMPLE_SSID is empty. Please set it in conf file.");

static void handle_wifi_scan_result(struct net_mgmt_event_callback *cb)
{
	const struct wifi_scan_result *entry =
		(const struct wifi_scan_result *)cb->info;
	char mac_string_buf[sizeof("xx:xx:xx:xx:xx:xx")];
	uint8_t ssid_print[WIFI_SSID_MAX_LEN + 1];

	scan_context.scan_result++;

	if (scan_context.scan_result == 1U) {
		printk("\n%-4s | %-32s %-5s | %-13s | %-4s | %-20s | %-17s | %-8s\n",
		   "Num", "SSID", "(len)", "Chan (Band)", "RSSI", "Security", "BSSID", "MFP");
	}

	snprintk(ssid_print, sizeof(ssid_print), "%s", entry->ssid);
	if (entry->mac_length) {
		snprintk(mac_string_buf, sizeof(mac_string_buf), MACSTR,
			 entry->mac[0], entry->mac[1], entry->mac[2], entry->mac[3],
			 entry->mac[4], entry->mac[5]);
	}

	printk("%-4d | %-32s %-5u | %-4u (%-6s) | %-4d | %-20s | %-17s | %-8s\n",
	   scan_context.scan_result, ssid_print, entry->ssid_length, entry->channel,
	   wifi_band_txt(entry->band),
	   entry->rssi,
	   ((entry->wpa3_ent_type) ?
		wifi_wpa3_enterprise_txt(entry->wpa3_ent_type)
		 : (entry->security == WIFI_SECURITY_TYPE_EAP ? "WPA2 Enterprise"
		 : wifi_security_txt(entry->security))),
	   ((entry->mac_length) ? mac_string_buf : ""),
	   wifi_mfp_txt(entry->mfp));
}


static void wifi_event_handler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
			       struct net_if *iface)
{
	switch (mgmt_event) {
	case NET_EVENT_WIFI_CONNECT_RESULT: {
		LOG_INF("Connected to %s", CONFIG_WIFI_SAMPLE_SSID);
		break;
	}
	case NET_EVENT_WIFI_DISCONNECT_RESULT: {
		LOG_INF("Disconnected from %s", CONFIG_WIFI_SAMPLE_SSID);
		break;
	}
	case NET_EVENT_WIFI_SCAN_RESULT: {
		handle_wifi_scan_result(cb);
		break;
	}
	case NET_EVENT_WIFI_SCAN_DONE: {
		LOG_INF("Scan done");
		k_sem_give(&scan_sem);
		break;
	}
	default:
		break;
	}
}

static int scan_wifi()
{
	struct wifi_scan_params params = { 0 };
	struct net_if *sta_iface = net_if_get_wifi_sta();
	if (!sta_iface) {
		LOG_INF("STA: interface no initialized");
		return -EIO;
	}

	scan_context.scan_result = 0;
	if (net_mgmt(NET_REQUEST_WIFI_SCAN, sta_iface, &params, sizeof(params))) {
		LOG_ERR("Scan request failed\n");
		return -ENOEXEC;
	}

	k_sem_take(&scan_sem, K_FOREVER);

	return 0;
}

static int connect_to_wifi()
{
	struct net_if *sta_iface = net_if_get_wifi_sta();
	if (!sta_iface) {
		LOG_INF("STA: interface no initialized");
		return -EIO;
	}

	sta_config.ssid = (const uint8_t *)CONFIG_WIFI_SAMPLE_SSID;
	sta_config.ssid_length = sizeof(CONFIG_WIFI_SAMPLE_SSID) - 1;
	sta_config.psk = (const uint8_t *)CONFIG_WIFI_SAMPLE_PSK;
	sta_config.psk_length = sizeof(CONFIG_WIFI_SAMPLE_PSK) - 1;
	sta_config.security = WIFI_SECURITY_TYPE_PSK;
	sta_config.channel = WIFI_CHANNEL_ANY;

	LOG_INF("Connecting to SSID: %s\n", sta_config.ssid);

	int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, sta_iface, &sta_config,
			   sizeof(struct wifi_connect_req_params));
	if (ret) {
		LOG_ERR("Unable to Connect to (%s)", CONFIG_WIFI_SAMPLE_SSID);
	}

	return ret;
}

int main(void)
{
	net_mgmt_init_event_callback(&wifi_event_cb, wifi_event_handler, NET_EVENT_WIFI_MASK);
	net_mgmt_add_event_callback(&wifi_event_cb);

	scan_wifi();

	connect_to_wifi();

	return 0;
}
