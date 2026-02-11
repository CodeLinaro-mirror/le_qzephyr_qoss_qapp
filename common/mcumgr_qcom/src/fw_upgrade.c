/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*                                                                                                                           */
/*       Firmware Upgrade */
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
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/reboot.h>
#include "fw_upgrade.h"

/* Forward declaration for missing function */
extern void nt_system_sw_reset(void);

LOG_MODULE_DECLARE(qcom_img_mgmt, CONFIG_QCOM_IMG_MGMT_LOG_LEVEL);
/**********************************************************************************************************/
/* Preprocessor Definitions and Constants																  */
/**********************************************************************************************************/

#define TAKE_LOCK(__lock__) (k_mutex_lock(&(__lock__), K_FOREVER) == 0)
#define RELEASE_LOCK(__lock__)          \
    do {                                \
        k_mutex_unlock(&(__lock__));    \
    } while (0)

#if defined(DEBUG_FW_UPGRADE_PRINTF)
#define FW_UPGRADE_D_PRINTF(args...) LOG_INF(args)
#else
#define FW_UPGRADE_D_PRINTF(args...)
#endif

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

#define FW_UPGRADE_FLAG_AUTO_REBOOT             (1 << 0)
#define FW_UPGRADE_FLAG_DUPLICATE_ACTIVE_FS     (1 << 1)
#define FW_UPGRADE_FLAG_DUPLICATE_KEEP_TRIAL_FS (1 << 2)

#define UNUSED(x) (void)(x)

/**********************************************************************************************************/
/* Globals																                                  */
/**********************************************************************************************************/
fw_upgrade_context_t *fw_upgrade_sess_cxt = NULL;
fw_upgrade_image_hdr_t *fw_upgrade_image_hdr = NULL; /* fw upgrade image header */
uint8_t fw_upgrade_mutex_init = 0;
fw_upgrade_result_t mcumgr_upgrade_result;

K_MUTEX_DEFINE(fw_upgrade_mutex);
int CONFIG_FW_UPGRADE_FWD_SUPPORT_NUM;

/* Delayed reset work queue and handler */
static void fw_upgrade_delayed_reset_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(fw_upgrade_reset_work, fw_upgrade_delayed_reset_handler);

/*
 * Delayed reset handler - performs system reboot after delay
 */
static void fw_upgrade_delayed_reset_handler(struct k_work *work)
{
    LOG_INF("fw_upgrade: Performing delayed system reset");
    /* sys_reboot(SYS_REBOOT_WARM); */
    nt_system_sw_reset();
}
/**********************************************************************************************************/
/* External Functions																                      */
/**********************************************************************************************************/

/**********************************************************************************************************/
/* Internal Functions																                      */
/**********************************************************************************************************/

/* Forward declarations */
static int32_t fw_upgrade_session_finalize(int32_t ret);

/*
 * get fw upgrade session context
 */
static fw_upgrade_context_t *fw_upgrade_get_context(void)
{
    return fw_upgrade_sess_cxt;
}

/*
 * set fw upgrade session state
 */
static void fw_upgrade_set_state(fw_upgrade_state_t state)
{
    fw_upgrade_context_t *fw_upgrade_cxt;

    if (TAKE_LOCK(fw_upgrade_mutex)) {
        /* get fw upgrade session context */
        fw_upgrade_cxt = fw_upgrade_get_context();

        if (fw_upgrade_cxt == NULL) {
            /*  TODO: dump error info */
            __ASSERT(0, "fw_upgrade_cxt is NULL");
        }

        if (fw_upgrade_cxt != NULL) {
            fw_upgrade_cxt->fw_upgrade_state = state;
        }
        RELEASE_LOCK(fw_upgrade_mutex);
    }
}

/*
 * get fw upgrade session state
 */
static fw_upgrade_state_t fw_upgrade_get_state(void)
{
    fw_upgrade_state_t ret = FW_UPGRADE_STATE_NOT_START_E;
    fw_upgrade_context_t *fw_upgrade_cxt;

    if (fw_upgrade_mutex_init == 0)
        return ret;

    if (TAKE_LOCK(fw_upgrade_mutex)) {
        /* get fw upgrade session context */
        fw_upgrade_cxt = fw_upgrade_get_context();
        if (fw_upgrade_cxt == NULL) {
            ret = FW_UPGRADE_STATE_NOT_START_E;
        } else {
            ret = fw_upgrade_cxt->fw_upgrade_state;
        }
        RELEASE_LOCK(fw_upgrade_mutex);
    }
    return ret;
}

/*
 * get fw upgrade session active status
 */
static fw_upgrade_session_status_t fw_upgrade_get_session_status(void)
{
    fw_upgrade_session_status_t ret = FW_UPGRADE_SESSION_NOT_START_E;
    fw_upgrade_context_t *fw_upgrade_cxt;

    if (fw_upgrade_mutex_init == 0) {
        return ret;
    }

    if (TAKE_LOCK(fw_upgrade_mutex)) {
        /* get fw upgrade session context */
        fw_upgrade_cxt = fw_upgrade_get_context();
        if (fw_upgrade_cxt == NULL) {
            ret = FW_UPGRADE_SESSION_NOT_START_E;
        } else {
            ret = fw_upgrade_cxt->fw_upgrade_session_status;
        }
        RELEASE_LOCK(fw_upgrade_mutex);
    }

    return ret;
}

/*
 * set fw upgrade session active status
 */
static fw_upgrade_status_code_t fw_upgrade_set_session_status(fw_upgrade_session_status_t status)
{
    fw_upgrade_status_code_t ret = FW_UPGRADE_ERR_SESSION_NOT_START_E;
    fw_upgrade_context_t *fw_upgrade_cxt;

    if (fw_upgrade_mutex_init == 0) {
        return ret;
    }

    if (TAKE_LOCK(fw_upgrade_mutex)) {
        /* get fw upgrade session context */
        fw_upgrade_cxt = fw_upgrade_get_context();
        if (fw_upgrade_cxt == NULL) {
            ret = FW_UPGRADE_ERR_SESSION_NOT_START_E;
        } else {
            fw_upgrade_cxt->fw_upgrade_session_status = status;
            ret = FW_UPGRADE_OK_E;
        }
        RELEASE_LOCK(fw_upgrade_mutex);
    }
    return ret;
}

/*
 * set fw upgrade session error code
 */
static void fw_upgrade_set_error_code(int32_t err_code)
{
    fw_upgrade_context_t *fw_upgrade_cxt = fw_upgrade_get_context();

    if (fw_upgrade_cxt != NULL) {
        fw_upgrade_cxt->error_code = err_code;
    }
}


/*
 * fw upgrade session fin
 */
static fw_upgrade_status_code_t fw_upgrade_session_fin(void)
{
    if (fw_upgrade_sess_cxt != NULL && fw_upgrade_sess_cxt->config_buf != NULL) {
        k_free(fw_upgrade_sess_cxt->config_buf);
        fw_upgrade_sess_cxt->config_buf = NULL;
    }

	if(fw_upgrade_sess_cxt->tmp_buffer)
	{
		k_free(fw_upgrade_sess_cxt->tmp_buffer);
		fw_upgrade_sess_cxt->tmp_buffer = NULL;
	}

    /*  TODO: free AON MEM */
    if (fw_upgrade_image_hdr != NULL) {
        k_free(fw_upgrade_image_hdr);
        fw_upgrade_image_hdr = NULL;
    }
    if (fw_upgrade_sess_cxt != NULL) {
        if (fw_upgrade_sess_cxt->partition_hdl != NULL) {
            ((fu_partition_client_t *)(fw_upgrade_sess_cxt->partition_hdl))->ref_count = 0;
            fw_upgrade_sess_cxt->partition_hdl = NULL;
        }
        if (fw_upgrade_sess_cxt->digest_ctx != NULL) {
            k_free((void *)(fw_upgrade_sess_cxt->digest_ctx));
            fw_upgrade_sess_cxt->digest_ctx = NULL;
        }
        k_free(fw_upgrade_sess_cxt);
        fw_upgrade_sess_cxt = NULL;
    }

    /* Note: K_MUTEX_DEFINE creates a static mutex, no need to delete */

    return FW_UPGRADE_OK_E;
}

/*
 * fw upgrade session init
 */
