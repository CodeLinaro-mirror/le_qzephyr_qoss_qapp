/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "../../Port/qc_port.h"

#if defined(QC_OS_FREERTOS) && defined(QCC730_ATCMD_ENABLE)

#ifdef SHELL_FEATURE
#include "shell.h"
#endif

#include "../../Service/ring/ring_service.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ============================================================================
 * AT Command Configuration
 * ============================================================================ */

#define AT_ARGC_MAX 128    /* Maximum number of arguments */
#define AT_CMD_MAX_SIZE 32 /* Maximum command name size */
#define ATCMD_PARSER_FUNC_LIST_MAX_SIZE 32
#define AT_RESPONSE_MAX 2048  /* Maximum response buffer size */
#define ATCMD_BUF_LEN 1400    /* AT command buffer length */
#define MAX_LOOP 1000000      /* Maximum loop count */
#define TEST_BUFFER_SIZE 1400 /* Test buffer size */
#define ONE_GB_BYTES (1024UL * 1024 * 1024UL)

#define RING_AT RING_0
#define RING_DATA RING_1

#define demo_print printf

/* Control whether received AT responses are printed.
 * Use shell command "resp_print 0" to disable and "resp_print 1" to enable.
 */
static int g_resp_print_enable = 1;

/* ============================================================================
 * Global Variables
 * ============================================================================ */

int qat_demo_parser_enable = 1;
uint8_t tx_test_start;

/* Test parameters */
typedef struct {
    int task_started;
    int num;
    int len;
    int mqtt_recv_count;
    int mqtt_recv_timeout;
    uint32_t start_tick;
    uint32_t end_tick;
} atcmd_mqtt_rx_test_t;

typedef struct {
    uint32_t interval;
    uint32_t len;
    uint32_t time;
    uint32_t bytes;
    uint32_t g_bytes;
} atcmd_tx_test_t;

typedef struct {
    char *cmd;
    int (*func)(int argc, char **argv, char *orig_cmd);
    char *description;
} atcmd_parser_func_t;

typedef enum { QAT_RESP_TYPE, QAT_EVT_TYPE } atcmd_parser_type;

typedef enum {
    QCC730_SPI_NOT_READY,
    QCC730_SPI_READY,
} qcc730_spi_state;

typedef enum {
    QCC730_SLEEP_STATE_AWAKE,
    QCC730_SLEEP_STATE_SLEEPING,
} qcc730_sleep_state;

/**
 * @brief AT command context structure
 *
 * Manages AT command buffers and SPI state
 */
typedef struct qcc730_atcmd_s {
    uint8_t rx_buf[ATCMD_BUF_LEN]; /* RX buffer */
    uint8_t tx_buf[ATCMD_BUF_LEN]; /* TX buffer */
    int spi_state;                 /* SPI state (QCC730_SPI_NOT_READY/QCC730_SPI_READY) */
} qcc730_atcmd_t;

/* Global AT command context */
static qcc730_atcmd_t g_qcc730_atcmd = {
    .spi_state = QCC730_SPI_NOT_READY,
};
static qcc730_atcmd_t *qcc730_atcmd = &g_qcc730_atcmd;

/* QCC730 sleep state tracking */
static int g_qcc730_sleep_state = QCC730_SLEEP_STATE_AWAKE;

static int atcmd_parser_resp_func_list_size = 0;
static int atcmd_parser_evt_func_list_size = 0;

static atcmd_parser_func_t atcmd_parser_resp_func_list[ATCMD_PARSER_FUNC_LIST_MAX_SIZE];
static atcmd_parser_func_t atcmd_parser_evt_func_list[ATCMD_PARSER_FUNC_LIST_MAX_SIZE];

atcmd_mqtt_rx_test_t *rx_mqtt_test_param = NULL;
atcmd_tx_test_t *tx_test_param = NULL;
uint8_t tx_quit = 0;
uint8_t rx_quit = 0;

/* HTTP test variables */
static int http_mode = -1;
static int http_send_num_temp = 0;
static int http_send_num = 0;
static int http_send_num_max = 10;
static int http_Range = 400;
static int http_send_data_num_max = 10;
static int http_send_data_num = 0;
static int http_cmd_wait_time = 500;  /* ms */
static int http_data_wait_time = 500; /* ms */

typedef enum {
    HTTP_TEST_MODEL_CHECK_DATA_LENGTH,
    HTTP_TEST_MODEL_CHECK_DATA_LENGTH_AND_VALUE,
    HTTP_TEST_MODEL_CHECK_BIG_DATA,
    HTTP_TEST_MODEL_CHECK_BIG_DATA_ON_DATA_MODEL,
} http_mode_t;

/* HTTP test buffers */
char test_buf_cmd[] = "AT+HTTPPOST=\"https://192.168.0.200/index.asp\",9000\r";
char test_buf[] =
    "1234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890"
    "1234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890"
    "1234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890"
    "1234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890"
    "1234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890"
    "1234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890"
    "1234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890"
    "1234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890"
    "1234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890"
    "1234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890\r";

char test_buf1[] = "AT+HTTPCLIENT=2,0,\"https://192.168.0.200/get_test_1200.txt\"\r";
char test_buf2[] = "AT+HTTPCLIENT=2,0,\"https://192.168.0.200/get_test_1400.txt\"\r";
char test_buf3[] = "AT+HTTPCLIENT=2,0,\"https://192.168.0.200/get_test_1600.txt\",\"Range:bytes=0-399\"\r";
char test_buf4[] = "AT+HTTPCLIENT=2,0,\"https://192.168.0.200/get_test_1600.txt\",\"Range:bytes=400-799\"\r";
char test_buf5[] = "AT+HTTPCLIENT=2,0,\"https://192.168.0.200/get_test_1600.txt\",\"Range:bytes=800-1199\"\r";
char test_buf6[] = "AT+HTTPCLIENT=2,0,\"https://192.168.0.200/get_test_1600.txt\",\"Range:bytes=1200-1599\"\r";

char test_buf_end0[] = "000";
char test_buf_end1[] = "111";
char test_buf_end2[] = "222";
char test_buf_end3[] = "333";


