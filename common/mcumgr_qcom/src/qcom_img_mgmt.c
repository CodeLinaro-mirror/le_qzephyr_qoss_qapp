/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>
#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <string.h>

#include "qcom_img_mgmt.h"
#include "fw_upgrade.h"

LOG_MODULE_REGISTER(qcom_img_mgmt, CONFIG_QCOM_IMG_MGMT_LOG_LEVEL);

/* Delay before reset after confirm (in milliseconds) */
#define QCOM_IMG_MGMT_RESET_DELAY_MS 500

static void qcom_img_mgmt_reset_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(qcom_img_mgmt_reset_work, qcom_img_mgmt_reset_work_handler);

/* SHA256 support - check if mbedtls is available */
#if defined(CONFIG_MBEDTLS) && defined(CONFIG_MBEDTLS_SHA256)
#include <mbedtls/sha256.h>
#define HASH_ENABLED 1
#define SHA256_DIGEST_SIZE 32
#else
#define HASH_ENABLED 0
#define SHA256_DIGEST_SIZE 32
/* Dummy SHA256 context if mbedtls not available */
typedef struct {
	uint8_t dummy[128];
} mbedtls_sha256_context;
#endif

#define FLASH_BLOCK_SIZE 4096  /* 4KB flash block size */

/* FWD Magic number - same as defined in fw_upgrade_mem.c */
#define FW_UPGRADE_MAGIC_V1 0x54445746  /* "FWDT" */

/* MCUmgr image management group ID */
#define MGMT_GROUP_ID_IMAGE 1

/* MCUmgr image management command IDs */
#define IMG_MGMT_ID_STATE       0
#define IMG_MGMT_ID_UPLOAD      1
#define IMG_MGMT_ID_ERASE       5

/* Flash device */
static const struct device *flash_dev;


	/* Upload context - Simplified to only include used members */
static struct {
	uint32_t offset;            /* Current offset - only used member */
} upload_ctx;



/* Forward declarations */
static int qcom_img_mgmt_state_read(struct smp_streamer *ctxt);
static int qcom_img_mgmt_state_write(struct smp_streamer *ctxt);
static int qcom_img_mgmt_upload(struct smp_streamer *ctxt);
static int qcom_img_mgmt_erase(struct smp_streamer *ctxt);

/**
 * Work handler for system reset after image confirm
 * 
 * This function is called after a delay to perform the actual system reset.
 * The delay allows the MCUmgr response to be sent before the reset occurs.
 */
static void qcom_img_mgmt_reset_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	
	LOG_INF("Performing system reset after image confirm");
	/* sys_reboot(SYS_REBOOT_WARM); */
	nt_system_sw_reset();
}

/* MCUmgr handlers */
static const struct mgmt_handler qcom_img_mgmt_handlers[] = {
	[IMG_MGMT_ID_STATE] = {
		.mh_read = qcom_img_mgmt_state_read,
		.mh_write = qcom_img_mgmt_state_write,
	},
	[IMG_MGMT_ID_UPLOAD] = {
		.mh_read = NULL,
		.mh_write = qcom_img_mgmt_upload,
	},
	[IMG_MGMT_ID_ERASE] = {
		.mh_read = NULL,
		.mh_write = qcom_img_mgmt_erase,
	},
};

static struct mgmt_group qcom_img_mgmt_group = {
	.mg_handlers = qcom_img_mgmt_handlers,
	.mg_handlers_count = ARRAY_SIZE(qcom_img_mgmt_handlers),
	.mg_group_id = MGMT_GROUP_ID_IMAGE,
};

/* Get flash device */
static const struct device *get_flash_device(void)
{
	if (flash_dev == NULL) {
		/* Try to get flash device - check multiple possible labels
		 * Priority order based on board configurations:
		 * - qspi_nor_flash3: used by qcc730mi
		 * - qspi_nor_flash2: used by qcc730evbx  
		 * - qspi_nor_flash1: fallback option
		 * - rram: internal RRAM as last resort
		 * 
		 * Use DT_HAS_STATUS to check if node exists AND is enabled
		 */
#if DT_NODE_HAS_STATUS(DT_NODELABEL(qspi_nor_flash3), okay)
		flash_dev = DEVICE_DT_GET(DT_NODELABEL(qspi_nor_flash3));
		LOG_INF("Using qspi_nor_flash3 device");
#elif DT_NODE_HAS_STATUS(DT_NODELABEL(qspi_nor_flash2), okay)
		flash_dev = DEVICE_DT_GET(DT_NODELABEL(qspi_nor_flash2));
		LOG_INF("Using qspi_nor_flash2 device");
#elif DT_NODE_HAS_STATUS(DT_NODELABEL(qspi_nor_flash1), okay)
		flash_dev = DEVICE_DT_GET(DT_NODELABEL(qspi_nor_flash1));
		LOG_INF("Using qspi_nor_flash1 device");
#else
		LOG_ERR("No suitable flash device found in device tree");
		return NULL;
#endif
		if (!device_is_ready(flash_dev)) {
			LOG_ERR("Flash device not ready");
			return NULL;
		}
	}
	return flash_dev;
}

