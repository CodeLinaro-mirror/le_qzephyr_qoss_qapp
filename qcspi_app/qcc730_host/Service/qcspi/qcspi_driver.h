/*
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef QCSPI_DRIVER_H_
#define QCSPI_DRIVER_H_

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Initialize QCSPI driver
 *
 * Initializes the platform hardware and QCSPI driver.
 * Platform hardware (SPI, GPIO) must be initialized before calling this:
 * - FreeRTOS: MX_SPI_Init() and MX_GPIO_Init() must be called first
 * - Zephyr: Hardware initialized automatically by device tree
 *
 * @return 0 on success, negative errno on failure
 */
int qcspi_init(void);

/**
 * @brief Read data from Slave memory with 4-byte alignment handling
 *
 * @param handle Handle (reserved for future use)
 * @param bytes_to_read Number of bytes to read
 * @param addr Remote memory address
 * @param read_data_buf Buffer to store read data
 * @return 0 on success, negative errno on failure
 */
int qcspi_read_addr_align(uint32_t handle, uint16_t bytes_to_read, uint32_t addr, uint8_t *read_data_buf);

/**
 * @brief Write data to Slave memory with 4-byte alignment handling
 *
 * @param handle Handle (reserved for future use)
 * @param bytes_to_write Number of bytes to write
 * @param addr Remote memory address
 * @param write_data_buf Buffer containing data to write
 * @return 0 on success, negative errno on failure
 */
int qcspi_write_addr_align(uint32_t handle, uint16_t bytes_to_write, uint32_t addr, uint8_t *write_data_buf);

/**
 * @brief Read data from Slave memory (must be 4-byte aligned)
 *
 * @param handle Handle (reserved for future use)
 * @param size Number of bytes to read (must be multiple of 4)
 * @param address Remote memory address (must be 4-byte aligned)
 * @param rcv_buf Buffer to store read data
 * @return 0 on success, negative errno on failure
 */
int qcspi_read(uint32_t handle, uint16_t size, uint32_t address, uint8_t *rcv_buf);

/**
 * @brief Write data to Slave memory (must be 4-byte aligned)
 *
 * @param handle Handle (reserved for future use)
 * @param size Number of bytes to write (must be multiple of 4)
 * @param address Remote memory address (must be 4-byte aligned)
 * @param snd_buf Buffer containing data to write
 * @return 0 on success, negative errno on failure
 */
int qcspi_write(uint32_t handle, uint16_t size, uint32_t address, uint8_t *snd_buf);

/**
 * @brief Trigger interrupt on Slave (A2F interrupt)
 *
 * @param handle Handle (reserved for future use)
 * @return 0 on success, negative errno on failure
 */
int qcspi_interrupt(uint32_t handle);

/**
 * @brief Read Slave status register
 *
 * @param handle Handle (reserved for future use)
 * @param status Pointer to store status value
 * @return 0 on success, negative errno on failure
 */
int qcspi_RDSR(uint32_t handle, uint32_t *status);

/**
 * @brief Read Slave internal register (IRR command)
 *
 * @param handle Handle (reserved for future use)
 * @param reg_addr Register address
 * @param reg_val Pointer to store register value
 * @return 0 on success, negative errno on failure
 */
int qcspi_IRR(uint32_t handle, uint8_t reg_addr, uint32_t *reg_val);

/**
 * @brief Write Slave internal register (IRW command)
 *
 * @param handle Handle (reserved for future use)
 * @param reg_addr Register address
 * @param reg_val Value to write
 * @return 0 on success, negative errno on failure
 */
int qcspi_IRW(uint32_t handle, uint8_t reg_addr, uint32_t reg_val);

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

#endif /* QCSPI_DRIVER_H_ */