void qcc730_wkup()
{
    qc_hal_gpio_write(QCC730_WKUP_GPIO_Port, QCC730_WKUP_Pin, QC_HAL_GPIO_PIN_RESET);
    qc_hal_delay(300);
    qc_hal_gpio_write(QCC730_WKUP_GPIO_Port, QCC730_WKUP_Pin, QC_HAL_GPIO_PIN_SET);
}

/* ============================================================================
 * AT Command Send/Receive Functions
 * ============================================================================ */

/**
 * @brief Send AT command via ring service
 */
static int atcmd_send(const uint8_t *cmd, uint32_t len)
{
    if (!cmd || len == 0) {
        return -QC_OSAL_EINVAL;
    }

    /* Log command (skip for test data) */
    if (tx_test_start == 0) {
        if (strncmp((char *)cmd, test_buf, strlen((char *)cmd)) != 0) {
            demo_print("atcmd send: %d %s\r\n", len, cmd);
        }
    }

    /* Send in chunks if needed */
    uint32_t sent = 0;
    while (sent < len) {
        uint32_t chunk = (len - sent) > ATCMD_BUF_LEN ? ATCMD_BUF_LEN : (len - sent);
        int ret;
        do {
            ret = ring_send(RING_AT, cmd + sent, chunk, 1000);
            if (ret == -QC_OSAL_EAGAIN)
                qc_osal_msleep(1);
        } while (ret == -QC_OSAL_EAGAIN);
        if (ret < 0) {
            demo_print("ring_send error: %d\r\n", ret);
            return ret;
        }
        sent += chunk;
    }

    return 0;
}

/**
 * @brief Wrapper function for compatibility
 */
extern int qcc730_ring_reset();
void qcc730_atcmd_send_handler(uint8_t *cmd, uint32_t len)
{
    if (NULL == cmd || 1 >= len) {
        return;
    }

    // reset spi state
    if (QCC730_SPI_NOT_READY == qapi_atcmd_get_spi_state()) {
        if (qcc730_ring_reset() < 0) {
            demo_print("qcc730_ring_reset failed\r\n");
            return;
        }
        qapi_atcmd_set_spi_state(QCC730_SPI_READY);
    }

    atcmd_send(cmd, len);
}

/* ============================================================================
 * AT Response Parser Framework
 * ============================================================================ */

uint8_t atcmd_parser_exec_function_by_index(int argc, char *argv, char *original_cmd, int index, int flag)
{
    if (flag == QAT_RESP_TYPE)
        return atcmd_parser_resp_func_list[index].func(argc, argv, original_cmd);
    else if (flag == QAT_EVT_TYPE)
        return atcmd_parser_evt_func_list[index].func(argc, argv, original_cmd);

    return -1;
}

uint8_t atcmd_parser_find_command_index(int *index, char *header, int len, int flag)
{
    int i = 0;
    if (flag == QAT_RESP_TYPE) {
        for (i = 0; i < atcmd_parser_resp_func_list_size; i++) {
            if (!strcmp(atcmd_parser_resp_func_list[i].cmd, header)) {
                *index = i;
                return 0;
            }
        }
    } else if (flag == QAT_EVT_TYPE) {
        for (i = 0; i < atcmd_parser_evt_func_list_size; i++) {
            if (!strcmp(atcmd_parser_evt_func_list[i].cmd, header)) {
                *index = i;
                return 0;
            }
        }
    }
    return -1;
}

uint8_t atcmd_parser_func_add(char *cmd, int (*pfunc)(int argc, char **argv), char *description, int flag)
{
    if (flag == QAT_RESP_TYPE) {
        if (atcmd_parser_resp_func_list_size < ATCMD_PARSER_FUNC_LIST_MAX_SIZE) {
            atcmd_parser_resp_func_list[atcmd_parser_resp_func_list_size].cmd = cmd;
            atcmd_parser_resp_func_list[atcmd_parser_resp_func_list_size].func = pfunc;
            atcmd_parser_resp_func_list[atcmd_parser_resp_func_list_size].description = description;
            atcmd_parser_resp_func_list_size++;
            return 0;
        }
    } else if (flag == QAT_EVT_TYPE) {
        if (atcmd_parser_evt_func_list_size < ATCMD_PARSER_FUNC_LIST_MAX_SIZE) {
            atcmd_parser_evt_func_list[atcmd_parser_evt_func_list_size].cmd = cmd;
            atcmd_parser_evt_func_list[atcmd_parser_evt_func_list_size].func = pfunc;
            atcmd_parser_evt_func_list[atcmd_parser_evt_func_list_size].description = description;
            atcmd_parser_evt_func_list_size++;
            return 0;
        }
    }

    return -1;
}

/* ============================================================================
 * Response Parser Functions
 * ============================================================================ */

void atcmd_response_parser_RDMEM(int argc, uint32_t **argv, char *orig_cmd)
{
    printf("Response after parsing +RDMEM:\r\n");
    if (argc == 4) {
        printf("+RDMEM\r\n");
        printf("addr = %s\r\n", argv[0]);
        printf("size = %s\r\n", argv[1]);
        printf("value(hex) = %s\r\n", argv[2]);
        printf("value(dec) = %s\r\n", argv[3]);
    } else {
        printf("\r\n");
        printf("No need to parse, original response:\r\n");
        printf("%s\r\n", orig_cmd);
    }
}

void atcmd_response_parser_IPDHEX(int argc, uint32_t **argv, char *orig_cmd)
{
    int i = 0;
    uint8_t *buf = NULL;
    uint32_t buf_len = 0;

    if (argc == 5) {
        buf_len = atoi(argv[3]);
        if (buf_len != 0) {
            buf = (uint8_t *)qc_osal_malloc(buf_len);
            if (!buf) {
                printf("IPDHEX: malloc fail\r\n");
                return;
            }

            memset(buf, 0, buf_len);
            memcpy(buf, argv[4], buf_len);

            printf("+IPDHEX:%s,%s,%d,%d,", argv[0], argv[1], atoi(argv[2]), atoi(argv[3]));

            for (i = 0; i < buf_len; i++) {
                printf("%02x", buf[i]);
            }

            printf("\r\n");

            qc_osal_free(buf);
        }
    } else {
        printf("%s", orig_cmd);
    }
}

