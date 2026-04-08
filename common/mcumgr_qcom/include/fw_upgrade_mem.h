/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef _FW_UPGRADE_MEM_H
#define _FW_UPGRADE_MEM_H

/**********************************************************************************************************/
/* Preprocessor Definitions and Constants																  */
/**********************************************************************************************************/
#define FW_UPGRADE_FWD_RANK_TRIAL     0xFFFFFFFF
#define FW_UPGRADE_FWD_RANK_GOLDEN    0x00000000
#define FW_UPGRADE_FWD_STATUS_VALID   0x01
#define FW_UPGRADE_FWD_STATUS_INVALID 0x00
#define FW_UPGRADE_FWD_STATUS_UNUSED  0xFF
#define FW_UPGRADE_FWD_TRIAL_UNUSED   0xFF
#define FW_UPGRADE_FWD_IMAGE_UNUSED   0xFFFFFFFF

#define FLASH_OFFSET_MAGIC_NUM_ADDRESS 0
#define FLASH_ERASED_VALUE             0xFFFFFFFF

#define FW_UPGRADE_FWD_BIT_GOLDEN  (0)
#define FW_UPGRADE_FWD_BIT_CURRENT (1)
#define FW_UPGRADE_FWD_BIT_TRIAL   (2)

#define FW_UPGRADE_FWD_BOOT_TYPE_GOLDEN  (1 << FW_UPGRADE_FWD_BIT_GOLDEN)
#define FW_UPGRADE_FWD_BOOT_TYPE_CURRENT (1 << FW_UPGRADE_FWD_BIT_CURRENT)
#define FW_UPGRADE_FWD_BOOT_TYPE_TRIAL   (1 << FW_UPGRADE_FWD_BIT_TRIAL)

#if !defined(SBL_IMG_ID)
#define SBL_IMG_ID 1 /* SBL Image */
#endif

#if !defined(FS1_IMG_ID)
#define FS1_IMG_ID 5 /* Filesystem Image #1 */
#endif

#if !defined(APP_IMG_ID)
#define APP_IMG_ID 10 /* Application SubSystem Firmware Image */
#endif

#if !defined(RAMDUMP_IMG_ID)
#define RAMDUMP_IMG_ID 99 /* 0x63 -- RAMDUMP partition ID */
#endif

#if !defined(USERDATA_IMG_ID)
#define USERDATA_IMG_ID 101 /* 0x65 -- USERDATA partition ID */
#endif

/* 128..195 are reserved for QCOM IDs not known to CoreTech software */
#if !defined(FS2_IMG_ID)
#define FS2_IMG_ID 128 /* 0x80 -- Filesystem Image #2 */
#endif

#if !defined(UNUSED_IMG_ID)
#define UNUSED_IMG_ID 129 /* 0x81 -- Unused partition ID; never looked up */
#endif

/**********************************************************************************************************/
/* Type Declarations																                      */
/**********************************************************************************************************/
/**
 *  Enumeration that represents the valid error codes that can be returned by fw upgrade APIs
 */
