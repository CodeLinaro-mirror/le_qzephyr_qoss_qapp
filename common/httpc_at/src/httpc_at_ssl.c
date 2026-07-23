/*
 * Copyright (c) 2026 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#if defined(CONFIG_HTTPC_AT_TLS) && defined(CONFIG_TLS_CREDENTIALS)
#include <zephyr/net/tls_credentials.h>
#define HTTPC_AT_SSL_CRED_SUPPORTED 1
#else
#define HTTPC_AT_SSL_CRED_SUPPORTED 0
#endif

#if HTTPC_AT_SSL_CRED_SUPPORTED && defined(CONFIG_FILE_SYSTEM)
#include <zephyr/fs/fs.h>
#endif

#include "../inc/httpc_at_ssl.h"
#include "../inc/httpc_at_core.h"
#include "../inc/httpc_at_handler.h"

LOG_MODULE_REGISTER(httpc_at_ssl, LOG_LEVEL_DBG);

/* -------------------------------------------------------------------------
 * Persistent credential buffers
 *
 * Zephyr's tls_credential_add() stores the caller's pointer directly — it
 * does NOT copy the data internally.  The buffer MUST remain valid until
 * tls_credential_delete() is called.  We therefore keep one static pointer
 * per credential slot and free it only inside httpc_at_ssl_unload_certs().
 * ---------------------------------------------------------------------- */
#if HTTPC_AT_SSL_CRED_SUPPORTED
static uint8_t *s_ca_buf;    /* CA certificate buffer */
static uint8_t *s_cert_buf;  /* Client certificate buffer */
static uint8_t *s_key_buf;   /* Client private key buffer */
#endif

/* -------------------------------------------------------------------------
 * Private helpers
 * ---------------------------------------------------------------------- */

/**
 * @brief Delete a TLS credential if it exists.
 *
 * Errors from tls_credential_delete() are logged but not propagated,
 * since the credential may not have been registered yet.
 *
 * @param tag   sec_tag_t to delete
 * @param type  Credential type to delete
 */
#if HTTPC_AT_SSL_CRED_SUPPORTED
static void delete_credential_if_exists(sec_tag_t tag,
					enum tls_credential_type type)
{
	int ret = tls_credential_delete(tag, type);

	if (ret < 0 && ret != -ENOENT) {
		LOG_WRN("tls_credential_delete(tag=%d, type=%d) failed: %d",
			(int)tag, (int)type, ret);
	}
}
#endif

#if HTTPC_AT_SSL_CRED_SUPPORTED && defined(CONFIG_FILE_SYSTEM)

/**
 * @brief Read an entire file from the Zephyr filesystem into a heap buffer.
 *
 * The caller is responsible for freeing the returned buffer with k_free().
 * Only compiled when CONFIG_FILE_SYSTEM=y.
 *
 * @param path      Filesystem path (e.g., "/lfs/ca.pem")
 * @param buf_out   Output: pointer to allocated buffer (NUL-terminated)
 * @param len_out   Output: number of bytes read (excluding NUL terminator)
 * @return 0 on success, negative errno on failure
 */
static int read_file_to_buf(const char *path, uint8_t **buf_out,
			    size_t *len_out)
{
	struct fs_file_t file;
	struct fs_dirent entry;
	uint8_t *buf = NULL;
	ssize_t bytes_read;
	int ret;

	if (!path || !buf_out || !len_out) {
		return -EINVAL;
	}

	/* Stat the file to get its size */
	ret = fs_stat(path, &entry);
	if (ret < 0) {
		LOG_ERR("fs_stat(%s) failed: %d", path, ret);
		return ret;
	}

	if (entry.type != FS_DIR_ENTRY_FILE) {
		LOG_ERR("%s is not a regular file", path);
		return -EISDIR;
	}

	if (entry.size == 0) {
		LOG_ERR("%s is empty", path);
		return -ENODATA;
	}

	/* Allocate buffer: file size + 1 for NUL terminator */
	buf = k_malloc(entry.size + 1);
	if (!buf) {
		LOG_ERR("k_malloc(%zu) failed for %s", entry.size + 1, path);
		return -ENOMEM;
	}

	/* Open and read the file */
	fs_file_t_init(&file);
	ret = fs_open(&file, path, FS_O_READ);
	if (ret < 0) {
		LOG_ERR("fs_open(%s) failed: %d", path, ret);
		k_free(buf);
		return ret;
	}

	bytes_read = fs_read(&file, buf, entry.size);
	fs_close(&file);

	if (bytes_read < 0) {
		LOG_ERR("fs_read(%s) failed: %zd", path, bytes_read);
		k_free(buf);
		return (int)bytes_read;
	}

	if ((size_t)bytes_read != entry.size) {
		LOG_WRN("fs_read(%s): expected %zu bytes, got %zd",
			path, entry.size, bytes_read);
	}

	buf[bytes_read] = '\0'; /* NUL-terminate for PEM parsing */
	*buf_out = buf;
	*len_out = (size_t)bytes_read;

	LOG_DBG("Read %zd bytes from %s", bytes_read, path);
	return 0;
}