void atcmd_response_parser_RST(int argc, uint32_t **argv, char *orig_cmd)
{
    qapi_atcmd_set_spi_state(QCC730_SPI_NOT_READY);
}

void atcmd_response_parser_CIPDHCPV4C(int argc, uint32_t **argv, char *orig_cmd)
{
    printf("Response after parsing +CIPDHCPV4C:\r\n");
    if (argc == 5) {
        printf("+CIPDHCPV4C\r\n");
        printf("IP = %s\r\n", argv[0]);
        printf("Gateway = %s\r\n", argv[1]);
        printf("Mask = %s\r\n", argv[2]);
        printf("DNS0 = %s\r\n", argv[3]);
        printf("DNS1 = %s\r\n", argv[4]);
    } else {
        printf("\r\n");
        printf("No need to parse, original response:\r\n");
        printf("%s\r\n", orig_cmd);
    }
}

void atcmd_response_parser_EVT_MQTTSUBRECV(int argc, uint32_t **argv, char *orig_cmd)
{
    rx_mqtt_test_param->mqtt_recv_count++;

    if (rx_mqtt_test_param->mqtt_recv_count == 1)
        rx_mqtt_test_param->start_tick = qc_osal_uptime_get_ms();
}

void atcmd_response_parser_EVT_OTAFWUP_FIN(int argc, uint32_t **argv, char *orig_cmd)
{
    printf("OTAFWUP_FIN received, host will reset spi after 5s\r\n");
    qc_osal_msleep(5000);
    /* Reset will be handled by application */
}

void atcmd_response_parser_EVT_MQTTSUBRECVHEX(int argc, uint32_t **argv, char *orig_cmd)
{
    int i = 0;
    uint8_t *buf = NULL;
    uint32_t buf_len = 0;

    if (argc == 4) {
        buf_len = atoi(argv[2]);
        if (buf_len != 0) {
            buf = (uint8_t *)qc_osal_malloc(buf_len);
            memset(buf, 0, buf_len);
            memcpy(buf, argv[3], buf_len);

            printf("+EVT:MQTT_SUBRECVHEX:%s,%s,%s,", argv[0], argv[1], argv[2]);
            for (i = 0; i < buf_len; i++)
                printf("%x", *buf++);

            qc_osal_free(buf);
        }
    } else
        printf("%s", orig_cmd);
}

void atcmd_response_parser_EVT_DSLEEP_PRE(int argc, uint32_t **argv, char *orig_cmd)
{
    /* 730 will Reset after deep sleep, so host need reinit ring */
    qapi_atcmd_set_spi_state(QCC730_SPI_NOT_READY);
}

void atcmd_response_parser_EVT_DSLEEP_FAIL(int argc, uint32_t **argv, char *orig_cmd)
{
    qapi_atcmd_set_spi_state(QCC730_SPI_READY);
}

void atcmd_response_parser_EVT_WAKEUP(int argc, uint32_t **argv, char *orig_cmd)
{
    printf("QCC730 wakeup event received\r\n");
    g_qcc730_sleep_state = QCC730_SLEEP_STATE_AWAKE;
}

static char original_cmd[AT_RESPONSE_MAX] = {0};
uint8_t atcmd_response_handler(uint16_t buf_len, char *cmd)
{
    int argc;
    uint32_t *argv[AT_ARGC_MAX] = {0};
    // char original_cmd[AT_RESPONSE_MAX] = {0};
    char *p;
    char header[AT_CMD_MAX_SIZE] = "";
    int i = 0;
    int h = 0;
    int arg_start = 0;
    int flag = QAT_RESP_TYPE;

    /* Separate header and arguments.
     * IMPORTANT: header[] is only AT_CMD_MAX_SIZE (16) bytes.
     * Guard against buffer overflow when processing large non-AT payloads
     * (e.g. HTTP response body HTML).  If the header candidate grows beyond
     * AT_CMD_MAX_SIZE-1 bytes without finding ':', it cannot be a valid AT
     * command prefix — break early and let the find-command step fail. */
    for (i = 0; i < buf_len; i++) {
        if (cmd[i] == '\r' || cmd[i] == '\0' || cmd[i] == '\n') {
            continue;
        }
        if (cmd[i] == ':') {
            if (!strcmp(header, "+EVT")) {
                flag = QAT_EVT_TYPE;
            } else
                break;
        }
        if (h >= AT_CMD_MAX_SIZE - 1) {
            /* Header too long — not a recognised AT command prefix */
            h = 0;
            break;
        }
        header[h] = cmd[i];
        h++;
    }
    header[h] = '\0';
    arg_start = i;

    /* Search for the command and parameters */
    if (h != 0 && 0 == atcmd_parser_find_command_index(&i, header, h, flag)) {
        memset(original_cmd, 0, AT_RESPONSE_MAX);
        memcpy(original_cmd, cmd, buf_len);

        argc = 1;
        cmd += arg_start + 1;
        argv[0] = (uint32_t *)cmd;

        char *cmd_end = cmd + (buf_len - (arg_start + 1));

        for (p = cmd; p < cmd_end && argc < AT_ARGC_MAX; p++) {
            if (*p == '\r') {
                if ((p + 1) < cmd_end && *(p + 1) == '\n') {
                    *p = '\0';
                    p++;
                }
                *p = '\0';
                argv[argc] = (uint32_t *)(p + 1);
            } else if (*p == ',') {
                *p = '\0';
                argv[argc] = (uint32_t *)(p + 1);
                argc++;
            }
        }

        if (!strcmp(header, "+IPDHEX") && argc == 4) {
            argc++;
        }

        return atcmd_parser_exec_function_by_index(argc, (char *)argv, original_cmd, i, flag);
    }

    return -1;
}

/* Forward declarations for functions that will be implemented */
void test_http_process(uint8_t *data, uint32_t len);

