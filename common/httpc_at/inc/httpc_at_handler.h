/*
 * Copyright (c) 2026 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/**
 * @file httpc_at_handler.h
 * @brief Public API for the HTTP/HTTPS AT command handler module.
 *
 * This module handles the following AT commands:
 *   AT+HTTPCLIENT  - Send HTTP client request (HEAD/GET/POST/PUT)
 *   AT+HTTPGETSIZE - Get HTTP resource size via HEAD
 *   AT+HTTPGET     - Get HTTP resource via GET
 *   AT+HTTPPOST    - POST data to HTTP server (data mode)
 *   AT+HTTPPUT     - PUT data to HTTP server (data mode)
 *   AT+HTTPURLCFG  - Set/query long HTTP URL (data mode)
 *   AT+HTTPSSLCFG  - Set/query TLS certificate configuration
 *   AT+HTTPNETCFG  - Set/query network preferences
 *
 * AT command parsing is handled by the AT infrastructure layer.
 * This module only handles the command execution logic.
 *
 * Integration:
 *   1. Call httpc_at_init() once at startup with an output callback.
 *   2. Route parsed AT commands to the appropriate httpc_at_handle_*() function.
 *   3. Before parsing each received line, check httpc_at_is_in_data_mode().
 *      If true, route raw bytes to httpc_at_data_mode_input() instead.
 */

#ifndef HTTPC_AT_HANDLER_H
#define HTTPC_AT_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <zephyr/net/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

/*-------------------------------------------------------------------------
 * Constants
 *-----------------------------------------------------------------------*/

#define HTTPC_AT_MAX_URL_LEN           256
#define HTTPC_AT_MAX_HOST_LEN          64
#define HTTPC_AT_MAX_PATH_LEN          256
#define HTTPC_AT_MAX_HEADER_FIELDS     10
#define HTTPC_AT_MAX_FILE_PATH_LEN     32
#define HTTPC_AT_RECV_BUF_SIZE         2048
#define HTTPC_AT_SEND_BUF_SIZE         1000  /* max POST/PUT body in cache mode (data_cache=1) */
#define HTTPC_AT_MAX_STREAM_SIZE       65536 /* max POST/PUT body in stream mode (data_cache=0) */
#define HTTPC_AT_CHUNK_SIZE            1000
#define HTTPC_AT_DEFAULT_TIMEOUT_MS    10000
#define HTTPC_AT_DEFAULT_GET_TIMEOUT_MS 5000
#define HTTPC_AT_MAX_TIMEOUT_MS        1800000
#define HTTPC_AT_DEFAULT_HTTP_PORT     80
#define HTTPC_AT_DEFAULT_HTTPS_PORT    443

/*-------------------------------------------------------------------------
 * AT Command Operation Types
 * (Must match the AT infrastructure layer's operation type values)
 *-----------------------------------------------------------------------*/

#define HTTPC_AT_OP_EXEC           0x01  /**< AT+CMD (no params, template query) */
#define HTTPC_AT_OP_EXEC_W_PARAM   0x02  /**< AT+CMD=<params> */
#define HTTPC_AT_OP_QUERY          0x04  /**< AT+CMD? */
#define HTTPC_AT_OP_DATA_MODE      0x08  /**< Data mode input (internal use) */

/*-------------------------------------------------------------------------
 * Enumerations
 *-----------------------------------------------------------------------*/

/**
 * @brief SSL/TLS authentication scheme.
 * Matches AT+HTTPSSLCFG <scheme> parameter.
 */
typedef enum {
    HTTPC_AT_AUTH_NONE   = 0, /**< HTTPS, no certificate verification */
    HTTPC_AT_AUTH_SERVER = 1, /**< Verify server certificate (CA required) */
    HTTPC_AT_AUTH_CLIENT = 2, /**< Provide client certificate (cert+key required) */
    HTTPC_AT_AUTH_MUTUAL = 3, /**< Verify server + provide client cert */
} httpc_at_auth_type_t;

/**
 * @brief HTTP method.
 * Matches AT+HTTPCLIENT <opt> parameter.
 */
typedef enum {
    HTTPC_AT_METHOD_HEAD = 1,
    HTTPC_AT_METHOD_GET  = 2,
    HTTPC_AT_METHOD_POST = 3,
    HTTPC_AT_METHOD_PUT  = 4,
} httpc_at_method_t;

/**
 * @brief HTTP Content-Type.
 * Matches AT+HTTPCLIENT <content-type> and AT+HTTPPUT <content_type> parameters.
 */
