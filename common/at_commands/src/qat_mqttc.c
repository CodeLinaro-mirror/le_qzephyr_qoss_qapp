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
#include "../../mqttc_at/inc/mqttc_at_handler.h"

LOG_MODULE_REGISTER(qat_mqttc, LOG_LEVEL_ERR);

#define QAT_MQTTC_MAX_PARAMS 16

struct qat_mqttc_parsed_args {
    char arg_buf[QAT_RESPONSE_BUF_SIZE];
    mqttc_at_param_t params[QAT_MQTTC_MAX_PARAMS];
    uint32_t count;
    bool quoted[QAT_MQTTC_MAX_PARAMS];
};

static bool g_mqttc_initialized;

static void qat_mqttc_output_cb(const char *data, size_t len, void *user_data)
{
    ARG_UNUSED(user_data);

    if (!data || len == 0) {
        return;
    }

    while (len > 0) {
        size_t chunk = MIN(len, CONFIG_RING0_BUF_SIZE);

        if (QAT_Output((uint32_t)chunk, data) < 0) {
            LOG_ERR("QAT_Output failed while sending MQTTC response");
            return;
        }
        data += chunk;
        len -= chunk;
    }
}

static int qat_mqttc_ensure_initialized(void)
{
    int ret;

    if (g_mqttc_initialized) {
        return 0;
    }

    ret = mqttc_at_init(qat_mqttc_output_cb, NULL);
    if (ret < 0) {
        LOG_ERR("mqttc_at_init failed: %d", ret);
        return ret;
    }

    g_mqttc_initialized = true;
    return 0;
}

static char *qat_mqttc_trim(char *s)
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

static int qat_mqttc_unquote_in_place(char *s)
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

static bool qat_mqttc_parse_int_strict(const char *s, int *out)
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

static int qat_mqttc_parse_args(const uint8_t *data, size_t data_size, struct qat_mqttc_parsed_args *parsed)
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
            if (parsed->count >= QAT_MQTTC_MAX_PARAMS) {
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
            if (parsed->count >= QAT_MQTTC_MAX_PARAMS) {
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

        tok = qat_mqttc_trim((char *)parsed->params[i].str_val);
        parsed->params[i].str_val = tok;

        len = strlen(tok);
        parsed->quoted[i] = (len >= 2 && tok[0] == '"' && tok[len - 1] == '"');

        if (parsed->quoted[i]) {
            int ret = qat_mqttc_unquote_in_place(tok);
            if (ret < 0) {
                return ret;
            }
        }

        if (!parsed->quoted[i] && qat_mqttc_parse_int_strict(tok, &v)) {
            parsed->params[i].int_valid = true;
            parsed->params[i].int_val = v;
        } else {
            parsed->params[i].int_valid = false;
            parsed->params[i].int_val = 0;
        }
    }

    return 0;
}

static int qat_mqttc_data_mode_callback(const uint8_t *data, size_t len)
{
    int ret;

    ret = mqttc_at_data_mode_input(data, len);
    if (ret < 0) {
        LOG_ERR("mqttc_at_data_mode_input failed: %d", ret);
        if (QAT_Transfer_Mode_set(QAT_Transfer_Mode_AT_COMMAND_E, NULL) < 0) {
            LOG_ERR("Failed to exit online data mode after error");
        }
        qat_hold_exit_error();
        return ret;
    }

    if (!mqttc_at_is_in_data_mode()) {
        if (QAT_Transfer_Mode_set(QAT_Transfer_Mode_AT_COMMAND_E, NULL) < 0) {
            LOG_ERR("Failed to exit online data mode");
            qat_hold_exit_error();
            return -EIO;
        }
        qat_hold_exit_ok();
    }

    return (int)len;
}

typedef int (*qat_mqttc_handler_t)(uint32_t op_type, uint32_t param_count, mqttc_at_param_t *params);

