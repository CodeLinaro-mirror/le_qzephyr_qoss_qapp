/*
 * Copyright (c) 2026 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <cat.h>
#include "qat_api.h"
#include "../../httpc_at/inc/httpc_at_handler.h"

LOG_MODULE_REGISTER(qat_httpc, LOG_LEVEL_ERR);

#define QAT_HTTPC_MAX_PARAMS 16

struct qat_httpc_parsed_args {
    char arg_buf[QAT_RESPONSE_BUF_SIZE];
    httpc_at_param_t params[QAT_HTTPC_MAX_PARAMS];
    uint32_t count;
    bool quoted[QAT_HTTPC_MAX_PARAMS];
};

static bool g_httpc_initialized;

static void qat_httpc_output_cb(const char *data, size_t len, void *user_data)
{
    ARG_UNUSED(user_data);

    if (!data || len == 0) {
        return;
    }

    /* Ring buffer has a per-packet size limit; send in chunks. */
    while (len > 0) {
        size_t chunk = MIN(len, CONFIG_RING0_BUF_SIZE);

        if (QAT_Output((uint32_t)chunk, data) < 0) {
            LOG_ERR("QAT_Output failed while sending HTTPC response");
            return;
        }
        data += chunk;
        len -= chunk;
    }
}

static int qat_httpc_ensure_initialized(void)
{
    int ret;

    if (g_httpc_initialized) {
        return 0;
    }

    ret = httpc_at_init(qat_httpc_output_cb, NULL);
    if (ret < 0) {
        LOG_ERR("httpc_at_init failed: %d", ret);
        return ret;
    }

    g_httpc_initialized = true;
    return 0;
}

static char *qat_httpc_trim(char *s)
{
    char *end;

    while (*s && isspace((unsigned char)*s)) {
        s++;
    }

    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        end--;
    }

    *end = '\0';
    return s;
}

static int qat_httpc_unquote_in_place(char *s)
{
    size_t len;
    char *src;
    char *dst;
    char *end;

    len = strlen(s);
    if (len < 2 || s[0] != '"' || s[len - 1] != '"') {
        return 0;
    }

    src = s + 1;
    end = s + len - 1;
    dst = s;

    while (src < end) {
        char ch = *src++;

        if (ch == '\\') {
            if (src >= end) {
                return -EINVAL;
            }

            ch = *src++;
            switch (ch) {
            case '\\':
            case '"':
                break;
            case 'n':
                ch = '\n';
                break;
            case 'r':
                ch = '\r';
                break;
            case 't':
                ch = '\t';
                break;
            default:
                break;
            }
        }

        *dst++ = ch;
    }

    *dst = '\0';
    return 0;
}

static bool qat_httpc_parse_int_strict(const char *s, int *out)
{
    long val;
    char *end = NULL;

    if (!s || *s == '\0') {
        return false;
    }

    val = strtol(s, &end, 10);
    if (end == NULL || *end != '\0') {
        return false;
    }
    if (val < INT_MIN || val > INT_MAX) {
        return false;
    }

    *out = (int)val;
    return true;
}

static int qat_httpc_parse_args(const uint8_t *data, size_t data_size, struct qat_httpc_parsed_args *parsed)
{
    bool in_quotes = false;
    bool escape = false;
    char *token_start;
    size_t copy_len;

    memset(parsed, 0, sizeof(*parsed));

    if (!data || data_size == 0) {
        return 0;
    }

    copy_len = MIN(data_size, sizeof(parsed->arg_buf) - 1);
    if (copy_len != data_size) {
        return -E2BIG;
    }

    memcpy(parsed->arg_buf, data, copy_len);
    parsed->arg_buf[copy_len] = '\0';
    token_start = parsed->arg_buf;

    for (char *p = parsed->arg_buf;; p++) {
        char ch = *p;

        if (ch == '\0') {
            if (in_quotes) {
                return -EINVAL;
            }
            if (parsed->count >= QAT_HTTPC_MAX_PARAMS) {
                return -E2BIG;
            }
            parsed->params[parsed->count].str_val = token_start;
            parsed->count++;
            break;
        }

        if (escape) {
            escape = false;
            continue;
        }

        if (ch == '\\' && in_quotes) {
            escape = true;
            continue;
        }

        if (ch == '"') {
            in_quotes = !in_quotes;
            continue;
        }

        if (ch == ',' && !in_quotes) {
            *p = '\0';
            if (parsed->count >= QAT_HTTPC_MAX_PARAMS) {
                return -E2BIG;
            }
            parsed->params[parsed->count].str_val = token_start;
            parsed->count++;
            token_start = p + 1;
        }
    }

    for (uint32_t i = 0; i < parsed->count; i++) {
        int v;
        char *tok;
        size_t len;

        tok = qat_httpc_trim((char *)parsed->params[i].str_val);
        parsed->params[i].str_val = tok;

        len = strlen(tok);
        parsed->quoted[i] = (len >= 2 && tok[0] == '"' && tok[len - 1] == '"');

        if (parsed->quoted[i]) {
            int ret = qat_httpc_unquote_in_place(tok);
            if (ret < 0) {
                return ret;
            }
        }

        if (!parsed->quoted[i] && qat_httpc_parse_int_strict(tok, &v)) {
            parsed->params[i].int_valid = true;
            parsed->params[i].int_val = v;
        } else {
            parsed->params[i].int_valid = false;
            parsed->params[i].int_val = 0;
        }
    }

    return 0;
}