void print_atcmd_resp(uint8_t *data, uint32_t len)
{
    if (len >= 7 && !strncmp((char *)&data[2], "+CMD:", 5)) {
        printf("%.*s", (int)len, (char *)data);
        return;
    }

    if (len >= 8 && !strncmp((char *)&data[0], "+IPDHEX:", 8)) {
        return;
    }

    if (len >= 20 && !strncmp((char *)&data[0], "+EVT:MQTT_SUBRECVHEX", 20)) {
        return;
    }

    if (http_mode != -1) {
        test_http_process(data, len);
        return;
    }

    if (len >= 5 && !strncmp((char *)&data[0], "+IPD:", 5)) {
        return;
    }

    printf("%.*s", (int)len, (char *)data);
}

/**
 * @brief AT command receive callback (called from ring service work queue)
 */
static void atcmd_rx_callback(uint8_t ring_id, void *user_data)
{
    int ret;
    int packet_count = 0;
    int error_count = 0;

    /* Loop to read all available data packets */
    do {
        if (packet_count > 0 && packet_count % 10 == 0)
            qc_osal_msleep(1);
        /* Use rx_buf from qcc730_atcmd structure */
        memset(qcc730_atcmd->rx_buf, 0, ATCMD_BUF_LEN);
        ret = ring_recv(ring_id, qcc730_atcmd->rx_buf, sizeof(qcc730_atcmd->rx_buf), 0);
        if (ret > 0) {
            packet_count++;
            switch (ring_id) {
            case RING_AT:
                /* Print received response */
                if (rx_mqtt_test_param->task_started == 0) {
                    print_atcmd_resp(qcc730_atcmd->rx_buf, ret);
                }

                /* Parse response if parser is enabled */
                if (qat_demo_parser_enable == 1) {
                    atcmd_response_handler(ret, (char *)qcc730_atcmd->rx_buf);
                }
                break;
            case RING_DATA:
                break;
            case RING_2:
                break;
            default:
                break;
            }
        } else if (ret < 0) {
            QC_OSAL_LOG_ERR("Failed to receive data: %d", ret);
            break; /* Exit on error */ 
        } else {
            error_count = 0;
        }

        /* ret == 0 means no more data available, loop will exit */
    } while (ret > 0);
}

/* ============================================================================
 * Shell Command Handlers
 * ============================================================================ */

void atcmd_help(int argc, char **argv) { printf("AT+CMD?\r\n"); }

void atcmd_enable_parser(int argc, char **argv)
{
    if (argc > 2) {
        printf("usage: parser enable|disable\r\n");
        return;
    }

    if (strncmp(argv[1], "enable", 6) == 0) {
        printf("response parser enabled\r\n");
        qat_demo_parser_enable = 1;
    } else if (strncmp(argv[1], "disable", 7) == 0) {
        printf("response parser disabled\r\n");
        qat_demo_parser_enable = 0;
    } else {
        printf("usage: parser enable|disable\r\n");
    }
}

/* ============================================================================
 * Test Functions
 * ============================================================================ */

uint32_t obtain_mqtt_pub_recv_count(void) { return rx_mqtt_test_param->mqtt_recv_count; }

static void atcmd_demo_rx_mqtt(void)
{
    uint32_t start = 0, received = 0;
    uint16_t num = 0, len = 0;
    uint32_t diff = 0;
    uint32_t bits = 0;
    uint32_t result = 0;

    num = (uint16_t)rx_mqtt_test_param->num;
    len = (uint16_t)rx_mqtt_test_param->len;

    rx_mqtt_test_param->task_started = 1;
    rx_mqtt_test_param->start_tick = 0;

    start = obtain_mqtt_pub_recv_count();

    demo_print("wait to receive %d packets\r\n", num);

    while (received < num) {
        received = obtain_mqtt_pub_recv_count() - start;

        if (rx_mqtt_test_param->task_started == 0) {
            demo_print("test running but stopped by cmd\r\n");
            break;
        }
    }

    rx_mqtt_test_param->end_tick = qc_osal_uptime_get_ms();
    diff = rx_mqtt_test_param->end_tick - rx_mqtt_test_param->start_tick;
    rx_mqtt_test_param->task_started = 0;
    bits = received * len * 8;
    result = bits * 1000 / diff;

    demo_print("receive %d packets\r\n", received);
    demo_print("rx test done\r\n");
    demo_print("rx speed = %d(bits/sec)\r\n", result);

    qc_osal_thread_delete(NULL);
}

static void atcmd_demo_tx(uint32_t num, uint32_t size)
{
    uint32_t j = 0, count = 0;
    uint8_t *write_buf = qc_osal_malloc(1400);

    tx_test_start = 1;

    srand(time(NULL));

    for (j = 0; j < 1400; j++) {
        write_buf[j] = (rand() % 100) + 1;
    }

    uint16_t buf_len = 0;

    count = (num == 0 || num > MAX_LOOP) ? 10 : num;
    buf_len = (size == 0 || size > 1400) ? 1400 : size;

    uint32_t start = qc_osal_uptime_get_ms();

    for (j = 0; j < count; j++) {
        if (atcmd_send(write_buf, buf_len)) {
            printf("atcmd_send error\r\n");
        }
    }

    uint32_t end = qc_osal_uptime_get_ms();

    printf("tx speed = %d(bits/sec)\r\n", (count * buf_len * 8) * 1000 / (end - start));
    printf("send %d bits time:[now]=%d,start=%d, take ticks = %d, interval:%d ms\r\n", count * buf_len * 8, end, start,
           end - start, ((end - start) * 1000 / 1000));

    qc_osal_free(write_buf);
    tx_test_start = 0;
}