static fw_upgrade_status_code_t fw_upgrade_session_init(void)
{
    fw_upgrade_status_code_t ret = FW_UPGRADE_OK_E;
    uint32_t i;

    if (fw_upgrade_init() != FW_UPGRADE_OK_E) {
        ret = FW_UPGRADE_ERR_FLASH_INIT_TIMEOUT_E;
        goto session_init_end;
    }

    if (fw_upgrade_mutex_init == 0) {
        k_mutex_init(&fw_upgrade_mutex);
        fw_upgrade_mutex_init = 1;
    }

    /*  TODO: aon malloc */
    /*  Check if fw_upgrade_sess_cxt is already allocated to prevent memory leak */
    if (fw_upgrade_sess_cxt == NULL) {
        fw_upgrade_sess_cxt = (fw_upgrade_context_t *)k_malloc(sizeof(fw_upgrade_context_t));
        if (fw_upgrade_sess_cxt == NULL) {
            ret = FW_UPGRADE_ERR_INSUFFICIENT_MEMORY_E;
            goto session_init_end;
        }

        /* Clear all data */
        memset(fw_upgrade_sess_cxt, '\0', sizeof(fw_upgrade_context_t));
    } else {
        LOG_WRN("fw_upgrade_sess_cxt already allocated, reusing existing context");
        /* Clear all data but preserve allocated pointers */
        void *tmp_buffer_backup = fw_upgrade_sess_cxt->tmp_buffer;
        void *digest_ctx_backup = fw_upgrade_sess_cxt->digest_ctx;
        memset(fw_upgrade_sess_cxt, '\0', sizeof(fw_upgrade_context_t));
        fw_upgrade_sess_cxt->tmp_buffer = tmp_buffer_backup;
        fw_upgrade_sess_cxt->digest_ctx = digest_ctx_backup;
    }

	
	/*Allocate buffer - check if already allocated*/
	if (fw_upgrade_sess_cxt->tmp_buffer == NULL) {
		if ((fw_upgrade_sess_cxt->tmp_buffer = k_malloc(FW_UPGRADE_BUF_SIZE)) == NULL) {
			LOG_ERR("Out of memory error");
			k_free(fw_upgrade_sess_cxt);
			fw_upgrade_sess_cxt = NULL;
			return FW_UPGRADE_ERR_INSUFFICIENT_MEMORY_E;
		}
	} else {
		LOG_DBG("tmp_buffer already allocated, reusing existing buffer");
	}

    /* init the default setting */
    fw_upgrade_set_state(FW_UPGRADE_STATE_NOT_START_E);
    fw_upgrade_set_session_status(FW_UPGRADE_SESSION_RUNNING_E);
    fw_upgrade_set_error_code(FW_UPGRADE_OK_E);
    fw_upgrade_sess_cxt->partition_hdl = NULL;

    fw_upgrade_sess_cxt->flags = FW_UPGRADE_FLAG_AUTO_REBOOT | FW_UPGRADE_FLAG_DUPLICATE_ACTIVE_FS ; /* flags; */
    mcumgr_upgrade_result.trial_flag = 0;
	memset(&mcumgr_upgrade_result, 0, sizeof(mcumgr_upgrade_result));

    fw_upgrade_sess_cxt->is_first = 1;
    fw_upgrade_sess_cxt->format = 1; /* 1: partial upgrade, 2: all-in-one */
    fw_upgrade_sess_cxt->buf_len = 0;
    fw_upgrade_sess_cxt->buf_offset = 0;
    fw_upgrade_sess_cxt->file_read_count = 0;

    fw_upgrade_sess_cxt->image_index = 0;
    fw_upgrade_sess_cxt->image_wrt_count = 0;
    fw_upgrade_sess_cxt->image_wrt_length = 0;
    fw_upgrade_sess_cxt->total_images = 0;
    fw_upgrade_sess_cxt->hidden_images_count = 0;
    fw_upgrade_sess_cxt->config_buf = NULL;

    for (i = 0; i < FW_UPGRADE_MAX_IMAGES_NUM; i++) {
        fw_upgrade_sess_cxt->download_flag[i] = 1;
    }

    /* allocate crypto resource - check if already allocated */
    if (fw_upgrade_sess_cxt->digest_ctx == NULL) {
        fw_upgrade_sess_cxt->digest_ctx = (mbedtls_sha256_context *)k_malloc(sizeof(mbedtls_sha256_context));
        if (fw_upgrade_sess_cxt->digest_ctx == NULL) {
            ret = FW_UPGRADE_ERR_INSUFFICIENT_MEMORY_E;
            goto session_init_end;
        }
    } else {
        LOG_DBG("digest_ctx already allocated, reusing existing context");
    }
    mbedtls_sha256_init(fw_upgrade_sess_cxt->digest_ctx);
    mbedtls_sha256_starts(fw_upgrade_sess_cxt->digest_ctx, 0);

    fw_upgrade_set_session_status(FW_UPGRADE_SESSION_RUNNING_E);
    return FW_UPGRADE_OK_E;

session_init_end:
    fw_upgrade_set_error_code(ret);
    fw_upgrade_session_fin();
    return ret;
}

/*
 * fw upgrade session suspend
 */
static fw_upgrade_status_code_t fw_upgrade_session_prepare_suspend(void)
{
    if (fw_upgrade_sess_cxt) {
        if (fw_upgrade_sess_cxt->partition_hdl != NULL) {
            ((fu_partition_client_t *)(fw_upgrade_sess_cxt->partition_hdl))->ref_count = 0;
            fw_upgrade_sess_cxt->partition_hdl = NULL;
        }

        /*  free crypto */
        if (fw_upgrade_sess_cxt->digest_ctx != NULL) {
            free((void *)(fw_upgrade_sess_cxt->digest_ctx));
            fw_upgrade_sess_cxt->digest_ctx = NULL;
        }
    }
    return FW_UPGRADE_OK_E;
}

