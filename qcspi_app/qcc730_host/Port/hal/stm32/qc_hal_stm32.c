/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "qc_port.h"

#if defined(QC_PLATFORM_STM32)

#if defined(QC_OS_ZEPHYR)
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>
#include <errno.h>

LOG_MODULE_REGISTER(qc_hal_stm32, LOG_LEVEL_INF);

/* ============================================================================
 * STM32 HAL Context Structure
 * ============================================================================ */

struct qc_hal_stm32_ctx {
    const struct device *spi_dev;  /* SPI device */
    const struct device *gpio_dev; /* GPIO device (GPIOA) */
};

/* ============================================================================
 * GPIO Handle Structure
 * ============================================================================ */

struct qc_hal_stm32_gpio {
    struct gpio_dt_spec gpio_spec;
    bool active_low;
};

/* ============================================================================
 * SPI Handle Structure
 * ============================================================================ */

struct qc_hal_stm32_spi {
    const struct device *spi_dev;
    struct spi_config spi_cfg;
};

/* ============================================================================
 * Error Code Conversion
 * ============================================================================ */

static inline int zephyr_errno_to_hal(int zephyr_errno)
{
    if (zephyr_errno == 0) {
        return 0;
    }

    switch (zephyr_errno) {
    case EINVAL:
        return -QC_HAL_EINVAL;
    case ENOMEM:
        return -QC_HAL_ENOMEM;
    case ENODEV:
        return -QC_HAL_ENODEV;
    case EIO:
        return -QC_HAL_EIO;
    case EBUSY:
        return -QC_HAL_EBUSY;
    case ETIMEDOUT:
        return -QC_HAL_ETIMEDOUT;
    default:
        return -QC_HAL_EIO;
    }
}

/* ============================================================================
 * HAL Context Operations
 * ============================================================================ */

int qc_hal_init(qc_hal_ctx_t *ctx)
{
    struct qc_hal_stm32_ctx *stm32_ctx;

    if (!ctx) {
        return -QC_HAL_EINVAL;
    }

    /* Allocate context using OSAL */
    stm32_ctx = qc_osal_malloc(sizeof(struct qc_hal_stm32_ctx));
    if (!stm32_ctx) {
        QC_OSAL_LOG_ERR("Failed to allocate HAL context");
        return -QC_HAL_ENOMEM;
    }

    /* Get SPI device from device tree */
    stm32_ctx->spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi1));
    if (!device_is_ready(stm32_ctx->spi_dev)) {
        QC_OSAL_LOG_ERR("SPI device not ready");
        qc_osal_free(stm32_ctx);
        return -QC_HAL_ENODEV;
    }

    /* Get GPIO device (GPIOA) from device tree */
    stm32_ctx->gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpioa));
    if (!device_is_ready(stm32_ctx->gpio_dev)) {
        QC_OSAL_LOG_ERR("GPIO device not ready");
        qc_osal_free(stm32_ctx);
        return -QC_HAL_ENODEV;
    }

    QC_OSAL_LOG_INF("STM32 HAL initialized");

    *ctx = (qc_hal_ctx_t)stm32_ctx;
    return 0;
}

int qc_hal_deinit(qc_hal_ctx_t ctx)
{
    struct qc_hal_stm32_ctx *stm32_ctx = (struct qc_hal_stm32_ctx *)ctx;

    if (!stm32_ctx) {
        return -QC_HAL_EINVAL;
    }

    qc_osal_free(stm32_ctx);
    QC_OSAL_LOG_INF("STM32 HAL deinitialized");

    return 0;
}

/* ============================================================================
 * GPIO Operations
 * ============================================================================ */

