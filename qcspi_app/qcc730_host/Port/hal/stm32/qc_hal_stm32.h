/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qc_port.h"

#if defined(QC_PLATFORM_STM32)

#if defined(QC_OS_ZEPHYR)

#elif defined(QC_OS_FREERTOS)
#include "FreeRTOS.h"
#include "stm32u5xx_hal.h"
#include "main.h"
#include <stdio.h>

/*Platform SPI handle Structure definition*/
typedef SPI_HandleTypeDef QC_HAL_SPI_HandleTypeDef;

/*Platform UART Handle Type*/
typedef UART_HandleTypeDef QC_HAL_UART_HandleTypeDef;
#define UART_DEVICE huart1

/*Platform LED GPIO Control*/
#define QC_HAL_LED_GREEN_GPIO_Port LED_GREEN_GPIO_Port
#define QC_HAL_LED_BLUE_GPIO_Port LED_BLUE_GPIO_Port
#define QC_HAL_LED_RED_GPIO_Port LED_RED_GPIO_Port

#define QC_HAL_LED_GREEN_Pin LED_GREEN_Pin
#define QC_HAL_LED_BLUE_Pin LED_BLUE_Pin
#define QC_HAL_LED_RED_Pin LED_RED_Pin

/*Platform GPIO PIN*/
#define QC_HAL_GPIO_PIN_SET GPIO_PIN_SET
#define QC_HAL_GPIO_PIN_RESET GPIO_PIN_RESET

#define QC_CHIP_ON_Port GPIOC
#define QC_CHIO_ON_Pin GPIO_PIN_7

/*Platform minimum delay (in milliseconds)*/
#define qc_hal_delay(x) HAL_Delay(x)

/*Platform toggle the specified GPIO pin.*/
#define qc_hal_gpio_toggle(GPIOx, GPIO_Pin) HAL_GPIO_TogglePin(GPIOx, GPIO_Pin)

/*Platform increment a global variable "uwTick"*/
#define qc_hal_inc_tick() HAL_IncTick()

/*Platform set or clear the selected data port bit*/
#define qc_hal_gpio_write(GPIOx, GPIO_Pin, PinState) HAL_GPIO_WritePin(GPIOx, GPIO_Pin, PinState)

/*Platform send an amount of data in blocking mode*/
#define QC_HAL_UART_Transmit(huart, pData, Size, Timeout) HAL_UART_Transmit(huart, pData, Size, Timeout)

/*Platform Receive an amount of data in interrupt mode*/
#define QC_HAL_UART_Receive_IT(huart, pData, Size) HAL_UART_Receive_IT(huart, pData, Size)

extern QC_HAL_SPI_HandleTypeDef hspi1;
extern QC_HAL_UART_HandleTypeDef huart1;

#endif /* QC_OS_FREERTOS */

#endif /* QC_PLATFORM_STM32 */
