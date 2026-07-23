/*
* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
* SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <cat.h>
#include "qat_api.h"
#include "../../httpc_at/inc/httpc_at_handler.h"
#include "../../httpc_at/inc/httpc_at_core.h"
#include "fw_upgrade.h"

LOG_MODULE_REGISTER(qat_ota, LOG_LEVEL_INF);

#define OTA_FLASH_WRITE_SIZE  1024U  /* flash write granularity (bytes) */
#define OTA_RANGE_MAX_RETRIES 1      /* reconnect attempts on download failure */

extern void nt_system_sw_reset(void);
extern fw_upgrade_context_t *fw_upgrade_sess_cxt;

static atomic_t g_ota_in_progress = ATOMIC_INIT(0);

static struct k_thread g_ota_thread;
static K_THREAD_STACK_DEFINE(g_ota_stack, CONFIG_QAT_OTA_STACK_SIZE);

static void ota_trial_reboot_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g_ota_trial_reboot_work, ota_trial_reboot_handler);

struct ota_thread_params {
	char     url[FW_UPGRADE_URL_LEN + 1];
	char     filename[FW_UPGRADE_FILENAME_LEN + 1];
	uint32_t flags;
	uint32_t timeout_ms;
	uint32_t size;
	bool     range_get;
	bool     ipv6;
};

static struct ota_thread_params g_ota_params;
static uint32_t g_ota_offset;

/*
 * Shared body buffer: data is accumulated here and flushed to flash in
 * OTA_FLASH_WRITE_SIZE blocks.  Both range and non-range paths use the same
 * inline-flush approach; the final partial block is written by the worker
 * after httpc_at_execute() returns.
 *
 * Inline flash I/O is safe because the caller always uses SYS_FOREVER_MS,
 * so sector erases inside the callback cannot cause a timeout.
 */
static uint8_t  *g_ota_range_buf;
static uint32_t g_ota_range_buf_len;

static bool g_httpc_ota_initialized;

static int ota_ensure_httpc_initialized(void)
{
	int ret;

	if (g_httpc_ota_initialized) {
		return 0;
	}

	ret = httpc_at_init(NULL, NULL);
	if (ret < 0) {
		LOG_ERR("httpc_at_init failed: %d", ret);
		return ret;
	}

	g_httpc_ota_initialized = true;
	return 0;
}

/*
 * Synchronize fw_upgrade_sess_cxt->flags with the AT-command flags.
 * The session context may be reused from a previous OTA run (warning:
 * "already allocated, reusing existing context"), carrying stale flag bits —
 * most critically FW_UPGRADE_FLAG_AUTO_REBOOT.  If that bit is left set, the
 * backend reboots from within fw_upgrade_session_process() when it reaches
 * FINISH state, bypassing qat_ota.c's own reboot logic and +EVT:OTAFWUP_FIN.
 *
 * AUTO_REBOOT is always cleared; qat_ota.c issues the reboot after emitting
 * +EVT:OTAFWUP_FIN so the host can act on the event before the device resets.
 * DUPLICATE_ACTIVE_FS mirrors AT-command flag bit 1.
 */
static void ota_sync_flags(void)
{
	if (fw_upgrade_sess_cxt == NULL) {
		return;
	}

	uint32_t f = fw_upgrade_sess_cxt->flags;

	f &= ~FW_UPGRADE_FLAG_AUTO_REBOOT;

	if (g_ota_params.flags & FW_UPGRADE_FLAG_DUPLICATE_ACTIVE_FS) {
		f |= FW_UPGRADE_FLAG_DUPLICATE_ACTIVE_FS;
	} else {
		f &= ~FW_UPGRADE_FLAG_DUPLICATE_ACTIVE_FS;
	}

	fw_upgrade_sess_cxt->flags = f;
}

/*
 * HTTP body callback: accumulate incoming data in g_ota_range_buf and flush
 * to flash in OTA_FLASH_WRITE_SIZE blocks.  The worker writes the final
 * partial block after httpc_at_execute() returns.
 *
 * Inline flash I/O is safe: both range and non-range paths use SYS_FOREVER_MS.
 */
