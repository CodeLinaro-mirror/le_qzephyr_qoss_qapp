/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 * 
 * Flash adapter for Zephyr
 * Provides flash operations interface for firmware upgrade
 */

#include "fw_upgrade_types.h"
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/logging/log.h>
#include <errno.h>

LOG_MODULE_REGISTER(flash_adapter, CONFIG_LOG_DEFAULT_LEVEL);

/* Global flash device handle */
const struct device *flash_adapter_dev = NULL;

/* Global flash configuration */
static flash_config_data_t flash_config;
static bool flash_initialized = false;


/**
 * @brief Initialize flash adapter
 * @return FLASH_DEVICE_DONE on success, FLASH_DEVICE_FAIL on error
 */
int flash_adapter_init(void)
{
    if (flash_initialized) {
        return FLASH_DEVICE_DONE;
    }

    /* Get QSPI Flash device using nodelabel
     * Priority order: qspi_nor_flash2 (qcc730evbx) > qspi_nor_flash3 (qcc730mi) > qspi_nor_flash1
     */
    #if DT_NODE_HAS_STATUS(DT_NODELABEL(qspi_nor_flash2), okay)
        flash_adapter_dev = DEVICE_DT_GET(DT_NODELABEL(qspi_nor_flash2));
        LOG_INF("Using qspi_nor_flash2 for flash operations");
    #elif DT_NODE_HAS_STATUS(DT_NODELABEL(qspi_nor_flash3), okay)
        flash_adapter_dev = DEVICE_DT_GET(DT_NODELABEL(qspi_nor_flash3));
        LOG_INF("Using qspi_nor_flash3 for flash operations");
    #elif DT_NODE_HAS_STATUS(DT_NODELABEL(qspi_nor_flash1), okay)
        flash_adapter_dev = DEVICE_DT_GET(DT_NODELABEL(qspi_nor_flash1));
        LOG_INF("Using qspi_nor_flash1 for flash operations");
    #else
        LOG_ERR("No flash device found! Enable qspi_nor_flash1/2/3 in board DTS");
        return FLASH_DEVICE_FAIL;
    #endif
    
    if (!device_is_ready(flash_adapter_dev)) {
        LOG_ERR("Flash device not ready");
        return FLASH_DEVICE_FAIL;
    }

    /* Get flash size and calculate density in blocks */
    uint64_t flash_size_64;
    int ret = flash_get_size(flash_adapter_dev, &flash_size_64);
    uint32_t flash_size;
    
    if (ret == 0) {
        flash_size = (uint32_t)flash_size_64;
    } else {
        /* Fallback to default 4MB for QCC730 */
        flash_size = 4 * 1024 * 1024;
        LOG_WRN("Failed to get flash size, using default 4MB");
    }
    
    flash_config.density_in_blocks = flash_size / BLOCK_SIZE_IN_BYTES;
    flash_initialized = true;
    
    LOG_INF("Flash initialized: size=%u bytes, blocks=%u", 
            flash_size, flash_config.density_in_blocks);
    
    return FLASH_DEVICE_DONE;
}

/**
 * @brief Get flash configuration
 * @return Pointer to flash config structure, NULL on error
 */
flash_config_data_t *flash_adapter_get_config(void)
{
    if (!flash_initialized) {
        if (flash_adapter_init() != FLASH_DEVICE_DONE) {
            return NULL;
        }
    }
    
    return &flash_config;
}

/**
 * @brief Read data from flash
 * @param address Flash address to read from
 * @param size Number of bytes to read
 * @param buffer Buffer to store read data
 * @param callback Callback function (unused in Zephyr implementation)
 * @param context Callback context (unused in Zephyr implementation)
 * @return FLASH_DEVICE_DONE on success, FLASH_DEVICE_FAIL on error
 */
int flash_adapter_read(uint32_t address, uint32_t size, void *buffer, void *callback, void *context)
{
    if (!flash_initialized || !flash_adapter_dev || !buffer) {
        LOG_ERR("Flash not initialized or invalid parameters");
        return FLASH_DEVICE_FAIL;
    }
    
    int ret = flash_read(flash_adapter_dev, address, buffer, size);
    if (ret != 0) {
        LOG_ERR("Flash read failed at 0x%x, size=%u: %d", address, size, ret);
        return FLASH_DEVICE_FAIL;
    }
    
    LOG_DBG("Flash read: addr=0x%x, size=%u", address, size);
    return FLASH_DEVICE_DONE;
}

/**
 * @brief Write data to flash
 * @param address Flash address to write to
 * @param size Number of bytes to write
 * @param buffer Buffer containing data to write
 * @param callback Callback function (unused in Zephyr implementation)
 * @param context Callback context (unused in Zephyr implementation)
 * @return FLASH_DEVICE_DONE on success, FLASH_DEVICE_FAIL on error
 */
int flash_adapter_write(uint32_t address, uint32_t size, void *buffer, void *callback, void *context)
{
    if (!flash_initialized || !flash_adapter_dev || !buffer) {
        LOG_ERR("Flash not initialized or invalid parameters");
        return FLASH_DEVICE_FAIL;
    }
    
    int ret = flash_write(flash_adapter_dev, address, buffer, size);
    if (ret != 0) {
        LOG_ERR("Flash write failed at 0x%x, size=%u: %d", address, size, ret);
        return FLASH_DEVICE_FAIL;
    }
    
    LOG_DBG("Flash write: addr=0x%x, size=%u", address, size);
    return FLASH_DEVICE_DONE;
}

/**
 * @brief Erase flash blocks
 * @param erase_type Type of erase operation
 * @param start_block Starting block number
 * @param num_blocks Number of blocks to erase
 * @param callback Callback function (unused in Zephyr implementation)
 * @param context Callback context (unused in Zephyr implementation)
 * @return FLASH_DEVICE_DONE on success, FLASH_DEVICE_FAIL on error
 */
int flash_adapter_erase(flash_erase_type_t erase_type, uint32_t start_block, uint32_t num_blocks, void *callback, void *context)
{
    if (!flash_initialized || !flash_adapter_dev) {
        LOG_ERR("Flash not initialized");
        return FLASH_DEVICE_FAIL;
    }
    
    uint32_t address = start_block * BLOCK_SIZE_IN_BYTES;
    uint32_t size = num_blocks * BLOCK_SIZE_IN_BYTES;
    
    int ret = flash_erase(flash_adapter_dev, address, size);
    if (ret != 0) {
        LOG_ERR("Flash erase failed at block %u, count=%u (addr=0x%x, size=%u): %d", 
                start_block, num_blocks, address, size, ret);
        return FLASH_DEVICE_FAIL;
    }
    
    LOG_DBG("Flash erase: block=%u, count=%u (addr=0x%x, size=%u)", 
            start_block, num_blocks, address, size);
    return FLASH_DEVICE_DONE;
}