int qc_hal_gpio_init(qc_hal_ctx_t ctx, qc_hal_gpio_t *gpio, const struct qc_hal_gpio_config *config)
{
    struct qc_hal_stm32_ctx *stm32_ctx = (struct qc_hal_stm32_ctx *)ctx;
    struct qc_hal_stm32_gpio *stm32_gpio;
    gpio_flags_t flags;
    int ret;

    if (!stm32_ctx || !gpio || !config) {
        return -QC_HAL_EINVAL;
    }

    /* Allocate GPIO handle using OSAL */
    stm32_gpio = qc_osal_malloc(sizeof(struct qc_hal_stm32_gpio));
    if (!stm32_gpio) {
        return -QC_HAL_ENOMEM;
    }

    /* Setup GPIO spec */
    stm32_gpio->gpio_spec.port = stm32_ctx->gpio_dev;
    stm32_gpio->gpio_spec.pin = config->pin;
    stm32_gpio->gpio_spec.dt_flags = config->active_low ? GPIO_ACTIVE_LOW : GPIO_ACTIVE_HIGH;
    stm32_gpio->active_low = config->active_low;

    /* Configure GPIO direction and initial value */
    if (config->output) {
        if (config->initial_value) {
            flags = GPIO_OUTPUT_ACTIVE;
        } else {
            flags = GPIO_OUTPUT_INACTIVE;
        }
    } else {
        flags = GPIO_INPUT;
    }

    ret = gpio_pin_configure_dt(&stm32_gpio->gpio_spec, flags);
    if (ret < 0) {
        QC_OSAL_LOG_ERR("Failed to configure GPIO pin %d: %d", config->pin, ret);
        qc_osal_free(stm32_gpio);
        return zephyr_errno_to_hal(-ret);
    }

    *gpio = (qc_hal_gpio_t)stm32_gpio;
    return 0;
}

int qc_hal_gpio_set(qc_hal_gpio_t gpio, bool value)
{
    struct qc_hal_stm32_gpio *stm32_gpio = (struct qc_hal_stm32_gpio *)gpio;
    int ret;

    if (!stm32_gpio) {
        return -QC_HAL_EINVAL;
    }

    /* Set GPIO value (active/inactive) */
    ret = gpio_pin_set_dt(&stm32_gpio->gpio_spec, value ? 1 : 0);
    if (ret < 0) {
        return zephyr_errno_to_hal(-ret);
    }

    return 0;
}

int qc_hal_gpio_get(qc_hal_gpio_t gpio, bool *value)
{
    struct qc_hal_stm32_gpio *stm32_gpio = (struct qc_hal_stm32_gpio *)gpio;
    int ret;

    if (!stm32_gpio || !value) {
        return -QC_HAL_EINVAL;
    }

    ret = gpio_pin_get_dt(&stm32_gpio->gpio_spec);
    if (ret < 0) {
        return zephyr_errno_to_hal(-ret);
    }

    *value = (ret != 0);
    return 0;
}

int qc_hal_gpio_deinit(qc_hal_gpio_t gpio)
{
    struct qc_hal_stm32_gpio *stm32_gpio = (struct qc_hal_stm32_gpio *)gpio;

    if (!stm32_gpio) {
        return -QC_HAL_EINVAL;
    }

    qc_osal_free(stm32_gpio);
    return 0;
}

/* ============================================================================
 * SPI Operations
 * ============================================================================ */

int qc_hal_spi_init(qc_hal_ctx_t ctx, qc_hal_spi_t *spi, const struct qc_hal_spi_config *config)
{
    struct qc_hal_stm32_ctx *stm32_ctx = (struct qc_hal_stm32_ctx *)ctx;
    struct qc_hal_stm32_spi *stm32_spi;

    if (!stm32_ctx || !spi || !config) {
        return -QC_HAL_EINVAL;
    }

    /* Allocate SPI handle using OSAL */
    stm32_spi = qc_osal_malloc(sizeof(struct qc_hal_stm32_spi));
    if (!stm32_spi) {
        return -QC_HAL_ENOMEM;
    }

    stm32_spi->spi_dev = stm32_ctx->spi_dev;

    /* Configure SPI parameters */
    stm32_spi->spi_cfg.frequency = config->frequency;
    stm32_spi->spi_cfg.operation = SPI_WORD_SET(config->bits_per_word) | SPI_TRANSFER_MSB;

    /* Add mode configuration */
    switch (config->mode) {
    case QC_HAL_SPI_MODE_0:
        /* CPOL=0, CPHA=0 - default, no additional flags */
        break;
    case QC_HAL_SPI_MODE_1:
        stm32_spi->spi_cfg.operation |= SPI_MODE_CPHA;
        break;
    case QC_HAL_SPI_MODE_2:
        stm32_spi->spi_cfg.operation |= SPI_MODE_CPOL;
        break;
    case QC_HAL_SPI_MODE_3:
        stm32_spi->spi_cfg.operation |= SPI_MODE_CPOL | SPI_MODE_CPHA;
        break;
    default:
        qc_osal_free(stm32_spi);
        return -QC_HAL_EINVAL;
    }

    stm32_spi->spi_cfg.slave = 0;
    /* Manual CS control - set cs.gpio.port to NULL */
    stm32_spi->spi_cfg.cs.gpio.port = NULL;
    stm32_spi->spi_cfg.cs.delay = 0;

    *spi = (qc_hal_spi_t)stm32_spi;
    return 0;
}