/*
 * process fw upgrade conifg file
 *
 * Partial Upgrade Flow:
 *     receive whole config file
 *     check if fields are valid at config file header
 *     calc hash of config file and compare the result with hash field at config file
 *     save image entries
 *     check if fields are valid at each image entry
 *     calc hash at current image and determine if the image need to be downloaded.
 *     image will only be downloaded if the hash of the new image is different from the hash of the current image.
 *
 * All-in-one Upgrade Flow:
 *     receive imageset header
 *     check if fields are valid at config file header
 *     calc hash of config file and compare the result with hash field at header
 *     save image entries
 *     check if fields are valid at each image entry
 *
 * Firmware Upgrade Image HEADER format:
      uint32 sig
      uint32 ver
      uint32 format
      uint32 image_len
      uint8  num_images
      IMG_ENTRY
          ....
      IMG_ENTRY
      uint8 HASH[32]
*/
static fw_upgrade_status_code_t fw_upgrade_process_config_file(uint8_t *buf)
{
    fw_upgrade_status_code_t ret = FW_UPGRADE_OK_E;
    uint32_t i, len, offset, block_size, disk_size, nbytes;
    uint8_t hash[FW_UPGRADE_HASH_LEN], *hash_buf = NULL;
    uint8_t active_fwd;
    fw_upgrade_imageSet_hdr_part1_t *imgset_hdr;
    fw_upgrade_context_t *fw_upgrade_cxt;
    fw_upgrade_image_hdr_t *img_hdr;
    fu_part_hdl_t hdl;
    uint8_t totalImages;
    boolean sbl_image_exist = FALSE;
    boolean app_image_exist = FALSE;
    uint32_t sbl_disk_size = 0;

    fw_upgrade_cxt = fw_upgrade_get_context();
    if (fw_upgrade_cxt == NULL) {
        return FW_UPGRADE_ERR_SESSION_NOT_START_E;
    }

    /*  received first buffer for config file */
    if (fw_upgrade_cxt->config_buf == NULL) {
        if (fw_upgrade_cxt->buf_len < sizeof(fw_upgrade_imageSet_hdr_part1_t)) {
            ret = FW_UPGRADE_ERR_INCORRECT_IMAGE_HDR_E;
            goto parse_img_hdr_end;
        }

        /*  get firmware upgrade image header part1 */
        imgset_hdr = (fw_upgrade_imageSet_hdr_part1_t *)buf;

        /*  check total images */
        if (!(imgset_hdr->num_images > 0 && imgset_hdr->num_images <= FW_UPGRADE_MAX_IMAGES_NUM)) {
            FW_UPGRADE_D_PRINTF("num of firmware upgrade images are not correct\r\n");
            ret = FW_UPGRADE_ERR_INCORRECT_IMAGE_HDR_E;
            goto parse_img_hdr_end;
        }
        /*  check header */
        if ((imgset_hdr->magic == 0) || (imgset_hdr->length == 0) || (imgset_hdr->format == 0)) {
            FW_UPGRADE_D_PRINTF("firmware upgrade image signature is not correct\r\n");
            ret = FW_UPGRADE_ERR_INCORRECT_IMAGE_HDR_E;
            goto parse_img_hdr_end;
        }

        /*  save fw_upgrade format -- partial upgrade or all in one */
        fw_upgrade_cxt->format = imgset_hdr->format;

        len = imgset_hdr->length;
        if (len != (sizeof(fw_upgrade_imageSet_hdr_part1_t) + imgset_hdr->num_images * sizeof(fw_upgrade_image_hdr_t) +
                    FW_UPGRADE_HASH_LEN)) {
            FW_UPGRADE_D_PRINTF("hdr length is not correct\r\n");
            ret = FW_UPGRADE_ERR_INCORRECT_IMAGE_HDR_E;
            goto parse_img_hdr_end;
        }

        fw_upgrade_cxt->config_buf = (uint8_t *)k_malloc(len);
        if (fw_upgrade_cxt->config_buf == NULL) {
            ret = FW_UPGRADE_ERR_INSUFFICIENT_MEMORY_E;
            goto parse_img_hdr_end;
        }

        fw_upgrade_cxt->buf_offset = MIN(fw_upgrade_cxt->buf_len, len);
        memscpy(fw_upgrade_cxt->config_buf, fw_upgrade_cxt->buf_offset, buf, fw_upgrade_cxt->buf_offset);
     LOG_INF("header len=%d, buf_offset=%d", imgset_hdr->length, fw_upgrade_cxt->buf_offset);

        /*  don't receive whole config file at this packet yet */
        if (fw_upgrade_cxt->buf_len < len) {
            fw_upgrade_cxt->image_wrt_count = fw_upgrade_cxt->buf_len;
            /*  continue receiving data */
            fw_upgrade_set_state(FW_UPGRADE_STATE_RECEIVE_DATA_E);
            goto parse_img_hdr_end;
        }
    } else {
        imgset_hdr = (fw_upgrade_imageSet_hdr_part1_t *)fw_upgrade_cxt->config_buf;
        
        len = MIN(fw_upgrade_cxt->buf_len, imgset_hdr->length - fw_upgrade_cxt->image_wrt_count);
        memscpy(fw_upgrade_cxt->config_buf + fw_upgrade_cxt->image_wrt_count, len, buf, len);
        LOG_INF("header len=%d, saved_offset=%d, current_len=%d", 
            imgset_hdr->length, fw_upgrade_cxt->image_wrt_count, len);

        fw_upgrade_cxt->image_wrt_count += len;
        fw_upgrade_cxt->buf_offset = len;
        
        if (fw_upgrade_cxt->image_wrt_count < imgset_hdr->length) {
            /*  continue receiving data */
            fw_upgrade_set_state(FW_UPGRADE_STATE_RECEIVE_DATA_E);
            goto parse_img_hdr_end;
        }
    }

    /*  get firmware upgrade image header part1 */
    imgset_hdr = (fw_upgrade_imageSet_hdr_part1_t *)fw_upgrade_cxt->config_buf;

    /* calc hash offset */
    offset = sizeof(fw_upgrade_imageSet_hdr_part1_t) + imgset_hdr->num_images * sizeof(fw_upgrade_image_hdr_t);

    /*  init crypto */
    mbedtls_sha256_init(fw_upgrade_cxt->digest_ctx);
    mbedtls_sha256_starts(fw_upgrade_cxt->digest_ctx, 0);
    mbedtls_sha256_update(fw_upgrade_cxt->digest_ctx, (unsigned char *)imgset_hdr, offset);
    mbedtls_sha256_finish(fw_upgrade_cxt->digest_ctx, hash);

    /*  compare firmware upgrade image Header HASH */
    if (memcmp(fw_upgrade_cxt->config_buf + offset, hash, FW_UPGRADE_HASH_LEN) != 0) {
        LOG_ERR("HASH is incorrect\r\n");
        ret = FW_UPGRADE_ERR_INCORRECT_IMAGE_CHECKSUM_E;
        goto parse_img_hdr_end;
    }

	#if 1 /*  mcumgr */
    /*  write magic number */
    if (fw_upgrade_set_fwd_info(fw_upgrade_cxt->trial_fwd_idx, FW_UPGRADE_FWD_MAGIC_E, (uint8_t *)&imgset_hdr->magic) !=
        FW_UPGRADE_OK_E) {
        ret = FW_UPGRADE_ERR_FLASH_WRITE_FAIL_E;
        goto parse_img_hdr_end;
    }
	#else
	uint32 val;
	ret = fw_upgrade_get_fwd_magic_addr(fw_upgrade_cxt->trial_fwd_idx, &mcumgr_upgrade_result.trial_fwd_magic_addr);
	if(FW_UPGRADE_OK_E != ret)
	{
		goto parse_img_hdr_end;;
	}
	mcumgr_upgrade_result.trial_fwd_magic_val = imgset_hdr->magic;
	#endif

    /*  write version */
    if (fw_upgrade_set_fwd_info(fw_upgrade_cxt->trial_fwd_idx, FW_UPGRADE_FWD_VERSION_E,
                                (uint8_t *)&imgset_hdr->version) != FW_UPGRADE_OK_E) {
        ret = FW_UPGRADE_ERR_FLASH_WRITE_FAIL_E;
        goto parse_img_hdr_end;
    }

    /*  write num of images */
    totalImages = imgset_hdr->num_images + fw_upgrade_cxt->hidden_images_count;
    if (fw_upgrade_set_fwd_info(fw_upgrade_cxt->trial_fwd_idx, FW_UPGRADE_FWD_TOTAL_IMAGE_E, (uint8_t *)&totalImages) !=
        FW_UPGRADE_OK_E) {
        ret = FW_UPGRADE_ERR_FLASH_WRITE_FAIL_E;
        goto parse_img_hdr_end;
    }

    /*  save total images */
    fw_upgrade_cxt->total_images = imgset_hdr->num_images;

    /*Allocate buffer*/
    len = imgset_hdr->num_images * sizeof(fw_upgrade_image_hdr_t);

    /*  TODO: allocate aon mem */
    fw_upgrade_image_hdr = k_malloc(len);
    if (fw_upgrade_image_hdr == NULL) {
        ret = FW_UPGRADE_ERR_INSUFFICIENT_MEMORY_E;
        goto parse_img_hdr_end;
    }

    /*save firmware upgrade image entries */
    memscpy((uint8_t *)(fw_upgrade_image_hdr), len,
            fw_upgrade_cxt->config_buf + sizeof(fw_upgrade_imageSet_hdr_part1_t), len);

    /* check image entries */
    for (i = 0, len = 0, disk_size = 0, img_hdr = fw_upgrade_image_hdr; i < imgset_hdr->num_images; i++) {
        /*  check image length */
        if ((img_hdr->image_id == 0) || (img_hdr->magic == 0) || (img_hdr->hash_type == 0) ||
            (img_hdr->disk_size == 0) || (img_hdr->disk_size < img_hdr->image_length)) {
            FW_UPGRADE_D_PRINTF("firmware upgrade image length setting is not correct\r\n");
            ret = FW_UPGRADE_ERR_INCORRECT_IMAGE_HDR_E;
            goto parse_img_hdr_end;
        } else {
            if (img_hdr->image_id != SBL_IMG_ID) {
                /* adjust disk_size to align with block_size */
                fw_upgrade_get_mem_block_size(&block_size);
                if (img_hdr->disk_size % block_size != 0)
                    img_hdr->disk_size += block_size;
                img_hdr->disk_size = img_hdr->disk_size / block_size * block_size;
            }

            /* when calc the disk_size, exclude the first and second File system and SBL
               due to the File ssytem are pre-reserved already, SBL is located in specific
               place in RRAM.
            */
            if ((img_hdr->image_id != FS1_IMG_ID) && (img_hdr->image_id != FS2_IMG_ID) &&
                (img_hdr->image_id != SBL_IMG_ID)) {
                disk_size += img_hdr->disk_size;
                len += img_hdr->image_length;
            }
            if (img_hdr->image_id == SBL_IMG_ID) {
                sbl_image_exist = TRUE;
                sbl_disk_size = img_hdr->disk_size;
                if (img_hdr->image_length > img_hdr->disk_size) {
                    ret = FW_UPGRADE_ERR_INCORRECT_IMAGE_HDR_E;
                    goto parse_img_hdr_end;
                }
            }
            if (img_hdr->image_id == APP_IMG_ID) {
                app_image_exist = TRUE;
            }
        }

        /*  set pointer to next image header */
        img_hdr++;
    }

    /*  SBL image must be upgraded together with APP image. */
    if (sbl_image_exist && !(app_image_exist)) {
        ret = FW_UPGRADE_ERR_SBL_ONLY_NOT_SUPPORT_E;
        goto parse_img_hdr_end;
    }

    /*  check if memory has enough space to store images with disk_size */
    if (disk_size > fw_upgrade_cxt->trial_mem_size) {
        ret = FW_UPGRADE_ERR_FLASH_NOT_ENOUGH_SPACE_E;
        goto parse_img_hdr_end;
    }

    if (len > disk_size) {
        ret = FW_UPGRADE_ERR_INCORRECT_IMAGE_HDR_E;
        goto parse_img_hdr_end;
    }

    if (fw_upgrade_select_sbl_trial_fde(&fw_upgrade_cxt->trial_sbl_idx, &fw_upgrade_cxt->trial_sbl_start,
                                        &fw_upgrade_cxt->trial_sbl_size) != FW_UPGRADE_OK_E) {
        ret = FW_UPGRADE_ERR_SBL_NOT_SUPPORT_UPGRADE_E;
        goto parse_img_hdr_end;
    }
    if (sbl_disk_size > fw_upgrade_cxt->trial_sbl_size) {
        ret = FW_UPGRADE_ERR_SBL_NOT_ENOUGH_SPACE_E;
        goto parse_img_hdr_end;
    }

    /* adjust file read count for all-in-one fw upgrade */
    fw_upgrade_cxt->file_read_count = imgset_hdr->length;

    /* free config buffer */
    if (fw_upgrade_cxt->config_buf != NULL) {
        k_free(fw_upgrade_cxt->config_buf);
        fw_upgrade_cxt->config_buf = NULL;
    }

    /* all-in-one fw upgrade case */
    if (fw_upgrade_cxt->format != FW_UPGRADE_FORAMT_PARTIAL_UPGRADE) {
        /*  imageset header is fully received and processed, move to next stage */
        fw_upgrade_cxt->is_first = 0;
        
        /* Set buf_offset to point to image data start in current packet */
        #if 0
		if(imgset_hdr->length >= fw_upgrade_cxt->image_wrt_count)
		{
			fw_upgrade_cxt->buf_offset = imgset_hdr->length - fw_upgrade_cxt->image_wrt_count;
		}
		else
		{
		        fw_upgrade_cxt->buf_offset = imgset_hdr->length;
		}
		#endif
        
        LOG_DBG("Config file processing complete: config_len=%u, buf_len=%u, remaining_data=%u", 
                imgset_hdr->length, fw_upgrade_cxt->buf_len, 
                fw_upgrade_cxt->buf_len - fw_upgrade_cxt->buf_offset);
        
        if (fw_upgrade_cxt->buf_offset >= fw_upgrade_cxt->buf_len)
            fw_upgrade_set_state(FW_UPGRADE_STATE_RECEIVE_DATA_E);
        else
            fw_upgrade_set_state(FW_UPGRADE_STATE_PROCESS_IMAGE_E);
        goto parse_img_hdr_end;
    }

    /* this is for partial fw upgrade case */
    fw_upgrade_get_mem_block_size(&block_size);
    active_fwd = fw_upgrade_get_active_fwd(NULL, NULL);
    hash_buf = (uint8_t *)k_malloc(block_size);
    if (hash_buf == NULL) {
        ret = FW_UPGRADE_ERR_INSUFFICIENT_MEMORY_E;
        goto parse_img_hdr_end;
    }

    /* check images if need download */
    for (i = 0, img_hdr = fw_upgrade_image_hdr; i < fw_upgrade_cxt->total_images; i++, img_hdr++) {
        fw_upgrade_cxt->download_flag[i] = 1; /* default is to download */
        /*  TODO: SBL handle */
        if ((img_hdr->image_id != FS1_IMG_ID) && (img_hdr->image_id != FS2_IMG_ID) &&
            (img_hdr->image_id != SBL_IMG_ID)) {
            if (fw_upgrade_find_partition(active_fwd, img_hdr->image_id, &hdl) != FW_UPGRADE_OK_E)
                continue;

            mbedtls_sha256_init(fw_upgrade_cxt->digest_ctx);
            mbedtls_sha256_starts(fw_upgrade_cxt->digest_ctx, 0);

            disk_size = ((fu_partition_client_t *)hdl)->img_size;

            for (offset = 0; offset < disk_size; offset += block_size) {
                fw_upgrade_read_partition(hdl, offset, (char *)hash_buf, block_size, &nbytes);
                mbedtls_sha256_update(fw_upgrade_cxt->digest_ctx, hash_buf, nbytes);
            }
            ((fu_partition_client_t *)hdl)->ref_count = 0;
            mbedtls_sha256_finish(fw_upgrade_cxt->digest_ctx, hash);
            /*  compare firmware upgrade image Header HASH */
            if (memcmp(img_hdr->hash, hash, FW_UPGRADE_HASH_LEN) == 0) {
                fw_upgrade_cxt->download_flag[i] = 0;
            }
        }
    }

    if (hash_buf != NULL)
        k_free(hash_buf);

    /*  config file is fully received, move to next stage */
    fw_upgrade_cxt->is_first = 0;
    fw_upgrade_set_state(FW_UPGRADE_STATE_DISCONNECT_SERVER_E);
    return ret;

parse_img_hdr_end:
    if ((ret != FW_UPGRADE_OK_E) && (fw_upgrade_cxt->config_buf != NULL)) {
        k_free(fw_upgrade_cxt->config_buf);
        fw_upgrade_cxt->config_buf = NULL;
    }
    if (hash_buf != NULL)
        k_free(hash_buf);
    return ret;
}

