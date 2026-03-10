/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*****************************************************************************************************************************/
/*                                                                                                                           */
/*       Firmware Upgrade Mem */
/*                                                                                                                           */
/*****************************************************************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include "fw_upgrade.h"
#include "fw_upgrade_mem.h"

LOG_MODULE_DECLARE(qcom_img_mgmt, CONFIG_QCOM_IMG_MGMT_LOG_LEVEL);

/**********************************************************************************************************/
/* Preprocessor Definitions and Constants                                                                 */
/**********************************************************************************************************/
#if 0
extern uint32_t _ln_FDT_Start_Addr;
extern uint32_t _ln_SBL_MEM_Size;
#define FDT_START    ((void *)&_ln_FDT_Start_Addr)
#define SBL_MEM_SIZE ((void *)&_ln_SBL_MEM_Size)
#endif

#define FW_UPGRADE_MAX_FWD     (3)
#define FW_UPGRADE_INVALID_FWD (0xFF)

#define FW_UPGRADE_MAX_HANDLES (10)

#ifndef offsetof
#define offsetof(TYPE, MEMBER) ((uint32_t) & ((TYPE *)0)->MEMBER)
#endif

#define PARTITION_CLIENT_HANDLE_START &partition_handles[0]
#define PARTITION_CLIENT_HANDLE_END   &partition_handles[FW_UPGRADE_MAX_HANDLES - 1]

/**********************************************************************************************************/
/* Type and definitions                                                                                   */
/**********************************************************************************************************/
typedef struct {
    uint32_t mem_start;
    uint32_t mem_size;
    uint32_t fwd_region_size;
    uint32_t block_size;
} fw_upgrade_mem_info_t;

/**********************************************************************************************************/
/* Type and definitions of FDT mainly used by application                                                 */
/**********************************************************************************************************/
/* Maximum FW Image entries supported in the FWD */
#define MAX_FW_IMAGE_ENTRIES 30
/* FWD_SIGNATURE (=0x46574454 "FWDT") */
#define FW_UPGRADE_MAGIC_V1 0x54445746

/* FW Image Entry as described in the FW Descriptor Table of APP */
typedef struct {
    uint32 image_id;
    uint32 start_block;
    uint32 total_blocks;
    uint32 img_version;
} image_entry;

/* Firmware Descriptor Table in flash mainly for APP. 512 bytes in size */
typedef struct {
    uint32 signature;
    uint32 version;
    uint32 rank;
    uint8 status;
    uint8 total_images;
    uint8 reserved[18];
    image_entry image_entries[MAX_FW_IMAGE_ENTRIES];
} fwd;

/**********************************************************************************************************/
/* Type and definitions of FDT mainly used by bootloader                                                  */
/**********************************************************************************************************/
#define FDT_SIGNATURE 0x54445746
#define FDT_SIZE      1024
#define FDE_SIZE      32
#define FDE_START_IDX 0
#define FDT_MAX_ENTRY ((FDT_SIZE / FDE_SIZE) - 1)

/* Image id in firmware descriptor entry. */
typedef enum {
    FDE_IMG_ID_SBL = 0,
    FDE_IMG_ID_PBL_PATCH,
    FDE_IMG_ID_APP,
    FDE_IMG_ID_BDF,
} fde_img_id;

/* Image rank in firmware descriptor entry. */
typedef enum {
    FDE_IMG_RANK_AGED = 0,
    FDE_IMG_RANK_GOLDEN,
    FDE_IMG_RANK_CURRENT,
    FDE_IMG_RANK_TRIAL,
} fde_img_rank;

/* Image state in firmware descriptor entry. */
typedef enum {
    FDE_IMG_STATE_INVALID = 0,
    FDE_IMG_STATE_VALID,
    FDE_IMG_STATE_ABORTED,
    FDE_IMG_STATE_NEW,
    FDE_IMG_STATE_VERIFY_PENDING,
} fde_img_state;

/* FW Descriptor Header as described in the FW Descriptor Table. */
typedef struct {
    uint32_t signature;
    uint32_t num_fde;
    uint8_t reserve[24];
} fdt_hdr;

/* FW Descriptor Entry as described in the FW Descriptor Table. */
typedef struct {
    uint32_t id;
    uint32_t rank;
    uint32_t format;
    uint32_t addr;
    uint32_t state;
    uint32_t version;
    uint8_t reserve[8];
} fdt_entry;

/* Firmware Descriptor Table in FDT mainly for SBL. */
typedef struct {
    fdt_hdr header;
    fdt_entry fde[0];
} fdt_s;

/**********************************************************************************************************/
/* Globals                                                                                                */
/**********************************************************************************************************/
boolean fu_initialized = FALSE;
fu_partition_client_t partition_handles[FW_UPGRADE_MAX_HANDLES];
fw_upgrade_mem_info_t *fw_upgrade_mem_params = NULL;

/**********************************************************************************************************/
/* Internal Functions                                                                                     */
/**********************************************************************************************************/
/* Forward declaration */
static uint8_t fw_upgrade_get_partition_base_on_golden(void);

static fw_upgrade_status_code_t fw_upgrade_validation()
{
    if (fu_initialized == TRUE) {
        return FW_UPGRADE_OK_E;
    } else {
        return FW_UPGRADE_ERROR_E;
    }
}

static fu_partition_client_t *fw_upgrade_get_partition_client_entry()
{
    uint32_t i;

    i = 0;
    while (i < FW_UPGRADE_MAX_HANDLES) {
        if (partition_handles[i].ref_count == 0) {
            break;
        }
        i++;
    }

    /*  no entry available */
    if (i >= FW_UPGRADE_MAX_HANDLES) {
        return NULL;
    } else {
        return (&partition_handles[i]);
    }
}

static fw_upgrade_status_code_t fw_upgrade_mem_parameters_validation(fu_part_hdl_t handle, uint32_t byte_offset,
                                                                     uint32_t byte_count, void *buffer)
{
    uint32_t limit = 0;
    fw_upgrade_status_code_t ret = FW_UPGRADE_OK_E;
    fu_partition_client_t *private = NULL;

    if ((handle == NULL) || (buffer == NULL) || (byte_count == 0)) {
        ret = FW_UPGRADE_ERR_INVALID_PARAM_E;
        goto end1;
    }

    if (fw_upgrade_partition_handle_validation(handle) != FW_UPGRADE_OK_E) {
        ret = FW_UPGRADE_ERR_INVALID_PARAM_E;
        goto end1;
    }

private
    = (fu_partition_client_t *)handle;

    limit = private->img_start + private->img_size;

    /* Boundary Check */
    if ((private->img_start + byte_offset + byte_count) > limit) {
        ret = FW_UPGRADE_ERR_INVALID_PARAM_E;
        goto end1;
    }

end1:
    return ret;
}

/*
 * this function is to get the flash start address for the first image after FS1 and FS2
 */
