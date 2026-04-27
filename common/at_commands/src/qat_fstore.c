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

#define FSTORE_MOUNT_POINT  "/lfs"
#define FSTORE_PATH_MAX     128
#define FSTORE_READ_CHUNK   256

/* WRITEFILE state */
static struct {
    struct fs_file_t file;
    size_t total_len;
    size_t received_len;
    bool active;
} writefile_state;

/*
 * Normalize a path: if it doesn't start with FSTORE_MOUNT_POINT, prepend it.
 * Result written to out (size FSTORE_PATH_MAX).
 * Returns 0 on success, -ENAMETOOLONG if result would exceed FSTORE_PATH_MAX.
 */
static int normalize_path(const char *in, char *out)
{
    if (strncmp(in, FSTORE_MOUNT_POINT, strlen(FSTORE_MOUNT_POINT)) == 0) {
        if (snprintf(out, FSTORE_PATH_MAX, "%s", in) >= FSTORE_PATH_MAX) {
            return -ENAMETOOLONG;
        }
    } else {
        if (snprintf(out, FSTORE_PATH_MAX, "%s/%s", FSTORE_MOUNT_POINT, in) >= FSTORE_PATH_MAX) {
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
 * Online data mode callback for AT+WRITEFILE.
 * Called by QAT framework when data arrives on the ring channel.
 */
static int writefile_data_cb(const uint8_t *data, size_t len)
{
    if (!writefile_state.active) {
        return 0;
    }

    size_t remaining = writefile_state.total_len - writefile_state.received_len;
    size_t to_write = (len < remaining) ? len : remaining;

    ssize_t written = fs_write(&writefile_state.file, data, to_write);

    if (written < 0) {
        LOG_ERR("fs_write failed: %zd", written);
        fs_close(&writefile_state.file);
        writefile_state.active = false;
        QAT_Transfer_Mode_set(QAT_Transfer_Mode_AT_COMMAND_E, NULL);
        QAT_Response_Str(QAT_RC_QUIET, "ERROR\r\n");
        return (int)len;
    }

    writefile_state.received_len += (size_t)written;

    LOG_DBG("WRITEFILE: %zu/%zu bytes", writefile_state.received_len, writefile_state.total_len);

    if (writefile_state.received_len >= writefile_state.total_len) {
        fs_close(&writefile_state.file);
        writefile_state.active = false;
        QAT_Transfer_Mode_set(QAT_Transfer_Mode_AT_COMMAND_E, NULL);
        QAT_Response_Str(QAT_RC_QUIET, "OK\r\n");
    }

    return (int)len;
}

/*
 * AT+WRITEFILE=<path>,<size>
 * Enter online data mode; receive <size> bytes and write to filesystem path.
 */
static cat_return_state cmd_writefile_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK,
                            "AT+WRITEFILE=<path>,<size>: write <size> bytes to <path> on /lfs\r\n");
}

static cat_return_state cmd_writefile_set(const struct cat_command *cmd, const uint8_t *data,
                                          const size_t data_size, const size_t args_num)
{
    char path_arg[FSTORE_PATH_MAX];
    int size;
    char full_path[FSTORE_PATH_MAX];
    int ret;

    if (sscanf((char *)data, "%127[^,],%d", path_arg, &size) != 2 || size <= 0) {
        return QAT_Response_Str(QAT_RC_ERROR,
                                "+WRITEFILE: Invalid parameters\r\n"
                                "Usage: AT+WRITEFILE=<path>,<size>\r\n");
    }

    if (writefile_state.active) {
        return QAT_Response_Str(QAT_RC_ERROR, "+WRITEFILE: Transfer already in progress\r\n");
    }

    if (normalize_path(path_arg, full_path) != 0) {
        return QAT_Response_Str(QAT_RC_ERROR, "+WRITEFILE: Path too long\r\n");
    }

    ensure_parent_dir(full_path);

    fs_file_t_init(&writefile_state.file);
    ret = fs_open(&writefile_state.file, full_path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
    if (ret != 0) {
        LOG_ERR("fs_open(%s) failed: %d", full_path, ret);
        return QAT_Response_Str(QAT_RC_ERROR, "+WRITEFILE: Failed to open file\r\n");
    }

    writefile_state.total_len = (size_t)size;
    writefile_state.received_len = 0;
    writefile_state.active = true;

    ret = QAT_Transfer_Mode_set(QAT_Transfer_Mode_ONLINE_DATA_E, writefile_data_cb);
    if (ret != 0) {
        fs_close(&writefile_state.file);
        writefile_state.active = false;
        return QAT_Response_Str(QAT_RC_ERROR, "+WRITEFILE: Failed to enter data mode\r\n");
    }

    LOG_INF("WRITEFILE: receiving %d bytes -> %s", size, full_path);

    char response[FSTORE_PATH_MAX + 64];

    snprintf(response, sizeof(response),
             "+WRITEFILE: Ready, send %d bytes to %s\r\n", size, full_path);
    return QAT_Response_Str(QAT_RC_OK, response);
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
