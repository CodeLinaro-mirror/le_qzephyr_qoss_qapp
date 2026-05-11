/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <stdio.h>
#include <string.h>
#include <cat.h>
#include "qat_api.h"

LOG_MODULE_REGISTER(qat_fstore, LOG_LEVEL_INF);

#define FSTORE_MOUNT_POINT    "/lfs"
#define FSTORE_PATH_MAX       128
#define FSTORE_READ_CHUNK     256
#define FSTORE_CHUNK_HEX_MAX  128  /* max bytes per AT+WRITEFILE append chunk */

/* WRITEFILE state */
static struct {
    struct fs_file_t file;
    size_t total_len;    /* expected total bytes (0 = unknown) */
    size_t received_len;
    bool active;
} writefile_state;

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/*
 * Normalize a path: if it doesn't start with FSTORE_MOUNT_POINT, prepend it.
 * Result written to out (size FSTORE_PATH_MAX).
 * Returns 0 on success, -ENAMETOOLONG if result would exceed FSTORE_PATH_MAX.
 */
static int normalize_path(const char *in, char *out)
{
    /* strip optional surrounding double-quotes */
    size_t in_len = strlen(in);
    if (in_len >= 2 && in[0] == '"' && in[in_len - 1] == '"') {
        in++;
        in_len -= 2;
    }

    if (strncmp(in, FSTORE_MOUNT_POINT, strlen(FSTORE_MOUNT_POINT)) == 0) {
        if (snprintf(out, FSTORE_PATH_MAX, "%.*s", (int)in_len, in) >= FSTORE_PATH_MAX) {
            return -ENAMETOOLONG;
        }
    } else {
        if (snprintf(out, FSTORE_PATH_MAX, "%s/%.*s", FSTORE_MOUNT_POINT, (int)in_len, in) >=
            FSTORE_PATH_MAX) {
            return -ENAMETOOLONG;
        }
    }
    return 0;
}

/*
 * Ensure the parent directory of 'path' exists (create one level if needed).
 */
static void ensure_parent_dir(const char *path)
{
    char parent[FSTORE_PATH_MAX];
    const char *slash = strrchr(path, '/');

    if (!slash || slash == path) {
        return;
    }

    size_t parent_len = (size_t)(slash - path);

    if (parent_len == 0 || parent_len >= FSTORE_PATH_MAX) {
        return;
    }

    memcpy(parent, path, parent_len);
    parent[parent_len] = '\0';

    /* Skip mount point itself */
    if (strcmp(parent, FSTORE_MOUNT_POINT) == 0) {
        return;
    }

    struct fs_dirent entry;

    if (fs_stat(parent, &entry) == 0) {
        return; /* already exists */
    }

    int ret = fs_mkdir(parent);

    if (ret != 0 && ret != -EEXIST) {
        LOG_WRN("fs_mkdir(%s) failed: %d", parent, ret);
    }
}

/*
 * AT+WRITEFILE="<path>",C,<size>          — create / truncate file
 * AT+WRITEFILE="<path>",A,"<hex>"         — append hex-encoded chunk
 *
 * Replaces the legacy binary data-mode interface.  Raw binary data cannot be
 * relayed through the STM32 ring service, so files are transferred as hex
 * strings carried in standard AT command lines.
 *
 * Typical flow (upload_cert.py):
 *   AT+WRITEFILE="/lfs/server.crt",C,1107   ← create, declare total size
 *   AT+WRITEFILE="/lfs/server.crt",A,"2d2d..."  ← first 64-byte chunk
 *   ...repeat until all bytes sent...
 *   (file is closed automatically when received_len >= total_len)
 */
static cat_return_state cmd_writefile_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK,
                            "AT+WRITEFILE=\"<path>\",C,<size>  — create/truncate\r\n"
                            "AT+WRITEFILE=\"<path>\",A,\"<hex>\" — append hex chunk\r\n");
}