/* Read image state - MCUmgr command handler
 * 
 * This function reads all 3 FWD slots and reports their status according to MCUmgr protocol.
 * 
 * Mapping from QCC730 FWD to MCUmgr flags:
 * - bootable: FWD status == VALID
 * - active: FWD is the currently running image (determined by FDT)
 * - confirmed: FWD rank is Golden (1) or Current (>1)
 * - permanent: FWD rank is Golden (1)
 * - pending: FWD rank is Trial (0)
 */
static int qcom_img_mgmt_state_read(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	bool ok;
	int ret;
	int rc;
	uint8_t trial_idx, current_idx, active_fwd_idx;
	uint32_t max_rank;
	uint32_t config;
	
	/* Initialize fw_upgrade module */
	rc = fw_upgrade_init();
	if(rc != FW_UPGRADE_OK_E)
	{
		LOG_ERR("img mgmt init failed %d", rc); 
		return MGMT_ERR_EUNKNOWN;
	}


	extern int fw_upgrade_display_fwd(void);
	fw_upgrade_display_fwd();	

	/* Get FWD configuration (2 or 3 FWDs) */
	config = CONFIG_FW_UPGRADE_FWD_SUPPORT_NUM;
	
	/* Get trial, current FWD indices and max rank using fw_upgrade API */
	ret = fw_upgrade_get_trial_active_fwd_index(&trial_idx, &current_idx, &max_rank);
	
	/* Get active (running) FWD index */
	active_fwd_idx = fw_upgrade_get_active_fwd(NULL, NULL);
	
	LOG_INF("FWD Status: trial=%u, current=%u, active=%u, max_rank=%u, config=%u",
	        trial_idx, current_idx, active_fwd_idx, max_rank, config);

	/* Start images array */
	ok = zcbor_tstr_put_lit(zse, "images") &&
	     zcbor_list_start_encode(zse, config);
	
	if (!ok) {
		return MGMT_ERR_ENOMEM;
	}
		
	/* Read and report FWD slots based on configuration */
	for (int slot = 0; slot < config; slot++) {
		uint32_t magic, rank, version;
		uint8_t status, total_images;
		bool bootable = false;
		bool active = false;
		bool confirmed = false;
		bool permanent = false;
		bool pending = false;
		
		/* Read FWD info using fw_upgrade APIs */
		if (fw_upgrade_get_fwd_info(slot, FW_UPGRADE_FWD_MAGIC_E, (uint8_t *)&magic) != FW_UPGRADE_OK_E) {
			continue;
		}
		if (fw_upgrade_get_fwd_info(slot, FW_UPGRADE_FWD_RANK_E, (uint8_t *)&rank) != FW_UPGRADE_OK_E) {
			continue;
		}
		if (fw_upgrade_get_fwd_info(slot, FW_UPGRADE_FWD_STATUS_E, &status) != FW_UPGRADE_OK_E) {
			continue;
		}
		if (fw_upgrade_get_fwd_info(slot, FW_UPGRADE_FWD_TOTAL_IMAGE_E, &total_images) != FW_UPGRADE_OK_E) {
			continue;
		}
		if (fw_upgrade_get_fwd_info(slot, FW_UPGRADE_FWD_VERSION_E, (uint8_t *)&version) != FW_UPGRADE_OK_E) {
			version = 0;
		}
		
		/* Check if FWD is valid (bootable) */
		if (magic == FW_UPGRADE_MAGIC_V1 && 
		    status == FW_UPGRADE_FWD_STATUS_VALID &&
		    total_images > 0 && total_images != 0xFF) {
			bootable = true;
			
			/* Determine flags based on fw_upgrade_get_trial_active_fwd_index() result */
			if (ret == FW_UPGRADE_OK_E && slot == trial_idx) {
				/* This is the trial FWD */
				pending = true;
			} else if (slot == current_idx) {
				/* This is the current FWD */
				confirmed = true;
				if (rank == FW_UPGRADE_FWD_RANK_GOLDEN) {
					permanent = true;
				}
			} else if (rank == FW_UPGRADE_FWD_RANK_GOLDEN) {
				/* Golden FWD (slot 0 in 3-FWD config) */
				confirmed = true;
				permanent = true;
			}
			
			/* Check if this is the active (running) FWD */
			if (slot == active_fwd_idx) {
				active = true;
			}
		}
		
		LOG_DBG("Slot %d: bootable=%d, active=%d, confirmed=%d, permanent=%d, pending=%d",
		        slot, bootable, active, confirmed, permanent, pending);
		
		/* Start slot map */
		ok = zcbor_map_start_encode(zse, 10);
		if (!ok) {
			return MGMT_ERR_ENOMEM;
		}
		
		/* Slot number */
		ok = zcbor_tstr_put_lit(zse, "slot") &&
		     zcbor_uint32_put(zse, slot);
		if (!ok) {
			return MGMT_ERR_ENOMEM;
		}
		
		/* Only include detailed info if slot is bootable */
		if (bootable) {
			/* Version */
			char version_str[16];
			snprintf(version_str, sizeof(version_str), "%u.0.0", version);
			ok = zcbor_tstr_put_lit(zse, "version") &&
			     zcbor_tstr_put_term(zse, version_str, sizeof(version_str));
			if (!ok) {
				return MGMT_ERR_ENOMEM;
			}

			#if 0
			/* Hash (placeholder - would need actual hash from FWD) */
			uint8_t hash_placeholder[4] = {0};
			ok = zcbor_tstr_put_lit(zse, "hash") &&
			     zcbor_bstr_encode_ptr(zse, hash_placeholder, sizeof(hash_placeholder));
			if (!ok) {
				return MGMT_ERR_ENOMEM;
			}
			#endif
			
			/* Bootable flag */
			ok = zcbor_tstr_put_lit(zse, "bootable") &&
			     zcbor_bool_put(zse, bootable);
			if (!ok) {
				return MGMT_ERR_ENOMEM;
			}
			
			/* Pending flag */
			ok = zcbor_tstr_put_lit(zse, "pending") &&
			     zcbor_bool_put(zse, pending);
			if (!ok) {
				return MGMT_ERR_ENOMEM;
			}
			
			/* Confirmed flag */
			ok = zcbor_tstr_put_lit(zse, "confirmed") &&
			     zcbor_bool_put(zse, confirmed);
			if (!ok) {
				return MGMT_ERR_ENOMEM;
			}
			
			/* Active flag */
			ok = zcbor_tstr_put_lit(zse, "active") &&
			     zcbor_bool_put(zse, active);
			if (!ok) {
				return MGMT_ERR_ENOMEM;
			}
			
			/* Permanent flag */
			ok = zcbor_tstr_put_lit(zse, "permanent") &&
			     zcbor_bool_put(zse, permanent);
			if (!ok) {
				return MGMT_ERR_ENOMEM;
			}
		}
		
		/* End slot map */
		ok = zcbor_map_end_encode(zse, 10);
		if (!ok) {
			return MGMT_ERR_ENOMEM;
		}

	}
	
	/* End images array */
	ok = zcbor_list_end_encode(zse, config);
	if (!ok) {
		return MGMT_ERR_ENOMEM;
	}
	
	/* Split status (not used in our implementation) */
	ok = zcbor_tstr_put_lit(zse, "splitStatus") &&
	     zcbor_uint32_put(zse, 0);
	
	return ok ? MGMT_ERR_EOK : MGMT_ERR_ENOMEM;
}

