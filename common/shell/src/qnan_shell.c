/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 * qnan shell commands — debug-only wrapper over the NAN USD glue layer.
 *
 * In MINIMAL hostap mode (CONFIG_WIFI_NM_WPA_SUPPLICANT_MINIMAL) the
 * wpa_supplicant ctrl_iface is not available, so commands are forwarded
 * directly to qcc730_nan_de_glue.c instead of going through
 * zephyr_nan_ctrl_cmd().
 *
 * Usage:
 *   qnan publish   service_name=<name> srv_proto_type=<type> [ssi=<hex>] [ttl=<sec>] [unsolicited=0|1] [solicited=0|1] [freq_list=<MHz,...>]
 *   qnan subscribe service_name=<name> srv_proto_type=<type> [active=0|1] [ttl=<sec>] [freq=<MHz>]
 *   qnan followup handle=<id> peer=<xx:xx:xx:xx:xx:xx> peer_id=<id> [ssi=<hex>]
 *   qnan cancel_publish publish_id=<id>
 */

#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "inc/qcom_wifi_mgmt.h"

/* -------------------------------------------------------------------------
 * Argument parsing helpers
 * ------------------------------------------------------------------------- */

static const char *arg_find(const char *key, size_t argc, char **argv)
{
	size_t klen = strlen(key);

	for (size_t i = 1; i < argc; i++) {
		if (strncmp(argv[i], key, klen) == 0 && argv[i][klen] == '=')
			return &argv[i][klen + 1];
	}
	return NULL;
}

/* -------------------------------------------------------------------------
 * Shell command implementations
 * ------------------------------------------------------------------------- */

static int cmd_nan_publish(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "Usage: qnan publish service_name=<name> "
			    "srv_proto_type=<type> [ssi=<hex>] [ttl=<sec>] "
			    "[unsolicited=0|1] [solicited=0|1] "
			    "[band=2|5] [freq_list=<MHz,...>]");
		return -EINVAL;
	}

	const char *svc = arg_find("service_name", argc, argv);
	const char *proto_str = arg_find("srv_proto_type", argc, argv);
	const char *ssi_str = arg_find("ssi", argc, argv);
	const char *ttl_str = arg_find("ttl", argc, argv);
	const char *unsol_str = arg_find("unsolicited", argc, argv);
	const char *sol_str = arg_find("solicited", argc, argv);
	const char *band_str = arg_find("band", argc, argv);
	const char *flist_str = arg_find("freq_list", argc, argv);

	if (!svc || !proto_str) {
		shell_error(sh, "service_name and srv_proto_type are required");
		return -EINVAL;
	}

	uint8_t proto = (uint8_t)strtoul(proto_str, NULL, 0);
	unsigned int ttl = ttl_str ? (unsigned int)strtoul(ttl_str, NULL, 0) : 0;

	/* Default: both enabled (matches wpa_supplicant USD default and the
	 * Matter commissionee model). Pass unsolicited=0 / solicited=0 to
	 * select a single mode. */
	bool unsolicited = unsol_str ? (strtoul(unsol_str, NULL, 0) != 0) : true;
	bool solicited   = sol_str   ? (strtoul(sol_str,   NULL, 0) != 0) : true;

	if (!unsolicited && !solicited) {
		shell_error(sh, "at least one of unsolicited/solicited must be 1");
		return -EINVAL;
	}

	uint8_t ssi_buf[64];
	const uint8_t *ssi = NULL;
	size_t ssi_len = 0;

	if (ssi_str) {
		size_t hexlen = strlen(ssi_str);
		/* hex2bin from zephyr/sys/util.h: (hex, hexlen, buf, buflen) */
		size_t n = hex2bin(ssi_str, hexlen, ssi_buf, sizeof(ssi_buf));

		if (n == 0) {
			shell_error(sh, "ssi: invalid hex string");
			return -EINVAL;
		}
		ssi = ssi_buf;
		ssi_len = n;
	}

	/* Frequency selection priority (highest to lowest):
	 *   1. freq_list=<MHz,...>  — explicit list, full control
	 *   2. band=5               — 5GHz 3-channel rotation preset
	 *                             {5745,5745,5180,5220} (ch149 weighted 2x)
	 *   3. band=2 / no params   — 2.4GHz default (handled by glue layer,
	 *                             pass NULL freq_list)
	 * band= is resolved entirely in the shell layer; only freq_list is
	 * passed down to net_mgmt / glue. */
