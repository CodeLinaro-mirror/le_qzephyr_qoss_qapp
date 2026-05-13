/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/http/status.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <cat.h>
#include "qat_api.h"
#include <zephyr/fs/fs.h>
#include <zephyr/sys/util.h>
#include <wlan_lib_version.h>
#include <zephyr/net/tls_credentials.h>

LOG_MODULE_REGISTER(qat_httpserver, LOG_LEVEL_INF);

#define AT_WEB_MAX_SSID_SIZE   32
#define AT_WEB_MAX_PWD_SIZE    64
#define AT_WEB_SCRATCH_BUFSIZE 320
#define AT_WEB_EVT_BUFSIZE     (AT_WEB_MAX_SSID_SIZE + AT_WEB_MAX_PWD_SIZE + 32)

/* LittleFS credential storage — format: ssid=<ssid>&password=<pwd> */
#define AT_LFS_WIFI_FILE "/lfs/webprov"
#define AT_LFS_WIFI_BUFSIZE (AT_WEB_MAX_SSID_SIZE + AT_WEB_MAX_PWD_SIZE + 24)

/*-------------------------------------------------------------------------
 * TLS configuration
 *-----------------------------------------------------------------------*/
/* sec_tags 20/21/22 — separate from httpc_at which uses 10/11/12 */
#define AT_WEB_TLS_TAG_SERVER_CERT 20
#define AT_WEB_TLS_TAG_SERVER_KEY  21

#define AT_WEB_TLS_PATH_MAXLEN 64

struct qat_web_ssl_cfg {
    int  scheme; /* 0 = HTTP; 1 = HTTPS */
    char cert_file[AT_WEB_TLS_PATH_MAXLEN];
    char key_file[AT_WEB_TLS_PATH_MAXLEN];
};

static struct qat_web_ssl_cfg g_ssl_cfg;
static uint8_t *g_cert_buf;
static uint8_t *g_key_buf;

static const sec_tag_t g_https_sec_tags[] = {
    AT_WEB_TLS_TAG_SERVER_CERT,
    AT_WEB_TLS_TAG_SERVER_KEY,
};

/*-------------------------------------------------------------------------
 * State
 *-----------------------------------------------------------------------*/
static uint16_t g_http_port;   /* active port for plain HTTP service  */
static uint16_t g_https_port;  /* active port for HTTPS service       */
static bool g_server_running;
static int  g_server_scheme;   /* 0 = HTTP, 1 = HTTPS, mirrors g_ssl_cfg.scheme */

/* Scratch buffer for accumulated POST body (HTTP server is single-threaded) */
static char g_scratch[AT_WEB_SCRATCH_BUFSIZE];
static size_t g_scratch_len;

/*-------------------------------------------------------------------------
 * Static HTML page
 *-----------------------------------------------------------------------*/
static const char g_index_html[] =
	"<html><head><title>Wi-Fi Configuration</title></head>"
	"<body><h1>Wi-Fi Setup</h1>"
	"<form action=\"/setwifi\" method=\"POST\">"
	"SSID: <input type=\"text\" name=\"ssid\" required><br>"
	"Password: <input type=\"text\" name=\"password\" required><br>"
	"<button type=\"submit\">Submit</button>"
	"</form></body></html>";

/*-------------------------------------------------------------------------
 * HTTP service & resource forward declarations
 *-----------------------------------------------------------------------*/
static int index_handler(struct http_client_ctx *client, enum http_data_status status,
             const struct http_request_ctx *request_ctx,
             struct http_response_ctx *response_ctx, void *user_data);

static int getversion_handler(struct http_client_ctx *client, enum http_data_status status,
                  const struct http_request_ctx *request_ctx,
                  struct http_response_ctx *response_ctx, void *user_data);

static int setwifi_handler(struct http_client_ctx *client, enum http_data_status status,
               const struct http_request_ctx *request_ctx,
               struct http_response_ctx *response_ctx, void *user_data);

static int getssid_handler(struct http_client_ctx *client, enum http_data_status status,
               const struct http_request_ctx *request_ctx,
               struct http_response_ctx *response_ctx, void *user_data);