/* Write image state - MCUmgr command handler
 * 
 * This function handles the "confirm" operation which accepts the trial FWD
 * and promotes it to current status.
 * 
 * Reference: fw_upgrade_accept_trial_fwd() from fw_upgrade_mem.c
 * 
 * The confirm operation:
 * 1. Verifies that the active FWD is trial (only trial can be confirmed)
 * 2. Increments the trial FWD's rank to make it the new current
 * 3. Invalidates the old current FWD (except if it's golden in 3-FWD mode)
 */
static int qcom_img_mgmt_state_write(struct smp_streamer *ctxt)
{
	zcbor_state_t *zsd = ctxt->reader->zs;
	zcbor_state_t *zse = ctxt->writer->zs;
	size_t decoded;
	struct zcbor_string hash = { 0 };
	bool confirm = false;
	bool ok;
	int ret;
	int mgmt_err;

	struct zcbor_map_decode_key_val image_set_decode[] = {
		ZCBOR_MAP_DECODE_KEY_VAL(hash, zcbor_bstr_decode, &hash),
		ZCBOR_MAP_DECODE_KEY_VAL(confirm, zcbor_bool_decode, &confirm),
	};

	ok = zcbor_map_decode_bulk(zsd, image_set_decode,
				    ARRAY_SIZE(image_set_decode), &decoded);

	if (!ok) {
		return MGMT_ERR_EINVAL;
	}


	LOG_INF("Confirming current image confirm %d", confirm);
	if (confirm) {
		ret = fw_upgrade_session_done(1);  /* 1 indicates accept trial FWD */
		if (ret != 0) {
			LOG_ERR("Failed to accept trial FWD: %d", ret);
			
			mgmt_err = MGMT_ERR_EUNKNOWN;
			
			ok = zcbor_tstr_put_lit(zse, "rc") &&
			     zcbor_uint32_put(zse, mgmt_err);
			return mgmt_err;
		}
		
		LOG_INF("Trial FWD accepted and promoted to current");
		
		/* Schedule delayed reset to allow response to be sent */
		LOG_INF("System reset scheduled in %d ms", QCOM_IMG_MGMT_RESET_DELAY_MS);
		k_work_schedule(&qcom_img_mgmt_reset_work, K_MSEC(QCOM_IMG_MGMT_RESET_DELAY_MS));
	}

	/* Return success */
	ok = zcbor_tstr_put_lit(zse, "rc") &&
	     zcbor_uint32_put(zse, MGMT_ERR_EOK);

	return ok ? MGMT_ERR_EOK : MGMT_ERR_ENOMEM;
}

