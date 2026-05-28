/*
 * Copyright (c) 2026 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "../inc/httpc_at_handler.h"
#include "../inc/httpc_at_core.h"
#include "../inc/httpc_at_ssl.h"
#include "../inc/httpc_at_utils.h"

LOG_MODULE_REGISTER(httpc_at_handler, LOG_LEVEL_DBG);

/* -------------------------------------------------------------------------
 * Local helpers
 * ---------------------------------------------------------------------- */

/**
 * @brief Duplicate a string using k_malloc + memcpy.
 *
 * k_strdup() is not available in all Zephyr library linking contexts.
 * This helper provides equivalent functionality.
 *
 * @param s  Source string (must not be NULL)
 * @return Heap-allocated copy, or NULL on allocation failure.
 *         Caller must free with k_free().
 */
static char *httpc_strdup(const char *s)
{
    size_t len = strlen(s) + 1;
    char *copy = k_malloc(len);

    if (copy) {
        memcpy(copy, s, len);
    }
    return copy;
}

/*-------------------------------------------------------------------------
 * Global state
 *-----------------------------------------------------------------------*/

static struct httpc_at_global_cfg g_cfg;
static K_MUTEX_DEFINE(g_mutex);
static bool g_initialized;
static struct httpc_at_stream_ctx g_stream_ctx;
static bool g_stream_ssl_prepared;

static void release_ssl_if_needed(const char *url);

/*-------------------------------------------------------------------------
 * Private: output helpers
 *-----------------------------------------------------------------------*/

/**
 * @brief Single AT output function — sends string to transport and log.
 *
 * All AT responses go through this function.
 * - output_cb: transport layer (UART in production, test capture in tests)
 * - LOG_INF:   development visibility
 */
static void httpc_at_output_str(const char *str)
{
    if (!str) {
        return;
    }
    /* Transport: output_cb (UART in production, test capture in tests) */
    if (g_cfg.output_cb) {
        g_cfg.output_cb(str, strlen(str), g_cfg.output_user_data);
    }
    /* Development visibility */
    LOG_INF("AT> %s", str);
}

/**
 * @brief Thin wrapper kept for internal call-site compatibility.
 */
static void httpc_at_output(const char *str) { httpc_at_output_str(str); }

/**
 * @brief Write a formatted string via httpc_at_output_str.
 */
static void httpc_at_output_fmt(const char *fmt, ...)
{
    char buf[512];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    httpc_at_output_str(buf);
}

/*-------------------------------------------------------------------------
 * Private: resource management
 *-----------------------------------------------------------------------*/

/**
 * @brief Free all per-command temporary resources in g_cfg.
 *
 * Frees header field name/value strings, send_buf, and temp_url.
 * Resets data mode state.
 */
static void reset_temp_resources(void)
{
    const char *active_url = g_cfg.temp_url ? g_cfg.temp_url : g_cfg.url;

    if (g_stream_ctx.active) {
        httpc_at_stream_abort(&g_stream_ctx);
    }

    if (g_stream_ssl_prepared && active_url) {
        release_ssl_if_needed(active_url);
    }
    g_stream_ssl_prepared = false;

    /* Free header fields */
    for (int i = 0; i < g_cfg.header_field_num; i++) {
        k_free(g_cfg.header_fields[i].name);
        k_free(g_cfg.header_fields[i].value);
        g_cfg.header_fields[i].name = NULL;
        g_cfg.header_fields[i].value = NULL;
    }
    g_cfg.header_field_num = 0;

    /* Free send buffer */
    if (g_cfg.send_buf) {
        k_free(g_cfg.send_buf);
        g_cfg.send_buf = NULL;
    }
    g_cfg.send_buf_offset = 0;
    g_cfg.data_len = 0;

    /* Free temp URL */
    if (g_cfg.temp_url) {
        k_free(g_cfg.temp_url);
        g_cfg.temp_url = NULL;
    }

    /* Reset data mode */
    g_cfg.in_data_mode = false;
    g_cfg.data_mode_cmd = HTTPC_AT_DATA_MODE_NONE;
    g_cfg.force_stream = false;
}

/**
 * @brief Add a header field from a "Name: Value" string.
 *
 * Parses the string at the first ':' separator and stores name/value
 * in g_cfg.header_fields[].
 *
 * @param header_str  "Name: Value" string (e.g., "Content-Type: application/json")
 * @return 0 on success, -ENOMEM if no space, -EINVAL if malformed
 */
static int add_header_field(const char *header_str)
{
    const char *colon;
    size_t name_len;
    const char *val_start;
    char *name_copy;
    char *val_copy;

    if (!header_str || strlen(header_str) == 0) {
        return -EINVAL;
    }

    if (g_cfg.header_field_num >= HTTPC_AT_MAX_HEADER_FIELDS) {
        LOG_WRN("Max header fields reached (%d)", HTTPC_AT_MAX_HEADER_FIELDS);
        return -ENOMEM;
    }

    colon = strchr(header_str, ':');
    if (!colon) {
        LOG_WRN("Header field missing ':': %s", header_str);
        return -EINVAL;
    }

    name_len = (size_t)(colon - header_str);
    val_start = colon + 1;

    /* Skip leading whitespace in value */
    while (*val_start == ' ' || *val_start == '\t') {
        val_start++;
    }

    name_copy = k_malloc(name_len + 1);
    if (!name_copy) {
        return -ENOMEM;
    }
    memcpy(name_copy, header_str, name_len);
    name_copy[name_len] = '\0';

    val_copy = httpc_strdup(val_start);
    if (!val_copy) {
        k_free(name_copy);
        return -ENOMEM;
    }

    g_cfg.header_fields[g_cfg.header_field_num].name = name_copy;
    g_cfg.header_fields[g_cfg.header_field_num].value = val_copy;
    g_cfg.header_field_num++;

    LOG_DBG("Added header: %s: %s", name_copy, val_copy);
    return 0;
}

/**
 * @brief Add a Content-Type header field for the given content type enum.
 */
static int add_content_type_header(int content_type)
{
    char header_str[128];

    snprintf(header_str, sizeof(header_str), "Content-Type: %s", httpc_at_get_content_type_str(content_type));
    return add_header_field(header_str);
}

/**
 * @brief Build a flat array of "Name: Value\r\n" strings for httpc_at_request.
 *
 * Each string includes the mandatory CRLF terminator required by Zephyr's
 * http_client_req(), which sends optional_headers[] entries verbatim.
 *
 * The caller must free the returned array with free_header_strings().
 *
 * @param out_headers   Output: pointer to allocated array of strings
 * @param out_count     Output: number of strings in array
 * @return 0 on success, negative errno on failure
 */
