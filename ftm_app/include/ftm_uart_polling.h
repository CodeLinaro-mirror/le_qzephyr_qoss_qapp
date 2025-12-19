/**
 * @file ftm_uart.h
 * @brief UART Driver Wrapper for FTM Application
 * 
 * This file provides a polling-based UART interface for the FTM application.
 */

#ifndef FTM_UART_H
#define FTM_UART_H

#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void ftm_task_init(void);
void ftm_uart_init(void);
void uart_polling_receive(void);

#ifdef __cplusplus
}
#endif

#endif /* FTM_UART_H */

