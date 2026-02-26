/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#ifndef __QC_PLAT_AL_H__
#define __QC_PLAT_AL_H__

#include "qc_port_config.h"

#ifdef QC_PLATFORM_STM32
#include "main.h"
#include "stm32u5xx_hal.h"

/*Platform HAL Status structures definition*/
typedef enum
{
  QC_HAL_OK       = 0x00,
  QC_HAL_ERROR    = 0x01,
  QC_HAL_BUSY     = 0x02,
  QC_HAL_TIMEOUT  = 0x03
} QC_HAL_StatusTypeDef;

/*Platform Error handling*/
typedef enum plat_ret_s {
	QC_PLAT_OK                = 0,
	QC_PLAT_FAIL		      = 1,
	QC_PLAT_ERROR_RDSR        = 2,
	QC_PLAT_ERROR_NULLPTR     = 3,
	QC_PLAT_FAIL_NO_MEMORY    = 4,
	QC_PLAT_FAIL_NOT_FOUND    = 5,
	QC_PLAT_FAIL_NOT_FINISHED = 6,
	QC_PLAT_FAIL_ALIGNMENT    = 7,
}PLAT_RET;

/*Platform SPI handle Structure definition*/
typedef SPI_HandleTypeDef QC_HAL_SPI_HandleTypeDef;

/*Platform UART Handle Type*/
typedef UART_HandleTypeDef QC_HAL_UART_HandleTypeDef;
#define UART_DEVICE huart1

/*Platform LED GPIO Control*/
#define QC_HAL_LED_GREEN_GPIO_Port LED_GREEN_GPIO_Port
#define QC_HAL_LED_BLUE_GPIO_Port  LED_BLUE_GPIO_Port
#define QC_HAL_LED_RED_GPIO_Port   LED_RED_GPIO_Port

#define QC_HAL_LED_GREEN_Pin       LED_GREEN_Pin
#define QC_HAL_LED_BLUE_Pin        LED_BLUE_Pin
#define QC_HAL_LED_RED_Pin         LED_RED_Pin

/*Platform GPIO PIN*/
#define QC_HAL_GPIO_PIN_SET        GPIO_PIN_SET
#define QC_HAL_GPIO_PIN_RESET      GPIO_PIN_RESET

#define QC_CHIP_ON_Port             GPIOC
#define QC_CHIO_ON_Pin              GPIO_PIN_7


/*Platform minimum delay (in milliseconds)*/
#define qc_hal_delay(x)	\
	    HAL_Delay(x)

/*Platform toggle the specified GPIO pin.*/
#define qc_hal_gpio_toggle(GPIOx, GPIO_Pin) \
        HAL_GPIO_TogglePin(GPIOx, GPIO_Pin)

/*Platform increment a global variable "uwTick"*/
#define qc_hal_inc_tick() \
  	    HAL_IncTick()

/*Platform set or clear the selected data port bit*/                 	
#define qc_hal_gpio_write(GPIOx, GPIO_Pin, PinState) \
	    HAL_GPIO_WritePin(GPIOx, GPIO_Pin, PinState)

/*Platform send an amount of data in blocking mode*/
#define QC_HAL_UART_Transmit(huart, pData, Size, Timeout) \
	    HAL_UART_Transmit(huart, pData, Size, Timeout)

/*Platform Receive an amount of data in interrupt mode*/
#define QC_HAL_UART_Receive_IT(huart, pData, Size) \
        HAL_UART_Receive_IT(huart, pData, Size)

extern QC_HAL_SPI_HandleTypeDef hspi1;
extern QC_HAL_UART_HandleTypeDef huart1;


/**
  * @brief  API to be used to Transmit and Receive an amount of data in blocking mode.
  * @param[in]  hspi   : pointer to a QC_HAL_SPI_HandleTypeDef structure that contains
  *                  the configuration information for SPI module.
  * @param[in]  pTxData: pointer to transmission data buffer
  * @param[in]  pRxData: pointer to reception data buffer
  * @param[in]  Size   : amount of data to be sent and received
  * QC_HAL_OK -- On success.\n
  * Error code -- On failure.
  */

QC_HAL_StatusTypeDef qcspi_transfer(QC_HAL_SPI_HandleTypeDef *hspi, const uint8_t *pTxData, uint8_t *pRxData,
                                          uint16_t Size);
#endif
#endif /*__QC_PLAT_AL_H__*/