static uint32_t fw_upgrade_get_first_programming_image_start_address(void)
{
    fu_part_hdl_t hdl;
    uint32_t img1_size = 0, img1_start = 0, img_start = 0;
    uint8_t current;

    current = fw_upgrade_get_active_fwd(NULL, NULL);

    /* get FS1 info */
    if (fw_upgrade_find_partition(current, FS1_IMG_ID, &hdl) == FW_UPGRADE_OK_E) {
        img1_size = ((fu_partition_client_t *)hdl)->img_size;
        img1_start = ((fu_partition_client_t *)hdl)->img_start;
        ((fu_partition_client_t *)hdl)->ref_count = 0;
        img1_start += img1_size;
        if (img1_start > img_start) {
            img_start = img1_start;
        }
    }

    /* get FS2 info */
    if (fw_upgrade_find_partition(current, FS2_IMG_ID, &hdl) == FW_UPGRADE_OK_E) {
        img1_size = ((fu_partition_client_t *)hdl)->img_size;
        img1_start = ((fu_partition_client_t *)hdl)->img_start;
        ((fu_partition_client_t *)hdl)->ref_count = 0;
        img1_start += img1_size;
        if (img1_start > img_start) {
            img_start = img1_start;
        }
    }

    /* get RAMDUMP info */
    if (fw_upgrade_find_partition(current, RAMDUMP_IMG_ID, &hdl) == FW_UPGRADE_OK_E) {
        img1_size = ((fu_partition_client_t *)hdl)->img_size;
        img1_start = ((fu_partition_client_t *)hdl)->img_start;
        ((fu_partition_client_t *)hdl)->ref_count = 0;
        img1_start += img1_size;
        if (img1_start > img_start) {
            img_start = img1_start;
        }
    }

    /* get USER DATA info */
    if (fw_upgrade_find_partition(current, USERDATA_IMG_ID, &hdl) == FW_UPGRADE_OK_E) {
        img1_size = ((fu_partition_client_t *)hdl)->img_size;
        img1_start = ((fu_partition_client_t *)hdl)->img_start;
        ((fu_partition_client_t *)hdl)->ref_count = 0;
        img1_start += img1_size;
        if (img1_start > img_start) {
            img_start = img1_start;
        }
    }

    /*  make sure the start address should be bigger than FWDs */
    img1_start = FW_UPGRADE_MAX_FWD * fw_upgrade_mem_params->fwd_region_size;
    if (img1_start > img_start) {
        img_start = img1_start;
    }

    return img_start;
}

static fw_upgrade_status_code_t fw_upgrade_get_imageSet_end_addr(uint8_t fwd_num, uint32_t *endAddr)
{
    fw_upgrade_status_code_t ret = FW_UPGRADE_ERROR_E;
    uint32_t magic = 0, rtnAddr;
    fu_part_hdl_t hdl = NULL, hdl_next;
    fu_partition_client_t *client_entry;

    fw_upgrade_get_fwd_info(fwd_num, FW_UPGRADE_FWD_MAGIC_E, (uint8_t *)&magic);
    if (magic == FW_UPGRADE_MAGIC_V1) {
        if (fw_upgrade_first_partition(fwd_num, &hdl) == FW_UPGRADE_OK_E) {
            client_entry = (fu_partition_client_t *)hdl;
            /* SBL image is located in a different area. */
            if (client_entry->img_id != SBL_IMG_ID) {
                *endAddr = client_entry->img_start + client_entry->img_size;
            } else {
                *endAddr = 0;
            }

            while (fw_upgrade_next_partition(hdl, &hdl_next) == FW_UPGRADE_OK_E) {
                client_entry->ref_count = 0;
                hdl = hdl_next;
                client_entry = (fu_partition_client_t *)hdl;
                if (client_entry->img_id != SBL_IMG_ID) {
                    rtnAddr = client_entry->img_start + client_entry->img_size;
                    if (rtnAddr > *endAddr) {
                        *endAddr = rtnAddr;
                    }
                }
            }
            client_entry->ref_count = 0;
            ret = FW_UPGRADE_OK_E;
        }
    }

    return ret;
}

static fw_upgrade_status_code_t fw_upgrade_reject_sbl_trial_fde()
{
    fw_upgrade_status_code_t ret = FW_UPGRADE_OK_E;
    fdt_s *fdt;
    fdt_entry *fde;
    uint32_t i, sbl_trial_idx = 0xFF;
    fdt_entry update_fde;

    fdt = (fdt_s *)FDT_START;

    if (fdt == NULL || fdt->header.signature != FDT_SIGNATURE || fdt->header.num_fde == 0 ||
        fdt->header.num_fde > FDT_MAX_ENTRY) {
        return FW_UPGRADE_ERR_INVALID_FDT_E;
    }

    fde = fdt->fde;

    for (i = 0; i < fdt->header.num_fde; i++) {
        if (fde[i].id == FDE_IMG_ID_SBL && fde[i].rank == FDE_IMG_RANK_TRIAL) {
            sbl_trial_idx = i;
            break;
        }
    }

    if (sbl_trial_idx == 0xFF) {
        return FW_UPGRADE_ERR_INVALID_FDT_E;
    }

    memscpy(&update_fde, sizeof(fdt_entry), &(fdt->fde[sbl_trial_idx]), sizeof(fdt_entry));
    update_fde.rank = FDE_IMG_RANK_AGED;
    update_fde.state = FDE_IMG_STATE_INVALID;

    if (nt_rram_write(((uint32_t) & (fdt->fde[sbl_trial_idx])), (void *)&update_fde, sizeof(fdt_entry)) != 0) {
        return FW_UPGRADE_ERROR_E;
    }

    return ret;
}

/**********************************************************************************************************/
/*                                                                                                        */
/**********************************************************************************************************/
fw_upgrade_status_code_t fw_upgrade_init(void)
{
    uint32_t i;
    flash_config_data_t *flash_config;

    /* FU init is done already */
    if ((fu_initialized) && (flash_adapter_get_config() != NULL)) {
        return FW_UPGRADE_OK_E;
    }

    /* flash init */
    if (flash_adapter_init() != FLASH_DEVICE_DONE) {
        return FW_UPGRADE_ERROR_E;
    }

    flash_config = flash_adapter_get_config();
    if (flash_config == NULL) {
        return FW_UPGRADE_ERROR_E;
    }

    if (fw_upgrade_mem_params == NULL) {
        fw_upgrade_mem_params = k_malloc(sizeof(fw_upgrade_mem_info_t));
        if (fw_upgrade_mem_params == NULL) {
            return FW_UPGRADE_ERR_INSUFFICIENT_MEMORY_E;
        }
        LOG_DBG("Allocated fw_upgrade_mem_params at %p", fw_upgrade_mem_params);
    }

    memset(fw_upgrade_mem_params, 0, sizeof(fw_upgrade_mem_info_t));

    /* Get information about the device */
    fw_upgrade_mem_params->mem_start = 0;
    fw_upgrade_mem_params->mem_size = flash_config->density_in_blocks * BLOCK_SIZE_IN_BYTES;
    fw_upgrade_mem_params->fwd_region_size = BLOCK_SIZE_IN_BYTES;
    fw_upgrade_mem_params->block_size = BLOCK_SIZE_IN_BYTES;

    for (i = 0; i < FW_UPGRADE_MAX_HANDLES; i++) {
        partition_handles[i].ref_count = 0;
    }

	/* here, we add auto adjustment to decide the 2/3 fwd mode. */
	CONFIG_FW_UPGRADE_FWD_SUPPORT_NUM = fw_upgrade_get_partition_base_on_golden();
	if((CONFIG_FW_UPGRADE_FWD_SUPPORT_NUM != 2) && (CONFIG_FW_UPGRADE_FWD_SUPPORT_NUM != 3))
	{
		return FW_UPGRADE_ERR_FLASH_NOT_SUPPORT_FW_UPGRADE_E;
	}

    fu_initialized = TRUE;
    return FW_UPGRADE_OK_E;
}

fw_upgrade_status_code_t fw_upgrade_get_mem_block_size(uint32_t *size)
{
    if (fw_upgrade_validation() != FW_UPGRADE_OK_E) {
        return FW_UPGRADE_ERR_NOT_INIT_E;
    }

    /* Get block size */
    *size = fw_upgrade_mem_params->block_size;
    return FW_UPGRADE_OK_E;
}

fw_upgrade_status_code_t fw_upgrade_erase_fwd(uint8_t fwd_idx)
{
    fw_upgrade_status_code_t ret;

    if (fw_upgrade_validation() != FW_UPGRADE_OK_E) {
        ret = FW_UPGRADE_ERR_NOT_INIT_E;
        goto erase_FWD_end;
    }

    if (fwd_idx >= FW_UPGRADE_MAX_FWD) {
        ret = FW_UPGRADE_ERR_INVALID_PARAM_E;
        goto erase_FWD_end;
    }

    LOG_INF("Erasing FWD index=%u", fwd_idx);
    if (flash_adapter_erase(FLASH_BLOCK_ERASE_E, fwd_idx, 1, NULL, NULL) != FLASH_DEVICE_DONE) {
        ret = FW_UPGRADE_ERROR_E;
    } else {
        ret = FW_UPGRADE_OK_E;
    }

erase_FWD_end:
    return ret;
}

