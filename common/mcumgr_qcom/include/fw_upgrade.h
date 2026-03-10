/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FW_UPGRADE_H
#define _FW_UPGRADE_H

/**********************************************************************************************************/
/* Include Files 																			              */
/**********************************************************************************************************/
#include <mbedtls/sha256.h>
#include "fw_upgrade_types.h"
#include "fw_upgrade_mem.h"

/**********************************************************************************************************/
/* Preprocessor Definitions and Constants																  */
/**********************************************************************************************************/
#define FW_UPGRADE_BUF_SIZE               2048
#define FW_UPGRADE_HASH_LEN               32
#define FW_UPGRADE_INTERFACE_NAME_LEN     32
#define FW_UPGRADE_URL_LEN                256
#define FW_UPGRADE_FILENAME_LEN           128
#define FW_UPGRADE_URL_TOTAL_LEN          (FW_UPGRADE_URL_LEN + FW_UPGRADE_FILENAME_LEN)
#define FW_UPGRADE_MAX_IMAGES_NUM         30
#define FW_UPGRADE_FORAMT_PARTIAL_UPGRADE 1

#define FLASH_ERASED_VALUE 0xFFFFFFFF

/**********************************************************************************************************/
/* Type Declarations																                      */
/**********************************************************************************************************/
/*
 * Enumeration that represents fw upgrade session status.
 */
typedef enum {
    FW_UPGRADE_SESSION_NOT_START_E = 0,
    FW_UPGRADE_SESSION_RUNNING_E,
    FW_UPGRADE_SESSION_SUSPEND_E,
    FW_UPGRADE_SESSION_CANCEL_E,
    FW_UPGRADE_SESSION_ERROR_E,
} fw_upgrade_session_status_t;

/**
 *  Enumeration that represents the various states in firmware upgrade state machine.
 */
typedef enum {
    FW_UPGRADE_STATE_NOT_START_E = 0,       /**< Firmware upgrade operation is not started. */
    FW_UPGRADE_STATE_GET_TRIAL_INFO_E,      /**< Get trial image information at flash. */
    FW_UPGRADE_STATE_ERASE_FWD_E,           /**< Erase FWD. */
    FW_UPGRADE_STATE_ERASE_FLASH_E,         /**< Erase the partition. */
    FW_UPGRADE_STATE_ERASE_SECOND_FS_E,     /**< Erase the second file system. */
    FW_UPGRADE_STATE_PREPARE_FS_E,          /**< Prepare the file system. */
    FW_UPGRADE_STATE_ERASE_IMAGE_E,         /**< Erase the subimage. */
    FW_UPGRADE_STATE_PREPARE_CONNECT_E,     /**< Prepare to connect to a remote firmware upgrade server. */
    FW_UPGRADE_STATE_CONNECT_SERVER_E,      /**< Connect to a remote firmware upgrade server. */
    FW_UPGRADE_STATE_CONNECT_E = FW_UPGRADE_STATE_CONNECT_SERVER_E,
    										/**< Connected by a remote firmware upgrade client. */
    FW_UPGRADE_STATE_RESUME_SERVICE_E,      /**< Resume the firmware upgrade service. */
    FW_UPGRADE_STATE_RESUME_SERVER_E,       /**< Resume connecting to the firmware upgrade server. */
    FW_UPGRADE_STATE_RECEIVE_DATA_E,        /**< Receive data from the remote firmware upgrade server. */
    FW_UPGRADE_STATE_DISCONNECT_SERVER_E,   /**< Disconnected from a remote firmware upgrade server. */
    FW_UPGRADE_STATE_PROCESS_CONFIG_FILE_E, /**< Process firmware upgrade configuration file. */
    FW_UPGRADE_STATE_PROCESS_IMAGE_E,       /**< Process the image. */
    FW_UPGRADE_STATE_DUPLICATE_IMAGES_E,    /**< Duplicate the images from the current FWD. */
    FW_UPGRADE_STATE_DUPLICATE_FS_E,        /**< Duplicate the file system. */
    FW_UPGRADE_STATE_FINISH_E,              /**< Firmware upgrade is done. */
} fw_upgrade_state_t;

/*
 * Firmware Upgrade ImageSet Header Structure
 */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t format;
    uint32_t length;
    uint8_t num_images;
} __attribute__((packed)) fw_upgrade_imageSet_hdr_part1_t;

/*
 * Firmware Upgrade Sub Image Header Structure
 */
typedef struct {
    uint32_t magic;
    uint32_t image_id;
    uint32_t version;
    uint8_t image_file[FW_UPGRADE_FILENAME_LEN];
    uint32_t disk_size;
    uint32_t image_length;
    uint32_t hash_type;
    uint8_t hash[FW_UPGRADE_HASH_LEN];
} __attribute__((packed)) fw_upgrade_image_hdr_t;

/*
 * Data context for firmware upgrade session
 */
typedef struct {
    int32_t error_code;
    uint8_t is_first;

    uint32_t buf_len;    /* total available buffer length */
    uint32_t buf_offset; /* processed buffer length */

    uint32_t image_index;         /* image index number */
    uint32_t image_wrt_count;     /* image flashed length */
    uint32_t image_wrt_length;    /* image total length */
    uint32_t total_images;        /* total number of images */
    uint32_t file_read_count;     /* received length from remote file */
    uint32_t hidden_images_count; /* count of hidden images which is not downloaded by OTA, such as FS2 or RAMDUMP */

    fw_upgrade_state_t fw_upgrade_state;                   /* fw upgrade session state */
    fw_upgrade_session_status_t fw_upgrade_session_status; /* fw upgrade session status */
    uint8_t download_flag[FW_UPGRADE_MAX_IMAGES_NUM];      /* mark for download image or duplicate image from current */

    uint32_t flags;
    uint32_t format;          /* 1: partial fw upgrade, 2: all-in-one fw upgrade */
    uint32_t trial_mem_start; /* available memory start address to store upgraded images except SBL */
    uint32_t trial_mem_size;  /* trial partition size in memory */
    uint8_t trial_fwd_idx;    /* trial FWD number */
    uint32_t trial_sbl_start; /* available start address to store upgrade sbl iamge. */
    uint32_t trial_sbl_size;  /* trial sbl size. */
    uint32_t trial_sbl_idx;   /* trial SBL FDE number */

    uint8_t *tmp_buffer;	  /* FW_UPGRADE_BUF_SIZE */

    fu_part_hdl_t partition_hdl;

    mbedtls_sha256_context *digest_ctx;
    uint8_t *config_buf; /* buffer to store config file before parse */
} fw_upgrade_context_t;

typedef struct {
	uint8_t status;
	uint8_t trial_flag;
	uint8_t trial_sbl_idx;
	uint8_t trial_fwd_idx;
	uint32_t trial_sbl_version;
	uint32_t trial_fwd_magic_addr;
	uint32_t trial_fwd_magic_val;
	uint32_t trial_fwd_status_addr;
	uint8_t trial_fwd_status_val;
}fw_upgrade_result_t;

/**********************************************************************************************************/
/* Function Declarations																                  */
/**********************************************************************************************************/

/*
 * process result of OTA session
 */
fw_upgrade_status_code_t fw_upgrade_session_done(uint32_t result);

#endif /* _FW_UPGRADE_H */