static cat_return_state cmd_writefile_set(const struct cat_command *cmd, const uint8_t *data,
                                          const size_t data_size, const size_t args_num)
{
    /* buf must hold: "<path>",X,"<256-hex-chars>"  */
    char buf[FSTORE_PATH_MAX + 16 + FSTORE_CHUNK_HEX_MAX * 2 + 16];
    char path_arg[FSTORE_PATH_MAX];
    char full_path[FSTORE_PATH_MAX];
    int ret;

    if (data_size == 0 || data_size >= sizeof(buf)) {
        return QAT_Response_Str(QAT_RC_ERROR, "+WRITEFILE: invalid parameters\r\n");
    }
    memcpy(buf, data, data_size);
    buf[data_size] = '\0';

    char *p = buf;

    /* --- parse path (with or without surrounding quotes) --- */
    if (*p == '"') {
        p++;
        char *end = strchr(p, '"');

        if (!end) {
            return QAT_Response_Str(QAT_RC_ERROR, "+WRITEFILE: unterminated path\r\n");
        }
        size_t len = (size_t)(end - p);

        if (len >= sizeof(path_arg)) {
            return QAT_Response_Str(QAT_RC_ERROR, "+WRITEFILE: path too long\r\n");
        }
        memcpy(path_arg, p, len);
        path_arg[len] = '\0';
        p = end + 1; /* past closing quote */
    } else {
        char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);

        if (len >= sizeof(path_arg)) {
            return QAT_Response_Str(QAT_RC_ERROR, "+WRITEFILE: path too long\r\n");
        }
        memcpy(path_arg, p, len);
        path_arg[len] = '\0';
        p = end ? end : p + len;
    }
    if (*p == ',') {
        p++;
    }

    if (normalize_path(path_arg, full_path) != 0) {
        return QAT_Response_Str(QAT_RC_ERROR, "+WRITEFILE: path too long\r\n");
    }

    /* --- mode: 'C' (create) or 'A' (append hex) --- */
    char mode = *p;

    if (mode == '\0') {
        return QAT_Response_Str(QAT_RC_ERROR,
                                "+WRITEFILE: missing mode — use C (create) or A (append hex)\r\n");
    }
    p++;
    if (*p == ',') {
        p++;
    }

    /* ---- C: create / truncate ---- */
    if (mode == 'C' || mode == 'c') {
        size_t expected = (*p != '\0') ? (size_t)atol(p) : 0;

        if (writefile_state.active) {
            fs_close(&writefile_state.file);
            writefile_state.active = false;
        }
        ensure_parent_dir(full_path);
        fs_file_t_init(&writefile_state.file);
        ret = fs_open(&writefile_state.file, full_path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
        if (ret != 0) {
            LOG_ERR("fs_open(%s) failed: %d", full_path, ret);
            return QAT_Response_Str(QAT_RC_ERROR, "+WRITEFILE: failed to create file\r\n");
        }
        writefile_state.total_len    = expected;
        writefile_state.received_len = 0;
        writefile_state.active       = true;
        LOG_INF("WRITEFILE create: %s (expect %zu B)", full_path, expected);
        return QAT_Response_Str(QAT_RC_OK, NULL);
    }

    /* ---- A: append hex-encoded chunk ---- */
    if (mode == 'A' || mode == 'a') {
        if (!writefile_state.active) {
            return QAT_Response_Str(QAT_RC_ERROR,
                                    "+WRITEFILE: no file open — send C command first\r\n");
        }

        /* strip optional surrounding quotes from hex string */
        char *hex = p;

        if (*hex == '"') {
            hex++;
            char *end = strchr(hex, '"');

            if (end) {
                *end = '\0';
            }
        }

        size_t hex_len = strlen(hex);

        if (hex_len == 0 || hex_len % 2 != 0) {
            return QAT_Response_Str(QAT_RC_ERROR,
                                    "+WRITEFILE: hex data missing or odd length\r\n");
        }
        if (hex_len > FSTORE_CHUNK_HEX_MAX * 2) {
            return QAT_Response_Str(QAT_RC_ERROR, "+WRITEFILE: hex chunk too large\r\n");
        }

        uint8_t decode[FSTORE_CHUNK_HEX_MAX];
        size_t byte_count = hex_len / 2;

        for (size_t i = 0; i < byte_count; i++) {
            int hi = hex_val(hex[i * 2]);
            int lo = hex_val(hex[i * 2 + 1]);

            if (hi < 0 || lo < 0) {
                return QAT_Response_Str(QAT_RC_ERROR, "+WRITEFILE: invalid hex character\r\n");
            }
            decode[i] = (uint8_t)((hi << 4) | lo);
        }

        ssize_t written = fs_write(&writefile_state.file, decode, byte_count);

        if (written < 0) {
            LOG_ERR("fs_write failed: %zd", written);
            fs_close(&writefile_state.file);
            writefile_state.active = false;
            return QAT_Response_Str(QAT_RC_ERROR, "+WRITEFILE: write error\r\n");
        }
        writefile_state.received_len += (size_t)written;

        /* auto-close when total_len is known and reached */
        if (writefile_state.total_len > 0 &&
            writefile_state.received_len >= writefile_state.total_len) {
            fs_close(&writefile_state.file);
            writefile_state.active = false;
            char resp[48];

            snprintf(resp, sizeof(resp), "+WRITEFILE: %zu bytes written",
                     writefile_state.received_len);
            LOG_INF("WRITEFILE done: %zu bytes", writefile_state.received_len);
            return QAT_Response_Str(QAT_RC_OK, resp);
        }

        return QAT_Response_Str(QAT_RC_OK, NULL);
    }

    return QAT_Response_Str(QAT_RC_ERROR,
                            "+WRITEFILE: unknown mode — use C (create) or A (append hex)\r\n");
}

/*
 * AT+READFILE=<path>
 * Read a file from the filesystem and send its contents to the host.
 */