/*
 * verify image hash
 */
static fw_upgrade_status_code_t fw_upgrade_verify_image_hash(fw_upgrade_image_hdr_t *image_hdr)
{
    fw_upgrade_status_code_t ret = FW_UPGRADE_OK_E;
    fw_upgrade_context_t *fw_upgrade_cxt;
    uint8_t *hash_org, hash_result[FW_UPGRADE_HASH_LEN];
    uint8_t *hash_buf = NULL;
    uint32_t len;

    fw_upgrade_cxt = fw_upgrade_get_context();
    if (fw_upgrade_cxt == NULL) {
        return FW_UPGRADE_ERR_SESSION_NOT_START_E;
    }

    /* get org hash offset at image header */
    hash_org = (uint8_t *)image_hdr + sizeof(fw_upgrade_image_hdr_t) - FW_UPGRADE_HASH_LEN;

    /* Add detailed logging */
    LOG_DBG("=== Hash Verification for Image ID %u ===", image_hdr->image_id);
    LOG_DBG("Format: %s", fw_upgrade_cxt->format == FW_UPGRADE_FORAMT_PARTIAL_UPGRADE ? "PARTIAL" : "ALL-IN-ONE");
    LOG_DBG("Image length: %u, Disk size: %u", image_hdr->image_length, image_hdr->disk_size);
    LOG_HEXDUMP_DBG(hash_org, FW_UPGRADE_HASH_LEN, "Expected hash:");

    /* For all-in-one format, hash is calculated only on actual image data (image_length)
     * For partial upgrade format, need to add padding to disk_size */
    if (fw_upgrade_cxt->format == FW_UPGRADE_FORAMT_PARTIAL_UPGRADE) {
        len = image_hdr->disk_size - image_hdr->image_length;
        if (len > 0) {
            hash_buf = (uint8_t *)k_malloc(len);
            if (hash_buf == NULL) {
                LOG_ERR("Failed to allocate hash buffer");
                return FW_UPGRADE_ERR_INSUFFICIENT_MEMORY_E;
            }
            memset(hash_buf, 0xff, len);
            mbedtls_sha256_update(fw_upgrade_cxt->digest_ctx, hash_buf, len);
            LOG_INF("Added %u bytes of padding (0xFF) for partial upgrade", len);
        }
    } else {
        /* All-in-one format: hash is calculated only on image_length bytes
         * No padding needed - hash should match exactly what was written */
        LOG_INF("All-in-one format: hash calculated on %u bytes (no padding)", image_hdr->image_length);
    }

    /* get result */
    mbedtls_sha256_finish(fw_upgrade_cxt->digest_ctx, hash_result);
    LOG_HEXDUMP_DBG(hash_result, FW_UPGRADE_HASH_LEN, "Calculated hash:");

    if (hash_buf != NULL)
        k_free(hash_buf);

    /* compare fw upgrade image HASH */
    if (memcmp(hash_org, hash_result, FW_UPGRADE_HASH_LEN) != 0) {
        LOG_ERR("HASH verification FAILED for image_id=%u", image_hdr->image_id);
        LOG_ERR("Expected vs Calculated hash mismatch");
        ret = FW_UPGRADE_ERR_INCORRECT_IMAGE_CHECKSUM_E;
    } else {
        LOG_INF("HASH verification PASSED for image_id=%u", image_hdr->image_id);
    }

    return ret;
}

/*
 * process firmware upgrade image
 */
static fw_upgrade_status_code_t fw_upgrade_process_receive_image(uint8_t *buffer)
{
    fw_upgrade_status_code_t ret = FW_UPGRADE_OK_E;
    fw_upgrade_context_t *fw_upgrade_cxt;
    fw_upgrade_image_hdr_t *img_hdr;
    uint32_t buf_len = 0, write_len, block_size;
    uint32_t startAddr;
    uint32_t internal_offset = 0;  /* CRITICAL FIX: Internal offset for buffer management */

    fw_upgrade_cxt = fw_upgrade_get_context();
    if (fw_upgrade_cxt == NULL)
        return FW_UPGRADE_ERR_SESSION_NOT_START_E;

    /* get block size */
    fw_upgrade_get_mem_block_size(&block_size);
    
    /* CRITICAL FIX: Initialize internal_offset from buf_offset 
     * This handles the first packet where config file is followed by image data */
    internal_offset = fw_upgrade_cxt->buf_offset;

    while (1) {
        /* for all-in-one fw upgrade case */
        if (fw_upgrade_cxt->format != FW_UPGRADE_FORAMT_PARTIAL_UPGRADE) {
            /*  all buffers have been processed */
            if (internal_offset >= fw_upgrade_cxt->buf_len) {
                break;
            }

            /*  get available buf length */
            buf_len = fw_upgrade_cxt->buf_len - internal_offset;
        }

        img_hdr = fw_upgrade_image_hdr;
        img_hdr += fw_upgrade_cxt->image_index;

        LOG_DBG("%s: Processing image_index=%u, image_id=%u, image_length=%u, image_wrt_length=%u, internal_offset=%u", 
                __func__, fw_upgrade_cxt->image_index, img_hdr->image_id, img_hdr->image_length, 
                fw_upgrade_cxt->image_wrt_length, internal_offset);

        if (fw_upgrade_cxt->image_wrt_length == 0) {  /*  image entry not init */
            fw_upgrade_cxt->image_wrt_count = 0;
            fw_upgrade_cxt->image_wrt_length = img_hdr->image_length;

            /*  create one image entry */
            if (img_hdr->image_id == FS1_IMG_ID) {
                uint32_t disk_size, disk_start;
                if (fw_upgrade_find_partition(fw_upgrade_get_active_fwd(NULL, NULL), FS2_IMG_ID,
                                              &fw_upgrade_cxt->partition_hdl) != FW_UPGRADE_OK_E) {
                    ret = FW_UPGRADE_ERR_FLASH_IMAGE_NOT_FOUND_E;
                    break;
                }

                disk_size = ((fu_partition_client_t *)(fw_upgrade_cxt->partition_hdl))->img_size;
                disk_start = ((fu_partition_client_t *)(fw_upgrade_cxt->partition_hdl))->img_start;
                ((fu_partition_client_t *)(fw_upgrade_cxt->partition_hdl))->ref_count = 0;
                fw_upgrade_cxt->partition_hdl = NULL;

                /*  File system disk size is pre-set when first time download */
                /*  Firmware Upgrade can't change size other than original size */
                if (img_hdr->disk_size > disk_size) {
                    ret = FW_UPGRADE_ERR_INCORRECT_IMAGE_LENGTH_E;
                    break;
                }

                /*  create image for trial image's FS2 */
                if (fw_upgrade_create_partition(fw_upgrade_cxt->trial_fwd_idx, img_hdr->image_id, img_hdr->version,
                                                disk_start, disk_size,
                                                &fw_upgrade_cxt->partition_hdl) != FW_UPGRADE_OK_E) {
                    ret = FW_UPGRADE_ERR_FLASH_CREATE_PARTITION_E;
                    break;
                }
            } else {
                /*  TODO: special handling for SBL image */
                if (img_hdr->image_id == SBL_IMG_ID) {
                    startAddr = fw_upgrade_cxt->trial_sbl_start;
                } else {
                    startAddr = fw_upgrade_cxt->trial_mem_start;
                }
                if (fw_upgrade_create_partition(fw_upgrade_cxt->trial_fwd_idx, img_hdr->image_id, img_hdr->version,
                                                startAddr, img_hdr->disk_size,
                                                &fw_upgrade_cxt->partition_hdl) != FW_UPGRADE_OK_E) {
                    ret = FW_UPGRADE_ERR_FLASH_CREATE_PARTITION_E;
                    break;
                }

                /*  erase first block */
                if (fw_upgrade_erase_partition(fw_upgrade_cxt->partition_hdl, 0, block_size) != FW_UPGRADE_OK_E) {
                    ret = FW_UPGRADE_ERR_FLASH_ERASE_PARTITION_E;
                    break;
                }
            }

            /*  process the case of image length is 0x0 when using all-in-one fw upgrade */
            if ((fw_upgrade_cxt->format != FW_UPGRADE_FORAMT_PARTIAL_UPGRADE) && (img_hdr->image_length == 0)) {
                LOG_DBG("%s: Processing zero-length image, image_id=%u", __func__, img_hdr->image_id);
                /*  free partition handle */
                ((fu_partition_client_t *)(fw_upgrade_cxt->partition_hdl))->ref_count = 0;
                fw_upgrade_cxt->partition_hdl = NULL;

                /*  FS1_IMG has its own start address and size */
                if (img_hdr->image_id != FS1_IMG_ID && img_hdr->image_id != SBL_IMG_ID) {
                    /*  adjust memory start address for next entry */
                    fw_upgrade_cxt->trial_mem_start += img_hdr->disk_size;
                    LOG_DBG("%s: Adjusted trial_mem_start to 0x%x", __func__, fw_upgrade_cxt->trial_mem_start);
                }

                /*  still have data at buffer and move to next image entry */
                fw_upgrade_cxt->image_index++;
                fw_upgrade_cxt->image_wrt_length = 0;
                LOG_DBG("Moving to next image, new image_index=%u, total_images=%u", 
                        fw_upgrade_cxt->image_index, fw_upgrade_cxt->total_images);

                /*  check if we have received all images */
                if (fw_upgrade_cxt->image_index >= fw_upgrade_cxt->total_images) {
                    LOG_INF("%s: All images processed, transitioning to DUPLICATE_FS_E", __func__);
                    fw_upgrade_set_state(FW_UPGRADE_STATE_DUPLICATE_FS_E);
                    break;
                } else {
                    LOG_INF("Still have %u images to process", 
                            fw_upgrade_cxt->total_images - fw_upgrade_cxt->image_index);
                }

                continue;
            }

            /*  reset crypto engine */
            mbedtls_sha256_init(fw_upgrade_cxt->digest_ctx);
            mbedtls_sha256_starts(fw_upgrade_cxt->digest_ctx, 0);
        }

        /*  set write_flash_len */
        if (fw_upgrade_cxt->format == FW_UPGRADE_FORAMT_PARTIAL_UPGRADE) {
            write_len = fw_upgrade_cxt->buf_len;
            internal_offset = 0;
            if (write_len > (fw_upgrade_cxt->image_wrt_length - fw_upgrade_cxt->image_wrt_count)) {
                ret = FW_UPGRADE_ERR_INCORRECT_IMAGE_LENGTH_E;
                break;
            }
        } else {
            write_len = MIN(buf_len, (fw_upgrade_cxt->image_wrt_length - fw_upgrade_cxt->image_wrt_count));
        }

        LOG_DBG("%s: Calculated write_len=%u (buf_len=%u, remaining=%u, internal_offset=%u)", 
                __func__, write_len, buf_len, (fw_upgrade_cxt->image_wrt_length - fw_upgrade_cxt->image_wrt_count), internal_offset);

        /*  CRITICAL FIX: Use internal_offset to index buffer */
        mbedtls_sha256_update(fw_upgrade_cxt->digest_ctx, &buffer[internal_offset], write_len);

        /*  check flash block if need erase first */
        {
            uint32_t first_block, last_block;

            if ((fw_upgrade_cxt->image_wrt_count / block_size) !=
                ((fw_upgrade_cxt->image_wrt_count + write_len - 1) / block_size)) {
                first_block = fw_upgrade_cxt->image_wrt_count / block_size + 1;

                last_block = (fw_upgrade_cxt->image_wrt_count + write_len) / block_size;
                if (((fw_upgrade_cxt->image_wrt_count + write_len) % block_size) != 0)
                    last_block++;
                /*  erase blocks */
                if (fw_upgrade_erase_partition(fw_upgrade_cxt->partition_hdl, first_block * block_size,
                                               (last_block - first_block) * block_size) != FW_UPGRADE_OK_E) {
                    ret = FW_UPGRADE_ERR_FLASH_ERASE_PARTITION_E;
                    break;
                }
            }
        }

        /*  write flash */
        /* CRITICAL FIX: Use internal_offset to index buffer */
        if (fw_upgrade_write_partition(fw_upgrade_cxt->partition_hdl, fw_upgrade_cxt->image_wrt_count,
                                       (char *)&buffer[internal_offset], write_len) != FW_UPGRADE_OK_E) {
            ret = FW_UPGRADE_ERR_FLASH_WRITE_PARTITION_E;
            break;
        }

        /*  update record */
        /* CRITICAL FIX: Update internal_offset and global counters */
        internal_offset += write_len;
        fw_upgrade_cxt->file_read_count += write_len;
        fw_upgrade_cxt->image_wrt_count += write_len;

        LOG_DBG("Updated counts - internal_offset=%u, wrt_count=%u, wrt_length=%u", 
                internal_offset, fw_upgrade_cxt->image_wrt_count, fw_upgrade_cxt->image_wrt_length);

        /*  flash one image, move to next one */
        if (fw_upgrade_cxt->image_wrt_count >= fw_upgrade_cxt->image_wrt_length) {
            LOG_INF("Image %u completed, image_id=%u, wrt_count=%u, wrt_length=%u", 
                    fw_upgrade_cxt->image_index, img_hdr->image_id, fw_upgrade_cxt->image_wrt_count, fw_upgrade_cxt->image_wrt_length);
            
            /*  verify image HASH */
            if ((ret = fw_upgrade_verify_image_hash(img_hdr)) != FW_UPGRADE_OK_E) {
                LOG_ERR("Image hash verification failed for image_id=%u", img_hdr->image_id);
                /* CRITICAL FIX: Clean up partition handle before returning error */
                if (fw_upgrade_cxt->partition_hdl != NULL) {
                    ((fu_partition_client_t *)(fw_upgrade_cxt->partition_hdl))->ref_count = 0;
                    fw_upgrade_cxt->partition_hdl = NULL;
                }
                break;
            }
            LOG_INF("Image hash verification passed for image_id=%u", img_hdr->image_id);

            /*  free partition handle */
            ((fu_partition_client_t *)(fw_upgrade_cxt->partition_hdl))->ref_count = 0;
            fw_upgrade_cxt->partition_hdl = NULL;

            /*  FS1_IMG has its own start address and size */
            if (img_hdr->image_id != FS1_IMG_ID && img_hdr->image_id != SBL_IMG_ID) {
                /*  adjust memory start address for next entry */
                fw_upgrade_cxt->trial_mem_start += img_hdr->disk_size;
                LOG_INF("Adjusted trial_mem_start to 0x%x for next image", fw_upgrade_cxt->trial_mem_start);
            }

            /*  still have data at buffer and move to next image entry */
            fw_upgrade_cxt->image_index++;
            fw_upgrade_cxt->image_wrt_length = 0;
            
            /* CRITICAL FIX: Do NOT reset internal_offset!
             * internal_offset maintains current position to handle remaining data for next image */
            LOG_INF("Moving to next image, new image_index=%u, total_images=%u, internal_offset=%u, remaining=%u", 
                    fw_upgrade_cxt->image_index, fw_upgrade_cxt->total_images, internal_offset, fw_upgrade_cxt->buf_len - internal_offset);

            if (fw_upgrade_cxt->format == FW_UPGRADE_FORAMT_PARTIAL_UPGRADE) {
                LOG_DBG("Partial upgrade mode, transitioning to DISCONNECT_SERVER_E");
                /*  move to next state */
                fw_upgrade_set_state(FW_UPGRADE_STATE_DISCONNECT_SERVER_E);
            } else {
                /*  check if we have received all images */
                if (fw_upgrade_cxt->image_index >= fw_upgrade_cxt->total_images) {
                    LOG_INF("All images completed, transitioning to DUPLICATE_FS_E");
                    fw_upgrade_set_state(FW_UPGRADE_STATE_DUPLICATE_FS_E);
                    break;
                } else {
                    LOG_DBG("More images to process, continuing with internal_offset=%u", internal_offset);
                    /* Continue while loop to process remaining data in current packet for next image */
                }
            }
        }

        /*  check if need erase block for next round */
        if (((fw_upgrade_cxt->image_wrt_count % block_size) == 0) && (fw_upgrade_cxt->image_wrt_length > 0)) {
            /*  erase block */
            if (fw_upgrade_erase_partition(fw_upgrade_cxt->partition_hdl, fw_upgrade_cxt->image_wrt_count,
                                           block_size) != FW_UPGRADE_OK_E) {
                ret = FW_UPGRADE_ERR_FLASH_ERASE_PARTITION_E;
                break;
            }
        }

        if (fw_upgrade_cxt->format == FW_UPGRADE_FORAMT_PARTIAL_UPGRADE) {
            /* it is done for this round */
            break;
        }
    }

    /* CRITICAL FIX: Update global buf_offset to internal_offset
     * This ensures correct state for next call (though in all-in-one mode each call is new packet) */
    fw_upgrade_cxt->buf_offset = internal_offset;

    return ret;
}