static struct http_resource_detail_dynamic index_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET),
    },
    .cb = index_handler,
};

static struct http_resource_detail_dynamic getversion_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET),
    },
    .cb = getversion_handler,
};

static struct http_resource_detail_dynamic setwifi_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_POST),
    },
    .cb = setwifi_handler,
};

static struct http_resource_detail_dynamic getssid_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET),
    },
    .cb = getssid_handler,
};

/*
 * HTTP and HTTPS service instances are both defined at compile time.
 * AT+WEBSERVER sets g_http_port (HTTP mode) or g_https_port (HTTPS mode)
 * to the requested port; the other stays at 0 (binds to an ephemeral port,
 * effectively inaccessible to users).
 */
HTTP_SERVICE_DEFINE(qat_ws_http, "0.0.0.0", &g_http_port,
            CONFIG_HTTP_SERVER_MAX_CLIENTS, 5, NULL, NULL, NULL);
HTTPS_SERVICE_DEFINE(qat_ws_https, "0.0.0.0", &g_https_port,
             CONFIG_HTTP_SERVER_MAX_CLIENTS, 5,
             NULL, NULL, NULL, g_https_sec_tags, sizeof(g_https_sec_tags));
#if defined(CONFIG_NET_IPV6)
HTTP_SERVICE_DEFINE(qat_ws_http_v6, "::", &g_http_port,
            CONFIG_HTTP_SERVER_MAX_CLIENTS, 5, NULL, NULL, NULL);
HTTPS_SERVICE_DEFINE(qat_ws_https_v6, "::", &g_https_port,
             CONFIG_HTTP_SERVER_MAX_CLIENTS, 5,
             NULL, NULL, NULL, g_https_sec_tags, sizeof(g_https_sec_tags));
#endif

/* IPv4 HTTP resources */
HTTP_RESOURCE_DEFINE(index_res_http,        qat_ws_http, "/",           &index_detail);
HTTP_RESOURCE_DEFINE(indexhtml_res_http,    qat_ws_http, "/index.html", &index_detail);
HTTP_RESOURCE_DEFINE(getversion_res_http,   qat_ws_http, "/getversion", &getversion_detail);
HTTP_RESOURCE_DEFINE(setwifi_res_http,      qat_ws_http, "/setwifi",    &setwifi_detail);
HTTP_RESOURCE_DEFINE(getssid_res_http,      qat_ws_http, "/getssid",    &getssid_detail);

/* IPv4 HTTPS resources */
HTTP_RESOURCE_DEFINE(index_res_https,       qat_ws_https, "/",           &index_detail);
HTTP_RESOURCE_DEFINE(indexhtml_res_https,   qat_ws_https, "/index.html", &index_detail);
HTTP_RESOURCE_DEFINE(getversion_res_https,  qat_ws_https, "/getversion", &getversion_detail);
HTTP_RESOURCE_DEFINE(setwifi_res_https,     qat_ws_https, "/setwifi",    &setwifi_detail);
HTTP_RESOURCE_DEFINE(getssid_res_https,     qat_ws_https, "/getssid",    &getssid_detail);

#if defined(CONFIG_NET_IPV6)
/* IPv6 HTTP resources */
HTTP_RESOURCE_DEFINE(index_res_http_v6,     qat_ws_http_v6, "/",           &index_detail);
HTTP_RESOURCE_DEFINE(indexhtml_res_http_v6, qat_ws_http_v6, "/index.html", &index_detail);
HTTP_RESOURCE_DEFINE(getversion_res_http_v6,qat_ws_http_v6, "/getversion", &getversion_detail);
HTTP_RESOURCE_DEFINE(setwifi_res_http_v6,   qat_ws_http_v6, "/setwifi",    &setwifi_detail);
HTTP_RESOURCE_DEFINE(getssid_res_http_v6,   qat_ws_http_v6, "/getssid",    &getssid_detail);