static int qat_httpc_data_mode_callback(const uint8_t *data, size_t len)
{
    int ret;
    struct cat_object *cat_obj;
    cat_status hold_status;

    ret = httpc_at_data_mode_input(data, len);
    if (ret < 0) {
        LOG_ERR("httpc_at_data_mode_input failed: %d", ret);
        if (QAT_Transfer_Mode_set(QAT_Transfer_Mode_AT_COMMAND_E, NULL) < 0) {
            LOG_ERR("Failed to exit online data mode after error");
        }

        cat_obj = qat_get_cat_object();
        hold_status = cat_hold_exit(cat_obj, CAT_STATUS_ERROR);
        if (hold_status != CAT_STATUS_OK) {
            LOG_ERR("cat_hold_exit(ERROR) failed: %d", hold_status);
        }

        return ret;
    }

    if (!httpc_at_is_in_data_mode()) {
        if (QAT_Transfer_Mode_set(QAT_Transfer_Mode_AT_COMMAND_E, NULL) < 0) {
            LOG_ERR("Failed to exit online data mode");
            cat_obj = qat_get_cat_object();
            hold_status = cat_hold_exit(cat_obj, CAT_STATUS_ERROR);
            if (hold_status != CAT_STATUS_OK) {
                LOG_ERR("cat_hold_exit(ERROR) failed: %d", hold_status);
            }
            return -EIO;
        }

        cat_obj = qat_get_cat_object();
        hold_status = cat_hold_exit(cat_obj, CAT_STATUS_OK);
        if (hold_status != CAT_STATUS_OK) {
            LOG_ERR("cat_hold_exit(OK) failed: %d", hold_status);
            return -EIO;
        }
    }

    return (int)len;
}

typedef int (*qat_httpc_handler_t)(uint32_t op_type, uint32_t param_count, httpc_at_param_t *params);

static cat_return_state qat_httpc_dispatch(qat_httpc_handler_t handler, uint32_t op_type, const uint8_t *data,
                                           size_t data_size, size_t args_num)
{
    struct qat_httpc_parsed_args parsed;
    httpc_at_param_t *params = NULL;
    uint32_t count = 0;
    int ret;

    ARG_UNUSED(args_num);

    ret = qat_httpc_ensure_initialized();
    if (ret < 0) {
        return CAT_RETURN_STATE_ERROR;
    }

    if (op_type == HTTPC_AT_OP_EXEC_W_PARAM) {
        ret = qat_httpc_parse_args(data, data_size, &parsed);
        if (ret < 0) {
            LOG_ERR("Failed to parse HTTPC parameters: %d", ret);
            return CAT_RETURN_STATE_ERROR;
        }
        params = parsed.params;
        count = parsed.count;
    }

    ret = handler(op_type, count, params);
    if (ret < 0) {
        return CAT_RETURN_STATE_ERROR;
    }

    if (httpc_at_is_in_data_mode()) {
        if (QAT_Transfer_Mode_get() != QAT_Transfer_Mode_ONLINE_DATA_E) {
            ret = QAT_Transfer_Mode_set(QAT_Transfer_Mode_ONLINE_DATA_E, qat_httpc_data_mode_callback);
            if (ret < 0) {
                LOG_ERR("Failed to enter online data mode: %d", ret);
                return CAT_RETURN_STATE_ERROR;
            }
        }

        return CAT_RETURN_STATE_HOLD;
    }

    return CAT_RETURN_STATE_OK;
}

/* -------------------------------------------------------------------------
 * +HTTPCLIENT
 * ---------------------------------------------------------------------- */

static cat_return_state qat_httpc_httpclient_run(const struct cat_command *cmd)
{
    ARG_UNUSED(cmd);
    return qat_httpc_dispatch(httpc_at_handle_httpclient, HTTPC_AT_OP_EXEC, NULL, 0, 0);
}

