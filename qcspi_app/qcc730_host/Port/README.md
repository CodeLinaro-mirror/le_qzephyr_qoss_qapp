# Qualcomm Port Layer

这是 Qualcomm 的移植层（Port Layer），提供了跨平台、跨操作系统的统一抽象接口。

## 架构概览

Port 层采用三层解耦架构：

```
┌─────────────────────────────────────────┐
│         Application Layer               │
│    (使用 qc_port.h 统一接口)            │
└─────────────────────────────────────────┘
                    │
        ┌───────────┴───────────┐
        │                       │
┌───────▼────────┐    ┌────────▼────────┐
│  Transport     │    │      OSAL       │
│  (SPI/SDIO)    │    │  (OS抽象层)     │
└───────┬────────┘    └─────────────────┘
        │
┌───────▼────────┐
│      HAL       │
│  (硬件抽象层)   │
└────────────────┘
```

### 三层职责

1. **OSAL (Operating System Abstraction Layer)**
   - 提供 OS 无关的接口：线程、互斥锁、信号量、队列、时间、日志等
   - 支持：Zephyr、FreeRTOS
   - 位置：`osal/`

2. **HAL (Hardware Abstraction Layer)**
   - 提供硬件无关的接口：GPIO、SPI、UART 等
   - 支持：STM32（未来可扩展 ESP32、RPI 等）
   - 位置：`hal/`

3. **Transport Layer**
   - 提供传输协议抽象：SPI、SDIO、UART 等
   - 使用 HAL 进行硬件操作，使用 OSAL 进行同步
   - 位置：`transport/`

## 目录结构

```
Port/
├── README.md                          # 本文件
├── PORT_LAYER_REFACTORING_SUGGESTIONS.md  # 重构建议文档
├── MIGRATION_GUIDE.md                 # 迁移指南
├── qc_port.h                          # 统一接口头文件
├── qc_port_config.h                   # 配置文件
│
├── osal/                              # OS 抽象层
│   ├── qc_osal.h                      # OSAL 接口定义
│   ├── freertos/
│   │   └── qc_osal_freertos.c         # FreeRTOS 实现
│   └── zephyr/
│       └── qc_osal_zephyr.c           # Zephyr 实现
│
├── hal/                               # 硬件抽象层
│   ├── qc_hal.h                       # HAL 接口定义
│   └── stm32/
│       └── qc_hal_stm32.c             # STM32 实现（Zephyr）
│
└── transport/                         # 传输层
    ├── qc_transport.h                 # Transport 接口定义
    └── qc_transport_spi.c             # SPI 传输实现
```

## 快速开始

### 1. 基本使用

```c
#include "qc_port.h"

int main(void)
{
    struct qc_port_ctx port_ctx;
    uint8_t tx_buf[256];
    uint8_t rx_buf[256];
    int ret;

    /* 初始化 Port 层 */
    ret = qc_port_init(&port_ctx);
    if (ret < 0) {
        return ret;
    }

    /* 执行 SPI 传输 */
    ret = qc_transport_transceive(port_ctx.transport, 
                                   tx_buf, rx_buf, 256, false);
    if (ret < 0) {
        QC_OSAL_LOG_ERR("Transfer failed: %d", ret);
    }

    /* 清理 */
    qc_port_deinit(&port_ctx);

    return 0;
}
```

### 2. 配置

在 `qc_port_config.h` 中配置：

```c
/* 选择操作系统 */
#define QC_OS_ZEPHYR 1
// #define QC_OS_FREERTOS 1

/* 选择平台 */
#define QC_PLATFORM_STM32 1
// #define QC_PLATFORM_ESP32 1

/* 选择传输方式 */
#define QC_TRANSPORT_SPI 1
// #define QC_TRANSPORT_SDIO 1
```

## 核心特性

### ✅ 清晰的职责分离
- OSAL 只处理 OS 相关操作
- HAL 只处理硬件相关操作
- Transport 只处理传输协议逻辑

### ✅ 线性扩展性
- 添加新平台：只需实现 HAL 层
- 添加新 OS：只需实现 OSAL 层
- 添加新传输：只需实现 Transport 层
- 文件数量：N+M+K（而不是 N×M×K）

### ✅ 线程安全
- Transport 层内置互斥锁保护
- 支持多线程并发访问

### ✅ 统计功能
- 传输字节数统计
- 错误计数
- 超时计数
- 事务计数

### ✅ 易于测试
- 每一层都可以独立测试
- 支持 Mock 实现

## API 参考

### Port 层统一接口

```c
/* 初始化整个 Port 层 */
int qc_port_init(struct qc_port_ctx *port_ctx);

/* 清理整个 Port 层 */
int qc_port_deinit(struct qc_port_ctx *port_ctx);
```

### Transport 层接口

```c
/* 初始化传输层 */
int qc_transport_init(qc_transport_t *transport, 
                      const struct qc_transport_config *config);

/* 全双工传输 */
int qc_transport_transceive(qc_transport_t transport, 
                             const uint8_t *tx_buf, uint8_t *rx_buf, 
                             size_t len, bool hold_cs);

/* 只发送 */
int qc_transport_send(qc_transport_t transport, 
                      const uint8_t *data, size_t len);

/* 只接收 */
int qc_transport_recv(qc_transport_t transport, 
                      uint8_t *data, size_t len);

/* 获取统计信息 */
int qc_transport_get_stats(qc_transport_t transport, 
                           struct qc_transport_stats *stats);

/* 重置统计信息 */
int qc_transport_reset_stats(qc_transport_t transport);

/* 清理传输层 */
int qc_transport_deinit(qc_transport_t transport);
```

