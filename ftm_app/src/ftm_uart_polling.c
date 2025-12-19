/**
 * @file ftm_uart_pollling.c
 * @brief UART Driver Wrapper Implementation for FTM Application
 */

#include <stdint.h>
#include <zephyr/kernel.h>
#include <ftm_uart_polling.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <string.h>
#include <zephyr/logging/log.h>
#include "printfext.h"

#if (defined FTM_OVER_UART)
#ifdef cmdMAX_INPUT_SIZE
#undef cmdMAX_INPUT_SIZE
#endif
#define cmdMAX_INPUT_SIZE (1024 * 2)
#define UART_POLL_INTERVAL_MS 10
volatile uint8_t cInputString[cmdMAX_INPUT_SIZE] __attribute__((aligned(4)));
volatile uint16_t cInputIndex = 0;
uint16_t cInputLength = 0;
struct diag_pkt {
    size_t len;
    uint8_t buf[cmdMAX_INPUT_SIZE] __attribute__((aligned(4)));
};
#endif
#define UART_TERMINATION_CHAR 0x7E

static const struct device *uart_dev = NULL;
#define configUART_COMMAND_CONSOLE_STACK_SIZE		( 1024*2 )
#define configUART_COMMAND_CONSOLE_TASK_PRIORITY	( 7U )

static k_tid_t FTM_Task_handle;
K_THREAD_STACK_DEFINE(ftm_stack, configUART_COMMAND_CONSOLE_STACK_SIZE);
static struct k_thread ftm_thread;

K_FIFO_DEFINE(diag_fifo);
static k_tid_t Diag_Task_handle;
K_THREAD_STACK_DEFINE(diag_stack, configUART_COMMAND_CONSOLE_STACK_SIZE);
static struct k_thread diag_thread;

void ftm_task_init(void);
static void ftm_task(void *pvParameters1, void *pvParameters2, void *pvParameters3);
static void diag_task(void *pvParameters1, void *pvParameters2, void *pvParameters3);
void ftm_uart_init(void);
void uart_polling_receive(void);
extern int processDiagPacket(int client, unsigned char *inputMsg, unsigned int cmdLen);


void ftm_task_init(void)
{
    printk("Build FTM lib date and time: %s - %s\n", __DATE__, __TIME__);
    
    FTM_Task_handle = k_thread_create(
        &ftm_thread,
        ftm_stack,
        K_THREAD_STACK_SIZEOF(ftm_stack),
        ftm_task,
        NULL, NULL, NULL,
        configUART_COMMAND_CONSOLE_TASK_PRIORITY,
        0, K_NO_WAIT
    );

    if (FTM_Task_handle == NULL) {
        printk("ERROR: FTM task creation failed!\n");
        return;
    }
    k_thread_name_set(FTM_Task_handle, "FTM_Task");
    
    Diag_Task_handle = k_thread_create(
        &diag_thread,
        diag_stack,
        K_THREAD_STACK_SIZEOF(diag_stack),
        diag_task,
        NULL, NULL, NULL,
        configUART_COMMAND_CONSOLE_TASK_PRIORITY,
        0, K_NO_WAIT
    );
    
    if (Diag_Task_handle == NULL) {
        printk("ERROR: Diag task creation failed!\n");
        return;
    }
    k_thread_name_set(Diag_Task_handle, "Diag_Task");

    printk("FTM & Diag task created successfully!\n");
}

void ftm_uart_init(void)
{
    int ret;

    uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart0));
    if (!device_is_ready(uart_dev)) {
        printk("uart0 device not ready\n");
        return;
    }

    struct uart_config uart_cfg = {
        .baudrate = CONFIG_FTM_UART_BAUDRATE,
        .parity = UART_CFG_PARITY_NONE,
        .stop_bits = UART_CFG_STOP_BITS_1,
        .data_bits = UART_CFG_DATA_BITS_8,
        .flow_ctrl = UART_CFG_FLOW_CTRL_NONE
    };

    ret = uart_configure(uart_dev, &uart_cfg);
    if (ret != 0) {
        printk("Failed to configure UART: %d", ret);
        return;
    }

    uart_irq_rx_disable(uart_dev);
    uart_irq_tx_disable(uart_dev);
    
}

void uart_polling_receive(void)
{
    uint8_t data;
    int ret;
    static uint32_t no_data_count = 0;

    for(;;){
        ret = uart_poll_in(uart_dev, &data);
    
        
        if (ret == 0) {
            if (cInputIndex < cmdMAX_INPUT_SIZE) {
                cInputString[cInputIndex] = data;
                cInputIndex++;
            }else {
                printk("ERROR: UART buffer overflow, resetting\n");
                cInputIndex = 0;
                continue;
            }
            
            if (data == UART_TERMINATION_CHAR) {
                cInputLength = cInputIndex;
                printk("RX receive length: %d\n",cInputLength);
                k_msleep(1);

                struct diag_pkt *pkt = k_malloc(sizeof(struct diag_pkt));
                if (pkt) {
                    if (cInputLength > cmdMAX_INPUT_SIZE) {
                        printk("ERROR: Invalid packet length %d\n", cInputLength);
                        k_free(pkt);
                        cInputIndex = 0;
                        continue;
                    }
                    pkt->len = cInputLength;
                    memcpy(pkt->buf, cInputString, cInputLength);
                    k_fifo_put(&diag_fifo, pkt);
                }else{
                    printk("pkt malloc fail!\n");
                    cInputIndex = 0;
                    k_msleep(10);
                    continue;
                }
                
                cInputIndex = 0;
            }
            continue;
        } else if (ret < 0) {
            k_busy_wait(200);
            no_data_count++;
            if (no_data_count == 5) {
                no_data_count = 0;
                k_yield();
            }
        }
    }
}

static void ftm_task(void *pvParameters1, void *pvParameters2, void *pvParameters3)
{
    PRINT_LOG_FUNC_LINE_ENTRY;
    ftm_uart_init();

    uart_polling_receive();

}

static void diag_task(void *pvParameters1, void *pvParameters2, void *pvParameters3)
{
    PRINT_LOG_FUNC_LINE_ENTRY;
    for (;;) {
        struct diag_pkt *pkt = k_fifo_get(&diag_fifo, K_FOREVER);
        if (!pkt) continue;

        processDiagPacket(0, pkt->buf, pkt->len);

        k_free(pkt);
    }
}