/*
 * process duplicate file system
 */
static fw_upgrade_status_code_t fw_upgrade_process_duplicate_fs(uint32_t flags)
{
    uint8_t *buf = NULL;
    uint32_t size;
    uint32_t offset;
    uint32_t disk_size;
    uint32_t nbytes;
    fu_part_hdl_t hdl1 = NULL;
    fu_part_hdl_t hdl2 = NULL;
    fw_upgrade_status_code_t ret = FW_UPGRADE_OK_E;

    UNUSED(flags);

    fw_upgrade_get_mem_block_size(&size);
    buf = (uint8_t *)k_malloc(size);
    if (buf == NULL) {
        ret = FW_UPGRADE_ERR_INSUFFICIENT_MEMORY_E;
        goto dup_fs_end;
    }

    /*  copy FS1 to FS2 */
    if ((fw_upgrade_find_partition(fw_upgrade_get_active_fwd(NULL, NULL), FS1_IMG_ID, &hdl1) != FW_UPGRADE_OK_E) ||
        (fw_upgrade_find_partition(fw_upgrade_get_active_fwd(NULL, NULL), FS2_IMG_ID, &hdl2) != FW_UPGRADE_OK_E)) {
        ret = FW_UPGRADE_ERR_FLASH_IMAGE_NOT_FOUND_E;
        goto dup_fs_end;
    }

    disk_size = ((fu_partition_client_t *)hdl1)->img_size;

    for (offset = 0; offset < disk_size; offset += size) {
        if (fw_upgrade_read_partition(hdl1, offset, (char *)buf, size, &nbytes) != FW_UPGRADE_OK_E) {
            ret = FW_UPGRADE_ERR_FLASH_READ_FAIL_E;
            break;
        }
        /*  erase one block */
        if (fw_upgrade_erase_partition(hdl2, offset, size) != FW_UPGRADE_OK_E) {
            ret = FW_UPGRADE_ERR_FLASH_ERASE_PARTITION_E;
            break;
        }
        /*  write flash */
        if (fw_upgrade_write_partition(hdl2, offset, (char *)buf, size) != FW_UPGRADE_OK_E) {
            ret = FW_UPGRADE_ERR_FLASH_WRITE_PARTITION_E;
            break;
        }
    }

dup_fs_end:
    if (buf) {
        k_free(buf);
    }
    if (hdl1) {
        ((fu_partition_client_t *)hdl1)->ref_count = 0;
        hdl1 = NULL;
    }
    if (hdl2) {
        ((fu_partition_client_t *)hdl2)->ref_count = 0;
        hdl2 = NULL;
    }

    return ret;
}

/*
 * process duplicate images from current to trial if need
 */