static cat_return_state qat_httpc_httpclient_write(const struct cat_command *cmd, const uint8_t *data,
                                                   const size_t data_size, const size_t args_num)
{
    ARG_UNUSED(cmd);
    return qat_httpc_dispatch(httpc_at_handle_httpclient, HTTPC_AT_OP_EXEC_W_PARAM, data, data_size, args_num);
}

/* -------------------------------------------------------------------------
 * +HTTPGETSIZE
 * ---------------------------------------------------------------------- */

static cat_return_state qat_httpc_httpgetsize_run(const struct cat_command *cmd)
{
    ARG_UNUSED(cmd);
    return QAT_Response_Str(QAT_RC_OK, "+HTTPGETSIZE=\"url\",[timeout]\r\n");
}

static cat_return_state qat_httpc_httpgetsize_write(const struct cat_command *cmd, const uint8_t *data,
                                                    const size_t data_size, const size_t args_num)
{
    ARG_UNUSED(cmd);
    return qat_httpc_dispatch(httpc_at_handle_httpgetsize, HTTPC_AT_OP_EXEC_W_PARAM, data, data_size, args_num);
}

/* -------------------------------------------------------------------------
 * +HTTPGET
 * ---------------------------------------------------------------------- */

static cat_return_state qat_httpc_httpget_run(const struct cat_command *cmd)
{
    ARG_UNUSED(cmd);
    return QAT_Response_Str(QAT_RC_OK, "+HTTPCGET=\"url\",[timeout]\r\n");
}

static cat_return_state qat_httpc_httpget_write(const struct cat_command *cmd, const uint8_t *data,
                                                const size_t data_size, const size_t args_num)
{
    ARG_UNUSED(cmd);
    return qat_httpc_dispatch(httpc_at_handle_httpget, HTTPC_AT_OP_EXEC_W_PARAM, data, data_size, args_num);
}

/* -------------------------------------------------------------------------
 * +HTTPPOST
 * ---------------------------------------------------------------------- */

static cat_return_state qat_httpc_httppost_run(const struct cat_command *cmd)
{
    ARG_UNUSED(cmd);
    return QAT_Response_Str(QAT_RC_OK, "+HTTPPOST=\"url\",<length>,[\"http_req_header\"],[...]\r\n");
}

static cat_return_state qat_httpc_httppost_write(const struct cat_command *cmd, const uint8_t *data,
                                                 const size_t data_size, const size_t args_num)
{
    ARG_UNUSED(cmd);
    return qat_httpc_dispatch(httpc_at_handle_httppost, HTTPC_AT_OP_EXEC_W_PARAM, data, data_size, args_num);
}

/* -------------------------------------------------------------------------
 * +HTTPPUT
 * ---------------------------------------------------------------------- */

static cat_return_state qat_httpc_httpput_run(const struct cat_command *cmd)
{
    ARG_UNUSED(cmd);
    return QAT_Response_Str(QAT_RC_OK, "+HTTPPUT=\"url\",<content_type>,<length>,[\"http_req_header\"],[...]\r\n");
}

static cat_return_state qat_httpc_httpput_write(const struct cat_command *cmd, const uint8_t *data,
                                                const size_t data_size, const size_t args_num)
{
    ARG_UNUSED(cmd);
    return qat_httpc_dispatch(httpc_at_handle_httpput, HTTPC_AT_OP_EXEC_W_PARAM, data, data_size, args_num);
}

/* -------------------------------------------------------------------------
 * +HTTPURLCFG
 * ---------------------------------------------------------------------- */

static cat_return_state qat_httpc_httpurlcfg_run(const struct cat_command *cmd)
{
    ARG_UNUSED(cmd);
    return QAT_Response_Str(QAT_RC_OK, "+HTTPURLCFG=<length>\r\n");
}

static cat_return_state qat_httpc_httpurlcfg_read(const struct cat_command *cmd, uint8_t *data, size_t *data_size,
                                                  const size_t max_data_size)
{
    ARG_UNUSED(cmd);
    ARG_UNUSED(data);
    ARG_UNUSED(data_size);
    ARG_UNUSED(max_data_size);
    return qat_httpc_dispatch(httpc_at_handle_httpurlcfg, HTTPC_AT_OP_QUERY, NULL, 0, 0);
}

static cat_return_state qat_httpc_httpurlcfg_write(const struct cat_command *cmd, const uint8_t *data,
                                                   const size_t data_size, const size_t args_num)
{
    ARG_UNUSED(cmd);
    return qat_httpc_dispatch(httpc_at_handle_httpurlcfg, HTTPC_AT_OP_EXEC_W_PARAM, data, data_size, args_num);
}

/* -------------------------------------------------------------------------
 * +HTTPSSLCFG
 * ---------------------------------------------------------------------- */