static int build_header_strings(const char ***out_headers, uint8_t *out_count)
{
    uint8_t n = g_cfg.header_field_num;

    if (n == 0) {
        *out_headers = NULL;
        *out_count = 0;
        return 0;
    }

    const char **arr = k_malloc(n * sizeof(const char *));

    if (!arr) {
        return -ENOMEM;
    }

    for (uint8_t i = 0; i < n; i++) {
        /* Build "Name: Value\r\n" string. The trailing CRLF is mandatory:
         * Zephyr's http_client_req() writes each optional_headers[] entry
         * verbatim (zephyr/subsys/net/lib/http/http_client.c) and immediately
         * follows with "Content-Length: ...", so without CRLF here the two
         * headers fuse on the wire. The streaming path in httpc_at_core.c
         * applies an equivalent guard at send time.
         */
        size_t len = strlen(g_cfg.header_fields[i].name) + 2 + /* ": "   */
                     strlen(g_cfg.header_fields[i].value) + 2 + /* "\r\n" */
                     1;                                         /* '\0'   */
        char *s = k_malloc(len);

        if (!s) {
            /* Free already-allocated strings */
            for (uint8_t j = 0; j < i; j++) {
                k_free((void *)arr[j]);
            }
            k_free(arr);
            return -ENOMEM;
        }
        snprintf(s, len, "%s: %s\r\n", g_cfg.header_fields[i].name, g_cfg.header_fields[i].value);
        arr[i] = s;
    }

    *out_headers = arr;
    *out_count = n;
    return 0;
}

/**
 * @brief Free the array returned by build_header_strings().
 */
static void free_header_strings(const char **headers, uint8_t count)
{
    if (!headers) {
        return;
    }
    for (uint8_t i = 0; i < count; i++) {
        k_free((void *)headers[i]);
    }
    k_free(headers);
}

/**
 * @brief Resolve the URL to use for a request.
 *
 * If url_param is non-empty, use it. Otherwise fall back to g_cfg.url.
 * Returns a pointer to the URL string (not heap-allocated; caller must not free).
 *
 * @param url_param  URL from AT command parameter (may be empty string)
 * @return URL string, or NULL if no URL available
 */
static const char *resolve_url(const char *url_param)
{
    if (url_param && strlen(url_param) > 0) {
        return url_param;
    }
    if (g_cfg.url && g_cfg.url_len > 0) {
        return g_cfg.url;
    }
    return NULL;
}

/**
 * @brief Load TLS credentials if the URL is HTTPS.
 *
 * @param url  URL to check
 * @return 0 on success, negative errno on failure
 */
static int prepare_ssl_if_needed(const char *url)
{
    int ret;

    if (!httpc_at_is_https(url)) {
        return 0;
    }

    /*
     * When prealloc_ssl_buf=1, keep TLS credentials loaded across requests
     * to align with "pre-allocate/reuse" behavior.
     */
    if (g_cfg.prealloc_ssl_buf == 1 && g_cfg.tls_creds_loaded) {
        return 0;
    }

    ret = httpc_at_ssl_load_certs(g_cfg.https_auth_type, g_cfg.ca_file, g_cfg.cert_file, g_cfg.key_file);
    if (ret < 0) {
        g_cfg.tls_creds_loaded = false;
        return ret;
    }

    if (g_cfg.prealloc_ssl_buf == 1) {
        g_cfg.tls_creds_loaded = true;
    }

    return 0;
}

static void release_ssl_if_needed(const char *url)
{
    if (!httpc_at_is_https(url)) {
        return;
    }

    if (g_cfg.prealloc_ssl_buf == 1) {
        return;
    }

    httpc_at_ssl_unload_certs();
    g_cfg.tls_creds_loaded = false;
}

/*-------------------------------------------------------------------------
 * Private: body streaming context
 *
 * Used to count body bytes while forwarding them to the AT output.
 *-----------------------------------------------------------------------*/

struct body_stream_ctx {
    httpc_at_output_cb_t output_cb;
    void *user_data;
    size_t bytes_written;
    const char *prefix;
    bool prefix_emitted;
};

static void body_stream_cb(const char *data, size_t len, void *user_data)
{
    struct body_stream_ctx *ctx = (struct body_stream_ctx *)user_data;

    if (ctx->prefix && !ctx->prefix_emitted) {
        if (ctx->output_cb) {
            ctx->output_cb(ctx->prefix, strlen(ctx->prefix), ctx->user_data);
        }
        ctx->prefix_emitted = true;
    }

    if (ctx->output_cb) {
        ctx->output_cb(data, len, ctx->user_data);
    }
    ctx->bytes_written += len;
}

/*-------------------------------------------------------------------------
 * Module lifecycle
 *-----------------------------------------------------------------------*/

int httpc_at_init(httpc_at_output_cb_t output_cb, void *user_data)
{
    int ret;

    k_mutex_lock(&g_mutex, K_FOREVER);

    if (g_initialized) {
        LOG_WRN("httpc_at already initialized");
        k_mutex_unlock(&g_mutex);
        return 0;
    }

    memset(&g_cfg, 0, sizeof(g_cfg));
    memset(&g_stream_ctx, 0, sizeof(g_stream_ctx));
    g_stream_ctx.sock = -1;
    g_stream_ssl_prepared = false;

    /* Set defaults */
    g_cfg.http_port = HTTPC_AT_DEFAULT_HTTP_PORT;
    g_cfg.https_port = HTTPC_AT_DEFAULT_HTTPS_PORT;
    g_cfg.ip_family = AF_INET;
    g_cfg.prealloc_ssl_buf = 0;
    g_cfg.data_cache = 1;
    g_cfg.https_auth_type = HTTPC_AT_AUTH_NONE;

    g_cfg.output_cb = output_cb;
    g_cfg.output_user_data = user_data;

    ret = httpc_at_core_init();
    if (ret < 0) {
        LOG_ERR("httpc_at_core_init failed: %d", ret);
        k_mutex_unlock(&g_mutex);
        return ret;
    }

    g_initialized = true;
    k_mutex_unlock(&g_mutex);
    LOG_INF("HTTPC AT handler initialized");
    return 0;
}

void httpc_at_deinit(void)
{
    k_mutex_lock(&g_mutex, K_FOREVER);

    if (!g_initialized) {
        k_mutex_unlock(&g_mutex);
        return;
    }

    reset_temp_resources();

    if (g_cfg.url) {
        k_free(g_cfg.url);
        g_cfg.url = NULL;
        g_cfg.url_len = 0;
    }

    httpc_at_ssl_unload_certs();
    httpc_at_core_deinit();

    memset(&g_cfg, 0, sizeof(g_cfg));
    g_initialized = false;

    k_mutex_unlock(&g_mutex);
    LOG_INF("HTTPC AT handler deinitialized");
}

/*-------------------------------------------------------------------------
 * Data mode interface
 *-----------------------------------------------------------------------*/

bool httpc_at_is_in_data_mode(void) { return g_cfg.in_data_mode; }