static fw_upgrade_status_code_t fw_upgrade_process_duplicate_images(void)
{
    fw_upgrade_status_code_t ret = FW_UPGRADE_OK_E;
    uint32_t i, offset, size, nbytes;
    uint8_t *buf = NULL;
    uint8_t active_fwd;
    fw_upgrade_context_t *fw_upgrade_cxt;
    fw_upgrade_image_hdr_t *img_hdr;
    fu_part_hdl_t hdl = NULL;

    fw_upgrade_cxt = fw_upgrade_get_context();
    if (fw_upgrade_cxt == NULL) {
        return FW_UPGRADE_ERR_SESSION_NOT_START_E;
    }

    fw_upgrade_get_mem_block_size(&size);
    active_fwd = fw_upgrade_get_active_fwd(NULL, NULL);
    buf = (uint8_t *)k_malloc(size);
    if (buf == NULL) {
        ret = FW_UPGRADE_ERR_INSUFFICIENT_MEMORY_E;
        goto dup_img_end;
    }

    /*  check images if need download */
    for (i = 0, img_hdr = fw_upgrade_image_hdr; i < fw_upgrade_cxt->total_images; i++, img_hdr++) {
        if ((fw_upgrade_cxt->download_flag[i] == 0) && (img_hdr->image_id != FS1_IMG_ID) &&
            (img_hdr->image_id != FS2_IMG_ID) && (img_hdr->image_id != SBL_IMG_ID)) {
            if (fw_upgrade_find_partition(active_fwd, img_hdr->image_id, &hdl) != FW_UPGRADE_OK_E) {
                ret = FW_UPGRADE_ERR_INCORRECT_IMAGE_HDR_E;
                break;
            }

            if (fw_upgrade_create_partition(fw_upgrade_cxt->trial_fwd_idx, img_hdr->image_id, img_hdr->version,
                                            fw_upgrade_cxt->trial_mem_start, img_hdr->disk_size,
                                            &fw_upgrade_cxt->partition_hdl) != FW_UPGRADE_OK_E) {
                ret = FW_UPGRADE_ERR_FLASH_CREATE_PARTITION_E;
                break;
            }

            for (offset = 0; offset < img_hdr->disk_size; offset += size) {
                fw_upgrade_read_partition(hdl, offset, (char *)buf, size, &nbytes);
                /*  erase one block */
                if (fw_upgrade_erase_partition(fw_upgrade_cxt->partition_hdl, offset, size) != FW_UPGRADE_OK_E) {
                    ret = FW_UPGRADE_ERR_FLASH_ERASE_PARTITION_E;
                    break;
                }
                /*  write flash */
                if (fw_upgrade_write_partition(fw_upgrade_cxt->partition_hdl, offset, (char *)buf, size) !=
                    FW_UPGRADE_OK_E) {
                    ret = FW_UPGRADE_ERR_FLASH_WRITE_PARTITION_E;
                    break;
                }
            }
            ((fu_partition_client_t *)hdl)->ref_count = 0;
            hdl = NULL;
            ((fu_partition_client_t *)(fw_upgrade_cxt->partition_hdl))->ref_count = 0;
            fw_upgrade_cxt->partition_hdl = NULL;

            /*  adjust memory start address for next entry */
            fw_upgrade_cxt->trial_mem_start += img_hdr->disk_size;
        }
    }

dup_img_end:
    if (fw_upgrade_cxt->partition_hdl) {
        ((fu_partition_client_t *)(fw_upgrade_cxt->partition_hdl))->ref_count = 0;
        fw_upgrade_cxt->partition_hdl = NULL;
    }
    if (hdl != NULL) {
        ((fu_partition_client_t *)hdl)->ref_count = 0;
    }
    if (buf != NULL) {
        k_free(buf);
    }
    return ret;
}

