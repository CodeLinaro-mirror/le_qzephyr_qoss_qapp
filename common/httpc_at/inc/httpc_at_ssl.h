/*
 * Copyright (c) 2026 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/**
 * @file httpc_at_ssl.h
 * @brief TLS credential management for the HTTPC AT handler.
 *
 * Loads PEM certificate files from the Zephyr filesystem and registers
 * them with the Zephyr TLS credentials subsystem (tls_credential_add).
 *
 * Fixed sec_tag_t assignments (defined in httpc_at_core.h):
 *   HTTPC_AT_TLS_TAG_CA_CERT     = 10  (CA certificate)
 *   HTTPC_AT_TLS_TAG_CLIENT_CERT = 11  (client certificate)
 *   HTTPC_AT_TLS_TAG_CLIENT_KEY  = 12  (client private key)
 */

#ifndef HTTPC_AT_SSL_H
#define HTTPC_AT_SSL_H

#include <stdint.h>
#include <stddef.h>
#include "httpc_at_handler.h"
#include "httpc_at_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Load TLS credentials from filesystem and register with Zephyr TLS.
 *
 * Reads PEM files from the Zephyr filesystem (e.g., LittleFS) and calls
 * tls_credential_add() for each required credential based on auth_type:
 *
 *   HTTPC_AT_AUTH_NONE   : No credentials loaded (no-verify mode)
 *   HTTPC_AT_AUTH_SERVER : Load CA cert from ca_file
 *   HTTPC_AT_AUTH_CLIENT : Load client cert from cert_file, key from key_file
 *   HTTPC_AT_AUTH_MUTUAL : Load all three files
 *
 * Previously registered credentials with the same sec_tags are deleted
 * before adding new ones to avoid conflicts.
 *
 * @param auth_type  Authentication scheme
 * @param ca_file    Path to CA certificate PEM file (e.g., "/lfs/ca.pem")
 *                   May be NULL or empty if not required by auth_type.
 * @param cert_file  Path to client certificate PEM file
 *                   May be NULL or empty if not required by auth_type.
 * @param key_file   Path to client private key PEM file
 *                   May be NULL or empty if not required by auth_type.
 * @return 0 on success, negative errno on failure
 *         -EINVAL  : required file path is NULL or empty for the given auth_type
 *         -ENOENT  : file not found on filesystem
 *         -ENOMEM  : memory allocation failure
 *         -EIO     : filesystem read error
 */
int httpc_at_ssl_load_certs(httpc_at_auth_type_t auth_type,
                             const char *ca_file,
                             const char *cert_file,
                             const char *key_file);

/**
 * @brief Remove all TLS credentials registered by this module.
 *
 * Calls tls_credential_delete() for all three sec_tags
 * (HTTPC_AT_TLS_TAG_CA_CERT, HTTPC_AT_TLS_TAG_CLIENT_CERT,
 *  HTTPC_AT_TLS_TAG_CLIENT_KEY). Errors from individual deletes are
 * logged but do not stop the cleanup.
 *
 * Should be called after each HTTPS connection is closed to free
 * TLS credential memory.
 */
void httpc_at_ssl_unload_certs(void);

#ifdef __cplusplus
}
#endif

#endif /* HTTPC_AT_SSL_H */