fw_upgrade_status_code_t fw_upgrade_get_fwd_info_addr(uint8_t fwd_idx, fw_upgrade_fwd_info_t info, uint32_t *start_addr)
{
    fw_upgrade_status_code_t ret = FW_UPGRADE_OK_E;
    uint32_t offset, byte_cnt;
    fwd fwdInfo;

    if (fw_upgrade_validation() != FW_UPGRADE_OK_E) {
        ret = FW_UPGRADE_ERR_NOT_INIT_E;
        goto set_info_end;
    }

    if (fwd_idx >= FW_UPGRADE_MAX_FWD) {
        ret = FW_UPGRADE_ERR_INVALID_PARAM_E;
        goto set_info_end;
    }

    switch (info) {
        case FW_UPGRADE_FWD_MAGIC_E:
            offset = offsetof(fwd, signature);
            byte_cnt = sizeof(fwdInfo.signature);
            break;
        case FW_UPGRADE_FWD_VERSION_E:
            offset = offsetof(fwd, version);
            byte_cnt = sizeof(fwdInfo.version);
            break;
        case FW_UPGRADE_FWD_RANK_E:
            offset = offsetof(fwd, rank);
            byte_cnt = sizeof(fwdInfo.rank);
            break;
        case FW_UPGRADE_FWD_STATUS_E:
            offset = offsetof(fwd, status);
            byte_cnt = sizeof(fwdInfo.status);
            break;
        case FW_UPGRADE_FWD_TOTAL_IMAGE_E:
            offset = offsetof(fwd, total_images);
            byte_cnt = sizeof(fwdInfo.total_images);
            break;
        default:
            ret = FW_UPGRADE_ERR_INVALID_PARAM_E;
            break;
    }

    if ((ret == FW_UPGRADE_OK_E) && (start_addr != NULL)) {
        *start_addr = fw_upgrade_mem_params->mem_start + fw_upgrade_mem_params->fwd_region_size * fwd_idx + offset;
        return ret;
    }

set_info_end:
    return ret;
}

fw_upgrade_status_code_t fw_upgrade_set_fwd_info(uint8_t fwd_idx, fw_upgrade_fwd_info_t info, uint8_t *value)
{
    fw_upgrade_status_code_t ret = FW_UPGRADE_OK_E;
    uint32_t start_addr, offset, byte_cnt;
    fwd fwdInfo;

    if (fw_upgrade_validation() != FW_UPGRADE_OK_E) {
        ret = FW_UPGRADE_ERR_NOT_INIT_E;
        goto set_info_end;
    }

    if (fwd_idx >= FW_UPGRADE_MAX_FWD) {
        ret = FW_UPGRADE_ERR_INVALID_PARAM_E;
        goto set_info_end;
    }

    switch (info) {
        case FW_UPGRADE_FWD_MAGIC_E:
            offset = offsetof(fwd, signature);
            byte_cnt = sizeof(fwdInfo.signature);
            break;
        case FW_UPGRADE_FWD_VERSION_E:
            offset = offsetof(fwd, version);
            byte_cnt = sizeof(fwdInfo.version);
            break;
        case FW_UPGRADE_FWD_RANK_E:
            offset = offsetof(fwd, rank);
            byte_cnt = sizeof(fwdInfo.rank);
            break;
        case FW_UPGRADE_FWD_STATUS_E:
            offset = offsetof(fwd, status);
            byte_cnt = sizeof(fwdInfo.status);
            break;
        case FW_UPGRADE_FWD_TOTAL_IMAGE_E:
            offset = offsetof(fwd, total_images);
            byte_cnt = sizeof(fwdInfo.total_images);
            break;
        default:
            ret = FW_UPGRADE_ERR_INVALID_PARAM_E;
            break;
    }

    if (ret == FW_UPGRADE_OK_E) {
        start_addr = fw_upgrade_mem_params->mem_start + fw_upgrade_mem_params->fwd_region_size * fwd_idx + offset;

        if (flash_adapter_write(start_addr, byte_cnt, value, NULL, NULL) != FLASH_DEVICE_DONE) {
            ret = FW_UPGRADE_ERROR_E;
        }
    }

set_info_end:
    return ret;
}

fw_upgrade_status_code_t fw_upgrade_get_fwd_info(uint8_t fwd_idx, fw_upgrade_fwd_info_t info, uint8_t *result)
{
    fw_upgrade_status_code_t ret = FW_UPGRADE_OK_E;
    uint32_t start_addr, offset, byte_cnt;
    fwd fwdInfo;

    if (fw_upgrade_validation() != FW_UPGRADE_OK_E) {
        ret = FW_UPGRADE_ERR_NOT_INIT_E;
        goto get_info_end;
    }

    if (fwd_idx >= FW_UPGRADE_MAX_FWD) {
        ret = FW_UPGRADE_ERR_INVALID_PARAM_E;
        goto get_info_end;
    }

    switch (info) {
        case FW_UPGRADE_FWD_MAGIC_E:
            offset = offsetof(fwd, signature);
            byte_cnt = sizeof(fwdInfo.signature);
            break;
        case FW_UPGRADE_FWD_VERSION_E:
            offset = offsetof(fwd, version);
            byte_cnt = sizeof(fwdInfo.version);
            break;
        case FW_UPGRADE_FWD_RANK_E:
            offset = offsetof(fwd, rank);
            byte_cnt = sizeof(fwdInfo.rank);
            break;
        case FW_UPGRADE_FWD_STATUS_E:
            offset = offsetof(fwd, status);
            byte_cnt = sizeof(fwdInfo.status);
            break;
        case FW_UPGRADE_FWD_TOTAL_IMAGE_E:
            offset = offsetof(fwd, total_images);
            byte_cnt = sizeof(fwdInfo.total_images);
            break;
        default:
            ret = FW_UPGRADE_ERR_INVALID_PARAM_E;
            break;
    }

    if (ret == FW_UPGRADE_OK_E) {
        start_addr = fw_upgrade_mem_params->mem_start + fw_upgrade_mem_params->fwd_region_size * fwd_idx + offset;
        if (flash_adapter_read(start_addr, byte_cnt, result, NULL, NULL) != FLASH_DEVICE_DONE) {
            ret = FW_UPGRADE_ERROR_E;
        }
    }

get_info_end:
    return ret;
}

uint8_t fw_upgrade_get_active_fwd(uint32_t *fwd_boot_type, uint32_t *valid_fwd)
{
    fdt_s *fdt;
    fdt_entry *fde;
    uint32_t i;
    uint8_t fwd_block = FW_UPGRADE_INVALID_FWD;

    fdt = (fdt_s *)FDT_START;

    if (fdt == NULL || fdt->header.signature != FDT_SIGNATURE || fdt->header.num_fde == 0 ||
        fdt->header.num_fde > FDT_MAX_ENTRY) {
        return FW_UPGRADE_INVALID_FWD;
    }

    fde = fdt->fde;
    for (i = 0; i < fdt->header.num_fde; i++) {
        if (fde[i].id == FDE_IMG_ID_APP) {
            if (fwd_boot_type != NULL) {
                if (fde[i].rank == FDE_IMG_RANK_GOLDEN) {
                    *fwd_boot_type = FW_UPGRADE_FWD_BOOT_TYPE_GOLDEN;
                } else if (fde[i].rank == FDE_IMG_RANK_CURRENT) {
                    *fwd_boot_type = FW_UPGRADE_FWD_BOOT_TYPE_CURRENT;
                } else if (fde[i].rank == FDE_IMG_RANK_TRIAL) {
                    *fwd_boot_type = FW_UPGRADE_FWD_BOOT_TYPE_TRIAL;
                } else {
                    fwd_block = FW_UPGRADE_INVALID_FWD;
                    break;
                }
            }
            if (valid_fwd != NULL) {
                *valid_fwd = (uint32_t)(fde[i].reserve[0]);
            }
            fwd_block = (uint8_t)fde[i].addr;
            break;
        }
    }

    return fwd_block;
}