int httpc_at_data_mode_input(const uint8_t *data, size_t len)
{
    if (!g_cfg.in_data_mode) {
        return -EINVAL;
    }

    httpc_at_data_mode_cmd_t cmd = g_cfg.data_mode_cmd;
    bool is_post_put = (cmd == HTTPC_AT_DATA_MODE_POST || cmd == HTTPC_AT_DATA_MODE_PUT);
    bool use_cache = (!is_post_put) || ((g_cfg.data_cache == 1) && !g_cfg.force_stream);

    if (use_cache && !g_cfg.send_buf) {
        return -EINVAL;
    }

    /* Strip trailing '\r' from partial AT packets */
    size_t valid_len = httpc_at_get_valid_data_len((const char *)data, len, HTTPC_AT_CHUNK_SIZE);

    /* Clamp to remaining space */
    size_t space = g_cfg.data_len - g_cfg.send_buf_offset;

    if (valid_len > space) {
        valid_len = space;
    }

    if (use_cache && valid_len > 0) {
        memcpy(g_cfg.send_buf + g_cfg.send_buf_offset, data, valid_len);
    } else if (!use_cache && valid_len > 0) {
        int ret;
        const char *url = g_cfg.temp_url ? g_cfg.temp_url : g_cfg.url;

        if (!url) {
            LOG_ERR("No URL for data mode command");
            g_cfg.in_data_mode = false;
            g_cfg.data_mode_cmd = HTTPC_AT_DATA_MODE_NONE;
            httpc_at_output((cmd == HTTPC_AT_DATA_MODE_POST) ? "+HTTPCPOST:SEND FAIL\r\n" : "+HTTPCPUT:SEND FAIL\r\n");
            reset_temp_resources();
            return -EINVAL;
        }

        if (!g_stream_ctx.active) {
            const char **headers = NULL;
            uint8_t header_count = 0;

            ret = build_header_strings(&headers, &header_count);
            if (ret < 0) {
                LOG_ERR("build_header_strings failed: %d", ret);
                g_cfg.in_data_mode = false;
                g_cfg.data_mode_cmd = HTTPC_AT_DATA_MODE_NONE;
                httpc_at_output((cmd == HTTPC_AT_DATA_MODE_POST) ? "+HTTPCPOST:SEND FAIL\r\n"
                                                                 : "+HTTPCPUT:SEND FAIL\r\n");
                reset_temp_resources();
                return ret;
            }

            ret = prepare_ssl_if_needed(url);
            if (ret < 0) {
                LOG_ERR("SSL preparation failed: %d", ret);
                free_header_strings(headers, header_count);
                g_cfg.in_data_mode = false;
                g_cfg.data_mode_cmd = HTTPC_AT_DATA_MODE_NONE;
                httpc_at_output((cmd == HTTPC_AT_DATA_MODE_POST) ? "+HTTPCPOST:SEND FAIL\r\n"
                                                                 : "+HTTPCPUT:SEND FAIL\r\n");
                reset_temp_resources();
                return ret;
            }
            g_stream_ssl_prepared = true;

            struct httpc_at_request req = {
                .url = url,
                .method = (cmd == HTTPC_AT_DATA_MODE_POST) ? HTTPC_AT_METHOD_POST : HTTPC_AT_METHOD_PUT,
                .body = NULL,
                .body_len = g_cfg.data_len,
                .timeout_ms = HTTPC_AT_DEFAULT_TIMEOUT_MS,
                .extra_headers = headers,
                .extra_header_count = header_count,
                .auth_type = g_cfg.https_auth_type,
                .http_port = g_cfg.http_port,
                .https_port = g_cfg.https_port,
                .ip_family = g_cfg.ip_family,
            };

            ret = httpc_at_stream_begin(&req, &g_stream_ctx);
            free_header_strings(headers, header_count);
            if (ret < 0) {
                LOG_ERR("stream_begin failed: %d", ret);
                g_cfg.in_data_mode = false;
                g_cfg.data_mode_cmd = HTTPC_AT_DATA_MODE_NONE;
                httpc_at_output((cmd == HTTPC_AT_DATA_MODE_POST) ? "+HTTPCPOST:SEND FAIL\r\n"
                                                                 : "+HTTPCPUT:SEND FAIL\r\n");
                reset_temp_resources();
                return ret;
            }
        }

        ret = httpc_at_stream_send(&g_stream_ctx, data, valid_len);
        if (ret < 0) {
            LOG_ERR("stream_send failed: %d", ret);
            g_cfg.in_data_mode = false;
            g_cfg.data_mode_cmd = HTTPC_AT_DATA_MODE_NONE;
            httpc_at_output((cmd == HTTPC_AT_DATA_MODE_POST) ? "+HTTPCPOST:SEND FAIL\r\n" : "+HTTPCPUT:SEND FAIL\r\n");
            reset_temp_resources();
            return ret;
        }
    }

    g_cfg.send_buf_offset += valid_len;

    LOG_DBG("Data mode: %zu/%zu bytes received", g_cfg.send_buf_offset, g_cfg.data_len);

    /* Check if we have all expected data */
    if (g_cfg.send_buf_offset < g_cfg.data_len) {
        return 0; /* Still collecting */
    }

    /* All data received — execute the pending command */
    cmd = g_cfg.data_mode_cmd;

    g_cfg.in_data_mode = false;
    g_cfg.data_mode_cmd = HTTPC_AT_DATA_MODE_NONE;

    if (cmd == HTTPC_AT_DATA_MODE_URLCFG) {
        /* Store the URL */
        g_cfg.send_buf[g_cfg.send_buf_offset] = '\0';

        if (g_cfg.url) {
            k_free(g_cfg.url);
        }
        g_cfg.url = g_cfg.send_buf;
        g_cfg.url_len = g_cfg.send_buf_offset;
        g_cfg.send_buf = NULL; /* Ownership transferred to g_cfg.url */

        LOG_DBG("HTTPURLCFG: stored URL (%zu bytes)", g_cfg.url_len);
        reset_temp_resources();
        return 0;
    }

    if (!use_cache) {
        struct httpc_at_response resp = {0};
        struct body_stream_ctx stream_ctx = {
            .output_cb = g_cfg.output_cb,
            .user_data = g_cfg.output_user_data,
            .bytes_written = 0,
            .prefix = "+HTTPC:",
            .prefix_emitted = false,
        };
        int ret = httpc_at_stream_finish(&g_stream_ctx, &resp, body_stream_cb, &stream_ctx);

        if (ret < 0 || resp.status_code < 200 || resp.status_code >= 300) {
            LOG_ERR("HTTP %s failed: ret=%d status=%d", (cmd == HTTPC_AT_DATA_MODE_POST) ? "POST" : "PUT", ret,
                    resp.status_code);
            httpc_at_output((cmd == HTTPC_AT_DATA_MODE_POST) ? "+HTTPCPOST:SEND FAIL\r\n" : "+HTTPCPUT:SEND FAIL\r\n");
            if (ret >= 0) {
                ret = -EIO;
            }
            reset_temp_resources();
            return ret;
        }

        if (!stream_ctx.prefix_emitted) {
            httpc_at_output("+HTTPC:");
        }
        httpc_at_output_fmt(",%zu\r\n", stream_ctx.bytes_written);
        httpc_at_output((cmd == HTTPC_AT_DATA_MODE_POST) ? "+HTTPCPOST:SEND OK\r\n" : "+HTTPCPUT:SEND OK\r\n");

        reset_temp_resources();
        return 0;
    }

    /* POST or PUT: execute HTTP request */
    const char **headers = NULL;
    uint8_t header_count = 0;
    int ret;

    ret = build_header_strings(&headers, &header_count);
    if (ret < 0) {
        LOG_ERR("build_header_strings failed: %d", ret);
        httpc_at_output((cmd == HTTPC_AT_DATA_MODE_POST) ? "+HTTPCPOST:SEND FAIL\r\n" : "+HTTPCPUT:SEND FAIL\r\n");
        reset_temp_resources();
        return ret;
    }

    const char *url = g_cfg.temp_url ? g_cfg.temp_url : g_cfg.url;

    if (!url) {
        LOG_ERR("No URL for data mode command");
        free_header_strings(headers, header_count);
        httpc_at_output((cmd == HTTPC_AT_DATA_MODE_POST) ? "+HTTPCPOST:SEND FAIL\r\n" : "+HTTPCPUT:SEND FAIL\r\n");
        reset_temp_resources();
        return -EINVAL;
    }

    ret = prepare_ssl_if_needed(url);
    if (ret < 0) {
        LOG_ERR("SSL preparation failed: %d", ret);
        free_header_strings(headers, header_count);
        httpc_at_output((cmd == HTTPC_AT_DATA_MODE_POST) ? "+HTTPCPOST:SEND FAIL\r\n" : "+HTTPCPUT:SEND FAIL\r\n");
        reset_temp_resources();
        return ret;
    }

    struct httpc_at_request req = {
        .url = url,
        .method = (cmd == HTTPC_AT_DATA_MODE_POST) ? HTTPC_AT_METHOD_POST : HTTPC_AT_METHOD_PUT,
        .body = (const uint8_t *)g_cfg.send_buf,
        .body_len = g_cfg.send_buf_offset,
        .timeout_ms = HTTPC_AT_DEFAULT_TIMEOUT_MS,
        .extra_headers = headers,
        .extra_header_count = header_count,
        .auth_type = g_cfg.https_auth_type,
        .http_port = g_cfg.http_port,
        .https_port = g_cfg.https_port,
        .ip_family = g_cfg.ip_family,
    };

    struct httpc_at_response resp = {0};

    struct body_stream_ctx stream_ctx = {
        .output_cb = g_cfg.output_cb,
        .user_data = g_cfg.output_user_data,
        .bytes_written = 0,
        .prefix = "+HTTPC:",
        .prefix_emitted = false,
    };

    ret = httpc_at_execute(&req, &resp, body_stream_cb, &stream_ctx);

    free_header_strings(headers, header_count);

    if (ret < 0 || resp.status_code < 200 || resp.status_code >= 300) {
        LOG_ERR("HTTP %s failed: ret=%d status=%d", (cmd == HTTPC_AT_DATA_MODE_POST) ? "POST" : "PUT", ret,
                resp.status_code);
        httpc_at_output((cmd == HTTPC_AT_DATA_MODE_POST) ? "+HTTPCPOST:SEND FAIL\r\n" : "+HTTPCPUT:SEND FAIL\r\n");
        if (ret >= 0) {
            ret = -EIO;
        }
    } else {
        if (!stream_ctx.prefix_emitted) {
            httpc_at_output("+HTTPC:");
        }
        httpc_at_output_fmt(",%zu\r\n", stream_ctx.bytes_written);
        httpc_at_output((cmd == HTTPC_AT_DATA_MODE_POST) ? "+HTTPCPOST:SEND OK\r\n" : "+HTTPCPUT:SEND OK\r\n");
    }

    release_ssl_if_needed(url);

    reset_temp_resources();
    return (ret < 0) ? ret : 0;
}

