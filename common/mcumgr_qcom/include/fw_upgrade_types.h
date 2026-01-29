/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FW_UPGRADE_TYPES_H
#define _FW_UPGRADE_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>


/* extern uint32_t _ln_FDT_Start_Addr; */
/* extern uint32_t _ln_SBL_MEM_Size; */
#define FDT_START                      0x208000 /* ((void *)&_ln_FDT_Start_Addr) */
#define SBL_MEM_SIZE                     0x8000 /* ((uint32_t)&_ln_SBL_MEM_Size) */


/* Type mappings for QURT/QAPI to standard C types */
/* Use system types directly - don't redefine to avoid conflicts */
/* The HAL headers define uint32 as unsigned long, we need to use that */
#include "com_dtypes.h"  /* This defines uint32, uint8, boolean */

#ifndef TRUE
#define TRUE true
#endif
#ifndef FALSE
#define FALSE false
#endif

/* QAPI status type mapping */
typedef int32_t qapi_Status_t;
#define QAPI_OK 0
#define QAPI_ERROR -1

/* Use Zephyr flash APIs instead of drv_flash.h/ferm_flash.h */
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>

/* Flash erase type enum */
typedef enum {
    FLASH_BLOCK_ERASE_E = 0,
    FLASH_BULK_ERASE_E,
    FLASH_CHIP_ERASE_E
} flash_erase_type_t;

/* Flash block size constant */
#define BLOCK_SIZE_IN_BYTES 4096

/* Flash config data stub - simplified version */
typedef struct {
    uint32_t density_in_blocks;
} flash_config_data_t;

/* Memory copy function mapping */
#define memscpy(dst, dst_size, src, src_size) \
    memcpy(dst, src, (dst_size < src_size) ? dst_size : src_size)

/* RRAM access functions - these need to be implemented or stubbed */
extern int nt_rram_read(uint32_t address, void *buffer, uint32_t size);
extern int nt_rram_write(uint32_t address, void *buffer, uint32_t size);

/* Flash status constants - matching drv_flash.h */
#ifndef FLASH_DEVICE_DONE
#define FLASH_DEVICE_DONE 0
#endif

#ifndef FLASH_DEVICE_FAIL
#define FLASH_DEVICE_FAIL 1
#endif

/* Configuration defines - use extern declaration instead of #define to allow runtime modification */
#ifndef CONFIG_FW_UPGRADE_FWD_SUPPORT_NUM
extern int CONFIG_FW_UPGRADE_FWD_SUPPORT_NUM;
#endif

/* Flash driver function mappings to Zephyr APIs */
extern const struct device *fw_upgrade_flash_dev;

/* Flash driver function declarations */
int drv_flash_init(void);
flash_config_data_t *drv_flash_get_config(void);
int drv_flash_read(uint32_t address, uint32_t size, void *buffer, void *callback, void *context);
int drv_flash_write(uint32_t address, uint32_t size, void *buffer, void *callback, void *context);
int drv_flash_erase(flash_erase_type_t erase_type, uint32_t start_block, uint32_t num_blocks, void *callback, void *context);

#endif /* _FW_UPGRADE_TYPES_H */