static void atcmd_demo_tx_loop(void)
{
    uint8_t *write_buf = qc_osal_malloc(tx_test_param->len);
    uint32_t j = 0, packets_num_gb = 0, packets_num_b = 0, packets_num_per_10s = 0, runtime = 0, runtime_per_10s = 0,
             count_per_10s = 0;
    uint32_t is_test_done = 0;

    uint32_t start, start_per_10s;
    uint32_t end, end_per_10s;

    tx_quit = 0;
    tx_test_start = 1;

    srand(time(NULL));

    for (j = 0; j < tx_test_param->len; j++) {
        write_buf[j] = (rand() % 100) + 1;
    }

    start = qc_osal_uptime_get_ms();
    start_per_10s = qc_osal_uptime_get_ms();

    while (!is_test_done) {
        if (tx_quit) {
            is_test_done = 1;
            break;
        }

        end = qc_osal_uptime_get_ms();
        runtime = (end - start) / 1000;
        if ((tx_test_param->time > 0) && runtime * 1000 >= tx_test_param->time) {
            is_test_done = 1;
            break;
        }

        /* Data transmission */
        if (atcmd_send(write_buf, tx_test_param->len)) {
            printf("atcmd_send error\r\n");
        } else {
            packets_num_b += tx_test_param->len;
            packets_num_per_10s += tx_test_param->len;

            /* Handle GB rollover */
            if (packets_num_b >= ONE_GB_BYTES) {
                packets_num_gb++;
                packets_num_b -= ONE_GB_BYTES;
            }
        }

        if (tx_test_param->g_bytes > 0 || tx_test_param->bytes > 0) {
            if (packets_num_gb > tx_test_param->g_bytes) {
                is_test_done = 1;
                break;
            } else if (packets_num_gb == tx_test_param->g_bytes) {
                if (packets_num_b >= tx_test_param->bytes) {
                    is_test_done = 1;
                    break;
                }
            }
        }

        end_per_10s = qc_osal_uptime_get_ms();
        runtime_per_10s = (end_per_10s - start_per_10s) / 1000;

        if (runtime_per_10s >= 10) {
            printf("tx speed = %d(bits/sec) for %ds-%ds\r\n", (packets_num_per_10s * 8) / runtime_per_10s,
                   count_per_10s, count_per_10s + 10);
            printf("total bytes = %dG, %dB\r\n", packets_num_gb, packets_num_b);
            count_per_10s += 10;
            runtime_per_10s = 0;
            packets_num_per_10s = 0;
            end_per_10s = qc_osal_uptime_get_ms();
            start_per_10s = qc_osal_uptime_get_ms();
        }

        /* Delay if specified */
        if (tx_test_param->interval) {
            qc_osal_msleep(tx_test_param->interval);
        }
    }

    tx_test_start = 0;

    printf("tx speed = %d(bits/sec) for %ds-%ds\r\n", (packets_num_per_10s * 8) / runtime_per_10s, count_per_10s,
           count_per_10s + runtime_per_10s);

    memset(tx_test_param, 0, sizeof(atcmd_tx_test_t));

    qc_osal_free(write_buf);

    printf("delete thread\r\n");

    qc_osal_thread_delete(NULL);
}

static void atcmd_demo_net_tx_loop(void)
{
    uint8_t *write_buf = NULL;
    uint32_t j = 0, packets_num_gb = 0, packets_num_b = 0, packets_num_per_10s = 0, runtime = 0, runtime_per_10s = 0,
             count_per_10s = 0;
    uint32_t is_test_done = 0;
    uint32_t start, start_per_10s;
    uint32_t end, end_per_10s;

    if (tx_test_param->len == 0 || tx_test_param->len > ATCMD_BUF_LEN) {
        printf("net_tx_loop: invalid len %u, must be 1..%d\r\n", tx_test_param->len, ATCMD_BUF_LEN);
        qc_osal_thread_delete(NULL);
        return;
    }

    write_buf = qc_osal_malloc(tx_test_param->len);
    if (!write_buf) {
        printf("net_tx_loop: failed to allocate %u bytes\r\n", tx_test_param->len);
        qc_osal_thread_delete(NULL);
        return;
    }

    tx_quit = 0;
    tx_test_start = 1;

    srand(time(NULL));

    for (j = 0; j < tx_test_param->len; j++) {
        write_buf[j] = 'A' + (j % 26);
    }

    printf("net_tx_loop start: len=%u interval=%u(ms) time=%u(ms) target=%uG %uB\r\n",
           tx_test_param->len, tx_test_param->interval, tx_test_param->time,
           tx_test_param->g_bytes, tx_test_param->bytes);
    printf("NOTE: run AT+CIPSTART and AT+CIPSEND=<id>,0 before net_tx_loop\r\n");

    start = qc_osal_uptime_get_ms();
    start_per_10s = qc_osal_uptime_get_ms();

    while (!is_test_done) {
        if (tx_quit) {
            is_test_done = 1;
            break;
        }

        end = qc_osal_uptime_get_ms();
        runtime = (end - start) / 1000;
        if ((tx_test_param->time > 0) && ((end - start) >= tx_test_param->time)) {
            is_test_done = 1;
            break;
        }

        qcc730_atcmd_send_handler(write_buf, tx_test_param->len);
        packets_num_b += tx_test_param->len;
        packets_num_per_10s += tx_test_param->len;

        if (packets_num_b >= ONE_GB_BYTES) {
            packets_num_gb++;
            packets_num_b -= ONE_GB_BYTES;
        }

        if (tx_test_param->g_bytes > 0 || tx_test_param->bytes > 0) {
            if (packets_num_gb > tx_test_param->g_bytes) {
                is_test_done = 1;
                break;
            } else if (packets_num_gb == tx_test_param->g_bytes) {
                if (packets_num_b >= tx_test_param->bytes) {
                    is_test_done = 1;
                    break;
                }
            }
        }

        end_per_10s = qc_osal_uptime_get_ms();
        runtime_per_10s = (end_per_10s - start_per_10s) / 1000;

        if (runtime_per_10s >= 10) {
            printf("net tx speed = %d(bits/sec) for %ds-%ds\r\n",
                   (packets_num_per_10s * 8) / runtime_per_10s,
                   count_per_10s, count_per_10s + 10);
            printf("net total bytes = %dG, %dB\r\n", packets_num_gb, packets_num_b);
            count_per_10s += 10;
            runtime_per_10s = 0;
            packets_num_per_10s = 0;
            end_per_10s = qc_osal_uptime_get_ms();
            start_per_10s = qc_osal_uptime_get_ms();
        }

        if (tx_test_param->interval) {
            qc_osal_msleep(tx_test_param->interval);
        }
    }

    qcc730_atcmd_send_handler((uint8_t *)"+++", 3);

    tx_test_start = 0;

    if (runtime_per_10s == 0) {
        uint32_t total_ms = qc_osal_uptime_get_ms() - start;
        if (total_ms == 0) {
            total_ms = 1;
        }
        printf("net tx avg speed = %lu(bits/sec)\r\n",
               (unsigned long)(((uint64_t)(packets_num_gb * ONE_GB_BYTES + packets_num_b) * 8 * 1000) / total_ms));
    } else {
        printf("net tx speed = %d(bits/sec) for %ds-%ds\r\n",
               (packets_num_per_10s * 8) / runtime_per_10s,
               count_per_10s, count_per_10s + runtime_per_10s);
    }

    printf("net_tx_loop done, total bytes = %dG, %dB\r\n", packets_num_gb, packets_num_b);

    memset(tx_test_param, 0, sizeof(atcmd_tx_test_t));
    qc_osal_free(write_buf);

    qc_osal_thread_delete(NULL);
}

