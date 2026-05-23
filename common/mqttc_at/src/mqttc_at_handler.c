/*
 * Copyright (c) 2026 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "mqttc_at_handler.h"

LOG_MODULE_REGISTER(mqttc_at_handler, CONFIG_MQTTC_AT_LOG_LEVEL);

/* Forward declarations for core functions */
int mqttc_at_core_init(mqttc_at_output_cb_t output_cb, void *user_data);
int mqttc_at_core_init_session(int sid, bool use_ssl, const char *ca_file, const char *cert_file, const char *key_file);
int mqttc_at_core_connect(int sid, const char *host, uint16_t port, const char *client_id, const char *username,
                          const char *password, uint16_t keepalive, bool clean_session);
int mqttc_at_core_subscribe(int sid, const char *topic, uint8_t qos);
int mqttc_at_core_publish(int sid, const char *topic, uint8_t qos, const uint8_t *payload, size_t payload_len,
                          uint8_t retain);
int mqttc_at_core_unsubscribe(int sid, const char *topic);
int mqttc_at_core_disconnect(int sid);
int mqttc_at_core_destroy(int sid);
int mqttc_at_core_query_conn(int sid, char *buf, size_t buf_len);
int mqttc_at_core_query_sub(int sid, char *buf, size_t buf_len);
void mqttc_at_core_set_recv_mode(int mode);
bool mqttc_at_core_is_in_data_mode(void);
void mqttc_at_core_cancel_data_mode(void);
int mqttc_at_core_start_pubraw(int sid, const char *topic, size_t length, uint8_t qos, uint8_t retain);
int mqttc_at_core_data_mode_input(const uint8_t *data, size_t len);

static bool g_initialized;
static mqttc_at_output_cb_t g_output_cb;
static void *g_output_user_data;

static void mqttc_output(const char *buf)
{
    if (g_output_cb) {
        g_output_cb(buf, strlen(buf), g_output_user_data);
    }
}

/* -------------------------------------------------------------------------
 * Public lifecycle
 * ---------------------------------------------------------------------- */

int mqttc_at_init(mqttc_at_output_cb_t output_cb, void *user_data)
{
    int rc;

    if (g_initialized) {
        return 0;
    }

    g_output_cb = output_cb;
    g_output_user_data = user_data;

    rc = mqttc_at_core_init(output_cb, user_data);
    if (rc < 0) {
        return rc;
    }

    g_initialized = true;
    return 0;
}

bool mqttc_at_is_in_data_mode(void) { return mqttc_at_core_is_in_data_mode(); }

void mqttc_at_cancel_data_mode(void) { mqttc_at_core_cancel_data_mode(); }

int mqttc_at_data_mode_input(const uint8_t *data, size_t len) { return mqttc_at_core_data_mode_input(data, len); }

/* -------------------------------------------------------------------------
 * AT+MQTTINIT
 * ---------------------------------------------------------------------- */