typedef enum {
    FW_UPGRADE_OK_E = 0,                            /**< Operation performed successfully. */
    FW_UPGRADE_ERROR_E = 1,                         /**< Operation failed. */
    FW_UPGRADE_ERR_INVALID_PARAM_E,                 /**< Invalid parameter. */
    FW_UPGRADE_ERR_NOT_INIT_E,                      /**< Invalid parameter. */
    FW_UPGRADE_ERR_INCOMPLETE_E,                    /**< Operation is incomplete. */
    FW_UPGRADE_ERR_SESSION_IN_PROGRESS_E,           /**< Firmware upgrade session is inprogress. */
    FW_UPGRADE_ERR_SESSION_NOT_START_E,             /**< Firmware upgrade session is not started. */
    FW_UPGRADE_ERR_SESSION_NOT_READY_FOR_SUSPEND_E, /**< Firmware upgrade session is not ready to enter the Suspend
                                                       state. */
    FW_UPGRADE_ERR_SESSION_NOT_SUSPEND_E,           /**< Firmware upgrade session is not in the Suspend state. */
    FW_UPGRADE_ERR_SESSION_RESUME_NOT_SUPPORT_E, /**< Firmware upgrade session resume is not supported by the plugin. */
    FW_UPGRADE_ERR_SESSION_CANCELLED_E,          /**< Firmware upgrade session was cancelled. */
    FW_UPGRADE_ERR_SESSION_SUSPEND_E,            /**< Firmware upgrade session was suspended. */
    FW_UPGRADE_ERR_INTERFACE_NAME_TOO_LONG_E,    /**< Interface name is too long. */
    FW_UPGRADE_ERR_URL_TOO_LONG_E,               /**< URL is too long. */
    FW_UPGRADE_ERR_FLASH_NOT_SUPPORT_FW_UPGRADE_E, /**< Not supported firmware upgrade. */
    FW_UPGRADE_ERR_FLASH_INIT_TIMEOUT_E,           /**< Flash initialization timeout. */
    FW_UPGRADE_ERR_FLASH_READ_FAIL_E,              /**< Flash read failure. */
    FW_UPGRADE_ERR_FLASH_WRITE_FAIL_E,             /**< Flash write failure. */
    FW_UPGRADE_ERR_FLASH_ERASE_FAIL_E,             /**< Flash erase failure. */
    FW_UPGRADE_ERR_FLASH_NOT_ENOUGH_SPACE_E,       /**< Not enough free space in flash. */
    FW_UPGRADE_ERR_FLASH_CREATE_PARTITION_E,       /**< Partition creation failure. */
    FW_UPGRADE_ERR_FLASH_IMAGE_NOT_FOUND_E,        /**< Partition image was not found. */
    FW_UPGRADE_ERR_FLASH_ERASE_PARTITION_E,        /**< Partition erase failure. */
    FW_UPGRADE_ERR_FLASH_WRITE_PARTITION_E,        /**< Partition write failure. */
    FW_UPGRADE_ERR_GET_PARTITION_NULL_E,           /**< NULL partition. */
    FW_UPGRADE_ERR_REACH_MAX_IMAGE_ENTRY_E,        /**< Reach max image entry. */
    FW_UPGRADE_ERR_IMAGE_UNUSED_E,                 /**< Image entry is not used. */
    FW_UPGRADE_ERR_IMAGE_NOT_FOUND_E,              /**< Image not found failure. */
    FW_UPGRADE_ERR_IMAGE_DOWNLOAD_FAIL_E,          /**< Image download failure. */
    FW_UPGRADE_ERR_INCORRECT_IMAGE_CHECKSUM_E,     /**< Incorrect image checksum failure. */
    FW_UPGRADE_ERR_SERVER_RSP_TIMEOUT_E,           /**< Server communication timeout. */
    FW_UPGRADE_ERR_INVALID_FILENAME_E,             /**< Image file name is invalid. */
    FW_UPGRADE_ERR_INCORRECT_IMAGE_HDR_E,          /**< Firmware upgrade image header is invalid. */
    FW_UPGRADE_ERR_INSUFFICIENT_MEMORY_E,          /**< Not enough memory. */
    FW_UPGRADE_ERR_INCORRECT_SIGNATURE_E,          /**< Firmware upgrade image signature is invalid. */
    FW_UPGRADE_ERR_INCORRCT_VERSION_E,             /**< Firmware upgrade image version is invalid. */
    FW_UPGRADE_ERR_INCORRECT_NUM_IMAGES_E,         /**< Firmware upgrade image number of images is invalid. */
    FW_UPGRADE_ERR_INCORRECT_IMAGE_LENGTH_E,       /**< Firmware upgrade image length is invalid. */
    FW_UPGRADE_ERR_INCORRECT_HASH_TYPE_E,          /**< Firmware upgrade image hash type is invalid. */
    FW_UPGRADE_ERR_INCORRECT_IMAGE_ID_E,           /**< Firmware upgrade image ID is invalid. */
    FW_UPGRADE_ERR_SBL_ONLY_NOT_SUPPORT_E,         /**< Upgrade SBL only is not supported. */
    FW_UPGRADE_ERR_SBL_NOT_SUPPORT_UPGRADE_E,      /**< SBL upgrade is not supported. */
    FW_UPGRADE_ERR_SBL_NOT_ENOUGH_SPACE_E,         /**< No enough memory for SBL upgrade. */
    FW_UPGRADE_ERR_INVALID_FDT_E,                  /**< Invalid FDT. */
    FW_UPGRADE_ERR_BATTERY_LEVEL_TOO_LOW_E,        /**< Battery level is too low. */
    FW_UPGRADE_ERR_CRYPTO_FAIL_E,                  /**< Crypto check failure. */
    FW_UPGRADE_ERR_PLUGIN_ENTRY_EMPTY_E,           /**< Firmware upgrade plugin callback is empty. */
    FW_UPGRADE_ERR_TRIAL_IS_RUNNING_E,             /**< Trial image is running */
    FW_UPGRADE_ERR_FILE_NOT_FOUND_E,               /**< File was not found. */
    FW_UPGRADE_ERR_FILE_OPEN_ERROR_E,              /**< Open file failure. */
    FW_UPGRADE_ERR_FILE_NAME_TOO_LONG_E,           /**< File name is too long. */
    FW_UPGRADE_ERR_FILE_WRITE_ERROR_E,             /**< Write file failure. */
    FW_UPGRADE_ERR_MOUNT_FILE_SYSTEM_ERROR_E,      /**< Mount file system failure. */
    FW_UPGRADE_ERR_CREATE_THREAD_ERROR_E,          /**< Firmware upgrade create thread failure */
    FW_UPGRADE_ERR_PRESERVE_LAST_FAILED_E,
} fw_upgrade_status_code_t;