static cat_return_state qat_mqttc_dispatch(qat_mqttc_handler_t handler, uint32_t op_type, const uint8_t *data,
                                           size_t data_size, size_t args_num)
{
    struct qat_mqttc_parsed_args parsed;
    mqttc_at_param_t *params = NULL;
    uint32_t count = 0;
    int ret;

    ARG_UNUSED(args_num);

    ret = qat_mqttc_ensure_initialized();
    if (ret < 0) {
        return CAT_RETURN_STATE_ERROR;
    }

    if (op_type == MQTTC_AT_OP_EXEC_W_PARAM) {
        ret = qat_mqttc_parse_args(data, data_size, &parsed);
        if (ret < 0) {
            LOG_ERR("Failed to parse MQTTC parameters: %d", ret);
            return CAT_RETURN_STATE_ERROR;
        }
        params = parsed.params;
        count = parsed.count;
    }

    ret = handler(op_type, count, params);
    if (ret < 0) {
        return CAT_RETURN_STATE_ERROR;
    }

    if (mqttc_at_is_in_data_mode()) {
        if (QAT_Transfer_Mode_get() != QAT_Transfer_Mode_ONLINE_DATA_E) {
            ret = QAT_Transfer_Mode_set(QAT_Transfer_Mode_ONLINE_DATA_E, qat_mqttc_data_mode_callback);
            if (ret < 0) {
                LOG_ERR("Failed to enter online data mode: %d", ret);
                mqttc_at_cancel_data_mode();
                return CAT_RETURN_STATE_ERROR;
            }
        }
        /* Hold libcat — OK will be sent by qat_mqttc_data_mode_callback via qat_hold_exit_ok() */
        return CAT_RETURN_STATE_HOLD;
    }

    return CAT_RETURN_STATE_OK;
}

/* -------------------------------------------------------------------------
 * +MQTTINIT
 * ---------------------------------------------------------------------- */

static cat_return_state qat_mqttc_mqttinit_run(const struct cat_command *cmd)
{
    ARG_UNUSED(cmd);
    return qat_mqttc_dispatch(mqttc_at_handle_mqttinit, MQTTC_AT_OP_EXEC, NULL, 0, 0);
}

static cat_return_state qat_mqttc_mqttinit_write(const struct cat_command *cmd, const uint8_t *data,
                                                 const size_t data_size, const size_t args_num)
{
    ARG_UNUSED(cmd);
    return qat_mqttc_dispatch(mqttc_at_handle_mqttinit, MQTTC_AT_OP_EXEC_W_PARAM, data, data_size, args_num);
}

/* -------------------------------------------------------------------------
 * +MQTTCONN
 * ---------------------------------------------------------------------- */

static cat_return_state qat_mqttc_mqttconn_run(const struct cat_command *cmd)
{
    ARG_UNUSED(cmd);
    return qat_mqttc_dispatch(mqttc_at_handle_mqttconn, MQTTC_AT_OP_EXEC, NULL, 0, 0);
}

static cat_return_state qat_mqttc_mqttconn_read(const struct cat_command *cmd, uint8_t *data, size_t *data_size,
                                                const size_t max_data_size)
{
    ARG_UNUSED(cmd);
    ARG_UNUSED(data);
    ARG_UNUSED(data_size);
    ARG_UNUSED(max_data_size);
    return qat_mqttc_dispatch(mqttc_at_handle_mqttconn, MQTTC_AT_OP_QUERY, NULL, 0, 0);
}

static cat_return_state qat_mqttc_mqttconn_write(const struct cat_command *cmd, const uint8_t *data,
                                                 const size_t data_size, const size_t args_num)
{
    ARG_UNUSED(cmd);
    return qat_mqttc_dispatch(mqttc_at_handle_mqttconn, MQTTC_AT_OP_EXEC_W_PARAM, data, data_size, args_num);
}

/* -------------------------------------------------------------------------
 * +MQTTSUB
 * ---------------------------------------------------------------------- */

static cat_return_state qat_mqttc_mqttsub_run(const struct cat_command *cmd)
{
    ARG_UNUSED(cmd);
    return qat_mqttc_dispatch(mqttc_at_handle_mqttsub, MQTTC_AT_OP_EXEC, NULL, 0, 0);
}