/*-------------------------------------------------------------------------
 * AT+HTTPCLIENT handler
 *-----------------------------------------------------------------------*/

int httpc_at_handle_httpclient(uint32_t op_type, uint32_t param_count, httpc_at_param_t *params)
{
    if (op_type == HTTPC_AT_OP_EXEC) {
        /* Template query: print usage */
        httpc_at_output("+HTTPCLIENT=<opt>,<content-type>,\"url\"[,\"data\"][,\"http_req_header\"]\r\n");
        return 0;
    }

    if (op_type != HTTPC_AT_OP_EXEC_W_PARAM) {
        return -EINVAL;
    }

    if (param_count < 3) {
        LOG_ERR("HTTPCLIENT: need at least 3 params, got %u", param_count);
        httpc_at_output("+HTTPC:ERROR\r\n");
        return -EINVAL;
    }

    /* Parse <opt> */
    if (!params[0].int_valid) {
        LOG_ERR("HTTPCLIENT: param[0] (opt) must be integer");
        httpc_at_output("+HTTPC:ERROR\r\n");
        return -EINVAL;
    }
    int opt = params[0].int_val;

    if (opt < 1 || opt > 4) {
        LOG_ERR("HTTPCLIENT: invalid opt=%d (must be 1-4)", opt);
        httpc_at_output("+HTTPC:ERROR\r\n");
        return -EINVAL;
    }
    httpc_at_method_t method = (httpc_at_method_t)opt;

    /* Parse <content-type> */
    if (!params[1].int_valid) {
        LOG_ERR("HTTPCLIENT: param[1] (content-type) must be integer");
        httpc_at_output("+HTTPC:ERROR\r\n");
        return -EINVAL;
    }
    int content_type = params[1].int_val;

    if (content_type < HTTPC_AT_CTYPE_FORM_URLENCODED || content_type > HTTPC_AT_CTYPE_TEXT_XML) {
        LOG_ERR("HTTPCLIENT: invalid content_type=%d (must be 0-4)", content_type);
        httpc_at_output("+HTTPC:ERROR\r\n");
        return -EINVAL;
    }

    /* Parse <url> */
    const char *url = resolve_url(params[2].str_val);

    if (!url) {
        LOG_ERR("HTTPCLIENT: no URL provided and no stored URL");
        httpc_at_output("+HTTPC:ERROR\r\n");
        return -EINVAL;
    }

    if (!httpc_at_validate_url(url)) {
        LOG_ERR("HTTPCLIENT: invalid URL: %s", url);
        httpc_at_output("+HTTPC:ERROR\r\n");
        return -EINVAL;
    }

    /* Parse optional <data> (param[3]) for POST/PUT */
    const char *inline_data = NULL;
    size_t inline_data_len = 0;
    uint32_t header_start_idx = 3;

    if ((method == HTTPC_AT_METHOD_POST || method == HTTPC_AT_METHOD_PUT) && param_count >= 4 && params[3].str_val &&
        strlen(params[3].str_val) > 0) {
        inline_data = params[3].str_val;
        inline_data_len = strlen(inline_data);
        header_start_idx = 4;
    }

    /* Parse optional extra headers (param[4+]) */
    int ret;

    k_mutex_lock(&g_mutex, K_FOREVER);
    reset_temp_resources();

    /* Add Content-Type header for POST/PUT */
    if (method == HTTPC_AT_METHOD_POST || method == HTTPC_AT_METHOD_PUT) {
        ret = add_content_type_header(content_type);
        if (ret < 0) {
            LOG_ERR("HTTPCLIENT: add Content-Type header failed: %d", ret);
            reset_temp_resources();
            k_mutex_unlock(&g_mutex);
            httpc_at_output("+HTTPC:ERROR\r\n");
            return ret;
        }
    }

    /* Add any extra header fields */
    for (uint32_t i = header_start_idx; i < param_count; i++) {
        if (params[i].str_val && strlen(params[i].str_val) > 0) {
            ret = add_header_field(params[i].str_val);
            if (ret < 0) {
                LOG_ERR("HTTPCLIENT: invalid header[%u]: %d", i, ret);
                reset_temp_resources();
                k_mutex_unlock(&g_mutex);
                httpc_at_output("+HTTPC:ERROR\r\n");
                return ret;
            }
        }
    }

    /* Build header strings array */
    const char **headers = NULL;
    uint8_t header_count = 0;
    ret = build_header_strings(&headers, &header_count);

    if (ret < 0) {
        LOG_ERR("build_header_strings failed: %d", ret);
        reset_temp_resources();
        k_mutex_unlock(&g_mutex);
        httpc_at_output("+HTTPC:ERROR\r\n");
        return ret;
    }

    /* Load TLS credentials if needed */
    ret = prepare_ssl_if_needed(url);
    if (ret < 0) {
        LOG_ERR("SSL preparation failed: %d", ret);
        free_header_strings(headers, header_count);
        reset_temp_resources();
        k_mutex_unlock(&g_mutex);
        httpc_at_output("+HTTPC:ERROR\r\n");
        return ret;
    }

    struct httpc_at_request req = {
        .url = url,
        .method = method,
        .body = (const uint8_t *)inline_data,
        .body_len = inline_data_len,
        .timeout_ms = HTTPC_AT_DEFAULT_TIMEOUT_MS,
        .extra_headers = headers,
        .extra_header_count = header_count,
        .auth_type = g_cfg.https_auth_type,
        .http_port = g_cfg.http_port,
        .https_port = g_cfg.https_port,
        .ip_family = g_cfg.ip_family,
    };

    k_mutex_unlock(&g_mutex);

    struct httpc_at_response resp = {0};

    if (method == HTTPC_AT_METHOD_HEAD) {
        /* HEAD: no body, just report OK */
        ret = httpc_at_execute(&req, &resp, NULL, NULL);
        free_header_strings(headers, header_count);

        if (ret < 0 || resp.status_code < 200 || resp.status_code >= 300) {
            if (ret >= 0) {
                ret = -EIO;
            }
            httpc_at_output("+HTTPC:ERROR\r\n");
        } else {
            httpc_at_output("+HTTPC:OK\r\n");
        }
    } else if (method == HTTPC_AT_METHOD_GET) {
        /* GET: stream body, report data+size */
        struct body_stream_ctx stream_ctx = {
            .output_cb = g_cfg.output_cb,
            .user_data = g_cfg.output_user_data,
            .bytes_written = 0,
            .prefix = "+HTTPC:",
            .prefix_emitted = false,
        };

        ret = httpc_at_execute(&req, &resp, body_stream_cb, &stream_ctx);
        free_header_strings(headers, header_count);

        if (ret < 0 || resp.status_code < 200 || resp.status_code >= 300) {
            if (ret >= 0) {
                ret = -EIO;
            }
            httpc_at_output("+HTTPC:ERROR\r\n");
        } else {
            if (!stream_ctx.prefix_emitted) {
                httpc_at_output("+HTTPC:");
            }
            httpc_at_output_fmt(",%zu\r\n", stream_ctx.bytes_written);
        }
    } else {
        /* POST/PUT: execute and report OK/ERROR */
        ret = httpc_at_execute(&req, &resp, NULL, NULL);
        free_header_strings(headers, header_count);

        if (ret < 0 || resp.status_code < 200 || resp.status_code >= 300) {
            if (ret >= 0) {
                ret = -EIO;
            }
            httpc_at_output("+HTTPC:ERROR\r\n");
        } else {
            httpc_at_output("+HTTPC:OK\r\n");
        }
    }

    if (httpc_at_is_https(url)) {
        k_mutex_lock(&g_mutex, K_FOREVER);
        release_ssl_if_needed(url);
        k_mutex_unlock(&g_mutex);
    }

    reset_temp_resources();
    return ret;
}