/**
 *  Enumeration that represents FWD infomation.
 */
typedef enum {
    FW_UPGRADE_FWD_MAGIC_E,
    FW_UPGRADE_FWD_VERSION_E,
    FW_UPGRADE_FWD_RANK_E,
    FW_UPGRADE_FWD_STATUS_E,
    FW_UPGRADE_FWD_TOTAL_IMAGE_E,
} fw_upgrade_fwd_info_t;

/**
 *  Defines an opaque Flash Firmware Partition Handle type.
 */
typedef void *fu_part_hdl_t;

/**
 *  Defines partition structure.
 */
typedef struct fu_partition_client {
    uint8_t ref_count;
    uint8_t fwd_idx;
    uint8_t img_idx;
    uint32_t img_id;
    uint32_t img_version;
    uint32_t img_start;
    uint32_t img_size;
} fu_partition_client_t;

/**********************************************************************************************************/
/* Function Declarations																                  */
/**********************************************************************************************************/
fw_upgrade_status_code_t fw_upgrade_init(void);

fw_upgrade_status_code_t fw_upgrade_get_mem_block_size(uint32_t *size);

fw_upgrade_status_code_t fw_upgrade_erase_fwd(uint8_t fwd_idx);

fw_upgrade_status_code_t fw_upgrade_set_fwd_info(uint8_t fwd_idx, fw_upgrade_fwd_info_t info, uint8_t *value);

fw_upgrade_status_code_t fw_upgrade_get_fwd_info(uint8_t fwd_idx, fw_upgrade_fwd_info_t info, uint8_t *result);

uint8_t fw_upgrade_get_active_fwd(uint32_t *fwd_boot_type, uint32_t *valid_fwd);

fw_upgrade_status_code_t fw_upgrade_accept_trial_fwd(void);

fw_upgrade_status_code_t fw_upgrade_reject_trial_fwd(void);

fw_upgrade_status_code_t fw_upgrade_select_trial_fwd(uint8_t *fwd_index, uint32_t *start_address, uint32_t *size);

fw_upgrade_status_code_t fw_upgrade_get_current_index(uint8_t *current);

fw_upgrade_status_code_t fw_upgrade_get_trial_active_fwd_index(uint8_t *trial, uint8_t *current, uint32_t *rank);

fw_upgrade_status_code_t fw_upgrade_partition_handle_validation(fu_part_hdl_t handle);

fw_upgrade_status_code_t fw_upgrade_create_partition(uint8_t fwd_idx, uint32_t id, uint32_t version, uint32_t start,
                                                     uint32_t size, fu_part_hdl_t *hdl);

fw_upgrade_status_code_t fw_upgrade_first_partition(uint8_t fwd_idx, fu_part_hdl_t *hdl);

fw_upgrade_status_code_t fw_upgrade_next_partition(fu_part_hdl_t curr, fu_part_hdl_t *hdl);

fw_upgrade_status_code_t fw_upgrade_find_partition(uint8_t fwd_idx, uint32_t id, fu_part_hdl_t *hdl);

fw_upgrade_status_code_t fw_upgrade_set_image_id(fu_part_hdl_t *hdl, uint32_t id);

fw_upgrade_status_code_t fw_upgrade_set_image_version(fu_part_hdl_t *hdl, uint32_t version);

fw_upgrade_status_code_t fw_upgrade_set_image_size(fu_part_hdl_t *hdl, uint32_t size);

fw_upgrade_status_code_t fw_upgrade_erase_partition(fu_part_hdl_t hdl, uint32_t offset, uint32_t nbytes);

fw_upgrade_status_code_t fw_upgrade_write_partition(fu_part_hdl_t hdl, uint32_t offset, char *buf, uint32_t nbytes);

fw_upgrade_status_code_t fw_upgrade_read_partition(fu_part_hdl_t hdl, uint32_t offset, char *buf, uint32_t max_bytes,
                                                   uint32_t *nbytes);

fw_upgrade_status_code_t fw_upgrade_select_sbl_trial_fde(uint32_t *sbl_idx, uint32_t *start_address, uint32_t *size);

fw_upgrade_status_code_t fw_upgrade_set_sbl_trial_fde(uint32_t sbl_trial_idx, uint32_t version);

fw_upgrade_status_code_t fw_upgrade_set_force_boot(uint8_t force_boot_fwd);

#endif /* _FW_UPGRADE_MEM_H */