static void ota_http_body_cb(const char *data, size_t len, void *user_data)
{
	int *error_out = (int *)user_data;

	if (*error_out != 0 || len == 0) {
		return;
	}

	while (len > 0) {
		uint32_t space = (uint32_t)(OTA_FLASH_WRITE_SIZE - g_ota_range_buf_len);

		if (space == 0) {
			/* Flush full block to flash */
			int fret = fw_upgrade_session_process(g_ota_offset, g_ota_range_buf,
							      g_ota_range_buf_len);

			if (fret != 0) {
				LOG_ERR("fw_upgrade_session_process failed at %u: %d",
					g_ota_offset, fret);
				*error_out = fret;
				return;
			}

			ota_sync_flags();
			g_ota_offset += g_ota_range_buf_len;
			g_ota_range_buf_len = 0;
			space = OTA_FLASH_WRITE_SIZE;
		}

		uint32_t copy = (uint32_t)MIN(len, (size_t)space);

		memcpy(g_ota_range_buf + g_ota_range_buf_len, data, copy);
		g_ota_range_buf_len += copy;
		data += copy;
		len  -= copy;
	}
}

static void emit_ota_error(int code)
{
	char buf[40];

	snprintf(buf, sizeof(buf), "\r\n+EVT:OTAFWUP_ERROR:%d\r\n", code);
	QAT_Output((uint32_t)strlen(buf), buf);
}

static void ota_worker_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int                      flash_error = 0;
	int                      http_ret;
	struct httpc_at_request  req = {0};
	struct httpc_at_response resp = {0};
	char                     full_url[FW_UPGRADE_URL_LEN + FW_UPGRADE_FILENAME_LEN + 16];
	char                     range_hdr[64];
	const char              *extra_headers[1];

	g_ota_range_buf = k_malloc(OTA_FLASH_WRITE_SIZE);
	if (!g_ota_range_buf) {
		emit_ota_error(-ENOMEM);
		atomic_set(&g_ota_in_progress, 0);
		return;
	}

	QAT_Output(sizeof("\r\n+EVT:OTAFWUP_START\r\n") - 1, "\r\n+EVT:OTAFWUP_START\r\n");

	snprintf(full_url, sizeof(full_url), "%s/%s", g_ota_params.url, g_ota_params.filename);

	req.url        = full_url;
	req.method     = HTTPC_AT_METHOD_GET;
	req.body       = NULL;
	req.body_len   = 0;
	req.auth_type  = HTTPC_AT_AUTH_NONE;
	req.http_port  = HTTPC_AT_DEFAULT_HTTP_PORT;
	req.https_port = HTTPC_AT_DEFAULT_HTTPS_PORT;
	req.ip_family  = g_ota_params.ipv6 ? AF_INET6 : AF_INET;

	if (g_ota_params.range_get) {
		/*
		 * Range download: one TCP connection per attempt, with an open-ended
		 * Range header starting from the last committed offset.
		 *
		 * Range serves resume-after-failure only: g_ota_offset always reflects
		 * the last successfully written byte, so reconnecting with
		 * "Range: bytes=<g_ota_offset>-" continues exactly where the previous
		 * connection left off.  There is no per-block reconnection.
		 *
		 * SYS_FOREVER_MS: flash sector erases inside ota_http_body_cb may
		 * block for tens of seconds; a finite deadline would fire mid-download.
		 */
		req.timeout_ms         = SYS_FOREVER_MS;
		req.extra_headers      = extra_headers;
		req.extra_header_count = 1;

		int retries = 0;

		while (true) {
			snprintf(range_hdr, sizeof(range_hdr),
				 "Range: bytes=%u-\r\n", g_ota_offset);
			extra_headers[0] = range_hdr;

			g_ota_range_buf_len = 0;
			flash_error = 0;
			memset(&resp, 0, sizeof(resp));

			http_ret = httpc_at_execute(&req, &resp,
						    ota_http_body_cb,
						    &flash_error);

			/* Write final partial block left in the buffer */
			if (g_ota_range_buf_len > 0) {
				int fret = fw_upgrade_session_process(
					g_ota_offset,
					g_ota_range_buf,
					g_ota_range_buf_len);

				if (fret != 0) {
					LOG_ERR("fw_upgrade_session_process failed "
						"at offset %u: %d", g_ota_offset, fret);
					emit_ota_error(fret);
					goto cleanup;
				}

				ota_sync_flags();
				g_ota_offset += g_ota_range_buf_len;
				g_ota_range_buf_len = 0;
			}

			if (flash_error != 0) {
				emit_ota_error(flash_error);
				goto cleanup;
			}

			if (http_ret < 0) {
				if (retries < OTA_RANGE_MAX_RETRIES) {
					char retry_buf[48];

					snprintf(retry_buf, sizeof(retry_buf),
						 "\r\n+EVT:OTAFWUP_RETRY:%u\r\n",
						 g_ota_offset);
					QAT_Output((uint32_t)strlen(retry_buf), retry_buf);
					retries++;
					continue;
				}
				emit_ota_error(http_ret);
				goto cleanup;
			}

			if (resp.status_code != 206 && resp.status_code != 200) {
				emit_ota_error(-resp.status_code);
				goto cleanup;
			}

			break;
		}
	} else {
		/*
		 * Non-range path: single GET streamed to flash via ota_http_body_cb.
		 * The callback flushes full 1024-byte blocks inline; the final partial
		 * block remains in g_ota_range_buf and is written below.
		 * SYS_FOREVER_MS: flash sector erases are unbounded in duration; a
		 * finite deadline would fire mid-download.
		 */
		g_ota_range_buf_len = 0;
		req.timeout_ms = SYS_FOREVER_MS;

		http_ret = httpc_at_execute(&req, &resp, ota_http_body_cb, &flash_error);

		if (http_ret < 0) {
			emit_ota_error(http_ret);
			goto cleanup;
		}

		if (resp.status_code != 200 && resp.status_code != 206) {
			emit_ota_error(-resp.status_code);
			goto cleanup;
		}

		if (flash_error != 0) {
			emit_ota_error(flash_error);
			goto cleanup;
		}

		/* Write the final partial block (< 1024 B) buffered in the callback */
		if (g_ota_range_buf_len > 0) {
			int fret = fw_upgrade_session_process(g_ota_offset, g_ota_range_buf,
							      g_ota_range_buf_len);

			if (fret != 0) {
				LOG_ERR("fw_upgrade_session_process failed at %u: %d",
					g_ota_offset, fret);
				emit_ota_error(fret);
				goto cleanup;
			}

			ota_sync_flags();
			g_ota_offset += g_ota_range_buf_len;
			g_ota_range_buf_len = 0;
		}
	}

	if (g_ota_params.flags & FW_UPGRADE_FLAG_AUTO_REBOOT) {
		QAT_Output(sizeof("\r\n+EVT:OTAFWUP_FIN:reset\r\n") - 1,
			   "\r\n+EVT:OTAFWUP_FIN:reset\r\n");
	} else {
		QAT_Output(sizeof("\r\n+EVT:OTAFWUP_FIN\r\n") - 1, "\r\n+EVT:OTAFWUP_FIN\r\n");
	}

	atomic_set(&g_ota_in_progress, 0);
	if (g_ota_params.flags & FW_UPGRADE_FLAG_AUTO_REBOOT) {
		k_sleep(K_MSEC(400));
		nt_system_sw_reset();
	}
	k_free(g_ota_range_buf);
	g_ota_range_buf = NULL;
	return;