/* IPv6 HTTPS resources */
HTTP_RESOURCE_DEFINE(index_res_https_v6,    qat_ws_https_v6, "/",           &index_detail);
HTTP_RESOURCE_DEFINE(indexhtml_res_https_v6,qat_ws_https_v6, "/index.html", &index_detail);
HTTP_RESOURCE_DEFINE(getver_res_https_v6,   qat_ws_https_v6, "/getversion", &getversion_detail);
HTTP_RESOURCE_DEFINE(setwifi_res_https_v6,  qat_ws_https_v6, "/setwifi",    &setwifi_detail);
HTTP_RESOURCE_DEFINE(getssid_res_https_v6,  qat_ws_https_v6, "/getssid",    &getssid_detail);
#endif

/*-------------------------------------------------------------------------
 * URL decode helpers
 *-----------------------------------------------------------------------*/
static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Decode URL-encoded string in-place. Null-terminates result. */
static void url_decode(char *buf, size_t len)
{
	size_t r = 0, w = 0;

	while (r < len) {
		if (buf[r] == '+') {
			buf[w++] = ' ';
			r++;
		} else if (buf[r] == '%' && r + 2 < len && hex_val(buf[r + 1]) >= 0 &&
			   hex_val(buf[r + 2]) >= 0) {
			buf[w++] = (char)((hex_val(buf[r + 1]) << 4) | hex_val(buf[r + 2]));
			r += 3;
		} else {
			buf[w++] = buf[r++];
		}
	}
	buf[w] = '\0';
}

/* Extract value for key= from URL-encoded form body into dst. */
static bool form_get_field(const char *body, const char *key, char *dst, size_t dst_size)
{
	char search[48];
	size_t key_len;
	const char *p, *end;
	size_t val_len;

	snprintf(search, sizeof(search), "%s=", key);
	key_len = strlen(search);

	p = body;
	while (*p) {
		if (strncmp(p, search, key_len) == 0) {
			p += key_len;
			end = strchr(p, '&');
			val_len = end ? (size_t)(end - p) : strlen(p);
			if (val_len >= dst_size) {
				return false;
			}
			memcpy(dst, p, val_len);
			dst[val_len] = '\0';
			url_decode(dst, val_len);
			return true;
		}
		p = strchr(p, '&');
		if (!p) {
			break;
		}
		p++;
	}
	return false;
}

/*-------------------------------------------------------------------------
 * LittleFS credential helper
 *-----------------------------------------------------------------------*/

/* Read WiFi credentials from /lfs/webprov.
 * Returns 0 on success, negative errno on failure (file not found, parse error, etc.). */
static int read_wifi_credentials(char *ssid, size_t ssid_size, char *password, size_t pwd_size)
{
	struct fs_file_t f;
	char buf[AT_LFS_WIFI_BUFSIZE];
	ssize_t nr;
	int ret;

	fs_file_t_init(&f);
	ret = fs_open(&f, AT_LFS_WIFI_FILE, FS_O_READ);
	if (ret < 0) {
		return ret;
	}

	nr = fs_read(&f, buf, sizeof(buf) - 1);
	fs_close(&f);

	if (nr <= 0) {
		return -EIO;
	}

	buf[nr] = '\0';

	if (!form_get_field(buf, "ssid", ssid, ssid_size) ||
	    !form_get_field(buf, "password", password, pwd_size)) {
		return -EINVAL;
	}

	return 0;
}

/*-------------------------------------------------------------------------
 * TLS cert load / unload helpers
 *-----------------------------------------------------------------------*/

static void delete_cred_if_exists(sec_tag_t tag, enum tls_credential_type type)
{
	int ret = tls_credential_delete(tag, type);

	if (ret < 0 && ret != -ENOENT) {
		LOG_WRN("tls_credential_delete(tag=%d type=%d): %d", (int)tag, (int)type, ret);
	}
}

/* Read a PEM file from LittleFS into a heap buffer and register it as a TLS credential.
 * Ownership of the allocated buffer is transferred to *buf_keep on success.
 * The buffer must remain valid until tls_credential_delete() is called. */
