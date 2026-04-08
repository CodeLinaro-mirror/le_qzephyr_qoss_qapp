/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef INC_SHELL_H_
#define INC_SHELL_H_
/* Includes ------------------------------------------------------------------*/
#include "qc_port.h"
#include "qc_hal_stm32.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

extern char c;
extern qc_osal_queue_t qShell;

uint8_t uart_write(char *s, uint16_t size);
uint8_t cmd_shell_init(QC_HAL_UART_HandleTypeDef *huart);
uint8_t cmd_shell_add(char *cmd, int (*pfunc)(int argc, char **argv), char *description);
uint8_t cmd_shell_char_received();
uint8_t cmd_shell_exec(char *cmd);
uint8_t cmd_shell_find_command_index(int *index, char *header, int len);
uint8_t cmd_shell_exec_function_by_index(int argc, char *argv, int index);

#endif /* INC_SHELL_H_ */
