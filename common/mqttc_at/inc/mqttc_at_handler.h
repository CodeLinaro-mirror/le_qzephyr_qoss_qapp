/*
 * Copyright (c) 2026 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/**
 * @file mqttc_at_handler.h
 * @brief Public API for the MQTT client AT command handler module.
 *
 * This module handles the following AT commands (80-Y8730-10 rev AH):
 *   AT+MQTTINIT    - Initialize MQTT session
 *   AT+MQTTCONN    - Connect to MQTT broker
 *   AT+MQTTSUB     - Subscribe to topic
 *   AT+MQTTPUB     - Publish string message
 *   AT+MQTTPUBRAW  - Publish binary message (data mode)
 *   AT+MQTTUNSUB   - Unsubscribe from topic
 *   AT+MQTTDISCONN - Disconnect from broker
 *   AT+MQTTDESTROY - Destroy session
 *   AT+MQTTMODE    - Set receive mode (string/hex)
 *
 * Integration:
 *   1. Call mqttc_at_init() once at startup with an output callback.
 *   2. Route parsed AT commands to the appropriate mqttc_at_handle_*() function.
 *   3. For MQTTPUBRAW data mode, check mqttc_at_is_in_data_mode() and route
 *      raw bytes to mqttc_at_data_mode_input() instead.
 */

#ifndef MQTTC_AT_HANDLER_H
#define MQTTC_AT_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*-------------------------------------------------------------------------
 * Constants
 *-----------------------------------------------------------------------*/

#define MQTTC_AT_MAX_HOST_LEN 128
#define MQTTC_AT_MAX_CLIENT_ID_LEN 64
#define MQTTC_AT_MAX_USERNAME_LEN 64
#define MQTTC_AT_MAX_PASSWORD_LEN 64
#define MQTTC_AT_MAX_TOPIC_LEN 128
#define MQTTC_AT_MAX_PAYLOAD_LEN     1000
#define MQTTC_AT_MAX_PUBRAW_LEN      65535
#define MQTTC_AT_DEFAULT_PORT 1883
#define MQTTC_AT_DEFAULT_KEEPALIVE 60

/*-------------------------------------------------------------------------
 * AT Command Operation Types
 *-----------------------------------------------------------------------*/

#define MQTTC_AT_OP_EXEC 0x01         /**< AT+CMD (no params, template query) */
#define MQTTC_AT_OP_EXEC_W_PARAM 0x02 /**< AT+CMD=<params> */
#define MQTTC_AT_OP_QUERY 0x04        /**< AT+CMD? */

/*-------------------------------------------------------------------------
 * Types
 *-----------------------------------------------------------------------*/

/**
 * @brief Parsed AT command parameter.
 */
typedef struct {
    const char *str_val; /**< String value (always valid, may be empty) */
    int int_val;         /**< Integer value (valid only if int_valid == true) */
    bool int_valid;      /**< True if parameter is a valid integer */
} mqttc_at_param_t;

/**
 * @brief Output callback type — sends data back to the AT host.
 *
 * @param data      Pointer to data to send.
 * @param len       Length of data.
 * @param user_data Opaque pointer passed to mqttc_at_init().
 */
typedef void (*mqttc_at_output_cb_t)(const char *data, size_t len, void *user_data);

/*-------------------------------------------------------------------------
 * Lifecycle
 *-----------------------------------------------------------------------*/

/**
 * @brief Initialize the MQTT AT handler.
 *
 * Must be called once before any mqttc_at_handle_*() function.
 *
 * @param output_cb Callback used to send AT responses to the host.
 * @param user_data Opaque pointer forwarded to output_cb.
 * @return 0 on success, negative errno on failure.
 */
int mqttc_at_init(mqttc_at_output_cb_t output_cb, void *user_data);

/*-------------------------------------------------------------------------
 * Data mode
 *-----------------------------------------------------------------------*/

/**
 * @brief Check whether the handler is currently accumulating PUBRAW data.
 *
 * @return true if in data mode, false otherwise.
 */
bool mqttc_at_is_in_data_mode(void);

/**
 * @brief Cancel an active PUBRAW data-mode session without publishing.
 *
 * Called when entering online data mode fails after start_pubraw activated.
 */
void mqttc_at_cancel_data_mode(void);

/**
 * @brief Feed raw bytes into the PUBRAW data accumulation buffer.
 *
 * Called by the QAT shim's online-data-mode callback.
 *
 * @param data Pointer to received bytes.
 * @param len  Number of bytes.
 * @return Number of bytes consumed on success, negative errno on failure.
 */
int mqttc_at_data_mode_input(const uint8_t *data, size_t len);

/*-------------------------------------------------------------------------
 * Command handlers
 *-----------------------------------------------------------------------*/

int mqttc_at_handle_mqttinit(uint32_t op_type, uint32_t param_count, mqttc_at_param_t *params);
int mqttc_at_handle_mqttconn(uint32_t op_type, uint32_t param_count, mqttc_at_param_t *params);
int mqttc_at_handle_mqttsub(uint32_t op_type, uint32_t param_count, mqttc_at_param_t *params);
int mqttc_at_handle_mqttpub(uint32_t op_type, uint32_t param_count, mqttc_at_param_t *params);
int mqttc_at_handle_mqttpubraw(uint32_t op_type, uint32_t param_count, mqttc_at_param_t *params);
int mqttc_at_handle_mqttunsub(uint32_t op_type, uint32_t param_count, mqttc_at_param_t *params);
int mqttc_at_handle_mqttdisconn(uint32_t op_type, uint32_t param_count, mqttc_at_param_t *params);
int mqttc_at_handle_mqttdestroy(uint32_t op_type, uint32_t param_count, mqttc_at_param_t *params);
int mqttc_at_handle_mqttmode(uint32_t op_type, uint32_t param_count, mqttc_at_param_t *params);

#ifdef __cplusplus
}
#endif

#endif /* MQTTC_AT_HANDLER_H */
