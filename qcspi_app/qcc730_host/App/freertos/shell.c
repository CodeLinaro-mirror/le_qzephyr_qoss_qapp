/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
#include "shell.h"
#include "qc_osal.h"
#include "qc_platform.h"

typedef struct {
    char *cmd;
    int (*func)(int argc, char **argv);
    char *description;
} shell_func_t;

#define SHELL_FUNC_LIST_MAX_SIZE 20
#define SHELL_CMD_MAX_SIZE 32
#define SHELL_ARGC_MAX 8
#define SHELL_BUFFER_SIZE 1500

UART_HandleTypeDef *shell_huart = NULL;

char starting[] = "\r\n\r\n==== command Shell ====\r\n";
char prompt[] = "STM32 >> ";

char c = 0;       // character received
uint32_t pos = 0; // current buffer index
static char buf[SHELL_BUFFER_SIZE];
static char backspace[] = "\b \b";
uint8_t it_uart_rx_ready = 0;

static int shell_func_list_size = 0;
static shell_func_t shell_func_list[SHELL_FUNC_LIST_MAX_SIZE];

qc_osal_queue_t qShell;

uint8_t uart_write(char *s, uint16_t size)
{
    if (0 != QC_HAL_UART_Transmit(shell_huart, (uint8_t *)s, size, 0xFFFF)) {
        return 1;
    }
    return 0;
}

uint8_t cmd_sh_help(int argc, char **argv)
{
    int i;
    for (i = 0; i < shell_func_list_size; i++) {
        printf("%s : %s\r\n", shell_func_list[i].cmd, shell_func_list[i].description);
    }
    return 0;
}

uint8_t cmd_sh_example(int argc, char **argv)
{
    printf("argc = %d\r\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("arg %d = %s\r\n", i, argv[i]);
    }
    return 0;
}

uint8_t cmd_shell_init(UART_HandleTypeDef *huart)
{
    shell_huart = huart;

    uart_write(starting, sizeof(starting));
    uart_write(prompt, sizeof(prompt));
#ifndef QCC730_ATCMD_ENABLE
    cmd_shell_add("help", (void *)cmd_sh_help, "show help");
#endif
    //	cmd_shell_add("Eg:", (void *)cmd_sh_example, "Example");

    qc_osal_queue_init(&qShell, 10, sizeof(char));

    if (QC_PLATFORM_EOK != QC_HAL_UART_Receive_IT(shell_huart, (uint8_t *)&c, 1)) {
        return 1;
    }

    return 0;
}

uint8_t cmd_shell_add(char *cmd, int (*pfunc)(int argc, char **argv), char *description)
{
    if (shell_func_list_size < SHELL_FUNC_LIST_MAX_SIZE) {
        shell_func_list[shell_func_list_size].cmd = cmd;
        shell_func_list[shell_func_list_size].func = pfunc;
        shell_func_list[shell_func_list_size].description = description;
        shell_func_list_size++;
        return 0;
    }

    return -1;
}