cleanup:
	k_free(g_ota_range_buf);
	g_ota_range_buf = NULL;
	atomic_set(&g_ota_in_progress, 0);
}

static int ota_parse_args(const uint8_t *data, size_t data_size, struct ota_thread_params *p)
{
	char    buf[FW_UPGRADE_URL_LEN + FW_UPGRADE_FILENAME_LEN + 64];
	char   *tokens[6];
	int     token_count = 0;
	bool    in_quotes   = false;
	char   *start;
	size_t  copy_len;

	if (!data || data_size == 0) {
		return -EINVAL;
	}

	copy_len = data_size;
	if (copy_len >= sizeof(buf)) {
		return -E2BIG;
	}

	memcpy(buf, data, copy_len);
	buf[copy_len] = '\0';

	start = buf;
	for (char *p2 = buf; ; p2++) {
		char ch = *p2;

		if (ch == '"') {
			in_quotes = !in_quotes;
			continue;
		}

		if ((ch == ',' && !in_quotes) || ch == '\0') {
			if (token_count >= 6) {
				return -E2BIG;
			}
			tokens[token_count++] = start;
			if (ch == '\0') {
				break;
			}
			*p2   = '\0';
			start = p2 + 1;
		}
	}

	if (token_count < 3) {
		return -EINVAL;
	}

	/* token 0: protocol — strip quotes, must be "http" */
	{
		char *proto = tokens[0];
		size_t len  = strlen(proto);

		if (len >= 2 && proto[0] == '"' && proto[len - 1] == '"') {
			proto[len - 1] = '\0';
			proto++;
		}
		if (strcmp(proto, "http") != 0) {
			return -EINVAL;
		}
	}

	/* token 1: url */
	{
		char  *url = tokens[1];
		size_t len = strlen(url);

		if (len >= 2 && url[0] == '"' && url[len - 1] == '"') {
			url[len - 1] = '\0';
			url++;
			len -= 2;
		}

		/* Accept http:// or https:// as-is; prepend http:// if no scheme */
		if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) {
			if (len > sizeof(p->url) - 1) {
				return -ENAMETOOLONG;
			}
			strlcpy(p->url, url, FW_UPGRADE_URL_LEN);
		} else {
			/* After prepending "http://" the total length is len+7 */
			if (len + 7 + 1 > sizeof(p->url)) {
				return -ENAMETOOLONG;
			}
			int n = snprintf(p->url, sizeof(p->url), "http://%s", url);

			if (n < 0 || (size_t)n >= sizeof(p->url)) {
				return -ENAMETOOLONG;
			}
		}
		p->url[FW_UPGRADE_URL_LEN] = '\0';

		/* Detect IPv6 bracket notation: http://[...] or https://[...] */
		{
			const char *bracket = strchr(p->url, '[');

			p->ipv6 = (bracket != NULL);
		}
	}

	/* token 2: fw_filename */
	{
		char  *fname = tokens[2];
		size_t len   = strlen(fname);

		if (len >= 2 && fname[0] == '"' && fname[len - 1] == '"') {
			fname[len - 1] = '\0';
			fname++;
			len -= 2;
		}
		if (len > FW_UPGRADE_FILENAME_LEN) {
			return -ENAMETOOLONG;
		}
		strlcpy(p->filename, fname, FW_UPGRADE_FILENAME_LEN);
		p->filename[FW_UPGRADE_FILENAME_LEN] = '\0';
	}

	/* token 3: flag (optional, default 1) */
	p->flags = 1;
	if (token_count >= 4 && tokens[3][0] != '\0') {
		char *end = NULL;

		p->flags = (uint32_t)strtoul(tokens[3], &end, 10);
		if (!end || *end != '\0') {
			return -EINVAL;
		}
	}

	/* token 4: timeout (optional, default 20000) */
	p->timeout_ms = 20000;
	if (token_count >= 5 && tokens[4][0] != '\0') {
		char *end = NULL;

		p->timeout_ms = (uint32_t)strtoul(tokens[4], &end, 10);
		if (!end || *end != '\0') {
			return -EINVAL;
		}
	}

	/* token 5: size (optional, required when range_get) */
	p->size = 0;
	if (token_count >= 6 && tokens[5][0] != '\0') {
		char *end = NULL;

		p->size = (uint32_t)strtoul(tokens[5], &end, 10);
		if (!end || *end != '\0') {
			return -EINVAL;
		}
	}

	p->range_get = (p->flags & 0x04) != 0;

	return 0;
}