/*-------------------------------------------------------------------------
 * AT+HTTPGETSIZE handler
 *-----------------------------------------------------------------------*/

int httpc_at_handle_httpgetsize(uint32_t op_type, uint32_t param_count, httpc_at_param_t *params)
{
    if (op_type != HTTPC_AT_OP_EXEC_W_PARAM || param_count < 1 || param_count > 2) {
        httpc_at_output("+HTTPGETSIZE:ERROR\r\n");
        return -EINVAL;
    }

    const char *url = resolve_url(params[0].str_val);

    if (!url) {
        LOG_ERR("HTTPGETSIZE: no URL");
        httpc_at_output("+HTTPGETSIZE:ERROR\r\n");
        return -EINVAL;
    }

    if (!httpc_at_validate_url(url)) {
        LOG_ERR("HTTPGETSIZE: invalid URL: %s", url);
        httpc_at_output("+HTTPGETSIZE:ERROR\r\n");
        return -EINVAL;
    }

    /* Optional timeout */
    int32_t timeout_ms = HTTPC_AT_DEFAULT_GET_TIMEOUT_MS;

    if (param_count == 2) {
        if (!params[1].int_valid) {
            LOG_ERR("HTTPGETSIZE: timeout must be integer");
            httpc_at_output("+HTTPGETSIZE:ERROR\r\n");
            return -EINVAL;
        }

        int32_t t = (int32_t)params[1].int_val;

        if (t < 0 || t > HTTPC_AT_MAX_TIMEOUT_MS) {
            LOG_ERR("HTTPGETSIZE: invalid timeout=%d", (int)t);
            httpc_at_output("+HTTPGETSIZE:ERROR\r\n");
            return -EINVAL;
        }

        timeout_ms = t;
    }

    k_mutex_lock(&g_mutex, K_FOREVER);

    int ret = prepare_ssl_if_needed(url);

    if (ret < 0) {
        k_mutex_unlock(&g_mutex);
        httpc_at_output("+HTTPGETSIZE:ERROR\r\n");
        return ret;
    }

    struct httpc_at_request req = {
        .url = url,
        .method = HTTPC_AT_METHOD_HEAD,
        .body = NULL,
        .body_len = 0,
        .timeout_ms = timeout_ms,
        .extra_headers = NULL,
        .extra_header_count = 0,
        .auth_type = g_cfg.https_auth_type,
        .http_port = g_cfg.http_port,
        .https_port = g_cfg.https_port,
        .ip_family = g_cfg.ip_family,
    };

    k_mutex_unlock(&g_mutex);

    struct httpc_at_response resp = {0};

    ret = httpc_at_execute(&req, &resp, NULL, NULL);

    if (httpc_at_is_https(url)) {
        k_mutex_lock(&g_mutex, K_FOREVER);
        release_ssl_if_needed(url);
        k_mutex_unlock(&g_mutex);
    }

    if (ret < 0 || resp.status_code < 200 || resp.status_code >= 300) {
        LOG_ERR("HTTPGETSIZE failed: ret=%d status=%d", ret, resp.status_code);
        httpc_at_output("+HTTPGETSIZE:ERROR\r\n");
        return (ret < 0) ? ret : -EIO;
    }

    httpc_at_output_fmt("+HTTPGETSIZE:%zu\r\n", resp.content_length);
    return 0;
}