#define NAN_FREQ_LIST_MAX 8
	/* 5GHz rotation preset: ch149 weighted 2x to match Matter commissioner
	 * hardcoded default (CHIP_DEVICE_CONFIG_WIFIPAF_5G_UP_DEFAULT_CHNL). */
	static const int freq_list_5g_preset[] = {5745, 5745, 5180, 5220, 0};

	int freq_list_buf[NAN_FREQ_LIST_MAX + 1]; /* +1 for null terminator */
	const int *freq_list = NULL;

	if (flist_str) {
		/* Explicit freq_list overrides band= */
		char buf[64];
		strlcpy(buf, flist_str, sizeof(buf));
		int count = 0;
		char *tok = buf;
		char *end;

		while (*tok && count < NAN_FREQ_LIST_MAX) {
			long val = strtol(tok, &end, 10);

			if (end == tok)
				break;
			freq_list_buf[count++] = (int)val;
			tok = (*end == ',') ? end + 1 : end;
		}
		if (count == 0) {
			shell_error(sh, "freq_list: no valid frequencies");
			return -EINVAL;
		}
		freq_list_buf[count] = 0; /* null terminate */
		freq_list = freq_list_buf;
	} else if (band_str && strtoul(band_str, NULL, 0) == 5) {
		/* band=5: use 5GHz rotation preset */
		freq_list = freq_list_5g_preset;
	}
	/* band=2 or no band/freq_list: pass NULL, glue uses 2.4GHz default */

	struct qcom_wifi_nan_publish_params params = {
		.service_name   = svc,
		.srv_proto_type = proto,
		.freq_list      = freq_list,
		.ssi            = ssi,
		.ssi_len        = ssi_len,
		.ttl            = ttl,
		.unsolicited    = unsolicited,
		.solicited      = solicited,
	};

	struct net_if *iface = net_if_get_wifi_sta();
	int id = net_mgmt(NET_REQUEST_WIFI_QCOM_NAN_PUBLISH, iface,
			  &params, sizeof(params));

	if (id < 0) {
		shell_error(sh, "NAN publish failed (ret=%d)", id);
		return id;
	}
	shell_print(sh, "OK publish_id=%d", id);
	return 0;
}

static int cmd_nan_cancel_publish(const struct shell *sh,
				  size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "Usage: qnan cancel_publish publish_id=<id>");
		return -EINVAL;
	}

	const char *id_str = arg_find("publish_id", argc, argv);

	if (!id_str) {
		shell_error(sh, "publish_id is required");
		return -EINVAL;
	}

	int pub_id = (int)strtol(id_str, NULL, 0);

	struct qcom_wifi_nan_cancel_publish_params params = {
		.publish_id = pub_id,
	};

	struct net_if *iface = net_if_get_wifi_sta();
	net_mgmt(NET_REQUEST_WIFI_QCOM_NAN_CANCEL_PUBLISH, iface,
		 &params, sizeof(params));
	shell_print(sh, "OK");
	return 0;
}

static int cmd_nan_subscribe(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "Usage: qnan subscribe service_name=<name> "
			    "srv_proto_type=<type> [active=0|1] [ttl=<sec>] "
			    "[freq=<MHz>]");
		return -EINVAL;
	}

	const char *svc = arg_find("service_name", argc, argv);
	const char *proto_str = arg_find("srv_proto_type", argc, argv);
	const char *active_str = arg_find("active", argc, argv);
	const char *ttl_str = arg_find("ttl", argc, argv);
	const char *freq_str = arg_find("freq", argc, argv);

	if (!svc || !proto_str) {
		shell_error(sh, "service_name and srv_proto_type are required");
		return -EINVAL;
	}

	uint8_t proto = (uint8_t)strtoul(proto_str, NULL, 0);
	/* Default passive; active=1 transmits Subscribe SDFs to solicit
	 * replies from solicited publishers. */
	bool active = active_str ? (strtoul(active_str, NULL, 0) != 0) : false;
	/* Default ttl=30s: keeps subscriber alive long enough to receive
	 * follow-ups across dwell windows while not running indefinitely.
	 * Pass ttl=300 for manual testing / sniffer captures. */
	unsigned int ttl = ttl_str ? (unsigned int)strtoul(ttl_str, NULL, 0) : 30;
	/* Default freq=0: glue layer resolves to NAN_USD_DEFAULT_FREQ (2437).
	 * Subscriber listens on a single fixed channel; publisher's freq_list
	 * drives multi-channel rotation. Aligned to upstream wpa_supplicant. */
	unsigned int freq = freq_str ? (unsigned int)strtoul(freq_str, NULL, 0) : 0;

	struct qcom_wifi_nan_subscribe_params params = {
		.service_name   = svc,
		.srv_proto_type = proto,
		.active         = active,
		.ttl            = ttl,
		.freq           = freq,
	};

	struct net_if *iface = net_if_get_wifi_sta();
	int id = net_mgmt(NET_REQUEST_WIFI_QCOM_NAN_SUBSCRIBE, iface,
			  &params, sizeof(params));

	if (id < 0) {
		shell_error(sh, "NAN subscribe failed (ret=%d)", id);
		return id;
	}
	shell_print(sh, "OK subscribe_id=%d", id);
	return 0;
}