static cat_return_state cmd_ota_otafwup_run(const struct cat_command *cmd)
{
	ARG_UNUSED(cmd);
	return QAT_Response_Str(QAT_RC_OK,
				"+OTAFWUP=http,<http|https url>,<fw_filename>,[flag],[timeout],[size]");
}

static cat_return_state cmd_ota_otafwup_write(const struct cat_command *cmd, const uint8_t *data,
					      const size_t data_size, const size_t args_num)
{
	ARG_UNUSED(cmd);
	ARG_UNUSED(args_num);

	int ret;

	ret = ota_ensure_httpc_initialized();
	if (ret < 0) {
		return CAT_RETURN_STATE_ERROR;
	}

	ret = ota_parse_args(data, data_size, &g_ota_params);
	if (ret < 0) {
		LOG_ERR("ota_parse_args failed: %d", ret);
		return CAT_RETURN_STATE_ERROR;
	}

	if (!atomic_cas(&g_ota_in_progress, 0, 1)) {
		LOG_ERR("OTA already in progress");
		return CAT_RETURN_STATE_ERROR;
	}

	g_ota_offset      = 0;
	g_ota_range_buf_len = 0;

	k_thread_create(&g_ota_thread, g_ota_stack, K_THREAD_STACK_SIZEOF(g_ota_stack),
			ota_worker_thread_fn, NULL, NULL, NULL,
			CONFIG_QAT_OTA_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&g_ota_thread, "ota_worker");

	return CAT_RETURN_STATE_OK;
}