void atcmd_qat_perf(int argc, char **argv)
{
    uint8_t index = 0;
    uint32_t speed = 0;
    uint32_t qatperf_duration_ms = 1000;
    uint32_t qatperf_pkt_len = 1400;
    uint32_t qatperf_bytes_sent = 0;
    uint8_t *qatperf_data = NULL;

    if (argc < 3) {
        printf("\nUsage: qatperf [options]\n");
        printf("  -l = The length of buffers to read or write\n");
        printf("  -t = The time in seconds to transmit for\n");
        return;
    }

    for (index = 0; index < argc; index++) {
        if (0 == strcmp(argv[index], "-t")) {
            index++;
            qatperf_duration_ms = atoi(argv[index]);
            index++;
        }
    }
    for (index = 0; index < argc; index++) {
        if (0 == strcmp(argv[index], "-l")) {
            index++;
            qatperf_pkt_len = atoi(argv[index]);
            index++;
        }
    }

    printf("qatperf started with data length = %d and duration = %d(ms)\r\n", qatperf_pkt_len, qatperf_duration_ms);

    qatperf_data = (uint8_t *)qc_osal_malloc(qatperf_pkt_len);

    srand(time(NULL));

    for (int i = 0; i < qatperf_pkt_len; i++) {
        qatperf_data[i] = (rand() % 100) + 1;
    }

    uint32_t start = qc_osal_uptime_get_ms();
    uint32_t end = qc_osal_uptime_get_ms();

    while (end - start <= qatperf_duration_ms) {
        atcmd_send(qatperf_data, qatperf_pkt_len);
        qatperf_bytes_sent += qatperf_pkt_len;
        end = qc_osal_uptime_get_ms();
    }

    printf("sent done\r\n");
    printf("duration : %d(ms), take %d ticks\r\n", qatperf_duration_ms, end - start);
    printf("total sent data len : %d(bytes)\r\n", qatperf_bytes_sent);

    speed = qatperf_bytes_sent * 8 * 1000 / qatperf_duration_ms;

    printf("speed : %d(bits/sec)\r\n", speed);

    qc_osal_free(qatperf_data);
}

int test_atcmd_tx(int argc, char **argv)
{
    demo_print("tx test \r\n");
    int cnt = 0, size = 0;
    sscanf(argv[1], "%d", &cnt);
    sscanf(argv[2], "%d", &size);

    atcmd_demo_tx(cnt, size);

    return 0;
}

int test_atcmd_rx_mqtt(int argc, char **argv)
{
    int cnt = 0, len = 0;
    qc_osal_thread_t xHandle = NULL;

    if (qat_demo_parser_enable == 0) {
        demo_print("\r\nParser should be enabled before doing rx mqtt test!\r\n");
        return 0;
    }

    if (strncmp(argv[1], "stop", 4) == 0) {
        demo_print("\r\nMQTT rx test stop\r\n");
        rx_mqtt_test_param->task_started = 0;
    } else {
        demo_print("\r\nMQTT rx test start\r\n");

        memset(rx_mqtt_test_param, 0, sizeof(atcmd_mqtt_rx_test_t));

        sscanf(argv[1], "%d", &rx_mqtt_test_param->num);
        sscanf(argv[2], "%d", &rx_mqtt_test_param->len);

        if (argv[3] != NULL)
            sscanf(argv[3], "%d", &rx_mqtt_test_param->mqtt_recv_timeout);

        struct qc_osal_thread_config config = {.name = "atcmd_demo_rx_mqtt",
                                               .stack_size = 1024,
                                               .priority = 1,
                                               .entry = (qc_osal_thread_entry_t)atcmd_demo_rx_mqtt,
                                               .arg = NULL};

        int ret = qc_osal_thread_create(&xHandle, &config);
        if (ret != 0) {
            printf("Task creation error: Could not allocate required memory\r\n");
        }
    }

    return 0;
}