int mqttc_at_handle_mqttinit(uint32_t op_type, uint32_t param_count, mqttc_at_param_t *params)
{
    int sid;
    bool use_ssl;
    int rc;

    if (op_type == MQTTC_AT_OP_EXEC) {
        mqttc_output("+MQTTINIT=<session_id>,<TCP|SSL>[,<\"ca_file\">,<\"cert_file\">,<\"key_file\">]\r\n");
        return 0;
    }

    if (op_type != MQTTC_AT_OP_EXEC_W_PARAM) {
        return -EINVAL;
    }

    /* AT+MQTTINIT=<session_id>,<scheme>[,<"ca_file">,<"cert_file">,<"key_file">] */
    if (param_count < 2) {
        LOG_ERR("MQTTINIT: expected at least 2 params, got %u", param_count);
        return -EINVAL;
    }

    if (!params[0].int_valid) {
        LOG_ERR("MQTTINIT: session_id must be integer");
        return -EINVAL;
    }
    sid = params[0].int_val;

    if (strcmp(params[1].str_val, "SSL") == 0 || strcmp(params[1].str_val, "ssl") == 0) {
        use_ssl = true;
        if (param_count != 3 && param_count != 5) {
            LOG_ERR("MQTTINIT: SSL requires <ca_file> or <ca_file>,<cert_file>,<key_file>");
            return -EINVAL;
        }
        if (params[2].str_val[0] == '\0') {
            LOG_ERR("MQTTINIT: ca_file must not be empty");
            return -EINVAL;
        }
        if (param_count == 5 && (params[3].str_val[0] == '\0' || params[4].str_val[0] == '\0')) {
            LOG_ERR("MQTTINIT: cert_file and key_file must not be empty");
            return -EINVAL;
        }
    } else if (strcmp(params[1].str_val, "TCP") == 0 || strcmp(params[1].str_val, "tcp") == 0) {
        use_ssl = false;
        if (param_count != 2) {
            LOG_ERR("MQTTINIT: TCP takes exactly <session_id>,TCP");
            return -EINVAL;
        }
    } else {
        LOG_ERR("MQTTINIT: unknown scheme '%s' (use TCP or SSL)", params[1].str_val);
        return -EINVAL;
    }

    rc = mqttc_at_core_init_session(sid, use_ssl, param_count > 2 ? params[2].str_val : NULL,
                                    param_count > 3 ? params[3].str_val : NULL,
                                    param_count > 4 ? params[4].str_val : NULL);
    if (rc < 0) {
        LOG_ERR("MQTTINIT: init_session(%d) failed: %d", sid, rc);
        return rc;
    }

    char urc[32];
    snprintf(urc, sizeof(urc), "\r\n+EVT:MQTT_INITED:%d\r\n", sid);
    mqttc_output(urc);
    return 0;
}

/* -------------------------------------------------------------------------
 * AT+MQTTCONN
 * ---------------------------------------------------------------------- */

int mqttc_at_handle_mqttconn(uint32_t op_type, uint32_t param_count, mqttc_at_param_t *params)
{
    if (op_type == MQTTC_AT_OP_EXEC) {
        mqttc_output("+MQTTCONN=<session_id>,\"host\",<port>,\"clientid\","
                     "\"username\",\"password\",<keepalive>,<clean_session>\r\n");
        return 0;
    }

    if (op_type == MQTTC_AT_OP_QUERY) {
        char buf[256];

        for (int i = 0; i < CONFIG_MQTTC_AT_MAX_SESSIONS; i++) {
            mqttc_at_core_query_conn(i, buf, sizeof(buf));
            mqttc_output(buf);
        }
        return 0;
    }

    if (op_type != MQTTC_AT_OP_EXEC_W_PARAM) {
        return -EINVAL;
    }

    /* AT+MQTTCONN=<sid>,"host",<port>,"clientid","user","pass",<keepalive>,<clean> */
    if (param_count < 8) {
        LOG_ERR("MQTTCONN: expected 8 params, got %u", param_count);
        return -EINVAL;
    }

    if (!params[0].int_valid) {
        return -EINVAL;
    }
    int sid = params[0].int_val;

    const char *host = params[1].str_val;
    if (!params[2].int_valid || params[2].int_val < 1 || params[2].int_val > 65535) {
        LOG_ERR("MQTTCONN: invalid port %d (must be 1..65535)", params[2].int_val);
        return -EINVAL;
    }
    uint16_t port = (uint16_t)params[2].int_val;
    const char *client_id = params[3].str_val;
    const char *username = params[4].str_val;
    const char *password = params[5].str_val;
    if (params[6].int_valid && (params[6].int_val < 1 || params[6].int_val > 65535)) {
        LOG_ERR("MQTTCONN: invalid keepalive %d", params[6].int_val);
        return -EINVAL;
    }
    uint16_t keepalive = params[6].int_valid ? (uint16_t)params[6].int_val : MQTTC_AT_DEFAULT_KEEPALIVE;
    bool clean = params[7].int_valid ? (params[7].int_val != 0) : false;

    return mqttc_at_core_connect(sid, host, port, client_id, username, password, keepalive, clean);
}

/* -------------------------------------------------------------------------
 * AT+MQTTSUB
 * ---------------------------------------------------------------------- */