static int load_cert_file(const char *path, sec_tag_t tag, enum tls_credential_type type,
			   uint8_t **buf_keep)
{
	struct fs_file_t f;
	struct fs_dirent entry;
	uint8_t *buf;
	ssize_t nr;
	int ret;

	ret = fs_stat(path, &entry);
	if (ret < 0) {
		LOG_ERR("fs_stat(%s): %d", path, ret);
		return ret;
	}

	buf = k_malloc(entry.size + 1);
	if (!buf) {
		LOG_ERR("k_malloc(%zu) for %s: -ENOMEM", entry.size + 1, path);
		return -ENOMEM;
	}

	fs_file_t_init(&f);
	ret = fs_open(&f, path, FS_O_READ);
	if (ret < 0) {
		LOG_ERR("fs_open(%s): %d", path, ret);
		k_free(buf);
		return ret;
	}

	nr = fs_read(&f, buf, entry.size);
	fs_close(&f);

	if (nr < 0) {
		LOG_ERR("fs_read(%s): %zd", path, nr);
		k_free(buf);
		return (int)nr;
	}

	buf[nr] = '\0'; /* NUL-terminate for PEM parsing */

	delete_cred_if_exists(tag, type);
	if (buf_keep && *buf_keep) {
		k_free(*buf_keep);
		*buf_keep = NULL;
	}

	/* tls_credential_add() stores the caller's pointer directly — no copy.
	 * Pass nr+1 to include the NUL terminator (required by mbedTLS PEM parser). */
	ret = tls_credential_add(tag, type, buf, (size_t)nr + 1);
	if (ret < 0) {
		LOG_ERR("tls_credential_add(tag=%d type=%d): %d", (int)tag, (int)type, ret);
		k_free(buf);
		return ret;
	}

	if (buf_keep) {
		*buf_keep = buf;
	}

	LOG_INF("Loaded TLS credential: tag=%d from %s (%zd bytes)", (int)tag, path, nr);
	return 0;
}

