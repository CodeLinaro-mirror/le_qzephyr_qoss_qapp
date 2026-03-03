/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef QCSPI_PROTOCOL_H_
#define QCSPI_PROTOCOL_H_

#include <stdint.h>

/* SPI slave CMD codes */
#define SPI_CMDCODE_NOP 0x00
#define SPI_CMDCODE_WREN 0x06
#define SPI_CMDCODE_RESET_REQUEST_BYTE0 0xDA
#define SPI_CMDCODE_RESET_REQUEST_BYTE1 0xBA
#define SPI_CMDCODE_RESET_BYTE2 0x00
#define SPI_CMDCODE_REQUEST_BYTE2 0x80
#define SPI_CMDCODE_RDSR 0x05
#define SPI_CMDCODE_RDID 0x9F
#define SPI_CMDCODE_FREAD 0x0B
#define SPI_CMDCODE_WRITE 0x02
#define SPI_CMDCODE_IRR 0x81
#define SPI_CMDCODE_IRW 0x82
#define SPI_CMDCODE_READ_SINGLE 0x83

/* SPI slave internal registers */
#define SPI_SLAVE_DEVICE_ID 0x04
#define SPI_SLAVE_RDSR 0x05
#define SPI_SLAVE_STATUS 0x08
#define SPI_SLAVE_CONFIG 0x0C
#define SPI_SLAVE_TRNS_LEN 0x50

/* Command lengths */
#define IRW_CMD_LENGTH 6
#define IRR_CMD_LENGTH 8

/* QCC730 slave device ID */
#define QCSPI_ID1 0x7F
#define QCSPI_ID2 0x90

/* Configuration */
#define QCSPI_MAX_INIT_TRY_TIMES 50
#define QCSPI_READ_DUMMY_BYTES 10
#define QCSPI_MAX_TRANSFER_SIZE 1500

/* Helper macros */
#define QCSPI_CONFIG_HOST_IRQ_INT0_EN(s) ((s) << 5)
#define BIT_CHECK(var, pos) (((var) & (1 << (pos))) != 0)

/* Status register bits */
#define QCSPI_STATUS_TXUERR_BIT 28
#define QCSPI_STATUS_BUSY_BIT 24

#endif /* QCSPI_PROTOCOL_H_ */