static uint8_t fw_upgrade_get_partition_base_on_golden(void)
{
    fdt_s *fdt;
    fdt_entry *fde;
    uint32_t i;
    uint32_t start_addr;
    fwd fwdInfo;

    fdt = (fdt_s *)FDT_START;

    if (fdt == NULL || fdt->header.signature != FDT_SIGNATURE || fdt->header.num_fde == 0 ||
        fdt->header.num_fde > FDT_MAX_ENTRY) {
        return 1;
    }

    fde = fdt->fde;
    for (i = 0; i < fdt->header.num_fde; i++) {
        if (fde[i].id == FDE_IMG_ID_APP) {
                if (fde[i].rank == FDE_IMG_RANK_GOLDEN) {
                    return 3;
                }
        }
    }

	start_addr = fw_upgrade_mem_params->mem_start + fw_upgrade_mem_params->fwd_region_size * 0;
	if (flash_adapter_read(start_addr, sizeof(fwdInfo), &fwdInfo, NULL, NULL) != FLASH_DEVICE_DONE) {
		return 1;
	}

	if((fwdInfo.signature == FW_UPGRADE_MAGIC_V1) && (fwdInfo.rank == FW_UPGRADE_FWD_RANK_GOLDEN )) {
		return 3;
	}

    return 2;
}

fw_upgrade_status_code_t fw_upgrade_accept_trial_fwd(void)
{
    fw_upgrade_status_code_t ret;
    uint8_t trial, current, status;
    uint32_t rank, fwd_boot_type, config;

    /* reset wdog reset counter */
    /*  TODOl wdt reset */
    /*  qapi_System_WDTCount_Reset(); */

    ret = fw_upgrade_get_trial_active_fwd_index(&trial, &current, &rank);
    if (ret == FW_UPGRADE_OK_E) {
        fw_upgrade_get_active_fwd(&fwd_boot_type, NULL);
        /*  only allow active FWD is trial to accept */
        if (fwd_boot_type == FW_UPGRADE_FWD_BOOT_TYPE_TRIAL) {
            /*  set trial rank to the biggest number */
            rank += 1;
            ret = fw_upgrade_set_fwd_info(trial, FW_UPGRADE_FWD_RANK_E, (uint8_t *)&rank);
            config = CONFIG_FW_UPGRADE_FWD_SUPPORT_NUM;
            LOG_INF("FW upgrade config fwd num: %d, trial %d, current %d, rank %d", config, trial, current, rank);
            /*  set current FWD status to invalid except current FWD is golden */
            if ((ret == FW_UPGRADE_OK_E) && (config == 2 || (config == 3 && current != 0))) {
                status = FW_UPGRADE_FWD_STATUS_INVALID;
                ret = fw_upgrade_set_fwd_info(current, FW_UPGRADE_FWD_STATUS_E, &status);
            }
        } else {
            ret = FW_UPGRADE_ERROR_E;
        }
    }
    return ret;
}

fw_upgrade_status_code_t fw_upgrade_reject_trial_fwd(void)
{
    fw_upgrade_status_code_t ret;
    uint8_t trial, current, status;
    uint32_t rank;
    fu_part_hdl_t hdl;

    /* reset wdog reset counter */
    /*  TODOl wdt reset */
    /*  qapi_System_WDTCount_Reset(); */

    ret = fw_upgrade_get_trial_active_fwd_index(&trial, &current, &rank);
    if (ret == FW_UPGRADE_OK_E) {
        /*  set trial FWD status to invalid */
        status = FW_UPGRADE_FWD_STATUS_INVALID;
        ret = fw_upgrade_set_fwd_info(trial, FW_UPGRADE_FWD_STATUS_E, &status);
        /*  set trial FWD rank to 1 */
        if (ret == FW_UPGRADE_OK_E) {
            rank = 1;
            ret = fw_upgrade_set_fwd_info(trial, FW_UPGRADE_FWD_RANK_E, (uint8_t *)&rank);

            if (ret == FW_UPGRADE_OK_E) {
                ret = fw_upgrade_find_partition(trial, SBL_IMG_ID, &hdl);
                if (ret == FW_UPGRADE_OK_E) {
                    ret = fw_upgrade_reject_sbl_trial_fde();
                } else if (ret == FW_UPGRADE_ERR_REACH_MAX_IMAGE_ENTRY_E) {
                    ret = FW_UPGRADE_OK_E;
                }
            }
        }
    }
    return ret;
}

fw_upgrade_status_code_t fw_upgrade_select_trial_fwd(uint8_t *fwd_index, uint32_t *start_address, uint32_t *size)
{
    fw_upgrade_status_code_t ret = FW_UPGRADE_ERROR_E;
    uint32_t config, mem_size, mem_start, end_addr, rank;
    uint32_t img_start = 0;
    uint8_t trial, current;

    /* get FWD support num */
    config = CONFIG_FW_UPGRADE_FWD_SUPPORT_NUM;

    /*only support one partition, can't upgrade */
    if (config == 1) {
        return ret;
    }

    mem_size = fw_upgrade_mem_params->mem_size;
    mem_start = fw_upgrade_mem_params->mem_start;

    /* get the start addess of app image except file system image */
    img_start = fw_upgrade_get_first_programming_image_start_address();
    /* get current FWD index num and biggest rank num */
    fw_upgrade_get_trial_active_fwd_index(&trial, &current, &rank);

    if (config == 2) { /* support 2 FWDs case */
                       /* set trial FWD index num */
        *fwd_index = current == 0 ? 1 : 0;
        /* we need set size and start address align with block_size */
        mem_size = (mem_size - (img_start - mem_start)) / fw_upgrade_mem_params->block_size;
        *size = (mem_size / config * fw_upgrade_mem_params->block_size);
        *start_address = img_start + (*fwd_index) * (*size);
        if (*fwd_index == 1) {
            if (fw_upgrade_get_imageSet_end_addr(0, &end_addr) != FW_UPGRADE_OK_E) {
                /* can't get fwd info */
                return ret;
            }

            if (*start_address < end_addr) {
                *size = *size - (end_addr - *start_address);
                *start_address = end_addr;
            }
        }
    } else { /* support 3 (golgen+2) FWDs case */
        *fwd_index = current == 0 ? 1 : (current == 1) ? 2 : 1;
        if (fw_upgrade_get_imageSet_end_addr(0, &end_addr) != FW_UPGRADE_OK_E) {
            /* can't get fwd info */
            return ret;
        }

        /* we need set size and start address align with flash_block_size */
        mem_size = (mem_size - (end_addr - mem_start)) / fw_upgrade_mem_params->block_size;
        *size = (mem_size / (config - 1) * fw_upgrade_mem_params->block_size);
        *start_address = end_addr + (*fwd_index - 1) * (*size);
    }
    return FW_UPGRADE_OK_E;
}

fw_upgrade_status_code_t fw_upgrade_get_current_index(uint8_t *current)
{
    fw_upgrade_status_code_t ret = FW_UPGRADE_ERROR_E;
    uint32_t i, config, magic = 0, rank_l = 0;
    uint8_t status = 0, image_nums = 0;

    /* init return */
    *current = FW_UPGRADE_FWD_STATUS_UNUSED;

    /* init FW upgrade module */
    if (fw_upgrade_init() != FW_UPGRADE_OK_E)
        goto get_cur_end;

    /* we support 2 FWDs and 1 GLN+2FWDs mode */
    config = CONFIG_FW_UPGRADE_FWD_SUPPORT_NUM;

    /* verify FWD 1 and FWD 2 */
    for (i = 0; i < config; i++) {
        if (fw_upgrade_get_fwd_info(i, FW_UPGRADE_FWD_MAGIC_E, (uint8_t *)&magic) != FW_UPGRADE_OK_E) {
            goto get_cur_end;
        }
        if (fw_upgrade_get_fwd_info(i, FW_UPGRADE_FWD_RANK_E, (uint8_t *)&rank_l) != FW_UPGRADE_OK_E) {
            goto get_cur_end;
        }
        if (fw_upgrade_get_fwd_info(i, FW_UPGRADE_FWD_STATUS_E, (uint8_t *)&status) != FW_UPGRADE_OK_E) {
            goto get_cur_end;
        }
        if (fw_upgrade_get_fwd_info(i, FW_UPGRADE_FWD_TOTAL_IMAGE_E, (uint8_t *)&image_nums) != FW_UPGRADE_OK_E) {
            goto get_cur_end;
        }

        if ((magic == FW_UPGRADE_MAGIC_V1) && (status == FW_UPGRADE_FWD_STATUS_VALID) && (image_nums > 0) &&
            (image_nums != 0xff)) {
            if ((rank_l != FW_UPGRADE_FWD_RANK_TRIAL) && (rank_l > 0)) {
                *current = i;
                break;
            }
        }
    }

    /*  if *current is FW_UPGRADE_FWD_STATUS_UNUSED, it means we don't find current FWD */
    if (*current != FW_UPGRADE_FWD_STATUS_UNUSED)
        ret = FW_UPGRADE_OK_E;

get_cur_end:
    return ret;
}

