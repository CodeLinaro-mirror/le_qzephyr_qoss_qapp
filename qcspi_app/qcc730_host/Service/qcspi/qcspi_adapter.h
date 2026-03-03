/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef QCSPI_ADAPTER_H_
#define QCSPI_ADAPTER_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @brief Initialize QCSPI adapter
 *
 * Initializes the platform hardware and QCSPI adapter.
 * Platform hardware (SPI, GPIO) must be initialized before calling this:
 * - FreeRTOS: MX_SPI_Init() and MX_GPIO_Init() must be called first
 * - Zephyr: Hardware initialized automatically by device tree
 *
 * @return 0 on success, negative errno on failure
 */
int qcspi_adapter_init(void);

/**
 * @brief Deinitialize QCSPI adapter
 *
 * @return 0 on success, negative errno on failure
 */
int qcspi_adapter_deinit(void);

/**
 * @brief Read data from Slave memory
 *
 * Handles 4-byte alignment automatically.
 *
 * @param addr Remote memory address
 * @param buf Buffer to store read data
 * @param len Number of bytes to read
 * @return 0 on success, negative errno on failure
 */
int qcspi_adapter_mem_read(uint32_t addr, void *buf, size_t len);

/**
 * @brief Write data to Slave memory
 *
 * Handles 4-byte alignment automatically.
 *
 * @param addr Remote memory address
 * @param buf Buffer containing data to write
 * @param len Number of bytes to write
 * @return 0 on success, negative errno on failure
 */
int qcspi_adapter_mem_write(uint32_t addr, const void *buf, size_t len);

/**
 * @brief Trigger interrupt on Slave (A2F interrupt)
 *
 * @return 0 on success, negative errno on failure
 */
int qcspi_adapter_trigger_irq(void);

/**
 * @brief Check if QCSPI adapter is initialized
 *
 * @return true if initialized, false otherwise
 */
bool qcspi_adapter_is_initialized(void);

/* ========== Low-level QCSPI protocol functions (for advanced use) ========== */

/**
 * @brief Read data from Slave memory (must be 4-byte aligned)
 *
 * @param size Number of bytes to read (must be multiple of 4)
 * @param address Remote memory address (must be 4-byte aligned)
 * @param rcv_buf Buffer to store read data
 * @return 0 on success, negative errno on failure
 */
int qcspi_read(uint16_t size, uint32_t address, uint8_t *rcv_buf);

/**
 * @brief Write data to Slave memory (must be 4-byte aligned)
 *
 * @param size Number of bytes to write (must be multiple of 4)
 * @param address Remote memory address (must be 4-byte aligned)
 * @param snd_buf Buffer containing data to write
 * @return 0 on success, negative errno on failure
 */
int qcspi_write(uint16_t size, uint32_t address, uint8_t *snd_buf);

/**
 * @brief Read Slave status register
 *
 * @param status Pointer to store status value
 * @return 0 on success, negative errno on failure
 */
int qcspi_RDSR(uint32_t *status);

/**
 * @brief Read Slave internal register (IRR command)
 *
 * @param reg_addr Register address
 * @param reg_val Pointer to store register value
 * @return 0 on success, negative errno on failure
 */
int qcspi_IRR(uint8_t reg_addr, uint32_t *reg_val);

/**
 * @brief Write Slave internal register (IRW command)
 *
 * @param reg_addr Register address
 * @param reg_val Value to write
 * @return 0 on success, negative errno on failure
 */
int qcspi_IRW(uint8_t reg_addr, uint32_t reg_val);

/**
 * @brief Get Slave slave ID
 *
 * @param id_array Array to store 3-byte ID
 */
void qcspi_get_slaveid(uint8_t *id_array);

/**
 * @brief Reset Slave SPI core
 */
void qcspi_reset(void);

/**
 * @brief Convert 32-bit word to big-endian byte array
 *
 * @param byte_value_buf Output buffer (4 bytes)
 * @param word_value Input word value
 */
void qcspi_word_to_byte_big_endian(uint8_t *byte_value_buf, uint32_t word_value);

#endif /* QCSPI_ADAPTER_H_ */