int qc_hal_spi_transfer(qc_hal_spi_t spi, const uint8_t *tx_buf, uint8_t *rx_buf, size_t len)
{
    struct qc_hal_stm32_spi *stm32_spi = (struct qc_hal_stm32_spi *)spi;
    struct spi_buf tx_spi_buf;
    struct spi_buf rx_spi_buf;
    struct spi_buf_set tx_buf_set;
    struct spi_buf_set rx_buf_set;
    int ret;

    if (!stm32_spi || !tx_buf || !rx_buf || len == 0) {
        return -QC_HAL_EINVAL;
    }

    /* Prepare SPI buffer sets */
    tx_spi_buf.buf = (void *)tx_buf;
    tx_spi_buf.len = len;
    rx_spi_buf.buf = rx_buf;
    rx_spi_buf.len = len;
    tx_buf_set.buffers = &tx_spi_buf;
    tx_buf_set.count = 1;
    rx_buf_set.buffers = &rx_spi_buf;
    rx_buf_set.count = 1;

    /* Perform SPI transceive */
    ret = spi_transceive(stm32_spi->spi_dev, &stm32_spi->spi_cfg, &tx_buf_set, &rx_buf_set);
    if (ret < 0) {
        QC_OSAL_LOG_ERR("SPI transfer failed: %d", ret);
        return zephyr_errno_to_hal(-ret);
    }

    return 0;
}

int qc_hal_spi_deinit(qc_hal_spi_t spi)
{
    struct qc_hal_stm32_spi *stm32_spi = (struct qc_hal_stm32_spi *)spi;

    if (!stm32_spi) {
        return -QC_HAL_EINVAL;
    }

    qc_osal_free(stm32_spi);
    return 0;
}

#elif defined(QC_OS_FREERTOS)
#include "FreeRTOS.h"
#include "stm32u5xx_hal.h"
#include <stdio.h>

/* External SPI handle from CubeMX generated code */
extern SPI_HandleTypeDef hspi1;

/* External UART handle from CubeMX generated code */
extern UART_HandleTypeDef huart1;

/* GPIO Port A base address */
#define GPIOA_BASE_ADDR GPIOA

/* ============================================================================
 * STM32 HAL Context Structure (FreeRTOS)
 * ============================================================================ */

struct qc_hal_stm32_ctx {
    SPI_HandleTypeDef *spi_handle; /* SPI handle from CubeMX */
    GPIO_TypeDef *gpio_port;       /* GPIO port (GPIOA) */
};

/* ============================================================================
 * GPIO Handle Structure (FreeRTOS)
 * ============================================================================ */

struct qc_hal_stm32_gpio {
    GPIO_TypeDef *port;
    uint16_t pin;
    bool active_low;
};

/* ============================================================================
 * SPI Handle Structure (FreeRTOS)
 * ============================================================================ */

struct qc_hal_stm32_spi {
    SPI_HandleTypeDef *spi_handle;
};

/* ============================================================================
 * Error Code Conversion
 * ============================================================================ */

static inline int hal_status_to_hal_errno(HAL_StatusTypeDef status)
{
    switch (status) {
    case HAL_OK:
        return 0;
    case HAL_ERROR:
        return -QC_HAL_EIO;
    case HAL_BUSY:
        return -QC_HAL_EBUSY;
    case HAL_TIMEOUT:
        return -QC_HAL_ETIMEDOUT;
    default:
        return -QC_HAL_EIO;
    }
}

/* ============================================================================
 * HAL Context Operations
 * ============================================================================ */