int test_atcmd_tx_loop(int argc, char **argv)
{
    qc_osal_thread_t xHandle = NULL;
    int index = 1;

    if (argc < 2) {
        demo_print("\nUsage: tx_loop [options]\r\n");
        demo_print("  -i  = Sets the interval time in microseconds between periodic bandwidth\r\n");
        demo_print("  -l = The length of buffers to read or write\r\n");
        demo_print("  -t = The time in seconds to transmit for\r\n");
        demo_print("  -m = The MB number to transmit, no more than 1024MB\r\n");
        demo_print("  -g = The GB number to transmit\r\n");
        demo_print("  stop = stop the transmit test\r\n");
        return 0;
    }

    while (index < argc) {
        if (0 == strcmp(argv[index], "-i")) {
            sscanf(argv[++index], "%d", &tx_test_param->interval);
            index++;
        } else if (0 == strcmp(argv[index], "-l")) {
            sscanf(argv[++index], "%d", &tx_test_param->len);
            index++;
        } else if (0 == strcmp(argv[index], "-t")) {
            sscanf(argv[++index], "%d", &tx_test_param->time);
            tx_test_param->time = tx_test_param->time * 1000;
            index++;
        } else if (0 == strcmp(argv[index], "-m")) {
            sscanf(argv[++index], "%d", &tx_test_param->bytes);
            if (tx_test_param->bytes >= 1024) {
                demo_print("value larger than 1024 is not allowed -m, please use -g instead \r\n");
                return 0;
            }
            tx_test_param->bytes = tx_test_param->bytes * 1024 * 1024;
            index++;
        } else if (0 == strcmp(argv[index], "-g")) {
            sscanf(argv[++index], "%d", &tx_test_param->g_bytes);
            index++;
        } else if (0 == strcmp(argv[index], "stop")) {
            tx_quit = 1;
            rx_quit = 1;
            return 0;
        } else {
            index++;
        }
    }

    struct qc_osal_thread_config config = {.name = "atcmd_demo_tx_loop",
                                           .stack_size = 1024,
                                           .priority = 6,
                                           .entry = (qc_osal_thread_entry_t)atcmd_demo_tx_loop,
                                           .arg = NULL};

    int ret = qc_osal_thread_create(&xHandle, &config);
    if (ret != 0) {
        printf("Task creation error: Could not allocate required memory\r\n");
    }

    return 0;
}

int test_net_tx_loop(int argc, char **argv)
{
    qc_osal_thread_t xHandle = NULL;
    int index = 1;

    if (argc < 2) {
        demo_print("\nUsage: net_tx_loop [options]\r\n");
        demo_print("  prerequisite: DUT must already be in AT+CIPSEND=<link_id>,0 online data mode\r\n");
        demo_print("  -i = interval time in ms between transmissions\r\n");
        demo_print("  -l = payload length to send each time (1..1400)\r\n");
        demo_print("  -t = time in seconds to transmit for\r\n");
        demo_print("  -m = MB number to transmit, no more than 1024MB\r\n");
        demo_print("  -g = GB number to transmit\r\n");
        demo_print("  stop = stop the transmit test\r\n");
        return 0;
    }

    memset(tx_test_param, 0, sizeof(atcmd_tx_test_t));

    while (index < argc) {
        if (0 == strcmp(argv[index], "-i")) {
            sscanf(argv[++index], "%d", &tx_test_param->interval);
            index++;
        } else if (0 == strcmp(argv[index], "-l")) {
            sscanf(argv[++index], "%d", &tx_test_param->len);
            index++;
        } else if (0 == strcmp(argv[index], "-t")) {
            sscanf(argv[++index], "%d", &tx_test_param->time);
            tx_test_param->time = tx_test_param->time * 1000;
            index++;
        } else if (0 == strcmp(argv[index], "-m")) {
            sscanf(argv[++index], "%d", &tx_test_param->bytes);
            if (tx_test_param->bytes >= 1024) {
                demo_print("value larger than 1024 is not allowed -m, please use -g instead \r\n");
                return 0;
            }
            tx_test_param->bytes = tx_test_param->bytes * 1024 * 1024;
            index++;
        } else if (0 == strcmp(argv[index], "-g")) {
            sscanf(argv[++index], "%d", &tx_test_param->g_bytes);
            index++;
        } else if (0 == strcmp(argv[index], "stop")) {
            tx_quit = 1;
            return 0;
        } else {
            index++;
        }
    }

    if (tx_test_param->len == 0) {
        tx_test_param->len = 1400;
    }

    struct qc_osal_thread_config config = {.name = "atcmd_demo_net_tx_loop",
                                           .stack_size = 1024,
                                           .priority = 6,
                                           .entry = (qc_osal_thread_entry_t)atcmd_demo_net_tx_loop,
                                           .arg = NULL};

    int ret = qc_osal_thread_create(&xHandle, &config);
    if (ret != 0) {
        printf("Task creation error: Could not allocate required memory\r\n");
    }

    return 0;
}

/* HTTP test functions - placeholder for now */
int test_send_http_cmd(int index)
{
    /* Implementation from old code */
    return 0;
}

void test_http_process(uint8_t *data, uint32_t len)
{ /* Implementation from old code */
}

int test_http(int argc, char **argv)
{
    int interval = 0;
    http_send_num_max = 10;
    http_mode = -1;

    if (argc > 1) {
        http_mode = atoi(argv[1]);
    }

    if (http_mode == HTTP_TEST_MODEL_CHECK_BIG_DATA_ON_DATA_MODEL) {
        if (argc > 2) {
            interval = atoi(argv[2]);
        }
        if (argc > 3) {
            http_send_num_max = atoi(argv[3]);
        }

        demo_print("httptest test_mode:%d, send_num_max:%d, test_buf len:%d, interval:%dms\r\n",
                   http_mode, http_send_num_max, (int)(strlen(test_buf) - 1), interval);

        for (int i = 0; i < http_send_num_max; i++) {
            demo_print("httptest num:%d\r\n", i + 1);
            qcc730_atcmd_send_handler(test_buf, strlen(test_buf));
            if (i < http_send_num_max - 1) {
                qc_hal_delay(interval);
            }
        }

        http_mode = -1;
    } else {
        demo_print("httptest: unsupported mode %d (only mode 3 supported)\r\n", http_mode);
    }

    return 0;
}

/**
 * @brief BMPS enable/disable command handler
 *
 * @param argc Argument count
 * @param argv Argument values
 * @return 0 on success
 */
int cmd_bmps_enable(int argc, char **argv)
{
    int enable = 0;

    if (argc < 2) {
        printf("Usage: bmps_enable <0|1>\r\n");
        return -1;
    }

    sscanf(argv[1], "%d", &enable);

    if (enable == 0) {
        printf("BMPS disabled, calling qcc730_wkup...\r\n");
        qcc730_wkup();
        while (g_qcc730_sleep_state == QCC730_SLEEP_STATE_SLEEPING)
            ;
        printf("qcc730_wkup completed\r\n");
    } else if (enable == 1) {
        printf("BMPS enabled, sending AT+DEVBUSY=0 to clear device busy\r\n");
        /* Send AT command to clear SPI busy on slave */
        atcmd_send((uint8_t *)"AT+DEVBUSY=0\r", 13);
        g_qcc730_sleep_state = QCC730_SLEEP_STATE_SLEEPING;
        printf("device busy clear command sent\r\n");
    }

    return 0;
}