/*-------------------------------------------------------------------------
 * AT+HTTPGET handler
 *-----------------------------------------------------------------------*/

int httpc_at_handle_httpget(uint32_t op_type, uint32_t param_count, httpc_at_param_t *params)
{
    if (op_type != HTTPC_AT_OP_EXEC_W_PARAM || param_count < 1 || param_count > 2) {
        httpc_at_output("+HTTPGET:ERROR\r\n");
        return -EINVAL;
    }

    const char *url = resolve_url(params[0].str_val);

    if (!url) {
        LOG_ERR("HTTPGET: no URL");
        httpc_at_output("+HTTPGET:ERROR\r\n");
        return -EINVAL;
    }

    if (!httpc_at_validate_url(url)) {
        LOG_ERR("HTTPGET: invalid URL: %s", url);
        httpc_at_output("+HTTPGET:ERROR\r\n");
        return -EINVAL;
    }

    /* Optional timeout */
    int32_t timeout_ms = HTTPC_AT_DEFAULT_GET_TIMEOUT_MS;

    if (param_count == 2) {
        if (!params[1].int_valid) {
            LOG_ERR("HTTPGET: timeout must be integer");
            httpc_at_output("+HTTPGET:ERROR\r\n");
            return -EINVAL;
        }

        int32_t t = (int32_t)params[1].int_val;

        if (t < 0 || t > HTTPC_AT_MAX_TIMEOUT_MS) {
            LOG_ERR("HTTPGET: invalid timeout=%d", (int)t);
            httpc_at_output("+HTTPGET:ERROR\r\n");
            return -EINVAL;
        }

        timeout_ms = t;
    }

    k_mutex_lock(&g_mutex, K_FOREVER);

    int ret = prepare_ssl_if_needed(url);

    if (ret < 0) {
        k_mutex_unlock(&g_mutex);
        httpc_at_output("+HTTPGET:ERROR\r\n");
        return ret;
    }

    struct httpc_at_request req = {
        .url = url,
        .method = HTTPC_AT_METHOD_GET,
        .body = NULL,
        .body_len = 0,
        .timeout_ms = timeout_ms,
        .extra_headers = NULL,
        .extra_header_count = 0,
        .auth_type = g_cfg.https_auth_type,
        .http_port = g_cfg.http_port,
        .https_port = g_cfg.https_port,
        .ip_family = g_cfg.ip_family,
    };

    struct body_stream_ctx stream_ctx = {
        .output_cb = g_cfg.output_cb,
        .user_data = g_cfg.output_user_data,
        .bytes_written = 0,
        .prefix = "+HTTPGET:",
        .prefix_emitted = false,
    };

    k_mutex_unlock(&g_mutex);

    struct httpc_at_response resp = {0};

    ret = httpc_at_execute(&req, &resp, body_stream_cb, &stream_ctx);

    if (httpc_at_is_https(url)) {
        k_mutex_lock(&g_mutex, K_FOREVER);
        release_ssl_if_needed(url);
        k_mutex_unlock(&g_mutex);
    }

    if (ret < 0 || resp.status_code < 200 || resp.status_code >= 300) {
        LOG_ERR("HTTPGET failed: ret=%d status=%d", ret, resp.status_code);
        httpc_at_output(stream_ctx.prefix_emitted ? "\r\n+HTTPGET:ERROR\r\n" : "+HTTPGET:ERROR\r\n");
        return (ret < 0) ? ret : -EIO;
    }

    if (!stream_ctx.prefix_emitted) {
        httpc_at_output("+HTTPGET:");
    }
    httpc_at_output_fmt(",%zu\r\n", stream_ctx.bytes_written);
    return 0;
}

/*-------------------------------------------------------------------------
 * AT+HTTPPOST handler
 *-----------------------------------------------------------------------*/

int httpc_at_handle_httppost(uint32_t op_type, uint32_t param_count, httpc_at_param_t *params)
{
    if (op_type != HTTPC_AT_OP_EXEC_W_PARAM || param_count < 2) {
        return -EINVAL;
    }

    /* Parse <url> */
    const char *url = params[0].str_val;

    if (!url || strlen(url) == 0 || !httpc_at_validate_url(url)) {
        LOG_ERR("HTTPPOST: invalid or missing URL");
        return -EINVAL;
    }

    /* Parse <length> */
    if (!params[1].int_valid) {
        LOG_ERR("HTTPPOST: param[1] (length) must be integer");
        return -EINVAL;
    }
    int data_len = params[1].int_val;

    /* The hard ceiling is the stream-mode cap. If cache mode is active but the
     * body exceeds the (small) cache buffer, transparently fall through to stream
     * mode for this single command — preserves the small-RAM cache default for
     * typical small POSTs while letting larger documented bodies (e.g. 9000 B)
     * succeed without requiring an explicit AT+HTTPNETCFG=4,0 first. */
    if (data_len <= 0 || data_len > HTTPC_AT_MAX_STREAM_SIZE) {
        LOG_ERR("HTTPPOST: invalid length=%d (max=%d)", data_len, HTTPC_AT_MAX_STREAM_SIZE);
        return -EINVAL;
    }

    k_mutex_lock(&g_mutex, K_FOREVER);
    reset_temp_resources();

    bool use_cache_this_cmd = (g_cfg.data_cache == 1) && (data_len <= HTTPC_AT_SEND_BUF_SIZE);
    g_cfg.force_stream = (g_cfg.data_cache == 1) && !use_cache_this_cmd;

    /* Store URL for data mode phase 2 */
    g_cfg.temp_url = httpc_strdup(url);
    if (!g_cfg.temp_url) {
        k_mutex_unlock(&g_mutex);
        return -ENOMEM;
    }

    if (use_cache_this_cmd) {
        /* Allocate send buffer (+1 for NUL terminator) */
        g_cfg.send_buf = k_malloc((size_t)data_len + 1);
        if (!g_cfg.send_buf) {
            k_free(g_cfg.temp_url);
            g_cfg.temp_url = NULL;
            k_mutex_unlock(&g_mutex);
            return -ENOMEM;
        }
    }

    /* Add Content-Type: application/x-www-form-urlencoded (default for POST) */
    int ret = add_content_type_header(HTTPC_AT_CTYPE_FORM_URLENCODED);
    if (ret < 0) {
        LOG_ERR("HTTPPOST: add Content-Type header failed: %d", ret);
        reset_temp_resources();
        k_mutex_unlock(&g_mutex);
        return ret;
    }

    /* Add any extra header fields from params[2+] */
    for (uint32_t i = 2; i < param_count; i++) {
        if (params[i].str_val && strlen(params[i].str_val) > 0) {
            ret = add_header_field(params[i].str_val);
            if (ret < 0) {
                LOG_ERR("HTTPPOST: invalid header[%u]: %d", i, ret);
                reset_temp_resources();
                k_mutex_unlock(&g_mutex);
                return ret;
            }
        }
    }

    g_cfg.data_len = (size_t)data_len;
    g_cfg.send_buf_offset = 0;
    g_cfg.in_data_mode = true;
    g_cfg.data_mode_cmd = HTTPC_AT_DATA_MODE_POST;

    k_mutex_unlock(&g_mutex);

    /* Phase 1 response: ready to receive data */
    httpc_at_output(">\r\n");
    return 0;
}