/* Parse "xx:xx:xx:xx:xx:xx" into 6 bytes. Returns 0 on success. */
static int parse_mac(const char *str, uint8_t out[6])
{
	unsigned int v[6];

	if (!str)
		return -EINVAL;
	if (sscanf(str, "%x:%x:%x:%x:%x:%x",
		   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
		return -EINVAL;
	for (int i = 0; i < 6; i++) {
		if (v[i] > 0xff)
			return -EINVAL;
		out[i] = (uint8_t)v[i];
	}
	return 0;
}

static int cmd_nan_followup(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 4) {
		shell_error(sh, "Usage: qnan followup handle=<id> "
			    "peer=<xx:xx:xx:xx:xx:xx> peer_id=<id> [ssi=<hex>]");
		return -EINVAL;
	}

	const char *handle_str = arg_find("handle", argc, argv);
	const char *peer_str = arg_find("peer", argc, argv);
	const char *peer_id_str = arg_find("peer_id", argc, argv);
	const char *ssi_str = arg_find("ssi", argc, argv);

	if (!handle_str || !peer_str || !peer_id_str) {
		shell_error(sh, "handle, peer and peer_id are required");
		return -EINVAL;
	}

	struct qcom_wifi_nan_transmit_params params = {0};

	params.handle = (int)strtol(handle_str, NULL, 0);
	params.req_instance_id = (uint8_t)strtoul(peer_id_str, NULL, 0);

	if (parse_mac(peer_str, params.peer_addr) != 0) {
		shell_error(sh, "peer: invalid MAC (expect xx:xx:xx:xx:xx:xx)");
		return -EINVAL;
	}

	uint8_t ssi_buf[64];

	if (ssi_str) {
		size_t n = hex2bin(ssi_str, strlen(ssi_str), ssi_buf,
				   sizeof(ssi_buf));

		if (n == 0) {
			shell_error(sh, "ssi: invalid hex string");
			return -EINVAL;
		}
		params.ssi = ssi_buf;
		params.ssi_len = n;
	}

	struct net_if *iface = net_if_get_wifi_sta();
	int ret = net_mgmt(NET_REQUEST_WIFI_QCOM_NAN_TRANSMIT, iface,
			   &params, sizeof(params));

	if (ret < 0) {
		shell_error(sh, "NAN followup failed (ret=%d)", ret);
		return ret;
	}
	shell_print(sh, "OK");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_qnan_commands,
	SHELL_CMD_ARG(publish, NULL,
		      "Start NAN USD Publisher (debug only).\n"
		      "Usage: qnan publish service_name=<name> "
		      "srv_proto_type=<type> [ssi=<hex>] [ttl=<sec>] "
		      "[unsolicited=0|1] [solicited=0|1] "
		      "[band=2|5] [freq_list=<MHz,...>]\n"
		      "  band=2 (default): 2.4GHz rotation {ch6,ch6,ch1,ch11}\n"
		      "  band=5:           5GHz rotation {ch149,ch149,ch36,ch44}\n"
		      "  freq_list=<MHz>:  override band, e.g. freq_list=5745\n",
		      cmd_nan_publish, 2, 10),
	SHELL_CMD_ARG(cancel_publish, NULL,
		      "Cancel NAN USD Publisher.\n"
		      "Usage: qnan cancel_publish publish_id=<id>\n",
		      cmd_nan_cancel_publish, 2, 0),
	SHELL_CMD_ARG(subscribe, NULL,
		      "Start NAN USD Subscriber (debug only).\n"
		      "Usage: qnan subscribe service_name=<name> "
		      "srv_proto_type=<type> [active=0|1] [ttl=<sec>] [freq=<MHz>]\n",
		      cmd_nan_subscribe, 2, 8),
	SHELL_CMD_ARG(followup, NULL,
		      "Send a NAN follow-up to a discovered peer.\n"
		      "Usage: qnan followup handle=<id> "
		      "peer=<xx:xx:xx:xx:xx:xx> peer_id=<id> [ssi=<hex>]\n",
		      cmd_nan_followup, 4, 4),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(qnan, &sub_qnan_commands, "NAN USD debug commands", NULL);