uint8_t cmd_shell_char_received()
{
    qc_osal_queue_recv(qShell, &c, QC_OSAL_TIMEOUT_FOREVER);

    switch (c) {
    case '\r':
        // ENTER key pressed
        printf("\r\n");
#ifdef QCC730_ATCMD_ENABLE
        if (strncmp(buf, "help", 4) == 0) {
            cmd_shell_exec(buf);
        } else if (strncmp(buf, "parser", 6) == 0) {
            cmd_shell_exec(buf);
        } else if (is_dfu_cmd(buf) == 0) {
            int dfu_status = -1;
            dfu_status = dfu_hook(buf);
        } else if (strncmp(buf, "qatperf", 7) == 0) {
            cmd_shell_exec(buf);
        } else if (strncmp(buf, "tx", 2) == 0) {
            cmd_shell_exec(buf);
        } else if (strncmp(buf, "rx_mqtt", 2) == 0) {
            cmd_shell_exec(buf);
        } else if (buf[0] != '\r') {
            buf[pos++] = '\r';
            extern void qcc730_atcmd_send_handler(uint8_t * cmd, uint32_t len);
            qcc730_atcmd_send_handler(buf, pos);
        }
#else
        cmd_shell_exec(buf);
#endif
        buf[pos++] = 0;
        pos = 0;
        memset(buf, 0, SHELL_BUFFER_SIZE);
        uart_write(prompt, sizeof(prompt));
        break;

    case '\b':
        // DELETE key pressed
        if (pos > 0) {
            pos--;
            uart_write(backspace, 3);
        }
        break;

    default:
        if (pos < SHELL_BUFFER_SIZE) {
            uart_write(&c, 1);
            buf[pos++] = c;
        }
    }

    return 0;
}
uint8_t cmd_shell_find_command_index(int *index, char *header, int len)
{
    int i = 0;

    for (i = 0; i < shell_func_list_size; i++) {
        if (!strncmp(shell_func_list[i].cmd, header, len)) {
            *index = i;
            return 0;
        }
    }
    return -1;
}
uint8_t cmd_shell_exec_function_by_index(int argc, char *argv, int index)
{
    return shell_func_list[index].func(argc, argv);
}

// uint8_t cmd_shell_exec(char * cmd) {
//	int argc;
//	char * argv[SHELL_ARGC_MAX];
//	char *p;
//
//	// Separating header and arguments
//	char header[SHELL_CMD_MAX_SIZE] = "";
//	int h = 0;
//
//	while(cmd[h] != ' ' && h < SHELL_CMD_MAX_SIZE){
//		header[h] = cmd[h];
//		h++;
//	}
//	header[h] = '\0';
//
//	// Searching for the command and parameters
//	for(int i = 0 ; i < shell_func_list_size ; i++) {
//		if (!strcmp(shell_func_list[i].cmd, header)) {
//			argc = 1;
//			argv[0] = cmd;
//
//			for(p = cmd ; *p != '\0' && argc < SHELL_ARGC_MAX ; p++){
//				if(*p == ' ') {
//					*p = '\0';
//					argv[argc++] = p+1;
//				}
//			}
//
//			return shell_func_list[i].func(argc, argv);
//		}
//	}
//	printf("%s: command not found\r\n", cmd);
//	return -1;
//}
uint8_t cmd_shell_exec(char *cmd)
{
    int argc;
    char *argv[SHELL_ARGC_MAX];
    char *p;

    // Check if command is empty or only whitespace
    if (cmd == NULL || cmd[0] == '\0' || cmd[0] == '\r' || cmd[0] == '\n') {
        return 0;
    }

    // Skip leading whitespace
    while (*cmd == ' ' || *cmd == '\t') {
        cmd++;
    }

    // Check again after skipping whitespace
    if (cmd[0] == '\0' || cmd[0] == '\r' || cmd[0] == '\n') {
        return 0;
    }

    // Separating header and arguments
    char header[SHELL_CMD_MAX_SIZE] = "";
    int h = 0;
    while (cmd[h] != ' ' && h < SHELL_CMD_MAX_SIZE) {
        header[h] = cmd[h];
        h++;
    }
    header[h] = '\0';

    // Check if header is empty
    if (header[0] == '\0') {
        return 0;
    }

    // Searching for the command and parameters
    for (int i = 0; i < shell_func_list_size; i++) {
        if (!strcmp(shell_func_list[i].cmd, header)) {
            argc = 1;
            argv[0] = cmd;
            for (p = cmd; *p != '\0' && argc < SHELL_ARGC_MAX; p++) {
                if (*p == ' ') {
                    *p = '\0';
                    argv[argc++] = p + 1;
                }
            }
            return shell_func_list[i].func(argc, argv);
        }
    }

    printf("%s: command not found\r\n", cmd);
    return -1;
}