static cat_return_state cmd_readfile_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK, "AT+READFILE=<path>: read file contents from /lfs\r\n");
}

static cat_return_state cmd_readfile_set(const struct cat_command *cmd, const uint8_t *data,
                                         const size_t data_size, const size_t args_num)
{
    char path_arg[FSTORE_PATH_MAX];
    char full_path[FSTORE_PATH_MAX];
    struct fs_dirent entry;
    struct fs_file_t file;
    uint8_t buf[FSTORE_READ_CHUNK];
    ssize_t bytes_read;
    int ret;

    if (data_size == 0 || data_size >= FSTORE_PATH_MAX) {
        return QAT_Response_Str(QAT_RC_ERROR, "+READFILE: Missing path\r\n");
    }
    memcpy(path_arg, data, data_size);
    path_arg[data_size] = '\0';

    if (normalize_path(path_arg, full_path) != 0) {
        return QAT_Response_Str(QAT_RC_ERROR, "+READFILE: Path too long\r\n");
    }

    ret = fs_stat(full_path, &entry);
    if (ret != 0) {
        return QAT_Response_Str(QAT_RC_ERROR, "+READFILE: File not found\r\n");
    }

    if (entry.type != FS_DIR_ENTRY_FILE) {
        return QAT_Response_Str(QAT_RC_ERROR, "+READFILE: Not a file\r\n");
    }

    char header[FSTORE_PATH_MAX + 64];

    snprintf(header, sizeof(header), "+READFILE: %s, %zu bytes\r\n", full_path, entry.size);
    QAT_Output(strlen(header), header);

    fs_file_t_init(&file);
    ret = fs_open(&file, full_path, FS_O_READ);
    if (ret != 0) {
        return QAT_Response_Str(QAT_RC_ERROR, "+READFILE: Failed to open file\r\n");
    }

    while ((bytes_read = fs_read(&file, buf, sizeof(buf))) > 0) {
        QAT_Output((uint32_t)bytes_read, (const char *)buf);
    }

    if (bytes_read < 0) {
        fs_close(&file);
        return QAT_Response_Str(QAT_RC_ERROR, "+READFILE: Read error\r\n");
    }

    fs_close(&file);

    return QAT_Response_Str(QAT_RC_OK, "\r\n");
}

/*
 * AT+DELFILE=<path>
 * Delete a file from the filesystem.
 */
static cat_return_state cmd_delfile_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK, "AT+DELFILE=<path>: delete file from /lfs\r\n");
}

static cat_return_state cmd_delfile_set(const struct cat_command *cmd, const uint8_t *data,
                                        const size_t data_size, const size_t args_num)
{
    char path_arg[FSTORE_PATH_MAX];
    char full_path[FSTORE_PATH_MAX];
    int ret;

    if (data_size == 0 || data_size >= FSTORE_PATH_MAX) {
        return QAT_Response_Str(QAT_RC_ERROR, "+DELFILE: Missing path\r\n");
    }
    memcpy(path_arg, data, data_size);
    path_arg[data_size] = '\0';

    if (normalize_path(path_arg, full_path) != 0) {
        return QAT_Response_Str(QAT_RC_ERROR, "+DELFILE: Path too long\r\n");
    }

    ret = fs_unlink(full_path);
    if (ret != 0) {
        LOG_ERR("fs_unlink(%s) failed: %d", full_path, ret);
        return QAT_Response_Str(QAT_RC_ERROR, "+DELFILE: Failed to delete file\r\n");
    }

    LOG_INF("DELFILE: deleted %s", full_path);
    return QAT_Response_Str(QAT_RC_OK, NULL);
}

/*-------------------------------------------------------------------------
 * Command List
 *-----------------------------------------------------------------------*/

static struct cat_command qat_fstore_cmds[] = {
    {
        .name = "+WRITEFILE",
        .description = "Write file from host to QCC730 filesystem",
        .run = cmd_writefile_exec,
        .write = cmd_writefile_set,
    },
    {
        .name = "+READFILE",
        .description = "Read file from QCC730 filesystem",
        .run = cmd_readfile_exec,
        .write = cmd_readfile_set,
    },
    {
        .name = "+DELFILE",
        .description = "Delete file from QCC730 filesystem",
        .run = cmd_delfile_exec,
        .write = cmd_delfile_set,
    },
};

static struct cat_command_group qat_fstore_cmd_group = {
    .name = "QAT_FSTORE",
    .cmd = qat_fstore_cmds,
    .cmd_num = ARRAY_SIZE(qat_fstore_cmds),
};

struct cat_command_group *qat_fstore_get_command_group(void)
{
    LOG_DBG("Registering QAT file store commands");
    return &qat_fstore_cmd_group;
}

/* Automatically register this command group with QAT */
QAT_REGISTER_CMD_GROUP(qat_fstore_get_command_group, "FSTORE");