### HAL 层接口

```c
/* 初始化 HAL */
int qc_hal_init(qc_hal_ctx_t *ctx);

/* GPIO 操作 */
int qc_hal_gpio_init(qc_hal_ctx_t ctx, qc_hal_gpio_t *gpio, 
                     const struct qc_hal_gpio_config *config);
int qc_hal_gpio_set(qc_hal_gpio_t gpio, bool value);
int qc_hal_gpio_get(qc_hal_gpio_t gpio, bool *value);
int qc_hal_gpio_deinit(qc_hal_gpio_t gpio);

/* SPI 操作 */
int qc_hal_spi_init(qc_hal_ctx_t ctx, qc_hal_spi_t *spi, 
                    const struct qc_hal_spi_config *config);
int qc_hal_spi_transfer(qc_hal_spi_t spi, const uint8_t *tx_buf, 
                        uint8_t *rx_buf, size_t len);
int qc_hal_spi_deinit(qc_hal_spi_t spi);

/* 清理 HAL */
int qc_hal_deinit(qc_hal_ctx_t ctx);
```

### OSAL 层接口

```c
/* 互斥锁 */
int qc_osal_mutex_init(qc_osal_mutex_t *mutex);
int qc_osal_mutex_lock(qc_osal_mutex_t mutex, int32_t timeout_ms);
int qc_osal_mutex_unlock(qc_osal_mutex_t mutex);

/* 信号量 */
int qc_osal_sem_init(qc_osal_sem_t *sem, uint32_t initial, uint32_t max);
int qc_osal_sem_take(qc_osal_sem_t sem, int32_t timeout_ms);
int qc_osal_sem_give(qc_osal_sem_t sem);

/* 时间 */
void qc_osal_msleep(uint32_t ms);
void qc_osal_usleep(uint32_t us);
uint32_t qc_osal_uptime_get_ms(void);

/* 日志 */
void qc_osal_log(qc_osal_log_level_t level, const char *fmt, ...);
#define QC_OSAL_LOG_ERR(fmt, ...)
#define QC_OSAL_LOG_WRN(fmt, ...)
#define QC_OSAL_LOG_INF(fmt, ...)
#define QC_OSAL_LOG_DBG(fmt, ...)

/* 内存 */
void *qc_osal_malloc(size_t size);
void qc_osal_free(void *ptr);

/* 线程 */
int qc_osal_thread_create(qc_osal_thread_t *thread, 
                          const struct qc_osal_thread_config *config);
int qc_osal_thread_delete(qc_osal_thread_t thread);
void qc_osal_thread_yield(void);

/* 消息队列 */
int qc_osal_queue_init(qc_osal_queue_t *queue, uint32_t max_msgs, size_t msg_size);
int qc_osal_queue_send(qc_osal_queue_t queue, const void *msg, int32_t timeout_ms);
int qc_osal_queue_recv(qc_osal_queue_t queue, void *msg, int32_t timeout_ms);
int qc_osal_queue_get_count(qc_osal_queue_t queue);

/* 工作队列 */
int qc_osal_work_init(qc_osal_work_t *work, qc_osal_work_handler_t handler);
int qc_osal_work_queue_init(qc_osal_work_q_t *work_q, size_t stack_size, int priority);
int qc_osal_work_submit(qc_osal_work_q_t work_q, qc_osal_work_t work);
```

## 支持的平台和操作系统

### 当前支持

| 平台  | Zephyr | FreeRTOS |
|-------|--------|----------|
| STM32 | ✅     | ✅       |

✅ = 已实现  
🚧 = 计划中  
❌ = 不支持

### 未来计划

- ESP32 平台支持
- Raspberry Pi 平台支持
- Linux 操作系统支持
- SDIO 传输支持
- UART 传输支持

## 性能

- **内存开销**: 每个 Transport 实例约 100 字节
- **CPU 开销**: 函数调用开销 < 1%
- **线程安全**: 内置互斥锁，支持多线程并发

## 文档

- [重构建议](PORT_LAYER_REFACTORING_SUGGESTIONS.md) - 详细的架构设计和改进建议
- [迁移指南](MIGRATION_GUIDE.md) - 从旧架构迁移到新架构的步骤和示例

## 贡献

### 添加新平台支持

1. 在 `hal/` 下创建平台目录，如 `hal/esp32/`
2. 实现 `qc_hal.h` 中定义的接口
3. 在 `qc_port_config.h` 中添加平台选项
4. 更新本 README

### 添加新 OS 支持

1. 在 `osal/` 下创建 OS 目录，如 `osal/linux/`
2. 实现 `qc_osal.h` 中定义的接口
3. 在 `qc_port_config.h` 中添加 OS 选项
4. 更新本 README

### 添加新传输方式

1. 在 `transport/` 下创建实现文件，如 `qc_transport_sdio.c`
2. 实现 `qc_transport.h` 中定义的接口
3. 在 `qc_port_config.h` 中添加传输选项
4. 更新本 README

## 许可证

Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
SPDX-License-Identifier: BSD-3-Clause-Clear

## 联系方式

如有问题或建议，请联系 Qualcomm 技术支持团队。