static int load_web_certs(void)
{
	int ret;

	ret = load_cert_file(g_ssl_cfg.cert_file, AT_WEB_TLS_TAG_SERVER_CERT,
			     TLS_CREDENTIAL_PUBLIC_CERTIFICATE, &g_cert_buf);
	if (ret < 0) {
		return ret;
	}

	ret = load_cert_file(g_ssl_cfg.key_file, AT_WEB_TLS_TAG_SERVER_KEY,
			     TLS_CREDENTIAL_PRIVATE_KEY, &g_key_buf);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

static void unload_web_certs(void)
{
	delete_cred_if_exists(AT_WEB_TLS_TAG_SERVER_CERT, TLS_CREDENTIAL_PUBLIC_CERTIFICATE);
	k_free(g_cert_buf);
	g_cert_buf = NULL;

	delete_cred_if_exists(AT_WEB_TLS_TAG_SERVER_KEY, TLS_CREDENTIAL_PRIVATE_KEY);
	k_free(g_key_buf);
	g_key_buf = NULL;
}

/*-------------------------------------------------------------------------
 * HTTP handlers
 *-----------------------------------------------------------------------*/

static int index_handler(struct http_client_ctx *client, enum http_data_status status,
			 const struct http_request_ctx *request_ctx,
			 struct http_response_ctx *response_ctx, void *user_data)
{
	if (status == HTTP_SERVER_DATA_FINAL) {
		response_ctx->body = (const uint8_t *)g_index_html;
		response_ctx->body_len = sizeof(g_index_html) - 1;
		response_ctx->final_chunk = true;
	}
	return 0;
}

static int getversion_handler(struct http_client_ctx *client, enum http_data_status status,
			      const struct http_request_ctx *request_ctx,
			      struct http_response_ctx *response_ctx, void *user_data)
{
	static char body[64];
	static bool body_ready;

	if (!body_ready) {
		struct qwifi_wlan_lib_version ver = {0};

		qwifi_get_wlan_lib_version(&ver);
		snprintf(body, sizeof(body), "{\"version\":\"%u.%u.%u.%u\"}",
			 ver.major, ver.minor, ver.patch, ver.build);
		body_ready = true;
	}

	if (status == HTTP_SERVER_DATA_FINAL) {
		response_ctx->body = (const uint8_t *)body;
		response_ctx->body_len = strlen(body);
		response_ctx->final_chunk = true;
	}
	return 0;
}

static int setwifi_handler(struct http_client_ctx *client, enum http_data_status status,
			   const struct http_request_ctx *request_ctx,
			   struct http_response_ctx *response_ctx, void *user_data)
{
	static const char ok_body[] = "{\"state\":\"OK\"}";
	static const char fail_body[] = "{\"state\":\"FAIL\"}";
	char ssid[AT_WEB_MAX_SSID_SIZE + 1];
	char password[AT_WEB_MAX_PWD_SIZE + 1];
	char evt[AT_WEB_EVT_BUFSIZE];

	if (status == HTTP_SERVER_DATA_ABORTED) {
		g_scratch_len = 0;
		return 0;
	}

	/* Accumulate body chunks into g_scratch */
	if (request_ctx->data_len > 0) {
		size_t space = AT_WEB_SCRATCH_BUFSIZE - 1 - g_scratch_len;

		if (request_ctx->data_len > space) {
			/* Body too large: drain remaining chunks, reject on FINAL */
			g_scratch_len = AT_WEB_SCRATCH_BUFSIZE; /* sentinel: overflow */
		} else {
			memcpy(g_scratch + g_scratch_len, request_ctx->data,
			       request_ctx->data_len);
			g_scratch_len += request_ctx->data_len;
		}
	}

	if (status != HTTP_SERVER_DATA_FINAL) {
		return 0;
	}

	/* FINAL: check for overflow */
	if (g_scratch_len >= AT_WEB_SCRATCH_BUFSIZE) {
		g_scratch_len = 0;
		response_ctx->status = HTTP_413_PAYLOAD_TOO_LARGE;
		response_ctx->body = (const uint8_t *)fail_body;
		response_ctx->body_len = sizeof(fail_body) - 1;
		response_ctx->final_chunk = true;
		return 0;
	}

	g_scratch[g_scratch_len] = '\0';
	g_scratch_len = 0;

	if (!form_get_field(g_scratch, "ssid", ssid, sizeof(ssid)) ||
	    !form_get_field(g_scratch, "password", password, sizeof(password))) {
		response_ctx->status = HTTP_400_BAD_REQUEST;
		response_ctx->body = (const uint8_t *)fail_body;
		response_ctx->body_len = sizeof(fail_body) - 1;
		response_ctx->final_chunk = true;
		return 0;
	}

	snprintf(evt, sizeof(evt), "+EVT:SYSCFG:WIFI:%s,%s", ssid, password);
	QAT_Response_Str(QAT_RC_QUIET, evt);
	LOG_INF("WiFi credentials received: ssid=%s", ssid);

	/* Persist credentials to LittleFS (/lfs/webprov) */
	{
		struct fs_file_t f;
		char wifi_str[AT_LFS_WIFI_BUFSIZE];
		int wlen;

		fs_file_t_init(&f);
		if (fs_open(&f, AT_LFS_WIFI_FILE,
			    FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC) == 0) {
			wlen = snprintf(wifi_str, sizeof(wifi_str), "ssid=%s&password=%s",
					ssid, password);
			fs_write(&f, wifi_str, wlen);
			fs_close(&f);
			LOG_INF("WiFi credentials saved to %s", AT_LFS_WIFI_FILE);
		} else {
			LOG_WRN("Failed to open %s for write", AT_LFS_WIFI_FILE);
		}
	}

	response_ctx->body = (const uint8_t *)ok_body;
	response_ctx->body_len = sizeof(ok_body) - 1;
	response_ctx->final_chunk = true;
	return 0;
}

/* Return stored WiFi credentials from /lfs/webprov, or empty if not configured */
static int getssid_handler(struct http_client_ctx *client, enum http_data_status status,
			   const struct http_request_ctx *request_ctx,
			   struct http_response_ctx *response_ctx, void *user_data)
{
	static char body[AT_WEB_MAX_SSID_SIZE + AT_WEB_MAX_PWD_SIZE + 40];
	char ssid[AT_WEB_MAX_SSID_SIZE + 1];
	char password[AT_WEB_MAX_PWD_SIZE + 1];

	if (status != HTTP_SERVER_DATA_FINAL) {
		return 0;
	}

	if (read_wifi_credentials(ssid, sizeof(ssid), password, sizeof(password)) == 0) {
		snprintf(body, sizeof(body), "{\"ssid\":\"%s\",\"password\":\"%s\"}",
			 ssid, password);
	} else {
		snprintf(body, sizeof(body), "{\"ssid\":\"\",\"password\":\"\"}");
	}

	response_ctx->body = (const uint8_t *)body;
	response_ctx->body_len = strlen(body);
	response_ctx->final_chunk = true;
	return 0;
}

/*-------------------------------------------------------------------------
 * AT+SYSCFG command handler
 *-----------------------------------------------------------------------*/

/* AT+SYSCFG — query stored WiFi credentials from /lfs/webprov */
static cat_return_state cmd_syscfg_exec(const struct cat_command *cmd)
{
	char ssid[AT_WEB_MAX_SSID_SIZE + 1];
	char password[AT_WEB_MAX_PWD_SIZE + 1];
	char buf[AT_WEB_MAX_SSID_SIZE + AT_WEB_MAX_PWD_SIZE + 24];

	if (read_wifi_credentials(ssid, sizeof(ssid), password, sizeof(password)) != 0) {
		return QAT_Response_Str(QAT_RC_ERROR, "+SYSCFG: get system config failed");
	}

	snprintf(buf, sizeof(buf), "+SYSCFG:WIFI:%s,%s", ssid, password);
	return QAT_Response_Str(QAT_RC_OK, buf);
}

/*-------------------------------------------------------------------------
 * AT+WEBSERVER command handlers
 *-----------------------------------------------------------------------*/

/* AT+WEBSERVER (no params) → usage hint */
static cat_return_state cmd_webserver_run(const struct cat_command *cmd)
{
	return QAT_Response_Str(QAT_RC_OK, "+WEBSERVER: use AT+WEBSERVER=<enable>,<port>");
}

/* AT+WEBSERVER? → +WEBSERVER: <state>,<scheme>,<port> */
static cat_return_state cmd_webserver_read(const struct cat_command *cmd, uint8_t *data,
					   size_t *data_size, const size_t max_data_size)
{
	char buf[48];
	uint16_t active_port = (g_server_scheme == 0) ? g_http_port : g_https_port;

	snprintf(buf, sizeof(buf), "+WEBSERVER: %d,%d,%u",
		 g_server_running ? 1 : 0, g_server_scheme, (unsigned)active_port);
	return QAT_Response_Str(QAT_RC_OK, buf);
}

/* AT+WEBSERVER=<enable>[,<port>] */
static cat_return_state cmd_webserver_write(const struct cat_command *cmd, const uint8_t *data,
					    const size_t data_size, const size_t args_num)
{
	char buf[64];
	char *saveptr;
	char *token;
	int enable;
	uint16_t port;
	int ret;

	if (data_size == 0 || data_size >= sizeof(buf)) {
		return QAT_Response_Str(QAT_RC_ERROR,
					"+WEBSERVER: use AT+WEBSERVER=<enable>,<port>");
	}

	memcpy(buf, data, data_size);
	buf[data_size] = '\0';

	token = strtok_r(buf, ",", &saveptr);
	if (!token) {
		return QAT_Response_Str(QAT_RC_ERROR,
					"+WEBSERVER: use AT+WEBSERVER=<enable>,<port>");
	}
	enable = atoi(token);
	if (enable < 0 || enable > 1) {
		return QAT_Response_Str(QAT_RC_ERROR, "+WEBSERVER: enable must be 0 or 1");
	}

	if (enable == 1) {
		token = strtok_r(NULL, ",", &saveptr);
		if (!token) {
			return QAT_Response_Str(QAT_RC_ERROR,
						"+WEBSERVER: port required when enable=1");
		}
		{
			int port_val = atoi(token);

			if (port_val <= 0 || port_val > 65535) {
				return QAT_Response_Str(QAT_RC_ERROR, "+WEBSERVER: invalid port");
			}
			port = (uint16_t)port_val;
		}

		if (g_server_running) {
			return QAT_Response_Str(QAT_RC_ERROR,
						"+WEBSERVER: already running, stop first");
		}

		if (g_ssl_cfg.scheme == 0) {
			/* HTTP mode — no certs needed */
			g_http_port  = port;
			g_https_port = 0;
		} else {
			/* HTTPS mode — certs required */
			if (g_ssl_cfg.cert_file[0] == '\0' || g_ssl_cfg.key_file[0] == '\0') {
				return QAT_Response_Str(QAT_RC_ERROR,
							"+WEBSERVER: AT+WEBSERVERCFG required for HTTPS");
			}
			ret = load_web_certs();
			if (ret != 0) {
				LOG_ERR("TLS cert load failed: %d", ret);
				return QAT_Response_Str(QAT_RC_ERROR,
							"+WEBSERVER: TLS cert load failed");
			}
			g_https_port = port;
			g_http_port  = 0;
		}

		ret = http_server_start();
		if (ret != 0) {
			LOG_ERR("http_server_start failed: %d", ret);
			g_http_port  = 0;
			g_https_port = 0;
			return QAT_Response_Str(QAT_RC_ERROR, NULL);
		}
		g_server_running = true;
		g_server_scheme  = g_ssl_cfg.scheme;
		LOG_INF("HTTP%s server started on port %u",
			g_ssl_cfg.scheme ? "S" : "", (unsigned)port);

	} else {
		if (g_server_running) {
			http_server_stop();
			if (g_server_scheme >= 1) {
				unload_web_certs();
			}
			g_http_port      = 0;
			g_https_port     = 0;
			g_server_running = false;
			g_server_scheme  = 0;
			LOG_INF("HTTP server stopped");
		}
		/* idempotent: OK even if already stopped */
	}

	return QAT_Response_Str(QAT_RC_OK, NULL);
}

/*-------------------------------------------------------------------------
 * AT+WEBSERVERCFG command
 *-----------------------------------------------------------------------*/

/* Extract a comma-separated token from *pp, strip optional surrounding quotes,
 * copy into dst.  Advances *pp past the consumed token (and the following comma
 * if present).  Returns false if the field is missing or dst_size is too small. */
static bool parse_cfg_field(const char **pp, char *dst, size_t dst_size)
{
	const char *p = *pp;
	const char *end;
	size_t len;
	bool quoted;

	if (!p || *p == '\0') {
		dst[0] = '\0';
		return false;
	}

	/* skip leading whitespace */
	while (*p == ' ') {
		p++;
	}

	quoted = (*p == '"');
	if (quoted) {
		p++; /* skip opening quote */
		end = strchr(p, '"');
		if (!end) {
			return false;
		}
		len = (size_t)(end - p);
		end++; /* skip closing quote */
	} else {
		end = strchr(p, ',');
		len = end ? (size_t)(end - p) : strlen(p);
	}

	if (len >= dst_size) {
		return false;
	}
	memcpy(dst, p, len);
	dst[len] = '\0';

	/* advance past field and optional separator comma */
	*pp = end ? (*end == ',' ? end + 1 : end) : p + len;
	return true;
}

/* AT+WEBSERVERCFG (no params) */
static cat_return_state cmd_webservercfg_run(const struct cat_command *cmd)
{
	ARG_UNUSED(cmd);
	return QAT_Response_Str(QAT_RC_OK,
				"+WEBSERVERCFG=<scheme>,\"cert_file\",\"key_file\""
				"  (scheme: 0=HTTP 1=HTTPS)");
}

/* AT+WEBSERVERCFG? */
static cat_return_state cmd_webservercfg_read(const struct cat_command *cmd, uint8_t *data,
					      size_t *data_size, const size_t max_data_size)
{
	ARG_UNUSED(cmd);
	ARG_UNUSED(data);
	ARG_UNUSED(data_size);
	ARG_UNUSED(max_data_size);

	char buf[AT_WEB_TLS_PATH_MAXLEN * 2 + 32];

	snprintf(buf, sizeof(buf), "+WEBSERVERCFG: %d,\"%s\",\"%s\"", g_ssl_cfg.scheme,
		 g_ssl_cfg.cert_file, g_ssl_cfg.key_file);
	return QAT_Response_Str(QAT_RC_OK, buf);
}

/* AT+WEBSERVERCFG=<scheme>[,"<cert>","<key>"]
 * scheme=0: HTTP (no certs needed)
 * scheme=1: HTTPS, one-way TLS (cert + key required)
 */
static cat_return_state cmd_webservercfg_write(const struct cat_command *cmd, const uint8_t *data,
					       const size_t data_size, const size_t args_num)
{
	ARG_UNUSED(cmd);
	ARG_UNUSED(args_num);

	char buf[AT_WEB_TLS_PATH_MAXLEN * 3 + 16];
	const char *p;
	char scheme_str[4];
	struct qat_web_ssl_cfg cfg = {0};

	if (data_size == 0 || data_size >= sizeof(buf)) {
		return QAT_Response_Str(QAT_RC_ERROR,
					"+WEBSERVERCFG: invalid parameters");
	}

	memcpy(buf, data, data_size);
	buf[data_size] = '\0';
	p = buf;

	/* field 1: scheme integer */
	if (!parse_cfg_field(&p, scheme_str, sizeof(scheme_str))) {
		return QAT_Response_Str(QAT_RC_ERROR, "+WEBSERVERCFG: missing scheme");
	}
	cfg.scheme = atoi(scheme_str);
	if (cfg.scheme < 0 || cfg.scheme > 1) {
		return QAT_Response_Str(QAT_RC_ERROR,
					"+WEBSERVERCFG: scheme must be 0 (HTTP) or 1 (HTTPS)");
	}

	if (cfg.scheme >= 1) {
		/* field 2: cert_file — required for HTTPS */
		if (!parse_cfg_field(&p, cfg.cert_file, sizeof(cfg.cert_file)) ||
		    cfg.cert_file[0] == '\0') {
			return QAT_Response_Str(QAT_RC_ERROR, "+WEBSERVERCFG: missing cert_file");
		}

		/* field 3: key_file — required for HTTPS */
		if (!parse_cfg_field(&p, cfg.key_file, sizeof(cfg.key_file)) ||
		    cfg.key_file[0] == '\0') {
			return QAT_Response_Str(QAT_RC_ERROR, "+WEBSERVERCFG: missing key_file");
		}
	}

	g_ssl_cfg = cfg;
	LOG_INF("WEBSERVERCFG: scheme=%d cert=%s key=%s", g_ssl_cfg.scheme,
		g_ssl_cfg.cert_file, g_ssl_cfg.key_file);

	return QAT_Response_Str(QAT_RC_OK, NULL);
}

/*-------------------------------------------------------------------------
 * Command group registration
 *-----------------------------------------------------------------------*/

static struct cat_command qat_httpserver_cmds[] = {
	{
		.name = "+WEBSERVER",
		.description = "Start/stop HTTP server for WiFi provisioning",
		.run = cmd_webserver_run,
		.read = cmd_webserver_read,
		.write = cmd_webserver_write,
	},
	{
		.name = "+SYSCFG",
		.description = "Query stored WiFi credentials from LittleFS",
		.run = cmd_syscfg_exec,
	},
	{
		.name = "+WEBSERVERCFG",
		.description = "Configure HTTP/HTTPS scheme and TLS cert/key files",
		.run = cmd_webservercfg_run,
		.read = cmd_webservercfg_read,
		.write = cmd_webservercfg_write,
	},
};

static struct cat_command_group qat_httpserver_cmd_group = {
	.name = "QAT_HTTPSERVER",
	.cmd = qat_httpserver_cmds,
	.cmd_num = ARRAY_SIZE(qat_httpserver_cmds),
};

struct cat_command_group *qat_httpserver_get_command_group(void)
{
	return &qat_httpserver_cmd_group;
}

QAT_REGISTER_CMD_GROUP(qat_httpserver_get_command_group, "HTTPSERVER");