static cat_return_state qat_mqttc_mqttsub_read(const struct cat_command *cmd, uint8_t *data, size_t *data_size,
                                               const size_t max_data_size)
{
    ARG_UNUSED(cmd);
    ARG_UNUSED(data);
    ARG_UNUSED(data_size);
    ARG_UNUSED(max_data_size);
    return qat_mqttc_dispatch(mqttc_at_handle_mqttsub, MQTTC_AT_OP_QUERY, NULL, 0, 0);
}

static cat_return_state qat_mqttc_mqttsub_write(const struct cat_command *cmd, const uint8_t *data,
                                                const size_t data_size, const size_t args_num)
{
    ARG_UNUSED(cmd);
    return qat_mqttc_dispatch(mqttc_at_handle_mqttsub, MQTTC_AT_OP_EXEC_W_PARAM, data, data_size, args_num);
}

/* -------------------------------------------------------------------------
 * +MQTTPUB
 * ---------------------------------------------------------------------- */

static cat_return_state qat_mqttc_mqttpub_run(const struct cat_command *cmd)
{
    ARG_UNUSED(cmd);
    return qat_mqttc_dispatch(mqttc_at_handle_mqttpub, MQTTC_AT_OP_EXEC, NULL, 0, 0);
}

static cat_return_state qat_mqttc_mqttpub_write(const struct cat_command *cmd, const uint8_t *data,
                                                const size_t data_size, const size_t args_num)
{
    ARG_UNUSED(cmd);
    return qat_mqttc_dispatch(mqttc_at_handle_mqttpub, MQTTC_AT_OP_EXEC_W_PARAM, data, data_size, args_num);
}

/* -------------------------------------------------------------------------
 * +MQTTPUBRAW
 * ---------------------------------------------------------------------- */

static cat_return_state qat_mqttc_mqttpubraw_run(const struct cat_command *cmd)
{
    ARG_UNUSED(cmd);
    return qat_mqttc_dispatch(mqttc_at_handle_mqttpubraw, MQTTC_AT_OP_EXEC, NULL, 0, 0);
}

static cat_return_state qat_mqttc_mqttpubraw_write(const struct cat_command *cmd, const uint8_t *data,
                                                   const size_t data_size, const size_t args_num)
{
    ARG_UNUSED(cmd);
    return qat_mqttc_dispatch(mqttc_at_handle_mqttpubraw, MQTTC_AT_OP_EXEC_W_PARAM, data, data_size, args_num);
}

/* -------------------------------------------------------------------------
 * +MQTTUNSUB
 * ---------------------------------------------------------------------- */

static cat_return_state qat_mqttc_mqttunsub_run(const struct cat_command *cmd)
{
    ARG_UNUSED(cmd);
    return qat_mqttc_dispatch(mqttc_at_handle_mqttunsub, MQTTC_AT_OP_EXEC, NULL, 0, 0);
}

static cat_return_state qat_mqttc_mqttunsub_write(const struct cat_command *cmd, const uint8_t *data,
                                                  const size_t data_size, const size_t args_num)
{
    ARG_UNUSED(cmd);
    return qat_mqttc_dispatch(mqttc_at_handle_mqttunsub, MQTTC_AT_OP_EXEC_W_PARAM, data, data_size, args_num);
}

/* -------------------------------------------------------------------------
 * +MQTTDISCONN
 * ---------------------------------------------------------------------- */

static cat_return_state qat_mqttc_mqttdisconn_run(const struct cat_command *cmd)
{
    ARG_UNUSED(cmd);
    return qat_mqttc_dispatch(mqttc_at_handle_mqttdisconn, MQTTC_AT_OP_EXEC, NULL, 0, 0);
}

static cat_return_state qat_mqttc_mqttdisconn_write(const struct cat_command *cmd, const uint8_t *data,
                                                    const size_t data_size, const size_t args_num)
{
    ARG_UNUSED(cmd);
    return qat_mqttc_dispatch(mqttc_at_handle_mqttdisconn, MQTTC_AT_OP_EXEC_W_PARAM, data, data_size, args_num);
}

/* -------------------------------------------------------------------------
 * +MQTTDESTROY
 * ---------------------------------------------------------------------- */

