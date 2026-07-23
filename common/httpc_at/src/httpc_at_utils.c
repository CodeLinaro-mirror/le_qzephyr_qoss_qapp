/*
 * Copyright (c) 2026 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "../inc/httpc_at_utils.h"
#include "../inc/httpc_at_handler.h"

LOG_MODULE_REGISTER(httpc_at_utils, LOG_LEVEL_DBG);

/*-------------------------------------------------------------------------
 * Content-Type strings
 *-----------------------------------------------------------------------*/

static const char * const g_content_type_strings[] = {
    "application/x-www-form-urlencoded", /* HTTPC_AT_CTYPE_FORM_URLENCODED = 0 */
    "application/json",                  /* HTTPC_AT_CTYPE_JSON            = 1 */
    "application/zip",                   /* HTTPC_AT_CTYPE_ZIP             = 2 */
    "multipart/form-data",               /* HTTPC_AT_CTYPE_FORM_DATA       = 3 */
    "text/xml",                          /* HTTPC_AT_CTYPE_TEXT_XML        = 4 */
};

#define CONTENT_TYPE_COUNT  (sizeof(g_content_type_strings) / sizeof(g_content_type_strings[0]))

/*-------------------------------------------------------------------------
 * Public Functions
 *-----------------------------------------------------------------------*/

const char *httpc_at_get_content_type_str(int content_type)
{
    if (content_type < 0 || (size_t)content_type >= CONTENT_TYPE_COUNT) {
        return g_content_type_strings[0]; /* default */
    }
    return g_content_type_strings[content_type];
}

bool httpc_at_is_https(const char *url)
{
    if (!url) {
        return false;
    }
    return (strncmp(url, "https://", 8) == 0);
}

bool httpc_at_validate_url(const char *url)
{
    const char *host_start = NULL;

    if (!url || strlen(url) == 0) {
        return false;
    }

    if (strncmp(url, "https://", 8) == 0) {
        host_start = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        host_start = url + 7;
    } else {
        return false;
    }

    if (strlen(host_start) == 0) {
        return false;
    }

    /* Hostname must contain at least one '.' (domain) or ':' (IPv6 or port) */
    if (strchr(host_start, '.') == NULL && strchr(host_start, ':') == NULL) {
        return false;
    }

    return true;
}

int httpc_at_parse_url(const char *url,
                       char *host, size_t host_size,
                       char *path, size_t path_size,
                       uint16_t *port)
{
    const char *p;
    const char *host_start;
    const char *host_end;
    const char *path_start;
    const char *port_start;
    size_t host_len;
    unsigned long port_val = 0;

    if (!url || !host || !path || !port) {
        return -EINVAL;
    }

    /* Skip scheme */
    if (strncmp(url, "https://", 8) == 0) {
        host_start = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        host_start = url + 7;
    } else {
        LOG_ERR("URL missing scheme: %s", url);
        return -EINVAL;
    }

    if (strlen(host_start) == 0) {
        LOG_ERR("URL has empty host");
        return -EINVAL;
    }

    /* Find end of host (either '/', ':', or end of string) */
    path_start = strchr(host_start, '/');
    port_start = strchr(host_start, ':');

    /* If port_start comes before path_start (or there is no path), parse port */
    if (port_start != NULL &&
        (path_start == NULL || port_start < path_start)) {
        /* Host ends at ':' */
        host_end = port_start;
        /* Parse port number */
        port_val = strtoul(port_start + 1, NULL, 10);
        if (port_val == 0 || port_val > 65535) {
            LOG_ERR("Invalid port in URL: %s", url);
            return -EINVAL;
        }
        *port = (uint16_t)port_val;
    } else {
        /* No explicit port */
        host_end = (path_start != NULL) ? path_start : (host_start + strlen(host_start));
        *port = 0;
    }

    host_len = (size_t)(host_end - host_start);
    if (host_len == 0 || host_len >= host_size) {
        LOG_ERR("Host buffer too small or empty host");
        return -ENOMEM;
    }

    memcpy(host, host_start, host_len);
    host[host_len] = '\0';

    /* Extract path */
    if (path_start != NULL) {
        size_t path_len = strlen(path_start);

        if (path_len >= path_size) {
            LOG_ERR("Path buffer too small");
            return -ENOMEM;
        }
        memcpy(path, path_start, path_len);
        path[path_len] = '\0';
    } else {
        /* No path in URL, use root */
        if (path_size < 2) {
            return -ENOMEM;
        }
        path[0] = '/';
        path[1] = '\0';
    }

    LOG_DBG("Parsed URL: host=%s path=%s port=%u", host, path, *port);
    return 0;
}

size_t httpc_at_get_valid_data_len(const char *data, size_t len, size_t at_buf_len)
{
    /*
     * Workaround for AT host data mode behavior:
     *
     * Case 1: Data packet is exactly at_buf_len bytes (full packet).
     *         The host does NOT append '\r'. Return len as-is.
     *
     * Case 2: Data packet is smaller than at_buf_len bytes.
     *         The host appends '\r' at the end. Strip it.
     */
    if (len == 0) {
        return 0;
    }

    if (len != at_buf_len && data[len - 1] == '\r') {
        return len - 1;
    }

    return len;
}