/*-------------------------------------------------------------------------
 * AT+HTTPPUT handler
 *-----------------------------------------------------------------------*/

int httpc_at_handle_httpput(uint32_t op_type, uint32_t param_count, httpc_at_param_t *params)
{
    if (op_type != HTTPC_AT_OP_EXEC_W_PARAM || param_count < 3) {
        return -EINVAL;
    }

    /* Parse <url> */
    const char *url = params[0].str_val;

    if (!url || strlen(url) == 0 || !httpc_at_validate_url(url)) {
        LOG_ERR("HTTPPUT: invalid or missing URL");
        return -EINVAL;
    }

    /* Parse <content_type> */
    if (!params[1].int_valid) {
        LOG_ERR("HTTPPUT: param[1] (content_type) must be integer");
        return -EINVAL;
    }
    int content_type = params[1].int_val;

    if (content_type < HTTPC_AT_CTYPE_FORM_URLENCODED || content_type > HTTPC_AT_CTYPE_TEXT_XML) {
        LOG_ERR("HTTPPUT: invalid content_type=%d (must be 0-4)", content_type);
        return -EINVAL;
    }

    /* Parse <length> */
    if (!params[2].int_valid) {
        LOG_ERR("HTTPPUT: param[2] (length) must be integer");
        return -EINVAL;
    }
    int data_len = params[2].int_val;

    /* The hard ceiling is the stream-mode cap. If cache mode is active but the
     * body exceeds the (small) cache buffer, transparently fall through to stream
     * mode for this single command (same policy as HTTPPOST). */
    if (data_len <= 0 || data_len > HTTPC_AT_MAX_STREAM_SIZE) {
        LOG_ERR("HTTPPUT: invalid length=%d (max=%d)", data_len, HTTPC_AT_MAX_STREAM_SIZE);
        return -EINVAL;
    }

    k_mutex_lock(&g_mutex, K_FOREVER);
    reset_temp_resources();

    bool use_cache_this_cmd = (g_cfg.data_cache == 1) && (data_len <= HTTPC_AT_SEND_BUF_SIZE);
    g_cfg.force_stream = (g_cfg.data_cache == 1) && !use_cache_this_cmd;

    /* Store URL for data mode phase 2 */
    g_cfg.temp_url = httpc_strdup(url);
    if (!g_cfg.temp_url) {
        k_mutex_unlock(&g_mutex);
        return -ENOMEM;
    }

    if (use_cache_this_cmd) {
        /* Allocate send buffer */
        g_cfg.send_buf = k_malloc((size_t)data_len + 1);
        if (!g_cfg.send_buf) {
            k_free(g_cfg.temp_url);
            g_cfg.temp_url = NULL;
            k_mutex_unlock(&g_mutex);
            return -ENOMEM;
        }
    }

    /* Add Content-Type header */
    int ret = add_content_type_header(content_type);
    if (ret < 0) {
        LOG_ERR("HTTPPUT: add Content-Type header failed: %d", ret);
        reset_temp_resources();
        k_mutex_unlock(&g_mutex);
        return ret;
    }

    /* Add any extra header fields from params[3+] */
    for (uint32_t i = 3; i < param_count; i++) {
        if (params[i].str_val && strlen(params[i].str_val) > 0) {
            ret = add_header_field(params[i].str_val);
            if (ret < 0) {
                LOG_ERR("HTTPPUT: invalid header[%u]: %d", i, ret);
                reset_temp_resources();
                k_mutex_unlock(&g_mutex);
                return ret;
            }
        }
    }

    g_cfg.data_len = (size_t)data_len;
    g_cfg.send_buf_offset = 0;
    g_cfg.in_data_mode = true;
    g_cfg.data_mode_cmd = HTTPC_AT_DATA_MODE_PUT;

    k_mutex_unlock(&g_mutex);

    /* Phase 1 response */
    httpc_at_output(">\r\n");
    return 0;
}

/*-------------------------------------------------------------------------
 * AT+HTTPURLCFG handler
 *-----------------------------------------------------------------------*/

int httpc_at_handle_httpurlcfg(uint32_t op_type, uint32_t param_count, httpc_at_param_t *params)
{
    if (op_type == HTTPC_AT_OP_QUERY) {
        /* Query: return stored URL */
        k_mutex_lock(&g_mutex, K_FOREVER);

        if (g_cfg.url && g_cfg.url_len > 0) {
            httpc_at_output_fmt("+HTTPURLCFG:%zu,%s\r\n", g_cfg.url_len, g_cfg.url);
        } else {
            httpc_at_output("+HTTPURLCFG:0,null\r\n");
        }

        k_mutex_unlock(&g_mutex);
        return 0;
    }

    if (op_type != HTTPC_AT_OP_EXEC_W_PARAM || param_count != 1) {
        return -EINVAL;
    }

    if (!params[0].int_valid) {
        LOG_ERR("HTTPURLCFG: param[0] (url_length) must be integer");
        return -EINVAL;
    }

    int url_length = params[0].int_val;

    k_mutex_lock(&g_mutex, K_FOREVER);

    if (url_length == 0) {
        /* Clear stored URL */
        if (g_cfg.url) {
            k_free(g_cfg.url);
            g_cfg.url = NULL;
            g_cfg.url_len = 0;
        }
        k_mutex_unlock(&g_mutex);
        return 0;
    }

    if (url_length < 8 || url_length > HTTPC_AT_MAX_URL_LEN) {
        LOG_ERR("HTTPURLCFG: invalid url_length=%d (must be 0 or [8,%d])", url_length, HTTPC_AT_MAX_URL_LEN);
        k_mutex_unlock(&g_mutex);
        return -EINVAL;
    }

    reset_temp_resources();

    /* Allocate buffer for incoming URL (+1 for NUL) */
    g_cfg.send_buf = k_malloc((size_t)url_length + 1);
    if (!g_cfg.send_buf) {
        k_mutex_unlock(&g_mutex);
        return -ENOMEM;
    }

    g_cfg.data_len = (size_t)url_length;
    g_cfg.send_buf_offset = 0;
    g_cfg.in_data_mode = true;
    g_cfg.data_mode_cmd = HTTPC_AT_DATA_MODE_URLCFG;

    k_mutex_unlock(&g_mutex);

    /* Phase 1 response */
    httpc_at_output(">\r\n");
    return 0;
}