/* Upload image - MCUmgr command handler */
static int qcom_img_mgmt_upload(struct smp_streamer *ctxt)
{
	zcbor_state_t *zsd = ctxt->reader->zs;
	zcbor_state_t *zse = ctxt->writer->zs;
	size_t decoded;
	struct zcbor_string data = { 0 };
	uint32_t image = 0;
	uint32_t len = 0;
	uint32_t off = 0;
	struct zcbor_string sha = { 0 };
	bool ok;
	int rc;


	struct zcbor_map_decode_key_val upload_decode[] = {
		ZCBOR_MAP_DECODE_KEY_VAL(image, zcbor_uint32_decode, &image),
		ZCBOR_MAP_DECODE_KEY_VAL(len, zcbor_uint32_decode, &len),
		ZCBOR_MAP_DECODE_KEY_VAL(off, zcbor_uint32_decode, &off),
		ZCBOR_MAP_DECODE_KEY_VAL(data, zcbor_bstr_decode, &data),
		ZCBOR_MAP_DECODE_KEY_VAL(sha, zcbor_bstr_decode, &sha),
	};

	LOG_DBG("=== Starting CBOR decode ===");
	
	ok = zcbor_map_decode_bulk(zsd, upload_decode,
				    ARRAY_SIZE(upload_decode), &decoded) == 0;

	if (!ok) {
		LOG_ERR("=== CBOR decode FAILED ===");
		LOG_ERR("Decoded count: %zu (expected 5)", decoded);
		LOG_ERR("This means %zu parameters were successfully decoded before failure", decoded);
		return MGMT_ERR_EINVAL;
	}
	
	LOG_DBG("=== CBOR decode SUCCESS ===");
	LOG_DBG("Decoded %zu parameters", decoded);

	LOG_DBG("Upload: off=%u len=%u data_len=%u", off, len, data.len);

	/* Initialize upload on first chunk */
	if (off == 0) {
		upload_ctx.offset = 0;  /* Initialize expected offset */
	}

	/* Validate offset to handle duplicated or unexpected data segments */
	if (off < upload_ctx.offset) {
		/* Duplicate data segment - client is retransmitting */
		LOG_WRN("Duplicate data segment: received_off=%u, expected_off=%u", off, upload_ctx.offset);
		
		/* Check if this is exactly the same segment we already processed */
		if (off + data.len <= upload_ctx.offset) {
			/* Completely duplicate segment - acknowledge without processing */
			LOG_INF("Completely duplicate segment, acknowledging without processing");
			goto send_response;
		} else {
			/* Partially duplicate segment - process only the new part */
			uint32_t skip_bytes = upload_ctx.offset - off;
			uint32_t new_data_len = data.len - skip_bytes;
			
			LOG_INF("Partially duplicate segment: skipping %u bytes, processing %u bytes", 
			        skip_bytes, new_data_len);
			
			if (new_data_len > 0) {
			rc = fw_upgrade_session_process(upload_ctx.offset, 
			                                data.value + skip_bytes, 
			                                new_data_len);
			if (rc != 0) {
				LOG_ERR("Upload write failed: %d", rc);
				goto err;
			}
				upload_ctx.offset += new_data_len;
			}
		}
	} else if (off > upload_ctx.offset) {
		/* Gap in data - unexpected offset */
		LOG_DBG("Unexpected data segment: received_off=%u, expected_off=%u (gap=%u)", 
		        off, upload_ctx.offset, off - upload_ctx.offset);
		
		/* Return current expected offset to allow client to resume correctly */
		goto send_response;
	} else {
		/* Expected offset - normal case */
		if (data.len > 0) {
			rc = fw_upgrade_session_process(off, data.value, data.len);
			if (rc != 0) {
				LOG_ERR("Upload write failed: %d", rc);
				goto err;
			}
			upload_ctx.offset = off + data.len;
		}
	}

	
	/* Return success with offset */
	ok = zcbor_tstr_put_lit(zse, "rc") &&
	     zcbor_uint32_put(zse, MGMT_ERR_EOK) &&
	     zcbor_tstr_put_lit(zse, "off") &&
	     zcbor_uint32_put(zse, off + data.len);

	return ok ? MGMT_ERR_EOK : MGMT_ERR_ENOMEM;

err:
	/* Map fw_upgrade error codes to appropriate MCUmgr error codes */
	{
		int mgmt_err;
		switch (rc) {
		case FW_UPGRADE_ERR_INCORRECT_IMAGE_CHECKSUM_E:
			/* Use ENOMEM instead of ECORRUPT to force mcumgr CLI to stop immediately
			 * Testing shows that ECORRUPT (9) may not stop some mcumgr CLI versions,
			 * but ENOMEM (2) is more reliably treated as a fatal error.
			 */
			mgmt_err = MGMT_ERR_ENOMEM;
			LOG_ERR("Image checksum verification failed - returning ENOMEM to stop upload");
			break;
		case FW_UPGRADE_ERR_INSUFFICIENT_MEMORY_E:
			mgmt_err = MGMT_ERR_ENOMEM;
			break;
		case FW_UPGRADE_ERR_INVALID_PARAM_E:
		case FW_UPGRADE_ERR_INCORRECT_IMAGE_HDR_E:
		case FW_UPGRADE_ERR_INCORRECT_IMAGE_LENGTH_E:
			mgmt_err = MGMT_ERR_EINVAL;
			break;
		case FW_UPGRADE_ERR_FLASH_NOT_ENOUGH_SPACE_E:
			mgmt_err = MGMT_ERR_ENOMEM;
			break;
		case FW_UPGRADE_ERR_FLASH_WRITE_FAIL_E:
		case FW_UPGRADE_ERR_FLASH_ERASE_FAIL_E:
		case FW_UPGRADE_ERR_FLASH_READ_FAIL_E:
			mgmt_err = MGMT_ERR_EUNKNOWN;  /* Hardware failure */
			break;
		case FW_UPGRADE_ERR_SESSION_IN_PROGRESS_E:
		case FW_UPGRADE_ERR_TRIAL_IS_RUNNING_E:
			mgmt_err = MGMT_ERR_EBUSY;
			break;
		case FW_UPGRADE_ERR_SESSION_NOT_START_E:
		case FW_UPGRADE_ERR_NOT_INIT_E:
			mgmt_err = MGMT_ERR_EBADSTATE;
			break;
		case FW_UPGRADE_ERR_SERVER_RSP_TIMEOUT_E:
		case FW_UPGRADE_ERR_FLASH_INIT_TIMEOUT_E:
			mgmt_err = MGMT_ERR_ETIMEOUT;
			break;
		default:
			mgmt_err = MGMT_ERR_EUNKNOWN;
			break;
		}
		
		LOG_ERR("fw_upgrade error %d mapped to MCUmgr error %d", rc, mgmt_err);
		
		ok = zcbor_tstr_put_lit(zse, "rc") &&
		     zcbor_uint32_put(zse, mgmt_err);
		return mgmt_err;
	}

send_response:
	/* Return success with current expected offset to allow client to resume */
	ok = zcbor_tstr_put_lit(zse, "rc") &&
	     zcbor_uint32_put(zse, MGMT_ERR_EOK) &&
	     zcbor_tstr_put_lit(zse, "off") &&
	     zcbor_uint32_put(zse, upload_ctx.offset);

	return ok ? MGMT_ERR_EOK : MGMT_ERR_ENOMEM;
}