typedef enum {
    HTTPC_AT_CTYPE_FORM_URLENCODED = 0, /**< application/x-www-form-urlencoded */
    HTTPC_AT_CTYPE_JSON            = 1, /**< application/json */
    HTTPC_AT_CTYPE_ZIP             = 2, /**< application/zip */
    HTTPC_AT_CTYPE_FORM_DATA       = 3, /**< multipart/form-data */
    HTTPC_AT_CTYPE_TEXT_XML        = 4, /**< text/xml */
} httpc_at_content_type_t;

/**
 * @brief HTTP net configuration type.
 * Matches AT+HTTPNETCFG <netcfg_type> parameter.
 */
typedef enum {
    HTTPC_AT_NETCFG_HTTP_PORT = 0,
    HTTPC_AT_NETCFG_HTTPS_PORT = 1,
    HTTPC_AT_NETCFG_IP_PREFER = 2,
    HTTPC_AT_NETCFG_PREALLOC_SSL_BUF = 3,
    HTTPC_AT_NETCFG_DATA_CACHE = 4,
} httpc_at_netcfg_type_t;

/**
 * @brief Data mode command context.
 * Identifies which AT command is currently collecting data.
 */
typedef enum {
    HTTPC_AT_DATA_MODE_NONE   = 0,
    HTTPC_AT_DATA_MODE_POST   = 1,
    HTTPC_AT_DATA_MODE_PUT    = 2,
    HTTPC_AT_DATA_MODE_URLCFG = 3,
} httpc_at_data_mode_cmd_t;

/*-------------------------------------------------------------------------
 * Data Structures
 *-----------------------------------------------------------------------*/

/**
 * @brief AT command parameter descriptor.
 *
 * The AT infrastructure layer parses each comma-separated parameter and
 * fills this structure. The handler uses str_val for string parameters
 * and int_val/int_valid for integer parameters.
 */
typedef struct {
    const char *str_val;   /**< String value (always valid, may be empty "") */
    int         int_val;   /**< Integer value (valid only if int_valid == true) */
    bool        int_valid; /**< True if the parameter is a valid integer */
} httpc_at_param_t;

/**
 * @brief HTTP header field key-value pair.
 */
struct httpc_at_header_field {
    char *name;   /**< Header field name (heap-allocated) */
    char *value;  /**< Header field value (heap-allocated) */
};

/**
 * @brief Response output callback.
 *
 * Called by the handler to write AT response strings to the host UART.
 * This replaces QAT_Response_Str() from the FreeRTOS implementation.
 *
 * @param data      Response string (NUL-terminated)
 * @param len       Length of data (excluding NUL terminator)
 * @param user_data Opaque pointer provided at httpc_at_init()
 */
typedef void (*httpc_at_output_cb_t)(const char *data, size_t len, void *user_data);

/**
 * @brief Global configuration and state for the HTTPC AT handler.
 *
 * This structure holds all persistent configuration (SSL, ports, stored URL)
 * and per-command temporary state (data mode buffers, header fields).
 */
struct httpc_at_global_cfg {
    /* --- Persistent SSL/TLS configuration (set via AT+HTTPSSLCFG) --- */
    httpc_at_auth_type_t https_auth_type;
    char ca_file[HTTPC_AT_MAX_FILE_PATH_LEN];
    char cert_file[HTTPC_AT_MAX_FILE_PATH_LEN];
    char key_file[HTTPC_AT_MAX_FILE_PATH_LEN];

    /* --- Persistent network configuration --- */
    uint16_t    http_port;   /**< Default HTTP port (default: 80) */
    uint16_t    https_port;  /**< Default HTTPS port (default: 443) */
    sa_family_t ip_family;   /**< AF_INET (IPv4, default) or AF_INET6 */
    uint8_t     prealloc_ssl_buf; /**< 0: default, 1: pre-allocate, 2: release */
    uint8_t     data_cache;       /**< 0: disable, 1: enable */
    bool        tls_creds_loaded; /**< Internal: TLS credentials currently preloaded */

    /* --- Persistent URL configuration (set via AT+HTTPURLCFG) --- */
    char  *url;      /**< Stored URL (heap-allocated, NULL if not set) */
    size_t url_len;  /**< Length of stored URL */

    /* --- Per-command temporary resources (freed after each command) --- */
    char  *temp_url;          /**< URL for current operation (heap-allocated) */
    char  *send_buf;          /**< Data buffer for POST/PUT body (heap-allocated) */
    size_t send_buf_offset;   /**< Bytes accumulated in send_buf so far */
    size_t data_len;          /**< Expected total data length for current command */

    /* --- Per-command HTTP header fields --- */
    struct httpc_at_header_field header_fields[HTTPC_AT_MAX_HEADER_FIELDS];
    uint8_t header_field_num;

