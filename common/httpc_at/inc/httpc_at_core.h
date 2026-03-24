/*
 * Copyright (c) 2026 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/**
 * @file httpc_at_core.h
 * @brief Internal HTTP/HTTPS execution engine for the AT command handler.
 *
 * Provides a synchronous, blocking HTTP/HTTPS request function using
 * Zephyr's native http_client_req() API (CONFIG_HTTP_CLIENT). Socket
 * creation, TLS setup, and DNS+connect use the zsock_*() API directly.
 *
 * TLS is handled via IPPROTO_TLS_1_2 socket type and Zephyr's
 * tls_credentials subsystem. Certificates must be registered before
 * calling httpc_at_execute() (see httpc_at_ssl.h).
 */

#ifndef HTTPC_AT_CORE_H
#define HTTPC_AT_CORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "httpc_at_handler.h"

#ifdef __cplusplus
extern "C" {
#endif

/*-------------------------------------------------------------------------
 * TLS sec_tag_t assignments for this module
 *-----------------------------------------------------------------------*/

/** sec_tag for CA certificate (used for server verification) */
#define HTTPC_AT_TLS_TAG_CA_CERT      10

/** sec_tag for client certificate (used for client authentication) */
#define HTTPC_AT_TLS_TAG_CLIENT_CERT  11

/** sec_tag for client private key (used for client authentication) */
#define HTTPC_AT_TLS_TAG_CLIENT_KEY   12

/*-------------------------------------------------------------------------
 * Data Structures
 *-----------------------------------------------------------------------*/

/**
 * @brief HTTP request descriptor.
 *
 * Passed to httpc_at_execute() to describe the request to perform.
 */
struct httpc_at_request {
    const char    *url;          /**< Full URL (http:// or https://) */
    httpc_at_method_t method;    /**< HTTP method */
    const uint8_t *body;         /**< Request body (POST/PUT only, may be NULL) */
    size_t         body_len;     /**< Length of body in bytes */
    int32_t        timeout_ms;   /**< Timeout in ms (0 = use module default) */

    /**
     * @brief Extra HTTP header fields to include in the request.
     *
     * Each entry is a complete header line string, e.g.:
     *   "Content-Type: application/json"
     *   "Authorization: Bearer <token>"
     *
     * The string should NOT include the trailing CRLF; the engine adds it.
     * Array must have extra_header_count valid entries.
     */
    const char **extra_headers;
    uint8_t      extra_header_count;

    /**
     * @brief SSL/TLS authentication type.
     * Determines which sec_tags are applied to the TLS socket.
     */
    httpc_at_auth_type_t auth_type;

    /**
     * @brief Network preferences from AT+HTTPNETCFG.
     *
     * - http_port / https_port are used as default ports when URL does not
     *   provide an explicit :port.
     * - ip_family controls DNS/socket family (AF_INET or AF_INET6).
     */
    uint16_t    http_port;
    uint16_t    https_port;
    sa_family_t ip_family;
};

/**
 * @brief HTTP response data.
 *
 * Filled by httpc_at_execute() upon completion.
 */
struct httpc_at_response {
    int    status_code;      /**< HTTP status code (e.g., 200, 404) */
    size_t content_length;   /**< Content-Length from response headers (0 if absent) */
    size_t received_bytes;   /**< Total body bytes received */
    bool   complete;         /**< True if response was fully received */
};

/**
 * @brief Streaming transfer context for HTTP POST/PUT no-cache mode.
 *
 * Lifetime:
 * - zero-initialize before use
 * - httpc_at_stream_begin()
 * - one or more httpc_at_stream_send()
 * - httpc_at_stream_finish() or httpc_at_stream_abort()
 */
struct httpc_at_stream_ctx {
    int              sock;
    bool             active;
    httpc_at_method_t method;
    size_t           content_length;
    size_t           sent_bytes;
    int32_t          timeout_ms;
};

/*-------------------------------------------------------------------------
 * Core Engine API
 *-----------------------------------------------------------------------*/

/**
 * @brief Execute a synchronous HTTP/HTTPS request.
 *
 * This function:
 *   1. Parses the URL to extract host, path, and port.
 *   2. Creates a TCP or TLS socket (based on URL scheme).
 *   3. Applies socket send/receive timeouts.
 *   4. Resolves the hostname via getaddrinfo().
 *   5. Connects to the server.
 *   6. Builds and sends the HTTP/1.1 request.
 *   7. Receives the response, streaming body data via output_cb.
 *   8. Closes the socket.
 *
 * The function blocks until the response is complete or a timeout occurs.
 *
 * Response body is streamed to the caller via output_cb as data arrives.
 * The caller is responsible for formatting the final AT response string
 * (e.g., "+HTTPGET:<size>\r\nOK\r\n") after this function returns.
 *
 * @param req             Request descriptor (must not be NULL)
 * @param resp            Response data (may be NULL if not needed)
 * @param output_cb       Callback to stream response body to host UART
 * @param output_user_data Opaque pointer passed to output_cb
 * @return 0 on success, negative errno on failure
 *         -EINVAL  : invalid URL or parameters
 *         -ENOMEM  : buffer allocation failure
 *         -EHOSTUNREACH : DNS resolution failed
 *         -ECONNREFUSED : connection refused
 *         -ETIMEDOUT : operation timed out
 */
int httpc_at_execute(const struct httpc_at_request *req,
                     struct httpc_at_response *resp,
                     httpc_at_output_cb_t output_cb,
                     void *output_user_data);

/**
 * @brief Start a streaming HTTP POST/PUT request (headers sent, body pending).
 *
 * @param req   Request descriptor. method must be POST or PUT.
 * @param ctx   Stream context (output).
 * @return 0 on success, negative errno on failure.
 */
int httpc_at_stream_begin(const struct httpc_at_request *req,
                          struct httpc_at_stream_ctx *ctx);

/**
 * @brief Send one payload fragment for an active streaming request.
 *
 * @param ctx   Active stream context.
 * @param data  Fragment bytes (may be NULL when len==0).
 * @param len   Fragment length in bytes.
 * @return 0 on success, negative errno on failure.
 */
int httpc_at_stream_send(struct httpc_at_stream_ctx *ctx,
                         const uint8_t *data,
                         size_t len);

/**
 * @brief Finalize streaming request and receive/parse HTTP response.
 *
 * @param ctx              Active stream context.
 * @param resp             Output response info.
 * @param output_cb        Optional response-body output callback.
 * @param output_user_data Opaque callback user data.
 * @return 0 on success, negative errno on failure.
 */
int httpc_at_stream_finish(struct httpc_at_stream_ctx *ctx,
                           struct httpc_at_response *resp,
                           httpc_at_output_cb_t output_cb,
                           void *output_user_data);

/**
 * @brief Abort a streaming request and close resources.
 *
 * Safe to call on an inactive context.
 */
void httpc_at_stream_abort(struct httpc_at_stream_ctx *ctx);

/**
 * @brief Initialize the core engine.
 *
 * Allocates the shared receive buffer used by httpc_at_execute().
 * Called internally by httpc_at_init().
 *
 * @return 0 on success, negative errno on failure
 */
int httpc_at_core_init(void);

/**
 * @brief Deinitialize the core engine.
 *
 * Frees the shared receive buffer.
 * Called internally by httpc_at_deinit().
 */
void httpc_at_core_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* HTTPC_AT_CORE_H */