static void ota_trial_reboot_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	nt_system_sw_reset();
}

static cat_return_state cmd_ota_otatrial_run(const struct cat_command *cmd)
{
	ARG_UNUSED(cmd);
	return QAT_Response_Str(QAT_RC_OK, "+OTATRIAL=<accept>,<reboot_flag>");
}

static cat_return_state cmd_ota_otatrial_write(const struct cat_command *cmd, const uint8_t *data,
					       const size_t data_size, const size_t args_num)
{
	ARG_UNUSED(cmd);
	ARG_UNUSED(args_num);

	char                  buf[32];
	char                 *tokens[2];
	int                   token_count = 0;
	char                 *start;
	int                   accept;
	int                   reboot_flag;
	fw_upgrade_status_code_t fw_ret;
	const char           *suffix;
	char                  resp_buf[64];
	QAT_Result_Enum_Type  rc;
	size_t                copy_len;

	if (!data || data_size == 0) {
		return CAT_RETURN_STATE_ERROR;
	}

	copy_len = data_size;
	if (copy_len >= sizeof(buf)) {
		return CAT_RETURN_STATE_ERROR;
	}

	memcpy(buf, data, copy_len);
	buf[copy_len] = '\0';

	start = buf;
	for (char *p = buf; ; p++) {
		char ch = *p;

		if (ch == ',' || ch == '\0') {
			if (token_count >= 2) {
				return CAT_RETURN_STATE_ERROR;
			}
			tokens[token_count++] = start;
			if (ch == '\0') {
				break;
			}
			*p    = '\0';
			start = p + 1;
		}
	}

	if (token_count != 2) {
		return CAT_RETURN_STATE_ERROR;
	}

	{
		char *end = NULL;
		long  v   = strtol(tokens[0], &end, 10);

		if (!end || *end != '\0' || (v != 0 && v != 1)) {
			return CAT_RETURN_STATE_ERROR;
		}
		accept = (int)v;
	}

	{
		char *end = NULL;
		long  v   = strtol(tokens[1], &end, 10);

		if (!end || *end != '\0' || (v != 0 && v != 1)) {
			return CAT_RETURN_STATE_ERROR;
		}
		reboot_flag = (int)v;
	}

	fw_ret = fw_upgrade_session_done((uint32_t)accept);

	if (accept == 1) {
		if (fw_ret == FW_UPGRADE_OK_E) {
			suffix = "Success to Accept Trial FWD";
			rc     = QAT_RC_OK;
		} else {
			suffix = "Fail to Accept Trial FWD";
			rc     = QAT_RC_ERROR;
		}
	} else {
		if (fw_ret == FW_UPGRADE_OK_E) {
			suffix = "Success to Reject Trial FWD";
			rc     = QAT_RC_OK;
		} else {
			suffix = "Fail to Reject Trial FWD";
			rc     = QAT_RC_ERROR;
		}
	}

	snprintf(resp_buf, sizeof(resp_buf), "+OTATRIAL:%s, %d\r\n", suffix, reboot_flag);
	QAT_Output((uint32_t)strlen(resp_buf), resp_buf);

	if (rc == QAT_RC_OK && reboot_flag == 1) {
		k_work_schedule(&g_ota_trial_reboot_work, K_MSEC(400));
	}

	if (rc == QAT_RC_ERROR) {
		return CAT_RETURN_STATE_ERROR;
	}
	return CAT_RETURN_STATE_OK;
}

static struct cat_command qat_ota_cmds[] = {
	{
		.name        = "+OTAFWUP",
		.description = "Initiate OTA firmware upgrade over HTTP",
		.run         = cmd_ota_otafwup_run,
		.write       = cmd_ota_otafwup_write,
	},
	{
		.name        = "+OTATRIAL",
		.description = "Accept or reject OTA trial image",
		.run         = cmd_ota_otatrial_run,
		.write       = cmd_ota_otatrial_write,
	},
};

static struct cat_command_group qat_ota_cmd_group = {
	.name    = "QAT_OTA",
	.cmd     = qat_ota_cmds,
	.cmd_num = ARRAY_SIZE(qat_ota_cmds),
};

static struct cat_command_group *qat_ota_get_command_group(void)
{
	return &qat_ota_cmd_group;
}

QAT_REGISTER_CMD_GROUP(qat_ota_get_command_group, "OTA");