    /* --- Data mode state --- */
    httpc_at_data_mode_cmd_t data_mode_cmd; /**< Which command is collecting data */
    bool in_data_mode;                       /**< True when collecting raw data */
    bool force_stream;                       /**< True: per-command override forcing stream mode
                                                  (set when data_cache==1 but body > HTTPC_AT_SEND_BUF_SIZE) */

    /* --- Response output --- */
    httpc_at_output_cb_t output_cb;    /**< Callback to write AT responses */
    void                *output_user_data;
};

/*-------------------------------------------------------------------------
 * Module Lifecycle
 *-----------------------------------------------------------------------*/

/**
 * @brief Initialize the HTTPC AT handler module.
 *
 * Must be called once at startup before any AT commands are processed.
 * Allocates receive and send buffers, sets default configuration values.
 *
 * @param output_cb   Callback to write AT response strings to host UART.
 * @param user_data   Opaque pointer passed to output_cb on every call.
 * @return 0 on success, negative errno on failure.
 */
int httpc_at_init(httpc_at_output_cb_t output_cb, void *user_data);

/**
 * @brief Deinitialize the HTTPC AT handler module.
 *
 * Frees all allocated resources. After this call, httpc_at_init() must
 * be called again before using any other functions.
 */
void httpc_at_deinit(void);

/*-------------------------------------------------------------------------
 * AT Command Handler Functions
 * (Called by the AT infrastructure layer after parameter parsing)
 *-----------------------------------------------------------------------*/

/**
 * @brief Handle AT+HTTPCLIENT command.
 *
 * Sends an HTTP request using the specified method.
 *
 * Input (HTTPC_AT_OP_EXEC_W_PARAM):
 *   params[0]: <opt>          - integer: 1=HEAD, 2=GET, 3=POST, 4=PUT
 *   params[1]: <content-type> - integer: 0-4
 *   params[2]: <url>          - string (empty = use stored URL)
 *   params[3]: <data>         - string (POST/PUT only, optional)
 *   params[4+]: <headers>     - strings (optional custom header fields)
 *
 * @param op_type     Operation type (HTTPC_AT_OP_EXEC or HTTPC_AT_OP_EXEC_W_PARAM)
 * @param param_count Number of parameters
 * @param params      Parameter array
 * @return 0 on success, negative errno on failure
 */
int httpc_at_handle_httpclient(uint32_t op_type,
                               uint32_t param_count,
                               httpc_at_param_t *params);

/**
 * @brief Handle AT+HTTPGETSIZE command.
 *
 * Gets HTTP resource size using HEAD method.
 *
 * Input (HTTPC_AT_OP_EXEC_W_PARAM):
 *   params[0]: <url>     - string (empty = use stored URL)
 *   params[1]: <timeout> - integer ms (optional, default 5000)
 *
 * @param op_type     Operation type
 * @param param_count Number of parameters
 * @param params      Parameter array
 * @return 0 on success, negative errno on failure
 */
int httpc_at_handle_httpgetsize(uint32_t op_type,
                                uint32_t param_count,
                                httpc_at_param_t *params);

/**
 * @brief Handle AT+HTTPGET command.
 *
 * Gets HTTP resource using GET method.
 *
 * Input (HTTPC_AT_OP_EXEC_W_PARAM):
 *   params[0]: <url>     - string (empty = use stored URL)
 *   params[1]: <timeout> - integer ms (optional, default 5000)
 *
 * @param op_type     Operation type
 * @param param_count Number of parameters
 * @param params      Parameter array
 * @return 0 on success, negative errno on failure
 */
int httpc_at_handle_httpget(uint32_t op_type,
                            uint32_t param_count,
                            httpc_at_param_t *params);

/**
 * @brief Handle AT+HTTPPOST command.
 *
 * Posts data to HTTP server. Uses two-phase data mode protocol:
 * Phase 1: Validate params, allocate buffer, enter data mode, output "OK\r\n>\r\n"
 * Phase 2: Data received via httpc_at_data_mode_input(), execute POST when complete
 *
 * Input (HTTPC_AT_OP_EXEC_W_PARAM):
 *   params[0]: <url>     - string
 *   params[1]: <length>  - integer (data length in bytes, max 1000)
 *   params[2+]: <headers> - strings (optional custom header fields)
 *
 * @param op_type     Operation type
 * @param param_count Number of parameters
 * @param params      Parameter array
 * @return 0 on success, negative errno on failure
 */
int httpc_at_handle_httppost(uint32_t op_type,
                             uint32_t param_count,
                             httpc_at_param_t *params);