static cat_return_state qat_httpc_httpsslcfg_run(const struct cat_command *cmd)
{
    ARG_UNUSED(cmd);
    return QAT_Response_Str(QAT_RC_OK, "+HTTPSSLCFG=<scheme>,[\"cert_file\"],[\"key_file\"],[\"ca_file\"]\r\n");
}

static cat_return_state qat_httpc_httpsslcfg_read(const struct cat_command *cmd, uint8_t *data, size_t *data_size,
                                                  const size_t max_data_size)
{
    ARG_UNUSED(cmd);
    ARG_UNUSED(data);
    ARG_UNUSED(data_size);
    ARG_UNUSED(max_data_size);
    return qat_httpc_dispatch(httpc_at_handle_httpsslcfg, HTTPC_AT_OP_QUERY, NULL, 0, 0);
}

static cat_return_state qat_httpc_httpsslcfg_write(const struct cat_command *cmd, const uint8_t *data,
                                                   const size_t data_size, const size_t args_num)
{
    ARG_UNUSED(cmd);
    return qat_httpc_dispatch(httpc_at_handle_httpsslcfg, HTTPC_AT_OP_EXEC_W_PARAM, data, data_size, args_num);
}

/* -------------------------------------------------------------------------
 * +HTTPNETCFG
 * ---------------------------------------------------------------------- */

static cat_return_state qat_httpc_httpnetcfg_run(const struct cat_command *cmd)
{
    ARG_UNUSED(cmd);
    return QAT_Response_Str(QAT_RC_OK, "+HTTPNETCFG=<netcfg_type>,<value>\r\n");
}

static cat_return_state qat_httpc_httpnetcfg_read(const struct cat_command *cmd, uint8_t *data, size_t *data_size,
                                                  const size_t max_data_size)
{
    ARG_UNUSED(cmd);
    ARG_UNUSED(data);
    ARG_UNUSED(data_size);
    ARG_UNUSED(max_data_size);
    return qat_httpc_dispatch(httpc_at_handle_httpnetcfg, HTTPC_AT_OP_QUERY, NULL, 0, 0);
}

static cat_return_state qat_httpc_httpnetcfg_write(const struct cat_command *cmd, const uint8_t *data,
                                                   const size_t data_size, const size_t args_num)
{
    ARG_UNUSED(cmd);
    return qat_httpc_dispatch(httpc_at_handle_httpnetcfg, HTTPC_AT_OP_EXEC_W_PARAM, data, data_size, args_num);
}

/* -------------------------------------------------------------------------
 * Command group
 * ---------------------------------------------------------------------- */

static struct cat_command qat_httpc_cmds[] = {
    {
        .name = "+HTTPCLIENT",
        .description = "Send HTTP client request",
        .run = qat_httpc_httpclient_run,
        .write = qat_httpc_httpclient_write,
    },
    {
        .name = "+HTTPGETSIZE",
        .description = "Get HTTP resource size",
        .run = qat_httpc_httpgetsize_run,
        .write = qat_httpc_httpgetsize_write,
    },
    {
        .name = "+HTTPGET",
        .description = "Get HTTP resource",
        .run = qat_httpc_httpget_run,
        .write = qat_httpc_httpget_write,
    },
    {
        .name = "+HTTPPOST",
        .description = "Post HTTP data",
        .run = qat_httpc_httppost_run,
        .write = qat_httpc_httppost_write,
    },
    {
        .name = "+HTTPPUT",
        .description = "Put HTTP data",
        .run = qat_httpc_httpput_run,
        .write = qat_httpc_httpput_write,
    },
    {
        .name = "+HTTPURLCFG",
        .description = "Set/Query long HTTP URL",
        .run = qat_httpc_httpurlcfg_run,
        .read = qat_httpc_httpurlcfg_read,
        .write = qat_httpc_httpurlcfg_write,
    },
    {
        .name = "+HTTPSSLCFG",
        .description = "Set/Query certificate file",
        .run = qat_httpc_httpsslcfg_run,
        .read = qat_httpc_httpsslcfg_read,
        .write = qat_httpc_httpsslcfg_write,
    },
    {
        .name = "+HTTPNETCFG",
        .description = "Set/Query net configuration",
        .run = qat_httpc_httpnetcfg_run,
        .read = qat_httpc_httpnetcfg_read,
        .write = qat_httpc_httpnetcfg_write,
    },
};

static struct cat_command_group qat_httpc_cmd_group = {
    .name = "QAT_HTTPC",
    .cmd = qat_httpc_cmds,
    .cmd_num = ARRAY_SIZE(qat_httpc_cmds),
};

static struct cat_command_group *qat_httpc_get_command_group(void)
{
    return &qat_httpc_cmd_group;
}

QAT_REGISTER_CMD_GROUP(qat_httpc_get_command_group, "HTTPC");