/**
 * @brief Load a single PEM file and register it as a TLS credential.
 *
 * IMPORTANT: tls_credential_add() stores the caller's pointer directly —
 * it does NOT copy the data.  The buffer must remain valid until
 * tls_credential_delete() is called.  On success, ownership of the
 * allocated buffer is transferred to *buf_keep; the caller must NOT free
 * it until after tls_credential_delete().
 *
 * @param path      Filesystem path to PEM file
 * @param tag       sec_tag_t to register under
 * @param type      Credential type
 * @param buf_keep  In/out: pointer to the persistent buffer slot.
 *                  If *buf_keep is non-NULL on entry, the old buffer is
 *                  freed after the old credential is deleted.
 *                  On success, *buf_keep is set to the newly allocated buf.
 * @return 0 on success, negative errno on failure
 */
static int load_and_register_credential(const char *path,
					sec_tag_t tag,
					enum tls_credential_type type,
					uint8_t **buf_keep)
{
	uint8_t *buf = NULL;
	size_t len = 0;
	int ret;

	if (!path || strlen(path) == 0) {
		LOG_ERR("Empty file path for credential tag=%d type=%d",
			(int)tag, (int)type);
		return -EINVAL;
	}

	ret = read_file_to_buf(path, &buf, &len);
	if (ret < 0) {
		LOG_ERR("Failed to read credential file %s: %d", path, ret);
		return ret;
	}

	/*
	 * Delete any existing credential with this tag/type before adding.
	 * After deletion the old credential no longer references the old
	 * buffer, so we can safely free it now.
	 */
	delete_credential_if_exists(tag, type);
	if (buf_keep && *buf_keep) {
		k_free(*buf_keep);
		*buf_keep = NULL;
	}

	/*
	 * Register the credential.
	 *
	 * Pass len+1 to include the NUL terminator — required so that
	 * mbedtls_x509_crt_parse() recognises the data as PEM:
	 *   crt_is_pem() checks buf[buflen-1] == '\0'
	 *
	 * NOTE: tls_credential_add() stores 'buf' by pointer only (no copy).
	 * Do NOT free 'buf' here.  Ownership is transferred to *buf_keep and
	 * the buffer must remain valid until tls_credential_delete() is called.
	 */
	ret = tls_credential_add(tag, type, buf, len + 1);
	if (ret < 0) {
		LOG_ERR("tls_credential_add(tag=%d, type=%d) failed: %d",
			(int)tag, (int)type, ret);
		k_free(buf);
		return ret;
	}

	/* Transfer buffer ownership to the caller's persistent slot */
	if (buf_keep) {
		*buf_keep = buf;
	}

	LOG_DBG("Registered credential: tag=%d type=%d from %s (%zu bytes)",
		(int)tag, (int)type, path, len);
	return 0;
}

#endif /* HTTPC_AT_SSL_CRED_SUPPORTED && CONFIG_FILE_SYSTEM */

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