int fw_upgrade_session_process(uint32_t offset, const uint8_t *data, uint32_t len)
{
	/* Auto-initialize on first write (offset == 0)
	 * If offset is 0 and upload is already active, this means the client
	 * is restarting the upload (possibly after an error). Reset the context.
	 */

    int32_t ret = FW_UPGRADE_OK_E;
    uint8_t *buffer = NULL;
    uint8_t run = 1;
    uint32_t received;
    fw_upgrade_context_t *fw_upgrade_cxt=NULL;

	LOG_DBG("%s: ENTRY offset=%u, len=%u", __func__, offset, len);

    if (offset == 0) {
        {            
			if(fw_upgrade_session_init() != FW_UPGRADE_OK_E)
				return FW_UPGRADE_ERR_SESSION_NOT_START_E;
		}		
			/* get fw upgrade context */
			fw_upgrade_cxt = fw_upgrade_get_context();
			if (fw_upgrade_cxt == NULL) {
				return FW_UPGRADE_ERR_SESSION_NOT_START_E;
			}
			
			/* Initialize buf_offset for MCUmgr packet tracking */
			fw_upgrade_cxt->buf_offset = 0;
		}
		
		/* get fw upgrade context */
		fw_upgrade_cxt = fw_upgrade_get_context();
		if (fw_upgrade_cxt == NULL) {
			LOG_ERR("qcom_img_mgmt_upload_write1: fw_upgrade_cxt is NULL");
			return FW_UPGRADE_ERR_SESSION_NOT_START_E;
		}
		
		/* Reset buf_offset for each new packet */
		fw_upgrade_cxt->buf_offset = 0;
		
		LOG_DBG("qcom_img_mgmt_upload_write1: buf_offset=%u, len=%u, state=%d", 
		        fw_upgrade_cxt->buf_offset, len, fw_upgrade_get_state());
		
                /* Check if all bytes from current packet have been consumed */
                while ((run == 1) &&
                       (((fw_upgrade_get_session_status() == FW_UPGRADE_SESSION_RUNNING_E) &&
                       (fw_upgrade_cxt->buf_offset <= len)) ||
                ((fw_upgrade_get_state() == FW_UPGRADE_STATE_DUPLICATE_IMAGES_E) ||
                (fw_upgrade_get_state() == FW_UPGRADE_STATE_DUPLICATE_FS_E) ||
                (fw_upgrade_get_state() == FW_UPGRADE_STATE_FINISH_E))))
		{
				LOG_DBG("qcom_img_mgmt_upload_write1: Processing state=%d, buf_offset=%u/%u", 
				        fw_upgrade_get_state(), fw_upgrade_cxt->buf_offset, len);
				
				switch (fw_upgrade_get_state()) {
					case FW_UPGRADE_STATE_NOT_START_E:
						fw_upgrade_set_state(FW_UPGRADE_STATE_GET_TRIAL_INFO_E);
						break;
		
					case FW_UPGRADE_STATE_GET_TRIAL_INFO_E:

				
						/*  locate available partition for trial FWD */
						if (fw_upgrade_select_trial_fwd(&(fw_upgrade_cxt->trial_fwd_idx), &(fw_upgrade_cxt->trial_mem_start),
														&(fw_upgrade_cxt->trial_mem_size)) != FW_UPGRADE_OK_E) {
							ret = FW_UPGRADE_ERR_FLASH_NOT_SUPPORT_FW_UPGRADE_E;
							run = 0;
							break;
						}
		
						if (fw_upgrade_get_active_fwd(NULL, NULL) == fw_upgrade_cxt->trial_fwd_idx) {
							/*  if current running FWD is trial FWD, just reject the request */
							ret = FW_UPGRADE_ERR_TRIAL_IS_RUNNING_E;
							run = 0;
							break;
						}
		
                        LOG_INF("Selected trial FWD index=%u, mem_start=0x%x, mem_size=0x%x",
                                 fw_upgrade_cxt->trial_fwd_idx,
                                 fw_upgrade_cxt->trial_mem_start,
                                 fw_upgrade_cxt->trial_mem_size);
						fw_upgrade_set_state(FW_UPGRADE_STATE_ERASE_FWD_E);
						break;
		
				case FW_UPGRADE_STATE_ERASE_FWD_E:
					/*  erase trial FWD */
					if (fw_upgrade_erase_fwd(fw_upgrade_cxt->trial_fwd_idx) != FW_UPGRADE_OK_E) {
						ret = FW_UPGRADE_ERR_FLASH_ERASE_FAIL_E;
						run = 0;
						break;
					}
#ifdef CONFIG_FW_UPGRADE_NO_FS
					fw_upgrade_set_state(FW_UPGRADE_STATE_PREPARE_FS_E);
#else
					fw_upgrade_set_state(FW_UPGRADE_STATE_ERASE_SECOND_FS_E);
#endif
					break;
		
					case FW_UPGRADE_STATE_ERASE_SECOND_FS_E: {
						uint32_t disk_size;
		
						if (fw_upgrade_find_partition(fw_upgrade_get_active_fwd(NULL, NULL), FS2_IMG_ID,
													  &fw_upgrade_cxt->partition_hdl) != FW_UPGRADE_OK_E) {
							ret = FW_UPGRADE_ERR_FLASH_IMAGE_NOT_FOUND_E;
							run = 0;
							break;
						}
		
                        disk_size = ((fu_partition_client_t *)(fw_upgrade_cxt->partition_hdl))->img_size;

                        /*  Print partition info before erase */
                        {
                            uint32_t block_size = 0;
                            
                            fw_upgrade_get_mem_block_size(&block_size);
                        }

                        /*  erase flash where to store the second FS */
                        if (fw_upgrade_erase_partition(fw_upgrade_cxt->partition_hdl, 0, disk_size) != FW_UPGRADE_OK_E) {
                            LOG_ERR("Failed to erase second FS partition");
                            ret = FW_UPGRADE_ERR_FLASH_ERASE_PARTITION_E;
							run = 0;
							break;
						}
						((fu_partition_client_t *)(fw_upgrade_cxt->partition_hdl))->ref_count = 0;
						fw_upgrade_cxt->partition_hdl = NULL;
		
						fw_upgrade_set_state(FW_UPGRADE_STATE_PREPARE_FS_E);
						break;
					}
		
					case FW_UPGRADE_STATE_PREPARE_FS_E: {
#ifndef CONFIG_FW_UPGRADE_NO_FS
						uint32_t disk_size = 0, disk_start = 0, version = 0;
						if (fw_upgrade_find_partition(fw_upgrade_get_active_fwd(NULL, NULL), FS1_IMG_ID,
													  &fw_upgrade_cxt->partition_hdl) != FW_UPGRADE_OK_E) {
							ret = FW_UPGRADE_ERR_FLASH_IMAGE_NOT_FOUND_E;
							run = 0;
                            LOG_ERR("Failed to find FS1 partition");
							break;
						}

						disk_size = ((fu_partition_client_t *)(fw_upgrade_cxt->partition_hdl))->img_size;
						disk_start = ((fu_partition_client_t *)(fw_upgrade_cxt->partition_hdl))->img_start;
						version = ((fu_partition_client_t *)(fw_upgrade_cxt->partition_hdl))->img_version;
						((fu_partition_client_t *)(fw_upgrade_cxt->partition_hdl))->ref_count = 0;
						fw_upgrade_cxt->partition_hdl = NULL;

                        /*  Create FS2 (ID: 128) in trial FWD */
						if (fw_upgrade_create_partition(fw_upgrade_cxt->trial_fwd_idx, FS2_IMG_ID, version, disk_start,
														disk_size, &fw_upgrade_cxt->partition_hdl) != FW_UPGRADE_OK_E) {
							LOG_ERR("Failed to create FS2 partition");
							ret = FW_UPGRADE_ERR_FLASH_CREATE_PARTITION_E;
							run = 0;
							break;
						}
						fw_upgrade_cxt->hidden_images_count++;
						((fu_partition_client_t *)(fw_upgrade_cxt->partition_hdl))->ref_count = 0;
						fw_upgrade_cxt->partition_hdl = NULL;
		
#endif
		
						if (fw_upgrade_find_partition(fw_upgrade_get_active_fwd(NULL, NULL), RAMDUMP_IMG_ID,
													  &fw_upgrade_cxt->partition_hdl) == FW_UPGRADE_OK_E) {
							uint32_t rd_disk_size = 0, rd_disk_start = 0, rd_version = 0;
							uint32_t ramdump_magic_number = 0;
							uint32_t nbytes = 0;
		
							rd_disk_size = ((fu_partition_client_t *)(fw_upgrade_cxt->partition_hdl))->img_size;
							rd_disk_start = ((fu_partition_client_t *)(fw_upgrade_cxt->partition_hdl))->img_start;
							rd_version = ((fu_partition_client_t *)(fw_upgrade_cxt->partition_hdl))->img_version;
		
							if ((fw_upgrade_read_partition(fw_upgrade_cxt->partition_hdl, FLASH_OFFSET_MAGIC_NUM_ADDRESS,
														   (char *)&ramdump_magic_number, sizeof(ramdump_magic_number),
														   &nbytes) != FW_UPGRADE_OK_E) ||
								(nbytes != sizeof(ramdump_magic_number))) {
								LOG_ERR("Failed to read ramdump partition magic number");

								ret = FW_UPGRADE_ERR_FLASH_ERASE_PARTITION_E;
								run = 0;
								break;
							}
		
							if (ramdump_magic_number != FLASH_ERASED_VALUE) {
								if (fw_upgrade_erase_partition(fw_upgrade_cxt->partition_hdl, 0, rd_disk_size) !=
									FW_UPGRADE_OK_E) {
									ret = FW_UPGRADE_ERR_FLASH_ERASE_PARTITION_E;
									run = 0;
								LOG_ERR("Failed to erase ramdump partition");
									break;
								}
							}
		
							((fu_partition_client_t *)(fw_upgrade_cxt->partition_hdl))->ref_count = 0;
							fw_upgrade_cxt->partition_hdl = NULL;
							if (fw_upgrade_create_partition(fw_upgrade_cxt->trial_fwd_idx, RAMDUMP_IMG_ID, rd_version,
															rd_disk_start, rd_disk_size,
															&fw_upgrade_cxt->partition_hdl) != FW_UPGRADE_OK_E) {
								ret = FW_UPGRADE_ERR_FLASH_CREATE_PARTITION_E;
								run = 0;
								LOG_ERR("Failed to create ramdump partition");

								break;
							}
							fw_upgrade_cxt->hidden_images_count++;
							((fu_partition_client_t *)(fw_upgrade_cxt->partition_hdl))->ref_count = 0;
							fw_upgrade_cxt->partition_hdl = NULL;
						}
		
						if (fw_upgrade_find_partition(fw_upgrade_get_active_fwd(NULL, NULL), USERDATA_IMG_ID,
													  &fw_upgrade_cxt->partition_hdl) == FW_UPGRADE_OK_E) {
							uint32_t usr_disk_size = 0, usr_disk_start = 0, usr_version = 0;
		
							usr_disk_size = ((fu_partition_client_t *)(fw_upgrade_cxt->partition_hdl))->img_size;
							usr_disk_start = ((fu_partition_client_t *)(fw_upgrade_cxt->partition_hdl))->img_start;
							usr_version = ((fu_partition_client_t *)(fw_upgrade_cxt->partition_hdl))->img_version;
							((fu_partition_client_t *)(fw_upgrade_cxt->partition_hdl))->ref_count = 0;
							fw_upgrade_cxt->partition_hdl = NULL;
		
							if (fw_upgrade_create_partition(fw_upgrade_cxt->trial_fwd_idx, USERDATA_IMG_ID, usr_version,
															usr_disk_start, usr_disk_size,
															&fw_upgrade_cxt->partition_hdl) != FW_UPGRADE_OK_E) {
								ret = FW_UPGRADE_ERR_FLASH_CREATE_PARTITION_E;
								run = 0;
								LOG_ERR("Failed to create userdata partition 1");

                                break;
							}
							fw_upgrade_cxt->hidden_images_count++;
							((fu_partition_client_t *)(fw_upgrade_cxt->partition_hdl))->ref_count = 0;
							fw_upgrade_cxt->partition_hdl = NULL;
						}
		
						fw_upgrade_set_state(FW_UPGRADE_STATE_PREPARE_CONNECT_E);
						break;
					}
		
					case FW_UPGRADE_STATE_PREPARE_CONNECT_E:
						/* check if received config file already */
						if (fw_upgrade_cxt->is_first == 0) {
							fw_upgrade_image_hdr_t *img_hdr;
							uint32_t disk_size, disk_start;
		
							for (; fw_upgrade_cxt->image_index < fw_upgrade_cxt->total_images; fw_upgrade_cxt->image_index++) {
								if (fw_upgrade_cxt->download_flag[fw_upgrade_cxt->image_index] != 0) {
									break;
								}
							}
		
							/*  this is special case for FS1 IMG and remote file size is 0 */
							img_hdr = fw_upgrade_image_hdr;
							img_hdr += fw_upgrade_cxt->image_index;
		
							if ((img_hdr->image_id == FS1_IMG_ID) && (img_hdr->image_length == 0)) {
								if (fw_upgrade_find_partition(fw_upgrade_get_active_fwd(NULL, NULL), FS2_IMG_ID,
															  &fw_upgrade_cxt->partition_hdl) != FW_UPGRADE_OK_E) {
									ret = FW_UPGRADE_ERR_FLASH_IMAGE_NOT_FOUND_E;
									run = 0;
									break;
								}
		
								disk_size = ((fu_partition_client_t *)(fw_upgrade_cxt->partition_hdl))->img_size;
								disk_start = ((fu_partition_client_t *)(fw_upgrade_cxt->partition_hdl))->img_start;
								((fu_partition_client_t *)(fw_upgrade_cxt->partition_hdl))->ref_count = 0;
								fw_upgrade_cxt->partition_hdl = NULL;
		
								/*  File system disk size is pre-set when first time download */
								/*  Firmware Upgrade can't change size other than original size */
								if (img_hdr->disk_size > disk_size) {
									ret = FW_UPGRADE_ERR_INCORRECT_IMAGE_LENGTH_E;
									run = 0;
									break;
								}
		
								/*  create image for trial image's FS2 */
								if (fw_upgrade_create_partition(fw_upgrade_cxt->trial_fwd_idx, img_hdr->image_id,
																img_hdr->version, disk_start, disk_size,
																&fw_upgrade_cxt->partition_hdl) != FW_UPGRADE_OK_E) {
									ret = FW_UPGRADE_ERR_FLASH_CREATE_PARTITION_E;
									run = 0;
									break;
								}
								((fu_partition_client_t *)(fw_upgrade_cxt->partition_hdl))->ref_count = 0;
								fw_upgrade_cxt->partition_hdl = NULL;
								fw_upgrade_cxt->image_index++;
								fw_upgrade_set_state(FW_UPGRADE_STATE_PREPARE_CONNECT_E);
								break;
							}
		
							if (fw_upgrade_cxt->image_index >= fw_upgrade_cxt->total_images) {
								fw_upgrade_set_state(FW_UPGRADE_STATE_DUPLICATE_IMAGES_E);
							} else {
								fw_upgrade_set_state(FW_UPGRADE_STATE_CONNECT_E);
							}
						} else {
							fw_upgrade_set_state(FW_UPGRADE_STATE_CONNECT_E);
						}
						break;
		
					case FW_UPGRADE_STATE_CONNECT_E:
						/*  jump to FW_UPGRADE_STATE_RECEIVE_DATA_E */
						fw_upgrade_set_state(FW_UPGRADE_STATE_RECEIVE_DATA_E);
						break;
		
					case FW_UPGRADE_STATE_RESUME_SERVICE_E: {
						uint32_t offset, len, nbytes, total;
						fw_upgrade_image_hdr_t *img_hdr;
		
						/* get fw upgrade session context */
						fw_upgrade_cxt = fw_upgrade_get_context();
						if (fw_upgrade_cxt == NULL) {
							ret = FW_UPGRADE_ERR_SESSION_NOT_START_E;
							run = 0;
							break;
						}
		
						img_hdr = fw_upgrade_image_hdr;
						img_hdr += fw_upgrade_cxt->image_index;
		
						/*  open partition handle */
						if (fw_upgrade_find_partition(fw_upgrade_cxt->trial_fwd_idx, img_hdr->image_id,
													  &fw_upgrade_cxt->partition_hdl) != FW_UPGRADE_OK_E) {
							ret = FW_UPGRADE_ERR_FLASH_IMAGE_NOT_FOUND_E;
							run = 0;
							break;
						}
		
						fw_upgrade_sess_cxt->digest_ctx = (mbedtls_sha256_context *)k_malloc(sizeof(mbedtls_sha256_context));
						if (fw_upgrade_sess_cxt->digest_ctx == NULL) {
							ret = FW_UPGRADE_ERR_INSUFFICIENT_MEMORY_E;
							run = 0;
							break;
						}
						mbedtls_sha256_init(fw_upgrade_sess_cxt->digest_ctx);
						mbedtls_sha256_starts(fw_upgrade_sess_cxt->digest_ctx, 0);
		
						offset = 0;
						len = 0;
						total = 0;
		
						/*  calculate fw upgrade image HASH */
						while (total < fw_upgrade_cxt->image_wrt_count) {
							if ((fw_upgrade_cxt->image_wrt_count - total) > FW_UPGRADE_BUF_SIZE) {
								len = FW_UPGRADE_BUF_SIZE;
							} else {
								len = fw_upgrade_cxt->image_wrt_count - total;
							}
							if (fw_upgrade_read_partition(fw_upgrade_cxt->partition_hdl, offset, (char *)buffer, len,
														  &nbytes) != FW_UPGRADE_OK_E) {
								ret = FW_UPGRADE_ERR_FLASH_READ_FAIL_E;
								run = 0;
								break;
							}
		
							mbedtls_sha256_update(fw_upgrade_cxt->digest_ctx, buffer, nbytes);
							offset += nbytes;
							total += nbytes;
						}
		
						/*  erase image if need */
		
						fw_upgrade_set_state(FW_UPGRADE_STATE_RESUME_SERVER_E);
						break;
					}

					case FW_UPGRADE_STATE_RESUME_SERVER_E:
					{
						fw_upgrade_set_state(FW_UPGRADE_STATE_RECEIVE_DATA_E);
						break;
					}
                    
					case FW_UPGRADE_STATE_RECEIVE_DATA_E:
						LOG_DBG("FW_UPGRADE_STATE_RECEIVE_DATA_E: buf_offset=%u, len=%u", 
						        fw_upgrade_cxt->buf_offset, len);

						/* Receiving data from MCUmgr packet */
						received = len - fw_upgrade_cxt->buf_offset;
						buffer   = (uint8_t*)data + fw_upgrade_cxt->buf_offset;
						ret 	 = FW_UPGRADE_OK_E;
						
						LOG_DBG("RECEIVE_DATA: received=%u bytes, is_first=%d", 
						        received, fw_upgrade_cxt->is_first);
						
						if ((ret == FW_UPGRADE_OK_E) && (received > 0)) {
							/* handle data */
							fw_upgrade_cxt->buf_len = received;
							fw_upgrade_cxt->buf_offset = 0;
		
							if (fw_upgrade_cxt->is_first == 1) {
								fw_upgrade_set_state(FW_UPGRADE_STATE_PROCESS_CONFIG_FILE_E);
							} else {
								fw_upgrade_set_state(FW_UPGRADE_STATE_PROCESS_IMAGE_E);
							}
						} else if ((ret == FW_UPGRADE_OK_E) && (received == 0)) {
							/*  no more data */
							run = 0;
						} else if (ret != FW_UPGRADE_OK_E) {
							/*  can't get data */
							run = 0;
						}
						break;
		
				case FW_UPGRADE_STATE_PROCESS_CONFIG_FILE_E:
					LOG_DBG("FW_UPGRADE_STATE_PROCESS_CONFIG_FILE_E: processing config file, buf_len=%u", 
					        fw_upgrade_cxt->buf_len);
					/* parse fw upgrade image Header */
					if ((ret = fw_upgrade_process_config_file(buffer)) != FW_UPGRADE_OK_E) {
						LOG_ERR("fw_upgrade_process_config_file FAILED: ret=%d", ret);
						run = 0;
					} else {
						LOG_INF("Config file processing succeeded, state after=%d", fw_upgrade_get_state());
						/* CRITICAL FIX: Do NOT override buf_offset set by fw_upgrade_process_config_file!
						 * The config file processing function correctly sets buf_offset to the actual
						 * config file length, preserving remaining image data in the first packet.
						 * DO NOT set fw_upgrade_cxt->buf_offset = len here!
						 */
						LOG_DBG("Config file processing preserved buf_offset=%u (not overriding to %u)", 
						        fw_upgrade_cxt->buf_offset, len);
					}
					break;
		
				case FW_UPGRADE_STATE_PROCESS_IMAGE_E:
					LOG_DBG("FW_UPGRADE_STATE_PROCESS_IMAGE_E: processing image data");
					/* CRITICAL FIX: Do NOT adjust buffer pointer here!
					 * Pass original data pointer to fw_upgrade_process_receive_image
					 * which will use internal_offset to manage buffer indexing */
					if ((ret = fw_upgrade_process_receive_image((uint8_t*)data)) != FW_UPGRADE_OK_E) {
						LOG_ERR("fw_upgrade_process_receive_image FAILED: ret=%d", ret);
						
						run = 0;
					} else {
						/* buf_offset is updated internally by fw_upgrade_process_receive_image */
						if (fw_upgrade_get_state() == FW_UPGRADE_STATE_PROCESS_IMAGE_E) {
							/*  continue receiving data */
							fw_upgrade_set_state(FW_UPGRADE_STATE_RECEIVE_DATA_E);
						}
					}
					break;
		
					case FW_UPGRADE_STATE_DISCONNECT_SERVER_E:
						fw_upgrade_set_state(FW_UPGRADE_STATE_PREPARE_CONNECT_E);
						break;
		
					case FW_UPGRADE_STATE_DUPLICATE_IMAGES_E:
						/*  duplicate images from current to trial if need */
						if ((ret = fw_upgrade_process_duplicate_images()) != FW_UPGRADE_OK_E) {
							run = 0;
							break;
						}
						fw_upgrade_set_state(FW_UPGRADE_STATE_DUPLICATE_FS_E);
						break;
		
					case FW_UPGRADE_STATE_DUPLICATE_FS_E:
						/*  check duplicate FS flag */
						if (fw_upgrade_cxt->flags & FW_UPGRADE_FLAG_DUPLICATE_ACTIVE_FS) {
							/*  copy files from FS1 to FS2 */
							if ((ret = fw_upgrade_process_duplicate_fs(fw_upgrade_cxt->flags)) != FW_UPGRADE_OK_E) {
								run = 0;
								break;
							}
						}
		
						fw_upgrade_set_state(FW_UPGRADE_STATE_FINISH_E);
						break;
					case FW_UPGRADE_STATE_FINISH_E:
                        fw_upgrade_session_finalize(FW_UPGRADE_OK_E);
						run = 0;
						break;
					default:
						break;
				}  /*  switch(... */
		
		}  /*  while(... */
		
		LOG_DBG("%s: EXIT ret=%d, final_buf_offset=%u/%u", 
	        __func__, ret, fw_upgrade_cxt ? fw_upgrade_cxt->buf_offset : 0, len);
	
	return ret;
}