/**
 * @brief Handle AT+HTTPPUT command.
 *
 * Puts data to HTTP server. Uses two-phase data mode protocol.
 *
 * Input (HTTPC_AT_OP_EXEC_W_PARAM):
 *   params[0]: <url>          - string
 *   params[1]: <content_type> - integer: 0-4
 *   params[2]: <length>       - integer (data length in bytes, max 1000)
 *   params[3+]: <headers>     - strings (optional custom header fields)
 *
 * @param op_type     Operation type
 * @param param_count Number of parameters
 * @param params      Parameter array
 * @return 0 on success, negative errno on failure
 */
int httpc_at_handle_httpput(uint32_t op_type,
                            uint32_t param_count,
                            httpc_at_param_t *params);

/**
 * @brief Handle AT+HTTPURLCFG command.
 *
 * Set/query long HTTP URL configuration.
 *
 * Input (HTTPC_AT_OP_EXEC_W_PARAM):
 *   params[0]: <url_length> - integer
 *     0:       Clear stored URL
 *     [8,256]: Enter data mode to receive URL
 *
 * Input (HTTPC_AT_OP_QUERY):
 *   No parameters. Response: +HTTPURLCFG:<len>,<url>
 *
 * @param op_type     Operation type
 * @param param_count Number of parameters
 * @param params      Parameter array
 * @return 0 on success, negative errno on failure
 */
int httpc_at_handle_httpurlcfg(uint32_t op_type,
                               uint32_t param_count,
                               httpc_at_param_t *params);

/**
 * @brief Handle AT+HTTPSSLCFG command.
 *
 * Set/query TLS certificate configuration.
 *
 * Input (HTTPC_AT_OP_EXEC_W_PARAM):
 *   params[0]: <scheme>    - integer: 0=none, 1=server, 2=client, 3=mutual
 *   params[1]: <cert_file> - string (scheme 2,3 only)
 *   params[2]: <key_file>  - string (scheme 2,3 only)
 *   params[3]: <ca_file>   - string (scheme 1,3 only)
 *
 * Input (HTTPC_AT_OP_QUERY):
 *   No parameters. Response: +HTTPSSLCFG:<scheme>,"<cert>","<key>","<ca>"
 *
 * @param op_type     Operation type
 * @param param_count Number of parameters
 * @param params      Parameter array
 * @return 0 on success, negative errno on failure
 */
int httpc_at_handle_httpsslcfg(uint32_t op_type,
                               uint32_t param_count,
                               httpc_at_param_t *params);

/**
 * @brief Handle AT+HTTPNETCFG command.
 *
 * Set/query HTTP net configuration:
 *   - HTTP port
 *   - HTTPS port
 *   - Preferred IP version
 *   - pre-allocate SSL buffer option
 *   - data cache option
 *
 * Input (HTTPC_AT_OP_EXEC_W_PARAM):
 *   params[0]: <netcfg_type> - integer 0..4
 *   params[1]: <value>       - integer (range depends on netcfg_type)
 *
 * Input (HTTPC_AT_OP_QUERY):
 *   No parameters. Response:
 *   +HTTPNETCFG:<http_port>,<https_port>,<ip_prefer>,<prealloc_ssl_buf>,<data_cache>
 *
 * @param op_type     Operation type
 * @param param_count Number of parameters
 * @param params      Parameter array
 * @return 0 on success, negative errno on failure
 */
int httpc_at_handle_httpnetcfg(uint32_t op_type,
                               uint32_t param_count,
                               httpc_at_param_t *params);

/*-------------------------------------------------------------------------
 * Data Mode Interface
 * (Called by AT infrastructure when in_data_mode == true)
 *-----------------------------------------------------------------------*/

/**
 * @brief Feed raw bytes to the data mode engine.
 *
 * Called by the AT parser when httpc_at_is_in_data_mode() returns true.
 * Accumulates data in the send buffer. When the expected number of bytes
 * has been received, executes the pending HTTP request automatically.
 *
 * @param data  Raw bytes received from host UART
 * @param len   Number of bytes
 * @return 0 on success, negative errno on failure
 */
int httpc_at_data_mode_input(const uint8_t *data, size_t len);

/**
 * @brief Query whether the module is currently in data mode.
 *
 * The AT parser must call this before processing each received line.
 * If true, route raw bytes to httpc_at_data_mode_input() instead of
 * the normal AT command parser.
 *
 * @return true if in data mode, false otherwise
 */
bool httpc_at_is_in_data_mode(void);

/*-------------------------------------------------------------------------
 * Utility
 *-----------------------------------------------------------------------*/

/**
 * @brief Get pointer to the global configuration structure (read-only).
 *
 * Useful for the AT infrastructure to inspect current state.
 *
 * @return Pointer to global config (do not modify directly)
 */
const struct httpc_at_global_cfg *httpc_at_get_config(void);

#ifdef __cplusplus
}
#endif

#endif /* HTTPC_AT_HANDLER_H */