int httpc_at_ssl_load_certs(httpc_at_auth_type_t auth_type,
			    const char *ca_file,
			    const char *cert_file,
			    const char *key_file)
{
	if (auth_type == HTTPC_AT_AUTH_NONE) {
		/*
		 * No-verify mode: no credentials needed.
		 * The TLS socket can still run with peer verification disabled.
		 */
		LOG_DBG("SSL auth_type=NONE: no credentials loaded");
		return 0;
	}

	if (auth_type < HTTPC_AT_AUTH_NONE || auth_type > HTTPC_AT_AUTH_MUTUAL) {
		LOG_ERR("Unknown auth_type: %d", (int)auth_type);
		return -EINVAL;
	}

#if !(HTTPC_AT_SSL_CRED_SUPPORTED)
	LOG_ERR("TLS credential support is not enabled");
	return -ENOTSUP;
#elif !defined(CONFIG_FILE_SYSTEM)
	LOG_ERR("FILE_SYSTEM is required for certificate loading");
	return -ENOTSUP;
#else
	int ret;

	switch (auth_type) {
	case HTTPC_AT_AUTH_SERVER:
		/* Verify server certificate: need CA cert */
		if (!ca_file || strlen(ca_file) == 0) {
			LOG_ERR("AUTH_SERVER requires ca_file");
			return -EINVAL;
		}
		ret = load_and_register_credential(ca_file,
						   HTTPC_AT_TLS_TAG_CA_CERT,
						   TLS_CREDENTIAL_CA_CERTIFICATE,
						   &s_ca_buf);
		if (ret < 0) {
			return ret;
		}
		break;

	case HTTPC_AT_AUTH_CLIENT:
		/* Provide client certificate: need cert + key */
		if (!cert_file || strlen(cert_file) == 0) {
			LOG_ERR("AUTH_CLIENT requires cert_file");
			return -EINVAL;
		}
		if (!key_file || strlen(key_file) == 0) {
			LOG_ERR("AUTH_CLIENT requires key_file");
			return -EINVAL;
		}
		ret = load_and_register_credential(cert_file,
						   HTTPC_AT_TLS_TAG_CLIENT_CERT,
						   TLS_CREDENTIAL_PUBLIC_CERTIFICATE,
						   &s_cert_buf);
		if (ret < 0) {
			return ret;
		}
		ret = load_and_register_credential(key_file,
						   HTTPC_AT_TLS_TAG_CLIENT_KEY,
						   TLS_CREDENTIAL_PRIVATE_KEY,
						   &s_key_buf);
		if (ret < 0) {
			return ret;
		}
		break;

	case HTTPC_AT_AUTH_MUTUAL:
		/* Verify server + provide client cert: need all three */
		if (!ca_file || strlen(ca_file) == 0) {
			LOG_ERR("AUTH_MUTUAL requires ca_file");
			return -EINVAL;
		}
		if (!cert_file || strlen(cert_file) == 0) {
			LOG_ERR("AUTH_MUTUAL requires cert_file");
			return -EINVAL;
		}
		if (!key_file || strlen(key_file) == 0) {
			LOG_ERR("AUTH_MUTUAL requires key_file");
			return -EINVAL;
		}
		ret = load_and_register_credential(ca_file,
						   HTTPC_AT_TLS_TAG_CA_CERT,
						   TLS_CREDENTIAL_CA_CERTIFICATE,
						   &s_ca_buf);
		if (ret < 0) {
			return ret;
		}
		ret = load_and_register_credential(cert_file,
						   HTTPC_AT_TLS_TAG_CLIENT_CERT,
						   TLS_CREDENTIAL_PUBLIC_CERTIFICATE,
						   &s_cert_buf);
		if (ret < 0) {
			return ret;
		}
		ret = load_and_register_credential(key_file,
						   HTTPC_AT_TLS_TAG_CLIENT_KEY,
						   TLS_CREDENTIAL_PRIVATE_KEY,
						   &s_key_buf);
		if (ret < 0) {
			return ret;
		}
		break;

	default:
		return -EINVAL;
	}

	return 0;
#endif
}

void httpc_at_ssl_unload_certs(void)
{
#if HTTPC_AT_SSL_CRED_SUPPORTED
	/*
	 * Delete credentials first (removes the reference to the buffer),
	 * then free the buffer.  Order matters: tls_credential_delete()
	 * clears credential->buf, so no dangling pointer remains in the
	 * credentials table after k_free().
	 */
	delete_credential_if_exists(HTTPC_AT_TLS_TAG_CA_CERT,
				    TLS_CREDENTIAL_CA_CERTIFICATE);
	k_free(s_ca_buf);
	s_ca_buf = NULL;

	delete_credential_if_exists(HTTPC_AT_TLS_TAG_CLIENT_CERT,
				    TLS_CREDENTIAL_PUBLIC_CERTIFICATE);
	k_free(s_cert_buf);
	s_cert_buf = NULL;

	delete_credential_if_exists(HTTPC_AT_TLS_TAG_CLIENT_KEY,
				    TLS_CREDENTIAL_PRIVATE_KEY);
	k_free(s_key_buf);
	s_key_buf = NULL;
#endif

	LOG_DBG("TLS credentials unloaded");
}