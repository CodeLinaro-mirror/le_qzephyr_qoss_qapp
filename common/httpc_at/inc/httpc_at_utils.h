/*
 * Copyright (c) 2026 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/**
 * @file httpc_at_utils.h
 * @brief URL parsing, validation, and content-type utilities.
 */

#ifndef HTTPC_AT_UTILS_H
#define HTTPC_AT_UTILS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parse a URL to extract host, path, and port.
 *
 * Handles both http:// and https:// schemes.
 * If no port is specified in the URL, *port is set to 0 (caller uses default).
 *
 * Examples:
 *   "http://example.com/path"       → host="example.com", path="/path", port=0
 *   "https://example.com:8443/path" → host="example.com", path="/path", port=8443
 *   "http://192.168.1.1"            → host="192.168.1.1", path="/", port=0
 *
 * @param url        Full URL string (must not be NULL)
 * @param host       Buffer to store hostname (NUL-terminated)
 * @param host_size  Size of host buffer
 * @param path       Buffer to store path (NUL-terminated, includes leading '/')
 * @param path_size  Size of path buffer
 * @param port       Pointer to store port number (0 if not specified in URL)
 * @return 0 on success, -EINVAL on malformed URL, -ENOMEM if buffers too small
 */
int httpc_at_parse_url(const char *url,
                       char *host, size_t host_size,
                       char *path, size_t path_size,
                       uint16_t *port);

/**
 * @brief Validate URL format.
 *
 * Checks that the URL:
 *   - Starts with "http://" or "https://"
 *   - Has a non-empty hostname containing at least one '.' or ':'
 *
 * @param url  URL string to validate
 * @return true if valid, false otherwise
 */
bool httpc_at_validate_url(const char *url);

/**
 * @brief Check if a URL uses the HTTPS scheme.
 *
 * @param url  URL string
 * @return true if URL starts with "https://", false otherwise
 */
bool httpc_at_is_https(const char *url);

/**
 * @brief Get the Content-Type string for a given content type enum value.
 *
 * @param content_type  Content type enum value (httpc_at_content_type_t)
 * @return Pointer to static content type string, e.g. "application/json"
 *         Returns "application/x-www-form-urlencoded" for unknown values.
 */
const char *httpc_at_get_content_type_str(int content_type);

/**
 * @brief Strip trailing carriage return from a data buffer.
 *
 * When the AT host sends data in data mode, it may append a '\r' at the
 * end of each packet (if the packet is smaller than the AT buffer size).
 * This function returns the effective data length, excluding the trailing '\r'.
 *
 * Matches the get_valid_data_len() logic from the FreeRTOS implementation.
 *
 * @param data     Data buffer
 * @param len      Raw length of data
 * @param at_buf_len  AT command buffer length (e.g., 1400). If len == at_buf_len,
 *                    the data is a full packet and no stripping is done.
 * @return Effective data length (may be len-1 if trailing '\r' is stripped)
 */
size_t httpc_at_get_valid_data_len(const char *data, size_t len, size_t at_buf_len);

#ifdef __cplusplus
}
#endif

#endif /* HTTPC_AT_UTILS_H */