/*
 * finalize fw upgrade session
 */
static int32_t fw_upgrade_session_finalize(int32_t ret)
{
    fw_upgrade_context_t *fw_upgrade_cxt;
    uint8_t status;
    fu_part_hdl_t hdl;
    fw_upgrade_status_code_t resp;
    /* get fw upgrade session context */
    fw_upgrade_cxt = fw_upgrade_get_context();
    if (fw_upgrade_cxt == NULL) {
        return FW_UPGRADE_ERR_SESSION_NOT_START_E;
    }

    /*  check if this session is cancelled or set to suspend */
    if (fw_upgrade_get_session_status() == FW_UPGRADE_SESSION_CANCEL_E) {
        ret = FW_UPGRADE_ERR_SESSION_CANCELLED_E;
    } else if ((ret == FW_UPGRADE_OK_E) && (fw_upgrade_get_state() != FW_UPGRADE_STATE_FINISH_E)) {
        ret = FW_UPGRADE_ERR_INCOMPLETE_E;
    }

    /*  set final error code */
    fw_upgrade_set_error_code(ret);

    /* everything is good */
    if ((ret == FW_UPGRADE_OK_E) && (fw_upgrade_get_state() == FW_UPGRADE_STATE_FINISH_E)) {
        status = FW_UPGRADE_FWD_STATUS_VALID;
#if 1 /*  mcumgr */
        fw_upgrade_set_fwd_info(fw_upgrade_cxt->trial_fwd_idx, FW_UPGRADE_FWD_STATUS_E, (uint8_t *)&status);
#else
		/* uint32 val; */
		ret = fw_upgrade_get_fwd_info_addr(fw_upgrade_cxt->trial_fwd_idx, FW_UPGRADE_FWD_STATUS_E, 
                                            &mcumgr_upgrade_result.trial_fwd_status_addr);
		if(FW_UPGRADE_OK_E != ret)
		{
			return ret;
		}
		mcumgr_upgrade_result.trial_flag = 1<<0;
		mcumgr_upgrade_result.trial_fwd_status_val = status;

#endif


        /* set SBL Trial FDE */
        resp = fw_upgrade_find_partition(fw_upgrade_cxt->trial_fwd_idx, SBL_IMG_ID, &hdl);
        if (resp == FW_UPGRADE_OK_E) {
#if 1 /*  mcumgr */
            fw_upgrade_set_sbl_trial_fde(fw_upgrade_cxt->trial_sbl_idx, ((fu_partition_client_t *)hdl)->img_version);
#else
		mcumgr_upgrade_result.trial_flag = 1<<1;
		mcumgr_upgrade_result.trial_sbl_idx = fw_upgrade_cxt->trial_sbl_idx;
		mcumgr_upgrade_result.trial_sbl_version = ((fu_partition_client_t *)hdl)->img_version;
#endif
        }


    }

    if (fw_upgrade_get_session_status() == FW_UPGRADE_SESSION_SUSPEND_E) {
        fw_upgrade_session_prepare_suspend();
        ret = FW_UPGRADE_ERR_SESSION_SUSPEND_E;
    } else {
        int boot_flag = fw_upgrade_cxt->flags & FW_UPGRADE_FLAG_AUTO_REBOOT;
        fw_upgrade_session_fin();
                /*  check AUTO_REBOOT flag */
        if (boot_flag) {
            /* fw_upgrade_session_fin(); */
            /*  Schedule delayed reset to allow response to be sent */
            LOG_INF("fw_upgrade: Scheduling delayed system reset in 500ms");
            k_work_schedule(&fw_upgrade_reset_work, K_MSEC(500));
        }
    }
    return ret;
}

/**********************************************************************************************************/
/* 																                                          */
/**********************************************************************************************************/

/*
 * done firmware upgrade session
 */
fw_upgrade_status_code_t fw_upgrade_session_done(uint32_t result)
{
    fw_upgrade_status_code_t ret = FW_UPGRADE_ERROR_E;
    uint8_t trial, current;
    uint32_t rank;

    if (fw_upgrade_get_trial_active_fwd_index(&trial, &current, &rank) == FW_UPGRADE_OK_E) {
        /* process trial image based on result */
        if (result) {
            /* accept trial image here */
            if (fw_upgrade_accept_trial_fwd() == FW_UPGRADE_OK_E)
                ret = FW_UPGRADE_OK_E;
        } else {
            /*reject trial image */
            if (fw_upgrade_reject_trial_fwd() == FW_UPGRADE_OK_E)
                ret = FW_UPGRADE_OK_E;
        }
    }

    return ret;
}