/* Erase slot - MCUmgr command handler
 * 
 * Note: Flash erase operations can take significant time (several seconds for large areas).
 * The MCUmgr client should set appropriate timeout values.
 */
static int qcom_img_mgmt_erase(struct smp_streamer *ctxt)
{
	zcbor_state_t *zsd = ctxt->reader->zs;
	zcbor_state_t *zse = ctxt->writer->zs;
	size_t decoded;
	uint32_t slot = 1;  /* Default to trial slot (slot 1) */
	bool ok;
	int rc;

	LOG_INF("MCUmgr erase command received");

	/* Try to decode slot parameter from request */
	struct zcbor_map_decode_key_val erase_decode[] = {
		ZCBOR_MAP_DECODE_KEY_VAL(slot, zcbor_uint32_decode, &slot),
	};

	ok = zcbor_map_decode_bulk(zsd, erase_decode,
				    ARRAY_SIZE(erase_decode), &decoded);

	/* If no slot parameter provided, use default (trial slot) */
	if (!ok || decoded == 0) {
		slot = 1;  /* Default to slot 1 (trial slot) */
		LOG_INF("No slot parameter, using default trial slot");
	}

	/* Validate slot parameter */
	if (slot >= CONFIG_FW_UPGRADE_FWD_SUPPORT_NUM) {
		LOG_ERR("Invalid slot: %u", slot);
		ok = zcbor_tstr_put_lit(zse, "rc") &&
		     zcbor_uint32_put(zse, MGMT_ERR_EINVAL);
		return MGMT_ERR_EINVAL;
	}

	LOG_INF("Erasing slot %u - this may take several seconds...", slot);

	rc = fw_upgrade_init();
	if(rc != FW_UPGRADE_OK_E)
	{
		LOG_INF("erase init failed %d", rc); 
		rc = MGMT_ERR_EINVAL;
	}
	else
	{
		rc = fw_upgrade_erase_fwd(slot);
		LOG_INF("erase failed %d", rc); 
		if(rc == FW_UPGRADE_ERR_INVALID_PARAM_E)
		{
			rc = MGMT_ERR_EINVAL;
		}
		else if (rc == FW_UPGRADE_ERR_NOT_INIT_E)
		{
			rc = MGMT_ERR_ENOENT;
		}
		else if (rc == FW_UPGRADE_ERROR_E)
		{
			rc = MGMT_ERR_ETIMEOUT;
		}

	}
	if (rc != 0) {
		LOG_ERR("Failed to erase slot %u: %d", slot, rc);
		
		/* Provide more specific error information */
		int mgmt_err;
		if (rc == -EPERM) {
			mgmt_err = MGMT_ERR_EBADSTATE;
		} else if (rc == -ENOENT) {
			mgmt_err = MGMT_ERR_ENOENT;
		} else {
			mgmt_err = MGMT_ERR_EUNKNOWN;
		}
		
		ok = zcbor_tstr_put_lit(zse, "rc") &&
		     zcbor_uint32_put(zse, mgmt_err);
		return mgmt_err;
	}

	LOG_INF("Slot %u erased successfully", slot);

	ok = zcbor_tstr_put_lit(zse, "rc") &&
	     zcbor_uint32_put(zse, MGMT_ERR_EOK);

	return ok ? MGMT_ERR_EOK : MGMT_ERR_ENOMEM;
}

/* Initialize image management */
int qcom_img_mgmt_init(void)
{
	/* Initialize state first */
	memset(&upload_ctx, 0, sizeof(upload_ctx));
	/* memset(&img_state, 0, sizeof(img_state)); */

	/* Register MCUmgr group */
	mgmt_register_group(&qcom_img_mgmt_group);

	/* Try to get flash device, but don't fail initialization if not available
	 * Flash device will be checked again when actually needed for operations */
	if (get_flash_device() == NULL) {
		LOG_WRN("Flash device not ready during init, will retry when needed");
		/* Don't return error - allow system to continue booting */
	} else {
		LOG_INF("Flash device ready");
	}

	LOG_INF("Image management initialized");

	return 0;
}

/* Auto-initialize at system startup */
SYS_INIT(qcom_img_mgmt_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