int qc_hal_init(qc_hal_ctx_t *ctx)
{
    struct qc_hal_stm32_ctx *stm32_ctx;

    if (!ctx) {
        return -QC_HAL_EINVAL;
    }

    /* Allocate context using FreeRTOS */
    stm32_ctx = pvPortMalloc(sizeof(struct qc_hal_stm32_ctx));
    if (!stm32_ctx) {
        printf("[HAL] Failed to allocate HAL context\r\n");
        return -QC_HAL_ENOMEM;
    }

    /* Get SPI handle from CubeMX (must be initialized before calling this) */
    stm32_ctx->spi_handle = &hspi1;
    if (stm32_ctx->spi_handle->Instance == NULL) {
        printf("[HAL] SPI not initialized by CubeMX\r\n");
        vPortFree(stm32_ctx);
        return -QC_HAL_ENODEV;
    }

    /* Get GPIO port */
    stm32_ctx->gpio_port = GPIOA_BASE_ADDR;

    printf("[HAL] STM32 HAL initialized (FreeRTOS)\r\n");

    *ctx = (qc_hal_ctx_t)stm32_ctx;
    return 0;
}

int qc_hal_deinit(qc_hal_ctx_t ctx)
{
    struct qc_hal_stm32_ctx *stm32_ctx = (struct qc_hal_stm32_ctx *)ctx;

    if (!stm32_ctx) {
        return -QC_HAL_EINVAL;
    }

    vPortFree(stm32_ctx);
    printf("[HAL] STM32 HAL deinitialized (FreeRTOS)\r\n");

    return 0;
}

/* ============================================================================
 * GPIO Operations
 * ============================================================================ */

int qc_hal_gpio_init(qc_hal_ctx_t ctx, qc_hal_gpio_t *gpio, const struct qc_hal_gpio_config *config)
{
    struct qc_hal_stm32_ctx *stm32_ctx = (struct qc_hal_stm32_ctx *)ctx;
    struct qc_hal_stm32_gpio *stm32_gpio;
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    if (!stm32_ctx || !gpio || !config) {
        return -QC_HAL_EINVAL;
    }

    /* Allocate GPIO handle using FreeRTOS */
    stm32_gpio = pvPortMalloc(sizeof(struct qc_hal_stm32_gpio));
    if (!stm32_gpio) {
        return -QC_HAL_ENOMEM;
    }

    stm32_gpio->port = stm32_ctx->gpio_port;
    stm32_gpio->pin = (1 << config->pin); /* Convert pin number to GPIO_PIN_x */
    stm32_gpio->active_low = config->active_low;

    /* Configure GPIO */
    GPIO_InitStruct.Pin = stm32_gpio->pin;
    GPIO_InitStruct.Mode = config->output ? GPIO_MODE_OUTPUT_PP : GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(stm32_gpio->port, &GPIO_InitStruct);

    /* Set initial value for output pins */
    if (config->output) {
        GPIO_PinState initial_state;
        if (config->active_low) {
            initial_state = config->initial_value ? GPIO_PIN_RESET : GPIO_PIN_SET;
        } else {
            initial_state = config->initial_value ? GPIO_PIN_SET : GPIO_PIN_RESET;
        }
        HAL_GPIO_WritePin(stm32_gpio->port, stm32_gpio->pin, initial_state);
    }

    *gpio = (qc_hal_gpio_t)stm32_gpio;
    return 0;
}

int qc_hal_gpio_set(qc_hal_gpio_t gpio, bool value)
{
    struct qc_hal_stm32_gpio *stm32_gpio = (struct qc_hal_stm32_gpio *)gpio;
    GPIO_PinState pin_state;

    if (!stm32_gpio) {
        return -QC_HAL_EINVAL;
    }

    /* Handle active-low logic */
    if (stm32_gpio->active_low) {
        pin_state = value ? GPIO_PIN_RESET : GPIO_PIN_SET;
    } else {
        pin_state = value ? GPIO_PIN_SET : GPIO_PIN_RESET;
    }

    HAL_GPIO_WritePin(stm32_gpio->port, stm32_gpio->pin, pin_state);
    return 0;
}