/*-------------------------------------------------------------------------
 * AT+HTTPSSLCFG handler
 *-----------------------------------------------------------------------*/

int httpc_at_handle_httpsslcfg(uint32_t op_type, uint32_t param_count, httpc_at_param_t *params)
{
    if (op_type == HTTPC_AT_OP_QUERY) {
        /* Query: return current SSL configuration */
        k_mutex_lock(&g_mutex, K_FOREVER);

        httpc_at_output_fmt("+HTTPSSLCFG:%d,\"%s\",\"%s\",\"%s\"\r\n", (int)g_cfg.https_auth_type, g_cfg.cert_file,
                            g_cfg.key_file, g_cfg.ca_file);

        k_mutex_unlock(&g_mutex);
        return 0;
    }

    if (op_type != HTTPC_AT_OP_EXEC_W_PARAM || param_count < 1 || param_count > 4) {
        return -EINVAL;
    }

    /* Parse <scheme> */
    if (!params[0].int_valid) {
        LOG_ERR("HTTPSSLCFG: param[0] (scheme) must be integer");
        return -EINVAL;
    }

    int scheme = params[0].int_val;

    if (scheme < 0 || scheme > 3) {
        LOG_ERR("HTTPSSLCFG: invalid scheme=%d (must be 0-3)", scheme);
        return -EINVAL;
    }

    k_mutex_lock(&g_mutex, K_FOREVER);

    if (g_cfg.tls_creds_loaded) {
        httpc_at_ssl_unload_certs();
        g_cfg.tls_creds_loaded = false;
    }

    g_cfg.https_auth_type = (httpc_at_auth_type_t)scheme;

    /* Clear existing file paths */
    memset(g_cfg.cert_file, 0, sizeof(g_cfg.cert_file));
    memset(g_cfg.key_file, 0, sizeof(g_cfg.key_file));
    memset(g_cfg.ca_file, 0, sizeof(g_cfg.ca_file));

    /* Parse optional file paths */
    if (param_count >= 2 && params[1].str_val && strlen(params[1].str_val) > 0) {
        strlcpy(g_cfg.cert_file, params[1].str_val, sizeof(g_cfg.cert_file));
    }
    if (param_count >= 3 && params[2].str_val && strlen(params[2].str_val) > 0) {
        strlcpy(g_cfg.key_file, params[2].str_val, sizeof(g_cfg.key_file));
    }
    if (param_count >= 4 && params[3].str_val && strlen(params[3].str_val) > 0) {
        strlcpy(g_cfg.ca_file, params[3].str_val, sizeof(g_cfg.ca_file));
    }

    LOG_DBG("HTTPSSLCFG: scheme=%d cert=%s key=%s ca=%s", scheme, g_cfg.cert_file, g_cfg.key_file, g_cfg.ca_file);

    k_mutex_unlock(&g_mutex);

    return 0;
}

/*-------------------------------------------------------------------------
 * AT+HTTPNETCFG handler
 *-----------------------------------------------------------------------*/

int httpc_at_handle_httpnetcfg(uint32_t op_type, uint32_t param_count, httpc_at_param_t *params)
{
    if (op_type == HTTPC_AT_OP_EXEC) {
        httpc_at_output("+HTTPNETCFG=<netcfg_type>,<value>\r\n");
        return 0;
    }

    if (op_type == HTTPC_AT_OP_QUERY) {
        int ip_prefer;

        k_mutex_lock(&g_mutex, K_FOREVER);
        ip_prefer = (g_cfg.ip_family == AF_INET6) ? 1 : 0;
        httpc_at_output_fmt("+HTTPNETCFG:%u,%u,%d,%u,%u\r\n", g_cfg.http_port, g_cfg.https_port, ip_prefer,
                            g_cfg.prealloc_ssl_buf, g_cfg.data_cache);
        k_mutex_unlock(&g_mutex);
        return 0;
    }

    if (op_type != HTTPC_AT_OP_EXEC_W_PARAM || param_count != 2) {
        return -EINVAL;
    }

    if (!params[0].int_valid || !params[1].int_valid) {
        return -EINVAL;
    }

    int netcfg_type = params[0].int_val;
    int value = params[1].int_val;

    k_mutex_lock(&g_mutex, K_FOREVER);

    switch (netcfg_type) {
    case HTTPC_AT_NETCFG_HTTP_PORT:
        if (value < 0 || value > 65535) {
            k_mutex_unlock(&g_mutex);
            return -EINVAL;
        }
        g_cfg.http_port = (uint16_t)value;
        break;
    case HTTPC_AT_NETCFG_HTTPS_PORT:
        if (value < 0 || value > 65535) {
            k_mutex_unlock(&g_mutex);
            return -EINVAL;
        }
        g_cfg.https_port = (uint16_t)value;
        break;
    case HTTPC_AT_NETCFG_IP_PREFER:
        if (value != 0 && value != 1) {
            k_mutex_unlock(&g_mutex);
            return -EINVAL;
        }
        g_cfg.ip_family = (value == 1) ? AF_INET6 : AF_INET;
        break;
    case HTTPC_AT_NETCFG_PREALLOC_SSL_BUF:
        if (value < 0 || value > 2) {
            k_mutex_unlock(&g_mutex);
            return -EINVAL;
        }
        g_cfg.prealloc_ssl_buf = (uint8_t)value;
        if (value == 0 || value == 2) {
            httpc_at_ssl_unload_certs();
            g_cfg.tls_creds_loaded = false;
        }
        break;
    case HTTPC_AT_NETCFG_DATA_CACHE:
        if (value != 0 && value != 1) {
            k_mutex_unlock(&g_mutex);
            return -EINVAL;
        }
        g_cfg.data_cache = (uint8_t)value;
        break;
    default:
        k_mutex_unlock(&g_mutex);
        return -EINVAL;
    }

    k_mutex_unlock(&g_mutex);
    return 0;
}

/*-------------------------------------------------------------------------
 * Utility
 *-----------------------------------------------------------------------*/

const struct httpc_at_global_cfg *httpc_at_get_config(void) { return &g_cfg; }
