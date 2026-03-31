/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>
#include <cat.h>
#include "qat_api.h"

LOG_MODULE_REGISTER(qat_tcpip, LOG_LEVEL_INF);

/* CIPSEND state */
static struct {
    size_t max_len;
    size_t received_len;
} cipsend_state = {
    .max_len = 0,
    .received_len = 0,
};

/**
 * Passthrough data callback
 * This is called when data is received in online data mode
 */
static int cipsend_data_callback(const uint8_t *data, size_t len)
{
    LOG_DBG("CIPSEND received %zu bytes (total: %zu/%zu)", len, cipsend_state.received_len + len,
            cipsend_state.max_len);

    /* Print received data for debugging */
    LOG_HEXDUMP_DBG(data, len, "RX Data:");

    cipsend_state.received_len += len;

    /* Check if max length reached (only if max_len > 0) */
    if (cipsend_state.max_len > 0 && cipsend_state.received_len >= cipsend_state.max_len) {
        LOG_DBG("Max length reached, exiting online data mode");

        /* Exit online data mode */
        QAT_Transfer_Mode_set(QAT_Transfer_Mode_AT_COMMAND_E, NULL);

        /* Send OK response */
        QAT_Response_Str(QAT_RC_QUIET, "OK\r\n");
    }

    return len;
}

/**
 * AT+CIPSEND - Enter online data mode
 */
static cat_return_state Extend_Command_CIPSEND_Exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK, "AT+CIPSEND: get usage of command\r\n"
                                       "AT+CIPSEND=<length>: enter online data mode\r\n"
                                       "  length > 0: exit after receiving <length> bytes\r\n"
                                       "  length = 0: exit with +++ command\r\n");
}

static cat_return_state Extend_Command_CIPSEND_Set(const struct cat_command *cmd, const uint8_t *data,
                                                   const size_t data_size, const size_t args_num)
{
    int len;
    int ret;

    /* Parse parameter: length */
    if (sscanf((char *)data, "%d", &len) != 1 || len < 0) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPSEND: Invalid parameter\r\n"
                                              "AT+CIPSEND=<length>: length must be >= 0\r\n");
    }

    /* Check if already in online data mode */
    if (QAT_Transfer_Mode_get() == QAT_Transfer_Mode_ONLINE_DATA_E) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPSEND: Already in online data mode\r\n");
    }

    /* Initialize state */
    cipsend_state.max_len = (size_t)len;
    cipsend_state.received_len = 0;

    /* Enter online data mode */
    ret = QAT_Transfer_Mode_set(QAT_Transfer_Mode_ONLINE_DATA_E, cipsend_data_callback);
    if (ret != 0) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPSEND: Failed to enter online data mode\r\n");
    }

    LOG_DBG("Entered online data mode: max_len=%zu", cipsend_state.max_len);

    if (len == 0) {
        return QAT_Response_Str(QAT_RC_OK, "+CIPSEND: Online data mode (exit with +++)\r\n");
    } else {
        char response[64];
        snprintf(response, sizeof(response), "+CIPSEND: Online data mode (max %d bytes)\r\n", len);
        return QAT_Response_Str(QAT_RC_OK, response);
    }
}

/*-------------------------------------------------------------------------
 * Command List
 *-----------------------------------------------------------------------*/

static struct cat_command qat_tcpip_cmds[] = {
    {
        .name = "+CIPSEND",
        .description = "Enter online data mode",
        .run = Extend_Command_CIPSEND_Exec,
        .write = Extend_Command_CIPSEND_Set,
    },
};

static struct cat_command_group qat_tcpip_cmd_group = {
    .name = "QAT_TCPIP",
    .cmd = qat_tcpip_cmds,
    .cmd_num = ARRAY_SIZE(qat_tcpip_cmds),
};

struct cat_command_group *qat_tcpip_get_command_group(void)
{
    LOG_DBG("Registering QAT TCP/IP commands");
    return &qat_tcpip_cmd_group;
}

/* Automatically register this command group with QAT */
QAT_REGISTER_CMD_GROUP(qat_tcpip_get_command_group, "TCPIP");