int mqttc_at_handle_mqttsub(uint32_t op_type, uint32_t param_count, mqttc_at_param_t *params)
{
    if (op_type == MQTTC_AT_OP_EXEC) {
        mqttc_output("+MQTTSUB=<session_id>,\"topic\",<qos>\r\n");
        return 0;
    }

    if (op_type == MQTTC_AT_OP_QUERY) {
        char buf[512];

        for (int i = 0; i < CONFIG_MQTTC_AT_MAX_SESSIONS; i++) {
            mqttc_at_core_query_sub(i, buf, sizeof(buf));
            mqttc_output(buf);
        }
        return 0;
    }

    if (op_type != MQTTC_AT_OP_EXEC_W_PARAM) {
        return -EINVAL;
    }

    /* AT+MQTTSUB=<sid>,"topic",<qos> */
    if (param_count < 3) {
        LOG_ERR("MQTTSUB: expected 3 params, got %u", param_count);
        return -EINVAL;
    }

    if (!params[0].int_valid) {
        return -EINVAL;
    }
    int sid = params[0].int_val;
    const char *topic = params[1].str_val;

    if (params[2].int_valid && (params[2].int_val < 0 || params[2].int_val > 2)) {
        LOG_ERR("MQTTSUB: invalid QoS %d (must be 0..2)", params[2].int_val);
        return -EINVAL;
    }
    uint8_t qos = params[2].int_valid ? (uint8_t)params[2].int_val : 0;

    return mqttc_at_core_subscribe(sid, topic, qos);
}

/* -------------------------------------------------------------------------
 * AT+MQTTPUB
 * ---------------------------------------------------------------------- */

int mqttc_at_handle_mqttpub(uint32_t op_type, uint32_t param_count, mqttc_at_param_t *params)
{
    if (op_type == MQTTC_AT_OP_EXEC) {
        mqttc_output("+MQTTPUB=<session_id>,\"topic\",<qos>,\"message\",<retain>\r\n");
        return 0;
    }

    if (op_type != MQTTC_AT_OP_EXEC_W_PARAM) {
        return -EINVAL;
    }

    /* AT+MQTTPUB=<sid>,"topic",<qos>,"message",<retain> */
    if (param_count < 5) {
        LOG_ERR("MQTTPUB: expected 5 params, got %u", param_count);
        return -EINVAL;
    }

    if (!params[0].int_valid) {
        return -EINVAL;
    }
    int sid = params[0].int_val;
    const char *topic = params[1].str_val;
    const char *message = params[3].str_val;

    if (params[2].int_valid && (params[2].int_val < 0 || params[2].int_val > 2)) {
        LOG_ERR("MQTTPUB: invalid QoS %d (must be 0..2)", params[2].int_val);
        return -EINVAL;
    }
    if (params[4].int_valid && (params[4].int_val < 0 || params[4].int_val > 1)) {
        LOG_ERR("MQTTPUB: invalid retain %d (must be 0..1)", params[4].int_val);
        return -EINVAL;
    }
    uint8_t qos = params[2].int_valid ? (uint8_t)params[2].int_val : 0;
    uint8_t retain = params[4].int_valid ? (uint8_t)params[4].int_val : 0;

    return mqttc_at_core_publish(sid, topic, qos, (const uint8_t *)message, strlen(message), retain);
}

/* -------------------------------------------------------------------------
 * AT+MQTTPUBRAW
 * ---------------------------------------------------------------------- */

int mqttc_at_handle_mqttpubraw(uint32_t op_type, uint32_t param_count, mqttc_at_param_t *params)
{
    if (op_type == MQTTC_AT_OP_EXEC) {
        mqttc_output("+MQTTPUBRAW=<session_id>,\"topic\",<length>,<qos>,<retain>\r\n");
        return 0;
    }

    if (op_type != MQTTC_AT_OP_EXEC_W_PARAM) {
        return -EINVAL;
    }

    /* AT+MQTTPUBRAW=<sid>,"topic",<length>,<qos>,<retain> */
    if (param_count < 5) {
        LOG_ERR("MQTTPUBRAW: expected 5 params, got %u", param_count);
        return -EINVAL;
    }

    if (!params[0].int_valid || !params[2].int_valid) {
        return -EINVAL;
    }
    int sid = params[0].int_val;
    const char *topic = params[1].str_val;
    if (params[2].int_val < 0) {
        LOG_ERR("MQTTPUBRAW: invalid length %d", params[2].int_val);
        return -EINVAL;
    }
    size_t length = (size_t)params[2].int_val;

    if (params[3].int_valid && (params[3].int_val < 0 || params[3].int_val > 2)) {
        LOG_ERR("MQTTPUBRAW: invalid QoS %d (must be 0..2)", params[3].int_val);
        return -EINVAL;
    }
    if (params[4].int_valid && (params[4].int_val < 0 || params[4].int_val > 1)) {
        LOG_ERR("MQTTPUBRAW: invalid retain %d (must be 0..1)", params[4].int_val);
        return -EINVAL;
    }
    uint8_t qos = params[3].int_valid ? (uint8_t)params[3].int_val : 0;
    uint8_t retain = params[4].int_valid ? (uint8_t)params[4].int_val : 0;

    int rc = mqttc_at_core_start_pubraw(sid, topic, length, qos, retain);
    if (rc < 0) {
        LOG_ERR("MQTTPUBRAW: start_pubraw failed: %d", rc);
        return rc;
    }

    /* Signal to QAT shim that data mode should be entered */
    mqttc_output(">\r\n");
    return 0;
}