int qc_hal_gpio_get(qc_hal_gpio_t gpio, bool *value)
{
    struct qc_hal_stm32_gpio *stm32_gpio = (struct qc_hal_stm32_gpio *)gpio;
    GPIO_PinState pin_state;

    if (!stm32_gpio || !value) {
        return -QC_HAL_EINVAL;
    }

    pin_state = HAL_GPIO_ReadPin(stm32_gpio->port, stm32_gpio->pin);

    /* Handle active-low logic */
    if (stm32_gpio->active_low) {
        *value = (pin_state == GPIO_PIN_RESET);
    } else {
        *value = (pin_state == GPIO_PIN_SET);
    }

    return 0;
}

int qc_hal_gpio_deinit(qc_hal_gpio_t gpio)
{
    struct qc_hal_stm32_gpio *stm32_gpio = (struct qc_hal_stm32_gpio *)gpio;

    if (!stm32_gpio) {
        return -QC_HAL_EINVAL;
    }

    HAL_GPIO_DeInit(stm32_gpio->port, stm32_gpio->pin);
    vPortFree(stm32_gpio);
    return 0;
}

/* ============================================================================
 * SPI Operations
 * ============================================================================ */

int qc_hal_spi_init(qc_hal_ctx_t ctx, qc_hal_spi_t *spi, const struct qc_hal_spi_config *config)
{
    struct qc_hal_stm32_ctx *stm32_ctx = (struct qc_hal_stm32_ctx *)ctx;
    struct qc_hal_stm32_spi *stm32_spi;

    if (!stm32_ctx || !spi || !config) {
        return -QC_HAL_EINVAL;
    }

    /* Allocate SPI handle using FreeRTOS */
    stm32_spi = pvPortMalloc(sizeof(struct qc_hal_stm32_spi));
    if (!stm32_spi) {
        return -QC_HAL_ENOMEM;
    }

    stm32_spi->spi_handle = stm32_ctx->spi_handle;

    /* Note: SPI configuration (frequency, mode, etc.) should be done in CubeMX
     * This function just stores the handle for later use
     * If you need to reconfigure SPI at runtime, you can add that logic here
     */

    *spi = (qc_hal_spi_t)stm32_spi;
    return 0;
}

int qc_hal_spi_transfer(qc_hal_spi_t spi, const uint8_t *tx_buf, uint8_t *rx_buf, size_t len)
{
    struct qc_hal_stm32_spi *stm32_spi = (struct qc_hal_stm32_spi *)spi;
    HAL_StatusTypeDef status = HAL_OK;
    int ret = 0;

    /* Parameter validation */
    if (!stm32_spi || !tx_buf || !rx_buf || len == 0) {
        return -QC_HAL_EINVAL;
    }

    /* Choose transfer method based on data length */
    if (len < 16) {
        /* Short data: use polling mode for efficiency */
        status = HAL_SPI_TransmitReceive(stm32_spi->spi_handle, (uint8_t *)tx_buf, rx_buf, len, HAL_MAX_DELAY);
    } else {
        /* Long data: use DMA mode to avoid CPU blocking */
        status = HAL_SPI_TransmitReceive_DMA(stm32_spi->spi_handle, (uint8_t *)tx_buf, rx_buf, len);

        if (status == HAL_OK) {
            /* Wait for DMA transfer completion with timeout */
            uint32_t timeout = HAL_GetTick() + 1000; /* 1 second timeout */
            while (HAL_SPI_GetState(stm32_spi->spi_handle) != HAL_SPI_STATE_READY) {
                if (HAL_GetTick() > timeout) {
                    status = HAL_TIMEOUT;
                    break;
                }
            }
        }
    }

    /* Convert HAL status to error code */
    if (status != HAL_OK) {
        printf("[HAL] SPI transfer failed: %d\r\n", status);
        ret = hal_status_to_hal_errno(status);
    }

    return ret;
}

int qc_hal_spi_deinit(qc_hal_spi_t spi)
{
    struct qc_hal_stm32_spi *stm32_spi = (struct qc_hal_stm32_spi *)spi;

    if (!stm32_spi) {
        return -QC_HAL_EINVAL;
    }

    vPortFree(stm32_spi);
    return 0;
}

#endif /* QC_OS_FREERTOS */

#endif /* QC_PLATFORM_STM32 */