static cat_return_state qat_mqttc_mqttdestroy_run(const struct cat_command *cmd)
{
    ARG_UNUSED(cmd);
    return qat_mqttc_dispatch(mqttc_at_handle_mqttdestroy, MQTTC_AT_OP_EXEC, NULL, 0, 0);
}

static cat_return_state qat_mqttc_mqttdestroy_write(const struct cat_command *cmd, const uint8_t *data,
                                                    const size_t data_size, const size_t args_num)
{
    ARG_UNUSED(cmd);
    return qat_mqttc_dispatch(mqttc_at_handle_mqttdestroy, MQTTC_AT_OP_EXEC_W_PARAM, data, data_size, args_num);
}

/* -------------------------------------------------------------------------
 * +MQTTMODE
 * ---------------------------------------------------------------------- */

static cat_return_state qat_mqttc_mqttmode_run(const struct cat_command *cmd)
{
    ARG_UNUSED(cmd);
    return qat_mqttc_dispatch(mqttc_at_handle_mqttmode, MQTTC_AT_OP_EXEC, NULL, 0, 0);
}

static cat_return_state qat_mqttc_mqttmode_write(const struct cat_command *cmd, const uint8_t *data,
                                                 const size_t data_size, const size_t args_num)
{
    ARG_UNUSED(cmd);
    return qat_mqttc_dispatch(mqttc_at_handle_mqttmode, MQTTC_AT_OP_EXEC_W_PARAM, data, data_size, args_num);
}

/* -------------------------------------------------------------------------
 * Command group
 * ---------------------------------------------------------------------- */

static struct cat_command qat_mqttc_cmds[] = {
    {
        .name = "+MQTTINIT",
        .description = "Initialize MQTT session",
        .run = qat_mqttc_mqttinit_run,
        .write = qat_mqttc_mqttinit_write,
    },
    {
        .name = "+MQTTCONN",
        .description = "Connect to MQTT broker",
        .run = qat_mqttc_mqttconn_run,
        .read = qat_mqttc_mqttconn_read,
        .write = qat_mqttc_mqttconn_write,
    },
    {
        .name = "+MQTTSUB",
        .description = "Subscribe to MQTT topic",
        .run = qat_mqttc_mqttsub_run,
        .read = qat_mqttc_mqttsub_read,
        .write = qat_mqttc_mqttsub_write,
    },
    {
        .name = "+MQTTPUB",
        .description = "Publish MQTT message",
        .run = qat_mqttc_mqttpub_run,
        .write = qat_mqttc_mqttpub_write,
    },
    {
        .name = "+MQTTPUBRAW",
        .description = "Publish long MQTT message (data mode)",
        .run = qat_mqttc_mqttpubraw_run,
        .write = qat_mqttc_mqttpubraw_write,
    },
    {
        .name = "+MQTTUNSUB",
        .description = "Unsubscribe from MQTT topic",
        .run = qat_mqttc_mqttunsub_run,
        .write = qat_mqttc_mqttunsub_write,
    },
    {
        .name = "+MQTTDISCONN",
        .description = "Disconnect from MQTT broker",
        .run = qat_mqttc_mqttdisconn_run,
        .write = qat_mqttc_mqttdisconn_write,
    },
    {
        .name = "+MQTTDESTROY",
        .description = "Destroy MQTT session",
        .run = qat_mqttc_mqttdestroy_run,
        .write = qat_mqttc_mqttdestroy_write,
    },
    {
        .name = "+MQTTMODE",
        .description = "Set MQTT receive mode (string/hex)",
        .run = qat_mqttc_mqttmode_run,
        .write = qat_mqttc_mqttmode_write,
    },
};

static struct cat_command_group qat_mqttc_cmd_group = {
    .name = "QAT_MQTTC",
    .cmd = qat_mqttc_cmds,
    .cmd_num = ARRAY_SIZE(qat_mqttc_cmds),
};

static struct cat_command_group *qat_mqttc_get_command_group(void) { return &qat_mqttc_cmd_group; }

QAT_REGISTER_CMD_GROUP(qat_mqttc_get_command_group, "MQTTC");