fw_upgrade_status_code_t fw_upgrade_get_trial_active_fwd_index(uint8_t *trial, uint8_t *current, uint32_t *rank)
{
    fw_upgrade_status_code_t ret = FW_UPGRADE_ERROR_E;
    uint32_t config, magic = 0, rank_l = 0, i, s1, s2;
    uint8_t status = 0, image_nums = 0;

    /* init return */
    *trial = FW_UPGRADE_FWD_TRIAL_UNUSED;
    *current = 0;
    *rank = 0;

    /* init FW upgrade module */
    if (fw_upgrade_init() != FW_UPGRADE_OK_E) {
        goto cmd_get_index_end;
    }

    /* we support 2 FWDs and 1 GLN+2FWDs mode */
    config = CONFIG_FW_UPGRADE_FWD_SUPPORT_NUM;

    if (config == 1) {
        goto cmd_get_index_end;
    }

    /*  two FWDs */
    if (config == 2) {
        s1 = 0;
        s2 = 2;
    } else {  /*  three FWDs */
        if (fw_upgrade_get_fwd_info(0, FW_UPGRADE_FWD_RANK_E, (uint8_t *)&rank_l) != FW_UPGRADE_OK_E) {
            goto cmd_get_index_end;
        }
        *rank = rank_l;

        s1 = 1;
        s2 = 3;
    }

    /* verify FWD 1 and FWD 2 */
    for (i = s1; i < s2; i++) {
        if (fw_upgrade_get_fwd_info(i, FW_UPGRADE_FWD_MAGIC_E, (uint8_t *)&magic) != FW_UPGRADE_OK_E) {
            goto cmd_get_index_end;
        }
        if (fw_upgrade_get_fwd_info(i, FW_UPGRADE_FWD_RANK_E, (uint8_t *)&rank_l) != FW_UPGRADE_OK_E) {
            goto cmd_get_index_end;
        }
        if (fw_upgrade_get_fwd_info(i, FW_UPGRADE_FWD_STATUS_E, (uint8_t *)&status) != FW_UPGRADE_OK_E) {
            goto cmd_get_index_end;
        }
        if (fw_upgrade_get_fwd_info(i, FW_UPGRADE_FWD_TOTAL_IMAGE_E, (uint8_t *)&image_nums) != FW_UPGRADE_OK_E) {
            goto cmd_get_index_end;
        }

        if ((magic == FW_UPGRADE_MAGIC_V1) && (status == FW_UPGRADE_FWD_STATUS_VALID) && (image_nums > 0) &&
            (image_nums != 0xff)) {
            if (rank_l == FW_UPGRADE_FWD_RANK_TRIAL) {
                *trial = i;
            } else {
                if (rank_l > *rank) {
                    *rank = rank_l;
                    *current = i;
                }
            }
        }
    }

    /*  if *trial is QAPI_FU_FWD_TRIAL_UNUSED, it means we don't find trial FWD */
    if (*trial != FW_UPGRADE_FWD_TRIAL_UNUSED) {
        ret = FW_UPGRADE_OK_E;
    }

cmd_get_index_end:
    return ret;
}

fw_upgrade_status_code_t fw_upgrade_partition_handle_validation(fu_part_hdl_t handle)
{
    fu_partition_client_t *h = PARTITION_CLIENT_HANDLE_START;

    while (h <= PARTITION_CLIENT_HANDLE_END) {
        if (((fu_partition_client_t *)handle == h) && (h->ref_count > 0)) {
            return FW_UPGRADE_OK_E;
        }
        h++;
    }

    return FW_UPGRADE_ERROR_E;
}

fw_upgrade_status_code_t fw_upgrade_create_partition(uint8_t fwd_idx, uint32_t id, uint32_t version, uint32_t start,
                                                     uint32_t size, fu_part_hdl_t *hdl)
{
    fw_upgrade_status_code_t ret = FW_UPGRADE_OK_E;
    uint32_t start_addr, i;
    fwd fwd;
    image_entry img_entry;
    fu_partition_client_t *client_entry = NULL;

    if (fw_upgrade_validation() != FW_UPGRADE_OK_E) {
        ret = FW_UPGRADE_ERR_NOT_INIT_E;
        goto create_partition_end;
    }

    if (fwd_idx >= FW_UPGRADE_MAX_FWD) {
        ret = FW_UPGRADE_ERR_INVALID_PARAM_E;
        goto create_partition_end;
    }

    if (id != SBL_IMG_ID) {
        /*  check start addr and size */
        if ((start % fw_upgrade_mem_params->block_size) != 0 || (size % fw_upgrade_mem_params->block_size) != 0) {
            ret = FW_UPGRADE_ERR_INVALID_PARAM_E;
            goto create_partition_end;
        }
    }

    /*  get valid client entry */
    if ((client_entry = fw_upgrade_get_partition_client_entry()) == NULL) {
        ret = FW_UPGRADE_ERROR_E;
        goto create_partition_end;
    }

    start_addr = fw_upgrade_mem_params->mem_start + fw_upgrade_mem_params->fwd_region_size * fwd_idx;
    if (flash_adapter_read(start_addr, sizeof(fwd), (uint8_t *)&fwd, NULL, NULL) != FLASH_DEVICE_DONE) {
        ret = FW_UPGRADE_ERROR_E;
        goto create_partition_end;
    }

    /*  find valid free entry */
    for (i = 0; i < MAX_FW_IMAGE_ENTRIES; i++) {
        if (fwd.image_entries[i].image_id == FW_UPGRADE_FWD_IMAGE_UNUSED) {
            break;
        }
    }

    /*  there is no free image entry */
    if (i >= MAX_FW_IMAGE_ENTRIES) {
        ret = FW_UPGRADE_ERROR_E;
        goto create_partition_end;
    }

    start_addr = start_addr + 32 + i * sizeof(image_entry);

    memset((void *)&img_entry, 0xff, sizeof(img_entry));
    img_entry.image_id = id;
    memscpy(&img_entry.img_version, 4, &version, 4);
    if (id != SBL_IMG_ID) {
        img_entry.start_block = start / fw_upgrade_mem_params->block_size;
        img_entry.total_blocks = size / fw_upgrade_mem_params->block_size;
    } else {
        img_entry.start_block = start;
        img_entry.total_blocks = size;
    }

    if (flash_adapter_write(start_addr, sizeof(image_entry), (uint8_t *)&img_entry, NULL, NULL) != FLASH_DEVICE_DONE) {
        ret = FW_UPGRADE_ERROR_E;
        goto create_partition_end;
    }

    client_entry->fwd_idx = fwd_idx;
    client_entry->img_idx = i;
    client_entry->img_id = id;
    client_entry->img_version = version;
    client_entry->img_start = start;
    client_entry->img_size = size;
    client_entry->ref_count = 1;

    *hdl = client_entry;

create_partition_end:
    return ret;
}