/* -------------------------------------------------------------------------
 * AT+MQTTUNSUB
 * ---------------------------------------------------------------------- */

int mqttc_at_handle_mqttunsub(uint32_t op_type, uint32_t param_count, mqttc_at_param_t *params)
{
    if (op_type == MQTTC_AT_OP_EXEC) {
        mqttc_output("+MQTTUNSUB=<session_id>,\"topic\"\r\n");
        return 0;
    }

    if (op_type != MQTTC_AT_OP_EXEC_W_PARAM) {
        return -EINVAL;
    }

    /* AT+MQTTUNSUB=<sid>,"topic" */
    if (param_count < 2) {
        LOG_ERR("MQTTUNSUB: expected 2 params, got %u", param_count);
        return -EINVAL;
    }

    if (!params[0].int_valid) {
        return -EINVAL;
    }
    int sid = params[0].int_val;
    const char *topic = params[1].str_val;

    return mqttc_at_core_unsubscribe(sid, topic);
}

/* -------------------------------------------------------------------------
 * AT+MQTTDISCONN
 * ---------------------------------------------------------------------- */

int mqttc_at_handle_mqttdisconn(uint32_t op_type, uint32_t param_count, mqttc_at_param_t *params)
{
    if (op_type == MQTTC_AT_OP_EXEC) {
        mqttc_output("+MQTTDISCONN=<session_id>\r\n");
        return 0;
    }

    if (op_type != MQTTC_AT_OP_EXEC_W_PARAM) {
        return -EINVAL;
    }

    if (param_count < 1 || !params[0].int_valid) {
        return -EINVAL;
    }

    return mqttc_at_core_disconnect(params[0].int_val);
}

/* -------------------------------------------------------------------------
 * AT+MQTTDESTROY
 * ---------------------------------------------------------------------- */

int mqttc_at_handle_mqttdestroy(uint32_t op_type, uint32_t param_count, mqttc_at_param_t *params)
{
    if (op_type == MQTTC_AT_OP_EXEC) {
        mqttc_output("+MQTTDESTROY=<session_id>\r\n");
        return 0;
    }

    if (op_type != MQTTC_AT_OP_EXEC_W_PARAM) {
        return -EINVAL;
    }

    if (param_count < 1 || !params[0].int_valid) {
        return -EINVAL;
    }

    return mqttc_at_core_destroy(params[0].int_val);
}

/* -------------------------------------------------------------------------
 * AT+MQTTMODE
 * ---------------------------------------------------------------------- */

int mqttc_at_handle_mqttmode(uint32_t op_type, uint32_t param_count, mqttc_at_param_t *params)
{
    if (op_type == MQTTC_AT_OP_EXEC) {
        mqttc_output("+MQTTMODE=<0|1: string|hex>\r\n");
        return 0;
    }

    if (op_type != MQTTC_AT_OP_EXEC_W_PARAM) {
        return -EINVAL;
    }

    if (param_count < 1 || !params[0].int_valid) {
        return -EINVAL;
    }

    int mode = params[0].int_val;
    if (mode != 0 && mode != 1) {
        return -EINVAL;
    }

    mqttc_at_core_set_recv_mode(mode);
    return 0;
}