#ifdef SHELL_FEATURE

void atcmd_add_evt_parser(void)
{
    atcmd_parser_func_add("+EVT:MQTT_SUBRECV", (void *)atcmd_response_parser_EVT_MQTTSUBRECV,
                          "parse EVT_MQTTSUBRECV response", QAT_EVT_TYPE);
    atcmd_parser_func_add("+EVT:MQTT_SUBRECVHEX", (void *)atcmd_response_parser_EVT_MQTTSUBRECVHEX,
                          "parse EVT_MQTTSUBRECVHEX response", QAT_EVT_TYPE);
    atcmd_parser_func_add("+EVT:OTAFWUP_FIN", (void *)atcmd_response_parser_EVT_OTAFWUP_FIN,
                          "parse EVT_OTAFWUP_FIN response", QAT_EVT_TYPE);
    atcmd_parser_func_add("+EVT:lp_presleep", (void *)atcmd_response_parser_EVT_DSLEEP_PRE, "parse DSLEEP response",
                          QAT_EVT_TYPE);
    atcmd_parser_func_add("+EVT:lp_sleepfail", (void *)atcmd_response_parser_EVT_DSLEEP_FAIL, "parse DSLEEP response",
                          QAT_EVT_TYPE);
    atcmd_parser_func_add("+EVT:wakeup", (void *)atcmd_response_parser_EVT_WAKEUP, "parse WAKE UP response",
                          QAT_EVT_TYPE);
}

void atcmd_add_command_parser(void)
{
    atcmd_parser_func_add("+CIPDHCPV4C", (void *)atcmd_response_parser_CIPDHCPV4C, "parse CIPDHCPV4C response",
                          QAT_RESP_TYPE);
    atcmd_parser_func_add("+RDMEM", (void *)atcmd_response_parser_RDMEM, "parse RDMEM response", QAT_RESP_TYPE);
    atcmd_parser_func_add("+IPDHEX", (void *)atcmd_response_parser_IPDHEX, "parse +IPD response", QAT_RESP_TYPE);
    atcmd_parser_func_add("+RST", (void *)atcmd_response_parser_RST, "parse +RST response", QAT_RESP_TYPE);
}

/**
 * @brief Initialize AT command demo
 */
void atcmd_demo_init(void)
{
    /* Initialize parser lists */
    memset(atcmd_parser_resp_func_list, 0, sizeof(atcmd_parser_resp_func_list));
    memset(atcmd_parser_evt_func_list, 0, sizeof(atcmd_parser_evt_func_list));

    /* Register response and event parsers */
    atcmd_add_command_parser();
    atcmd_add_evt_parser();

    /* Allocate test parameters */
    rx_mqtt_test_param = qc_osal_malloc(sizeof(atcmd_mqtt_rx_test_t));
    memset(rx_mqtt_test_param, 0, sizeof(atcmd_mqtt_rx_test_t));

    tx_test_param = qc_osal_malloc(sizeof(atcmd_tx_test_t));
    memset(tx_test_param, 0, sizeof(atcmd_tx_test_t));

    /* Register AT command receive callback */
    int ret = ring_register_callback(atcmd_rx_callback, NULL);
    if (ret < 0) {
        printf("Failed to register AT command callback: %d\r\n", ret);
    } else {
        printf("AT command demo initialized successfully\r\n");
    }
    qapi_atcmd_set_spi_state(QCC730_SPI_READY);
}

/**
 * @brief Register AT command shell commands
 */
void atcmd_demo_register_commands(void)
{
    cmd_shell_add("help", (void *)atcmd_help, "AT Command help!");
    cmd_shell_add("parser", (void *)atcmd_enable_parser, "Enable/disable AT response parser");
    cmd_shell_add("qatperf", (void *)atcmd_qat_perf, "performance test");
    cmd_shell_add("tx", (void *)test_atcmd_tx, "tx");
    cmd_shell_add("rx_mqtt", (void *)test_atcmd_rx_mqtt, "rx_mqtt");
    cmd_shell_add("tx_loop", (void *)test_atcmd_tx_loop, "tx stress test");
    cmd_shell_add("net_tx_loop", (void *)test_net_tx_loop, "network online-data throughput test");
    cmd_shell_add("httptest", (void *)test_http, "http test");
    cmd_shell_add("bmps_enable", (void *)cmd_bmps_enable, "Enable/disable BMPS (0=disable/wakeup, 1=enable)");
}

#endif /* SHELL_FEATURE */

/* ============================================================================
 * SPI State Management Functions
 * ============================================================================ */

/**
 * @brief Get SPI state
 *
 * @return Current SPI state (QCC730_SPI_NOT_READY or QCC730_SPI_READY)
 */
int qapi_atcmd_get_spi_state(void)
{
    if (qcc730_atcmd == NULL) {
        return QCC730_SPI_NOT_READY;
    }
    return qcc730_atcmd->spi_state;
}

/**
 * @brief Set SPI state
 *
 * @param state New SPI state (QCC730_SPI_NOT_READY or QCC730_SPI_READY)
 * @return 0 on success, negative error code on failure
 */
int qapi_atcmd_set_spi_state(int state)
{
    if (qcc730_atcmd == NULL) {
        return -QC_OSAL_EINVAL;
    }

    if (state != QCC730_SPI_NOT_READY && state != QCC730_SPI_READY) {
        return -QC_OSAL_EINVAL;
    }

    qcc730_atcmd->spi_state = state;
    printf("SPI state changed to: %s\r\n", state == QCC730_SPI_READY ? "READY" : "NOT_READY");

    return 0;
}

/**
 * @brief Get AT command context pointer
 *
 * @return Pointer to qcc730_atcmd_t structure
 */
qcc730_atcmd_t *qapi_atcmd_get_context(void) { return qcc730_atcmd; }

#endif /* QC_OS_FREERTOS && QCC730_ATCMD_ENABLE */