fw_upgrade_status_code_t fw_upgrade_first_partition(uint8_t fwd_idx, fu_part_hdl_t *hdl)
{
    fw_upgrade_status_code_t ret = FW_UPGRADE_OK_E;
    uint32_t start_addr;
    fwd fwd = {0};
    fu_partition_client_t *client_entry = NULL;

    if (fw_upgrade_validation() != FW_UPGRADE_OK_E) {
        ret = FW_UPGRADE_ERR_NOT_INIT_E;
        goto first_partition_end;
    }

    if (fwd_idx >= FW_UPGRADE_MAX_FWD) {
        ret = FW_UPGRADE_ERR_INVALID_PARAM_E;
        goto first_partition_end;
    }

    if ((client_entry = fw_upgrade_get_partition_client_entry()) == NULL) {
        ret = FW_UPGRADE_ERROR_E;
        goto first_partition_end;
    }

    /*  read FWD */
    start_addr = fw_upgrade_mem_params->mem_start + fw_upgrade_mem_params->fwd_region_size * fwd_idx;
    if (flash_adapter_read(start_addr, sizeof(fwd), (uint8_t *)&fwd, NULL, NULL) != FLASH_DEVICE_DONE) {
        ret = FW_UPGRADE_ERROR_E;
        goto first_partition_end;
    }

    if (fwd.total_images == 0) {
        ret = FW_UPGRADE_ERROR_E;
        goto first_partition_end;
    }

    client_entry->fwd_idx = fwd_idx;
    client_entry->img_idx = 0;
    client_entry->img_id = fwd.image_entries[0].image_id;
    memscpy(&client_entry->img_version, 4, &fwd.image_entries[0].img_version, 4);
    if (client_entry->img_id != SBL_IMG_ID) {
        client_entry->img_start = fwd.image_entries[0].start_block * fw_upgrade_mem_params->block_size;
        client_entry->img_size = fwd.image_entries[0].total_blocks * fw_upgrade_mem_params->block_size;
    } else {
        client_entry->img_start = fwd.image_entries[0].start_block;
        client_entry->img_size = fwd.image_entries[0].total_blocks;
    }
    client_entry->ref_count = 1;

    *hdl = client_entry;

first_partition_end:
    return ret;
}

fw_upgrade_status_code_t fw_upgrade_next_partition(fu_part_hdl_t curr, fu_part_hdl_t *hdl)
{
    fw_upgrade_status_code_t ret = FW_UPGRADE_OK_E;
    uint32_t start_addr, idx;
    fwd fwd;
    fu_partition_client_t *h, *client_entry = NULL;

    if (fw_upgrade_validation() != FW_UPGRADE_OK_E) {
        ret = FW_UPGRADE_ERR_NOT_INIT_E;
        goto next_partition_end;
    }

    if (fw_upgrade_partition_handle_validation(curr) != FW_UPGRADE_OK_E) {
        ret = FW_UPGRADE_ERR_INVALID_PARAM_E;
        goto next_partition_end;
    }

    h = (fu_partition_client_t *)curr;

    if ((client_entry = fw_upgrade_get_partition_client_entry()) == NULL) {
        ret = FW_UPGRADE_ERR_GET_PARTITION_NULL_E;
        goto next_partition_end;
    }

    start_addr = fw_upgrade_mem_params->mem_start + fw_upgrade_mem_params->fwd_region_size * h->fwd_idx;
    if (flash_adapter_read(start_addr, sizeof(fwd), (uint8_t *)&fwd, NULL, NULL) != FLASH_DEVICE_DONE) {
        ret = FW_UPGRADE_ERR_FLASH_READ_FAIL_E;
        goto next_partition_end;
    }

    /*  check if is valid id */
    idx = h->img_idx + 1;
    if (idx >= MAX_FW_IMAGE_ENTRIES) {
        ret = FW_UPGRADE_ERR_REACH_MAX_IMAGE_ENTRY_E;
        goto next_partition_end;
    }

    if (fwd.image_entries[idx].image_id == FW_UPGRADE_FWD_IMAGE_UNUSED) {
        ret = FW_UPGRADE_ERR_IMAGE_UNUSED_E;
        goto next_partition_end;
    }

    client_entry->fwd_idx = h->fwd_idx;
    client_entry->img_idx = idx;
    client_entry->img_id = fwd.image_entries[idx].image_id;
    memscpy(&client_entry->img_version, 4, &fwd.image_entries[idx].img_version, 4);
    if (client_entry->img_id != SBL_IMG_ID) {
        client_entry->img_start = fwd.image_entries[idx].start_block * fw_upgrade_mem_params->block_size;
        client_entry->img_size = fwd.image_entries[idx].total_blocks * fw_upgrade_mem_params->block_size;
    } else {
        client_entry->img_start = fwd.image_entries[idx].start_block;
        client_entry->img_size = fwd.image_entries[idx].total_blocks;
    }
    client_entry->ref_count = 1;

    *hdl = client_entry;

next_partition_end:
    return ret;
}

fw_upgrade_status_code_t fw_upgrade_find_partition(uint8_t fwd_idx, uint32_t id, fu_part_hdl_t *hdl)
{
    fw_upgrade_status_code_t ret = FW_UPGRADE_OK_E;
    uint32_t start_addr, i;
    fwd fwd;
    fu_partition_client_t *client_entry = NULL;

    if (fw_upgrade_validation() != FW_UPGRADE_OK_E) {
        ret = FW_UPGRADE_ERR_NOT_INIT_E;
        goto find_partition_end;
    }

    if (fwd_idx >= FW_UPGRADE_MAX_FWD) {
        ret = FW_UPGRADE_ERR_REACH_MAX_IMAGE_ENTRY_E;
        goto find_partition_end;
    }

    if ((client_entry = fw_upgrade_get_partition_client_entry()) == NULL) {
        ret = FW_UPGRADE_ERR_GET_PARTITION_NULL_E;
        goto find_partition_end;
    }

    start_addr = fw_upgrade_mem_params->mem_start + fw_upgrade_mem_params->fwd_region_size * fwd_idx;
    if (flash_adapter_read(start_addr, sizeof(fwd), (uint8_t *)&fwd, NULL, NULL) != FLASH_DEVICE_DONE) {
        ret = FW_UPGRADE_ERR_FLASH_READ_FAIL_E;
        goto find_partition_end;
    }

    /*  find valid entry */
    for (i = 0; i < MAX_FW_IMAGE_ENTRIES; i++) {
        if (fwd.image_entries[i].image_id == id) {
            break;
        }
    }

    /*  there is no free image entry */
    if (i >= MAX_FW_IMAGE_ENTRIES) {
        ret = FW_UPGRADE_ERR_REACH_MAX_IMAGE_ENTRY_E;
        goto find_partition_end;
    }

    client_entry->fwd_idx = fwd_idx;
    client_entry->img_idx = i;
    client_entry->img_id = id;
    memscpy(&client_entry->img_version, 4, &fwd.image_entries[i].img_version, 4);
    if (client_entry->img_id != SBL_IMG_ID) {
        client_entry->img_start = fwd.image_entries[i].start_block * fw_upgrade_mem_params->block_size;
        client_entry->img_size = fwd.image_entries[i].total_blocks * fw_upgrade_mem_params->block_size;
    } else {
        client_entry->img_start = fwd.image_entries[i].start_block;
        client_entry->img_size = fwd.image_entries[i].total_blocks;
    }
    client_entry->ref_count = 1;

    *hdl = client_entry;

find_partition_end:
    return ret;
}

