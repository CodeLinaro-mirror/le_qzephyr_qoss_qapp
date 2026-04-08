/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "../../Port/qc_port.h"

#if defined(QC_OS_FREERTOS)
#ifdef SHELL_FEATURE
#include "shell.h"
#endif
#include "../../Port/osal/qc_osal.h"
#include "../../Service/qcspi/qcspi_adapter.h"

#ifdef CONFIG_RING_SERVICE
#include "../../Service/ring/ring_service.h"
#endif

#ifdef QCC730_ATCMD_ENABLE
#include "qcc730_atcmd_demo.h"
#endif
#ifdef QCC730_SPI_ENABLE
#include "qcc730_spi_demo.h"
#endif

int qcc730_ring_reset()
{
    int ret = 0;
    /* Deinitialize ring service and adapter */
    ring_service_deinit();

    /* Initialize QCSPI adapter and ring service */
    ret = init_qring();

    return ret;
}
void qcc730_reset()
{
#define DELAY_TIMING 300
    int ret = -1;

    HAL_NVIC_DisableIRQ(EXTI12_IRQn);

    while (ret < 0) {
        /* host toggle */
        qc_hal_gpio_write(QC_CHIP_ON_Port, QC_CHIO_ON_Pin, QC_HAL_GPIO_PIN_RESET); // chip_on test
        qc_hal_delay(DELAY_TIMING);
        qc_hal_gpio_write(QC_CHIP_ON_Port, QC_CHIO_ON_Pin, QC_HAL_GPIO_PIN_SET); // chip_on test
        qc_hal_delay(DELAY_TIMING);
        qc_hal_delay(1000);

        ret = qcc730_ring_reset();
    }

    HAL_NVIC_EnableIRQ(EXTI12_IRQn);
}

#ifdef SHELL_FEATURE
void vTaskShell(void *p)
{

    cmd_shell_init(&(UART_DEVICE));
    qcc730_reset();

#if defined(QCC730_SPI_ENABLE)
    spi_demo_init();
    spi_demo_register_commands();
#elif defined(QCC730_ATCMD_ENABLE)
    atcmd_demo_init();
    atcmd_demo_register_commands();
#else
#warning "Neither QCC730_SPI_ENABLE nor QCC730_ATCMD_ENABLE is defined."
#endif

    while (1) {
        cmd_shell_char_received();
    }
}

void task_init_all(void)
{
    int ret;
    qc_osal_thread_t xHandle = NULL;
    struct qc_osal_thread_config config = {
        .name = "Shell", .stack_size = 2048, .priority = 5, .entry = vTaskShell, .arg = NULL};

    ret = qc_osal_thread_create(&xHandle, &config);
    if (ret != 0) {
        printf("Task Shell creation error: %d\r\n", ret);
    }
}
#endif
#endif