fw_upgrade_status_code_t fw_upgrade_set_image_id(fu_part_hdl_t *hdl, uint32_t id)
{
    fw_upgrade_status_code_t ret = FW_UPGRADE_OK_E;
    uint32_t start_addr, new_id;
    uint8_t fwd_idx;
    fu_partition_client_t *client_entry;

    if (fw_upgrade_validation() != FW_UPGRADE_OK_E) {
        ret = FW_UPGRADE_ERR_NOT_INIT_E;
        goto set_image_id_end;
    }

    if ((hdl == NULL) || (fw_upgrade_partition_handle_validation(hdl) != FW_UPGRADE_OK_E)) {
        ret = FW_UPGRADE_ERR_INVALID_PARAM_E;
        goto set_image_id_end;
    }

    client_entry = (fu_partition_client_t *)hdl;
    fwd_idx = client_entry->fwd_idx;

    if (fwd_idx >= FW_UPGRADE_MAX_FWD) {
        ret = FW_UPGRADE_ERR_INVALID_PARAM_E;
        goto set_image_id_end;
    }

    start_addr = (fw_upgrade_mem_params->mem_start + fw_upgrade_mem_params->fwd_region_size * fwd_idx +
                  offsetof(fwd, image_entries)) +
                 (client_entry->img_idx * sizeof(image_entry) + offsetof(image_entry, image_id));
    new_id = id;

    if (flash_adapter_write(start_addr, sizeof(uint32_t), (void *)&new_id, NULL, NULL) != FLASH_DEVICE_DONE) {
        ret = FW_UPGRADE_ERROR_E;
        goto set_image_id_end;
    }

    client_entry->img_id = new_id;

set_image_id_end:
    return ret;
}

fw_upgrade_status_code_t fw_upgrade_set_image_version(fu_part_hdl_t *hdl, uint32_t version)
{
    fw_upgrade_status_code_t ret = FW_UPGRADE_OK_E;
    uint32_t start_addr;
    uint8_t fwd_idx;
    fu_partition_client_t *client_entry;

    if (fw_upgrade_validation() != FW_UPGRADE_OK_E) {
        ret = FW_UPGRADE_ERR_NOT_INIT_E;
        goto set_image_version_end;
    }

    if ((hdl == NULL) || (fw_upgrade_partition_handle_validation(hdl) != FW_UPGRADE_OK_E)) {
        ret = FW_UPGRADE_ERR_INVALID_PARAM_E;
        goto set_image_version_end;
    }

    client_entry = (fu_partition_client_t *)hdl;
    fwd_idx = client_entry->fwd_idx;

    if (fwd_idx >= FW_UPGRADE_MAX_FWD) {
        ret = FW_UPGRADE_ERR_INVALID_PARAM_E;
        goto set_image_version_end;
    }

    start_addr = (fw_upgrade_mem_params->mem_start + fw_upgrade_mem_params->fwd_region_size * fwd_idx +
                  offsetof(fwd, image_entries)) +
                 (client_entry->img_idx * sizeof(image_entry) + offsetof(image_entry, img_version));
    if (flash_adapter_write(start_addr, sizeof(uint32_t), (void *)&version, NULL, NULL) != FLASH_DEVICE_DONE) {
        ret = FW_UPGRADE_ERROR_E;
        goto set_image_version_end;
    }

    client_entry->img_version = version;

set_image_version_end:
    return ret;
}

fw_upgrade_status_code_t fw_upgrade_set_image_size(fu_part_hdl_t *hdl, uint32_t size)
{
    fw_upgrade_status_code_t ret = FW_UPGRADE_OK_E;
    uint32_t start_addr;
    uint8_t fwd_idx = FW_UPGRADE_MAX_FWD, img_id;
    fu_partition_client_t *client_entry;

    if (fw_upgrade_validation() != FW_UPGRADE_OK_E) {
        ret = FW_UPGRADE_ERR_NOT_INIT_E;
        goto set_image_size_end;
    }

    if ((hdl == NULL) || (size != 0) || (fw_upgrade_partition_handle_validation(hdl) != FW_UPGRADE_OK_E)) {
        ret = FW_UPGRADE_ERR_INVALID_PARAM_E;
        goto set_image_size_end;
    }

    client_entry = (fu_partition_client_t *)hdl;
    fwd_idx = client_entry->fwd_idx;

    if (fwd_idx >= FW_UPGRADE_MAX_FWD) {
        ret = FW_UPGRADE_ERR_INVALID_PARAM_E;
        goto set_image_size_end;
    }

    img_id = client_entry->img_id;
    /* disable APP or SBL image is not allowed */
    if ((img_id == APP_IMG_ID) || (img_id == SBL_IMG_ID)) {
        ret = FW_UPGRADE_ERR_INVALID_PARAM_E;
        goto set_image_size_end;
    }

    start_addr = (fw_upgrade_mem_params->mem_start + fw_upgrade_mem_params->fwd_region_size * fwd_idx +
                  offsetof(fwd, image_entries)) +
                 (client_entry->img_idx * sizeof(image_entry) + offsetof(image_entry, total_blocks));
    if (flash_adapter_write(start_addr, sizeof(uint32_t), (void *)&size, NULL, NULL) != FLASH_DEVICE_DONE) {
        ret = FW_UPGRADE_ERROR_E;
        goto set_image_size_end;
    }

    client_entry->img_size = 0;

set_image_size_end:
    return ret;
}

fw_upgrade_status_code_t fw_upgrade_erase_partition(fu_part_hdl_t hdl, uint32_t offset, uint32_t nbytes)
{
    fw_upgrade_status_code_t ret = FW_UPGRADE_OK_E;
    fu_partition_client_t *h;
    uint32_t start_block, total_blocks;

    if (fw_upgrade_partition_handle_validation(hdl) != FW_UPGRADE_OK_E) {
        ret = FW_UPGRADE_ERR_INVALID_PARAM_E;
        goto erase_partition_end;
    }

    h = (fu_partition_client_t *)hdl;
    if (h->img_id == SBL_IMG_ID) {
        return ret;
    }

    if ((nbytes == 0) || (nbytes % fw_upgrade_mem_params->block_size != 0) ||
        (offset % fw_upgrade_mem_params->block_size != 0)) {
        ret = FW_UPGRADE_ERR_INVALID_PARAM_E;
        goto erase_partition_end;
    }

    if (offset + nbytes > h->img_size) {
        ret = FW_UPGRADE_ERR_INVALID_PARAM_E;
        goto erase_partition_end;
    }

    start_block = (h->img_start + offset) / fw_upgrade_mem_params->block_size;
    total_blocks = nbytes / fw_upgrade_mem_params->block_size;

    if (flash_adapter_erase(FLASH_BLOCK_ERASE_E, start_block, total_blocks, NULL, NULL) != FLASH_DEVICE_DONE) {
        ret = FW_UPGRADE_ERROR_E;
    }

erase_partition_end:
    return ret;
}

fw_upgrade_status_code_t fw_upgrade_write_partition(fu_part_hdl_t hdl, uint32_t offset, char *buf, uint32_t nbytes)
{
    fw_upgrade_status_code_t ret = FW_UPGRADE_OK_E;
    fu_partition_client_t *h;
    uint32_t start_addr;

    if (fw_upgrade_mem_parameters_validation(hdl, offset, nbytes, buf) != FW_UPGRADE_OK_E)
        return FW_UPGRADE_ERR_INVALID_PARAM_E;

    h = (fu_partition_client_t *)hdl;
    start_addr = h->img_start + offset;

    if (h->img_id == SBL_IMG_ID) {
        /* CRITICAL FIX: RRAM write requires aligned addresses
         * Check if source buffer is aligned, if not, use aligned temporary buffer */
        if (((uintptr_t)buf & 0x3) != 0) {
            /* Source buffer is not 4-byte aligned, need to use aligned buffer */
            uint8_t *aligned_buf = k_aligned_alloc(4, nbytes);
            if (aligned_buf == NULL) {
                LOG_ERR("Failed to allocate aligned buffer for RRAM write");
                return FW_UPGRADE_ERR_INSUFFICIENT_MEMORY_E;
            }
            memcpy(aligned_buf, buf, nbytes);
            
            if (nt_rram_write(start_addr, (void *)aligned_buf, nbytes) != 0) {
                k_free(aligned_buf);
                ret = FW_UPGRADE_ERROR_E;
            } else {
                k_free(aligned_buf);
            }
        } else {
            /* Source buffer is aligned, can write directly */
            if (nt_rram_write(start_addr, (void *)buf, nbytes) != 0) {
                ret = FW_UPGRADE_ERROR_E;
            }
        }
    } else {
        if (flash_adapter_write(start_addr, nbytes, (uint8_t *)buf, NULL, NULL) != FLASH_DEVICE_DONE) {
            ret = FW_UPGRADE_ERROR_E;
        }
    }

    return ret;
}

fw_upgrade_status_code_t fw_upgrade_read_partition(fu_part_hdl_t hdl, uint32_t offset, char *buf, uint32_t max_bytes,
                                                   uint32_t *nbytes)
{
    fu_partition_client_t *h;
    uint32_t start_addr;

    *nbytes = 0;

    if (fw_upgrade_mem_parameters_validation(hdl, offset, max_bytes, buf) != FW_UPGRADE_OK_E) {
        return FW_UPGRADE_ERR_INVALID_PARAM_E;
    }

    h = (fu_partition_client_t *)hdl;
    start_addr = h->img_start + offset;

    /*  SBL image is in RRAM */
    if (h->img_id == SBL_IMG_ID) {
        if (nt_rram_read(start_addr, (void *)buf, max_bytes) != 0) {
            return FW_UPGRADE_ERROR_E;
        }
    } else {
        if (flash_adapter_read(start_addr, max_bytes, (uint8_t *)buf, NULL, NULL) != FLASH_DEVICE_DONE) {
            return FW_UPGRADE_ERROR_E;
        }
    }

    *nbytes = max_bytes;

    return FW_UPGRADE_OK_E;
}

fw_upgrade_status_code_t fw_upgrade_select_sbl_trial_fde(uint32_t *sbl_idx, uint32_t *start_address, uint32_t *size)
{
    fw_upgrade_status_code_t ret = FW_UPGRADE_ERROR_E;
    fdt_s *fdt;
    fdt_entry *fde;
    uint32_t i;

    fdt = (fdt_s *)FDT_START;

    if (sbl_idx == NULL || start_address == NULL || size == NULL) {
        return FW_UPGRADE_ERR_INVALID_PARAM_E;
    }

    if (fdt == NULL || fdt->header.signature != FDT_SIGNATURE || fdt->header.num_fde == 0 ||
        fdt->header.num_fde > FDT_MAX_ENTRY) {
        return FW_UPGRADE_ERR_INVALID_FDT_E;
    }

    fde = fdt->fde;

    for (i = 0; i < fdt->header.num_fde; i++) {
        if (fde[i].id == FDE_IMG_ID_SBL && fde[i].rank == FDE_IMG_RANK_AGED) {
            *start_address = fde[i].addr;
            *size = (uint32_t)SBL_MEM_SIZE;
            *sbl_idx = i;
            ret = FW_UPGRADE_OK_E;
            break;
        }
    }

    return ret;
}

fw_upgrade_status_code_t fw_upgrade_set_sbl_trial_fde(uint32_t sbl_trial_idx, uint32_t version)
{
    fw_upgrade_status_code_t ret = FW_UPGRADE_OK_E;
    fdt_s *fdt;
    fdt_entry *fde;
    fdt_entry update_fde;

    fdt = (fdt_s *)FDT_START;

    if (fdt == NULL || fdt->header.signature != FDT_SIGNATURE || fdt->header.num_fde == 0 ||
        fdt->header.num_fde > FDT_MAX_ENTRY) {
        return FW_UPGRADE_ERR_INVALID_FDT_E;
    }

    fde = fdt->fde;

    if (sbl_trial_idx >= fdt->header.num_fde || fde[sbl_trial_idx].id != FDE_IMG_ID_SBL ||
        fde[sbl_trial_idx].rank != FDE_IMG_RANK_AGED) {
        return FW_UPGRADE_ERR_INVALID_PARAM_E;
    }

    memscpy(&update_fde, sizeof(fdt_entry), &(fdt->fde[sbl_trial_idx]), sizeof(fdt_entry));
    update_fde.rank = FDE_IMG_RANK_TRIAL;
    update_fde.state = FDE_IMG_STATE_NEW;
    update_fde.version = version;

    if (nt_rram_write(((uint32_t) & (fdt->fde[sbl_trial_idx])), (void *)&update_fde, sizeof(fdt_entry)) != 0) {
        return FW_UPGRADE_ERROR_E;
    }

    return ret;
}

fw_upgrade_status_code_t fw_upgrade_set_force_boot(uint8_t force_boot_fwd)
{
    fdt_s *fdt;
    fdt_entry *fde;
    uint32_t i, app_idx = 0xFF;
    fdt_entry update_fde;

    fdt = (fdt_s *)FDT_START;

    if (fdt == NULL || fdt->header.signature != FDT_SIGNATURE || fdt->header.num_fde == 0 ||
        fdt->header.num_fde > FDT_MAX_ENTRY) {
        return FW_UPGRADE_INVALID_FWD;
    }

    fde = fdt->fde;
    for (i = 0; i < fdt->header.num_fde; i++) {
        if (fde[i].id == FDE_IMG_ID_APP) {
            app_idx = i;
            break;
        }
    }

    if (app_idx == 0xFF) {
        return FW_UPGRADE_ERR_INVALID_FDT_E;
    }

    memscpy(&update_fde, sizeof(fdt_entry), &(fdt->fde[app_idx]), sizeof(fdt_entry));
    update_fde.reserve[1] = force_boot_fwd;

    if (nt_rram_write(((uint32_t) & (fdt->fde[app_idx])), (void *)&update_fde, sizeof(fdt_entry)) != 0) {
        return FW_UPGRADE_ERROR_E;
    }

    return FW_UPGRADE_OK_E;
}


void display_sub_image_info(fu_part_hdl_t hdl)
{
    fu_partition_client_t *partition = (fu_partition_client_t *)hdl;

    LOG_INF("        Image ID: 0x%X", partition->img_id);
    LOG_INF("   Image Version: 0x%X", partition->img_version);
    LOG_INF("     Image Start: 0x%X", partition->img_start);
    LOG_INF("      Image Size: 0x%X", partition->img_size);
}

int fw_upgrade_display_fwd(void)
{
    int32_t Index;
    uint32_t boot_type, fwd_present, magic = 0, Result_u32 = 0;
    uint8_t Result_u8 = 0;
    fu_part_hdl_t hdl = NULL, hdl_next;

    /* get active FWD */
    Index = fw_upgrade_get_active_fwd(&boot_type, &fwd_present);
    LOG_INF("Active FWD: %s  index:%d, present:%d", 
            (boot_type==(1<<FW_UPGRADE_FWD_BIT_TRIAL))?"Trial":
            (boot_type==(1<<FW_UPGRADE_FWD_BIT_CURRENT))?"Current":"Golden", 
            Index, fwd_present);

    for (Index = 0; Index < 3; Index++) {
        LOG_INF("FWD %d", Index);

        fw_upgrade_get_fwd_info(Index, FW_UPGRADE_FWD_MAGIC_E, (uint8_t *)&magic);
        LOG_INF("Magic: 0x%X", magic);

        fw_upgrade_get_fwd_info(Index, FW_UPGRADE_FWD_RANK_E, (uint8_t *)&Result_u32);
        LOG_INF("Rank: 0x%X", Result_u32);

        fw_upgrade_get_fwd_info(Index, FW_UPGRADE_FWD_VERSION_E, (uint8_t *)&Result_u32);
        LOG_INF("Version: 0x%X", Result_u32);

        fw_upgrade_get_fwd_info(Index, FW_UPGRADE_FWD_STATUS_E, (uint8_t *)&Result_u8);
        LOG_INF("Status: 0x%X", Result_u8);

        fw_upgrade_get_fwd_info(Index, FW_UPGRADE_FWD_TOTAL_IMAGE_E, (uint8_t *)&Result_u8);
        LOG_INF("Total Images: 0x%X", Result_u8);

        if (magic == 0x54445746) {
            if (fw_upgrade_first_partition(Index, &hdl) == FW_UPGRADE_OK_E) {
                display_sub_image_info(hdl);
                while (fw_upgrade_next_partition(hdl, &hdl_next) == FW_UPGRADE_OK_E) {
                    ((fu_partition_client_t *)(hdl))->ref_count = 0;
                    hdl = hdl_next;
                    display_sub_image_info(hdl);
                }
                ((fu_partition_client_t *)(hdl))->ref_count = 0;
            }
        }

        LOG_INF("");
    }

    return 0;
}
