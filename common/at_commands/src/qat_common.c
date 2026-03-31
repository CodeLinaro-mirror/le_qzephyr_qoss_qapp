 /*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/device.h>
#include <zephyr/pm/pm.h>
#include <zephyr/pm/device.h>
#include <stdio.h>
#include <string.h>
#include <cat.h>
#include "qat_api.h"
#include "wifi_fw_version.h"
#include "qapi_version.h"
#include <nt_sys_monitoring.h>
#include "qpower.h"

LOG_MODULE_REGISTER(qat_common, LOG_LEVEL_INF);

/* External SPI wakeup flag functions */
#ifdef CONFIG_PM_DEVICE
extern bool spi_is_ext_wakeup(void);
extern void spi_clear_ext_wakeup_flag(void);
#endif

/* Test memory variable */
static uint32_t mem_test = 0x12345678;

/*-------------------------------------------------------------------------
 * AT+GMR - Get SDK version information
 *-----------------------------------------------------------------------*/

/* AT+GMR */
static cat_return_state Extend_Command_Version_Exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK, "AT+GMR: get usage of command\r\n"
                                       "AT+GMR?: get SDK version\r\n");
}

static cat_return_state Extend_Command_Version_Query(const struct cat_command *cmd, uint8_t *data, size_t *data_size,
                                                     const size_t max_data_size)
{
    char response[QAT_RESPONSE_BUF_SIZE];

    /* Read OTP and version information from hardware registers */
    unsigned int otp_version = *(unsigned int *)0x1a002c;
    unsigned int PBL_version = *(unsigned int *)0x200168;
    unsigned int kdf_lock = *(unsigned int *)0x1a0090;
    unsigned int CUID_0 = *(unsigned int *)0x1a0004;
    unsigned short CUID_1 = *(unsigned short *)0x1a0008;

    /* Format version string with all hardware and software version info */
    snprintf(response, sizeof(response), "+GMR:%d.%d.%d,%s,%d.%d.%d,%s,%s,%d.%d,%d.%d.%d,0x%x,0x%x,%x,%s-%s\r\n",
             QAPI_VERSION_MAJOR, QAPI_VERSION_MINOR, QAPI_VERSION_NIT, CONFIG_QCC730_CRM_NUMBER, WIFI_FW_VER_MAJOR,
             WIFI_FW_VER_MINOR, WIFI_FW_VER_COUNT, WIFI_FW_VARIANT_NAME, CONFIG_BOARD, ((otp_version >> 24) & 0xff),
             (((otp_version >> 16) & 0xff)), ((PBL_version >> 24) & 0xff), (((PBL_version >> 16) & 0xff)),
             (((PBL_version >> 0) & 0xffff)), ((kdf_lock >> 8) & 0xff), CUID_1, CUID_0, __DATE__, __TIME__);

    return QAT_Response_Str(QAT_RC_OK, response);
}

/*-------------------------------------------------------------------------
 * AT+INFO - Get board information
 *-----------------------------------------------------------------------*/
static cat_return_state Extend_Command_Info_Exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(
        QAT_RC_OK, "AT+INFO: get usage of command\r\n"
                   "AT+INFO?: get board information, including temparature, battery voltage, and memory usage\r\n");
}

static cat_return_state Extend_Command_Info_Query(const struct cat_command *cmd, uint8_t *data, size_t *data_size,
                                                  const size_t max_data_size)
{
    char response[QAT_RESPONSE_BUF_SIZE];

    /* Get temperature from PMU */
    extern int pmu_ts_get_current_temperature(void);
    int temperature = pmu_ts_get_current_temperature();

    /* Get battery voltage */
    extern int tv_monitor_get_vbat_mV(void);
    int vbat_mv = tv_monitor_get_vbat_mV();

    /* Get heap statistics using Zephyr sys_heap API */
    uint32_t total_bytes = 0;
    uint32_t used_bytes = 0;
    uint32_t free_bytes = 0;
    uint32_t min_free_bytes = 0;

#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
    extern struct sys_heap *get_malloc_heap_address(void);
    struct sys_heap *malloc_heap = get_malloc_heap_address();

    if (malloc_heap) {
        struct sys_memory_stats stats;
        int err = sys_heap_runtime_stats_get(malloc_heap, &stats);
        if (err == 0) {
            free_bytes = stats.free_bytes;
            used_bytes = stats.allocated_bytes;
            total_bytes = free_bytes + used_bytes;
            min_free_bytes = total_bytes - stats.max_allocated_bytes;
        }
    }
#endif

    /* Format: +INFO:<temp>,<vbat_mV>,<total>,<used>,<free>,<min_free> */
    snprintf(response, sizeof(response), "+INFO:%d,%d,%u,%u,%u,%u\r\n", temperature, vbat_mv, total_bytes, used_bytes,
             free_bytes, min_free_bytes);

    return QAT_Response_Str(QAT_RC_OK, response);
}

/*-------------------------------------------------------------------------
 * AT+RST - System reset
 *-----------------------------------------------------------------------*/

/* Work item for delayed reset */
static void reset_work_handler(struct k_work *work)
{
    LOG_DBG("Executing system reset");
    nt_system_sw_reset();
}

static K_WORK_DELAYABLE_DEFINE(reset_work, reset_work_handler);

static cat_return_state Extend_Command_Reset_Exec(const struct cat_command *cmd)
{
    LOG_DBG("System reset requested, scheduling reset in 100ms");

    /* Schedule reset after 100ms to allow OK response to be sent */
    k_work_schedule(&reset_work, K_MSEC(100));

    return QAT_Response_Str(QAT_RC_OK, "+RST\r\n");
}

/*-------------------------------------------------------------------------
 * AT+WRTMEM - Write memory
 *-----------------------------------------------------------------------*/

static cat_return_state Extend_Command_Write_Memory_Exec(const struct cat_command *cmd)
{
    char response[QAT_RESPONSE_BUF_SIZE];
    snprintf(response, QAT_RESPONSE_BUF_SIZE, "+WRTMEM: <addr>,<size:1|2|4>,<value>\r\nmem: 0x%08x for test\r\n",
             &mem_test);

    return QAT_Response_Str(QAT_RC_OK, response);
}

static cat_return_state Extend_Command_Write_Memory_Set(const struct cat_command *cmd, const uint8_t *data,
                                                        const size_t data_size, const size_t args_num)
{
    uint32_t addr, size, value;

    /* Parse parameters */
    if (sscanf((char *)data, "%x,%u,%u", &addr, &size, &value) != 3) {
        return QAT_Response_Str(QAT_RC_ERROR, NULL);
    }

    /* Write memory based on size */
    if (size == 1) {
        *(volatile uint8_t *)addr = (uint8_t)value;
    } else if (size == 2) {
        *(volatile uint16_t *)addr = (uint16_t)value;
    } else if (size == 4) {
        *(volatile uint32_t *)addr = value;
    } else {
        return QAT_Response_Str(QAT_RC_ERROR, NULL);
    }

    LOG_DBG("Write memory: addr=0x%08x, size=%u, value=0x%x\r\n", addr, size, value);
    return QAT_Response_Str(QAT_RC_OK, NULL);
}

/*-------------------------------------------------------------------------
 * AT+RDMEM - Read memory
 *-----------------------------------------------------------------------*/

static cat_return_state Extend_Command_Read_Memory_Exec(const struct cat_command *cmd)
{
    char response[QAT_RESPONSE_BUF_SIZE];
    snprintf(response, QAT_RESPONSE_BUF_SIZE, "+RDMEM: <addr>,<size:1|2|4>\r\nmem: 0x%08x for test\r\n", &mem_test);

    return QAT_Response_Str(QAT_RC_OK, response);
}

static cat_return_state Extend_Command_Read_Memory_Set(const struct cat_command *cmd, const uint8_t *data,
                                                       const size_t data_size, const size_t args_num)
{
    uint32_t addr, size, value;
    char response[QAT_RESPONSE_BUF_SIZE];

    /* Parse parameters */
    if (sscanf((char *)data, "%x,%u", &addr, &size) != 2) {
        return QAT_Response_Str(QAT_RC_ERROR, NULL);
    }

    /* Validate size parameter */
    if (size != 1 && size != 2 && size != 4) {
        return QAT_Response_Str(QAT_RC_ERROR, NULL);
    }

    /* Read memory based on size */
    if (size == 1) {
        value = *(volatile uint8_t *)addr;
    } else if (size == 2) {
        value = *(volatile uint16_t *)addr;
    } else if (size == 4) {
        value = *(volatile uint32_t *)addr;
    } else {
        return QAT_Response_Str(QAT_RC_ERROR, NULL);
    }

    /* Format response: +RDMEM:<addr>,<size>,<data_hex>,<data_dec>
     * NOTE: Do NOT include trailing \r\n — QAT_Response_Str adds framing */
    snprintf(response, sizeof(response), "+RDMEM:0x%08x,%u,0x%0*x,%u", addr, size, (int)(size * 2), value, value);

    LOG_DBG("Read memory: addr=0x%08x, size=%u, value=0x%x (%u)\r\n", addr, size, value, value);

    return QAT_Response_Str(QAT_RC_OK, response);
}

/*-------------------------------------------------------------------------
 * AT+TIME - Set/Get RTC time
 *-----------------------------------------------------------------------*/

static cat_return_state Extend_Command_TIME_Exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK,
                            "AT+TIME: get usage of command\r\n"
                            "AT+TIME?: get RTC time\r\n"
                            "AT+TIME=<year[1980-2100]>,<month[1-12]>,<day[1-31]>,<hour[0-23]>,<minute[0-59]>,<"
                            "second[0-59]>,<day_Of_Week[0-6]>: set RTC time\r\n");
}

static cat_return_state Extend_Command_TIME_Set(const struct cat_command *cmd, const uint8_t *data,
                                                const size_t data_size, const size_t args_num)
{
    char response[QAT_RESPONSE_BUF_SIZE];
    struct rtc_time tm = {0};
    int year, month, day, hour, minute, second, day_of_week;
    int ret;

    /* Get RTC device */
    const struct device *rtc_dev = DEVICE_DT_GET(DT_ALIAS(rtc));

    if (!device_is_ready(rtc_dev)) {
        return QAT_Response_Str(QAT_RC_ERROR, "+TIME:RTC device not ready\r\n");
    }

    /* Parse parameters: year,month,day,hour,minute,second,day_of_week */
    if (sscanf((char *)data, "%d,%d,%d,%d,%d,%d,%d", &year, &month, &day, &hour, &minute, &second, &day_of_week) != 7) {
        return QAT_Response_Str(QAT_RC_ERROR,
                                "AT+TIME=<year[1980-2100]>,<month[1-12]>,<day[1-31]>,<hour[0-23]>,<minute[0-59]"
                                ">,<second[0-59]>,<day_Of_Week[0-6]>: set RTC time\r\n");
    }

    /* Validate year (1980-2100) */
    if (year < 1980 || year > 2100) {
        return QAT_Response_Str(QAT_RC_ERROR, "+TIME:Invalid year\r\n");
    }

    /* Validate month (1-12) */
    if (month < 1 || month > 12) {
        return QAT_Response_Str(QAT_RC_ERROR, "+TIME:Invalid month\r\n");
    }

    /* Validate day (1-31) */
    if (day < 1 || day > 31) {
        return QAT_Response_Str(QAT_RC_ERROR, "+TIME:Invalid day\r\n");
    }

    /* Validate hour (0-23) */
    if (hour < 0 || hour > 23) {
        return QAT_Response_Str(QAT_RC_ERROR, "+TIME:Invalid hour\r\n");
    }

    /* Validate minute (0-59) */
    if (minute < 0 || minute > 59) {
        return QAT_Response_Str(QAT_RC_ERROR, "+TIME:Invalid minute\r\n");
    }

    /* Validate second (0-59) */
    if (second < 0 || second > 59) {
        return QAT_Response_Str(QAT_RC_ERROR, "+TIME:Invalid second\r\n");
    }

    /* Validate day_of_week (0-6) */
    if (day_of_week < 0 || day_of_week > 6) {
        return QAT_Response_Str(QAT_RC_ERROR, "+TIME:Invalid day_Of_Week\r\n");
    }

    /* Convert to Zephyr RTC time format
     * Note: Zephyr uses tm_year = year - 1900, tm_mon = month - 1 */
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_wday = day_of_week;
    tm.tm_yday = -1;  /* Unknown */
    tm.tm_isdst = -1; /* Unknown */
    tm.tm_nsec = 0;   /* Unknown */

    /* Set RTC time */
    ret = rtc_set_time(rtc_dev, &tm);
    if (ret != 0) {
        snprintf(response, sizeof(response), "+TIME:FAIL,%d\r\n", ret);
        return QAT_Response_Str(QAT_RC_ERROR, response);
    }

    LOG_DBG("RTC time set: %04d-%02d-%02d %02d:%02d:%02d (wday=%d)", year, month, day, hour, minute, second,
            day_of_week);

    return QAT_Response_Str(QAT_RC_OK, NULL);
}

static cat_return_state Extend_Command_TIME_Query(const struct cat_command *cmd, uint8_t *data, size_t *data_size,
                                                  const size_t max_data_size)
{
    char response[QAT_RESPONSE_BUF_SIZE];
    struct rtc_time tm = {0};
    int ret;

    /* Get RTC device */
    const struct device *rtc_dev = DEVICE_DT_GET(DT_ALIAS(rtc));

    if (!device_is_ready(rtc_dev)) {
        return QAT_Response_Str(QAT_RC_ERROR, "+TIME:RTC device not ready\r\n");
    }

    /* Get RTC time */
    ret = rtc_get_time(rtc_dev, &tm);
    if (ret != 0) {
        if (ret == -ENODATA) {
            return QAT_Response_Str(QAT_RC_ERROR, "+TIME:Failed to get time. It would because time is not set\r\n");
        }
        snprintf(response, sizeof(response), "+TIME:Failed to get time, error=%d\r\n", ret);
        return QAT_Response_Str(QAT_RC_ERROR, response);
    }

    /* Convert from Zephyr RTC format to user format
     * Note: Zephyr uses tm_year = year - 1900, tm_mon = month - 1 */
    int year = tm.tm_year + 1900;
    int month = tm.tm_mon + 1;

    /* Format response: +TIME:year,month,day,hour,minute,second,day_of_week
     * Note: Original firmware also included timezone, but we'll keep it simple for now */
    snprintf(response, sizeof(response), "+TIME:%d,%d,%d,%d,%d,%d,%d", year, month, tm.tm_mday, tm.tm_hour, tm.tm_min,
             tm.tm_sec, tm.tm_wday);

    LOG_DBG("RTC time get: %04d-%02d-%02d %02d:%02d:%02d (wday=%d)", year, month, tm.tm_mday, tm.tm_hour, tm.tm_min,
            tm.tm_sec, tm.tm_wday);

    return QAT_Response_Str(QAT_RC_OK, response);
}

/*-------------------------------------------------------------------------
 * AT+DSLEEP - Set system to sleep
 *-----------------------------------------------------------------------*/

/* Sleep duration storage for work handler */
static uint32_t sleep_duration_ms = 0;

/* Work item for delayed deep sleep */
static void deep_sleep_work_handler(struct k_work *work)
{
    LOG_DBG("Executing deep sleep for %u ms", sleep_duration_ms);

    qapi_power_set_parameter(__QAPI_POWER_SOFTOFF_DURATION_MS, sleep_duration_ms);

    if (!pm_state_force(0u, &(struct pm_state_info){PM_STATE_SOFT_OFF, 0, 0})) {
        QAT_Response_Str(QAT_RC_QUIET, "+EVT:lp_sleepfail\r\n");
    }
}

static K_WORK_DELAYABLE_DEFINE(deep_sleep_work, deep_sleep_work_handler);

static cat_return_state Extend_Command_Deep_Sleep_Exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK, "+DSLEEP=<sleep duration in ms>\r\n");
}

static cat_return_state Extend_Command_Deep_Sleep_Set(const struct cat_command *cmd, const uint8_t *data,
                                                      const size_t data_size, const size_t args_num)
{
    /* Parse parameters: sleep duration in milliseconds */
    if (sscanf((char *)data, "%u", &sleep_duration_ms) != 1) {
        return QAT_Response_Str(QAT_RC_ERROR, "+DSLEEP=<sleep duration in ms>\r\n");
    }

    LOG_DBG("Deep sleep requested for %u ms, scheduling in 100ms", sleep_duration_ms);

    /* Schedule deep sleep after 100ms to allow OK response to be sent */
    k_work_schedule(&deep_sleep_work, K_MSEC(100));

    return QAT_Response_Str(QAT_RC_OK, "+EVT:lp_presleep\r\n");
}

/*-------------------------------------------------------------------------
 * AT+CMD - List available commands
 *-----------------------------------------------------------------------*/

static cat_return_state Extend_Command_Cmd_Exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK, "AT+CMD: get usage of command\r\n"
                                       "AT+CMD?: get available AT commands\r\n");
}

static cat_return_state Extend_Command_Cmd_Query(const struct cat_command *cmd, uint8_t *data, size_t *data_size,
                                                 const size_t max_data_size)
{
    struct cat_command_group **groups;
    uint8_t group_count;
    char response[QAT_RESPONSE_BUF_SIZE];
    int total_cmd = 0;
    int ret;

    /* Get registered command groups */
    ret = qat_get_cmd_groups(&groups, &group_count);
    if (ret != 0) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CMD:Failed to get command groups\r\n");
    }

    /* Iterate through all command groups and list commands */
    for (int g = 0; g < group_count; g++) {
        struct cat_command_group *group = groups[g];

        for (int i = 0; i < group->cmd_num; i++) {
            const struct cat_command *command = &group->cmd[i];
            int has_read = (command->read != NULL) ? 1 : 0;
            int has_run = (command->run != NULL) ? 1 : 0;
            int has_write = (command->write != NULL) ? 1 : 0;

            /* Format: +CMD:<index>,"<AT command name>",<support query>,<support execute>,<support set> */
            snprintf(response, sizeof(response), "+CMD:%d,\"%s\",%d,%d,%d\r\n", total_cmd, command->name, has_read,
                     has_run, has_write);

            /* Send immediately using QUIET mode (no OK/ERROR appended) */
            QAT_Response_Str(QAT_RC_QUIET, response);

            total_cmd++;
        }
    }

    /* Return OK after all commands have been sent */
    return QAT_Response_Str(QAT_RC_OK, NULL);
}

/*-------------------------------------------------------------------------
 * AT+DEVBUSY - Set/Clear device busy
 *-----------------------------------------------------------------------*/

/* Work item for delayed device busy clear */
static void busy_clear_work_handler(struct k_work *work)
{
    const struct device *spi_dev = DEVICE_DT_GET(DT_NODELABEL(qcspi));
    pm_device_busy_clear(spi_dev);
}

static K_WORK_DELAYABLE_DEFINE(busy_clear_work, busy_clear_work_handler);

static cat_return_state Extend_Command_DEV_Busy_Set(const struct cat_command *cmd, const uint8_t *data,
                                                     const size_t data_size, const size_t args_num)
{
    int enable;
    const struct device *spi_dev = DEVICE_DT_GET(DT_NODELABEL(qcspi));
    
    if (sscanf((char *)data, "%d", &enable) != 1) {
        return QAT_Response_Str(QAT_RC_ERROR, NULL);
    }
    
    if (!device_is_ready(spi_dev)) {
        return QAT_Response_Str(QAT_RC_ERROR, "+DEVBUSY:Device not ready\r\n");
    }
    
    if (enable == 0) {
        /* Clear SPI busy */
        k_work_schedule(&busy_clear_work, K_MSEC(100));
        LOG_DBG("SPI busy cleared");
    } else {
        /* Set SPI busy */
        pm_device_busy_set(spi_dev);
        LOG_DBG("SPI busy set");
    }
    
    return QAT_Response_Str(QAT_RC_OK, NULL);
}

/*-------------------------------------------------------------------------
 * Command List
 *-----------------------------------------------------------------------*/

static struct cat_command qat_common_cmds[] = {{
                                                   .name = "+CMD",
                                                   .description = "List available commands",
                                                   .run = Extend_Command_Cmd_Exec,
                                                   .read = Extend_Command_Cmd_Query,
                                               },
                                               {
                                                   .name = "+GMR",
                                                   .description = "Get SDK version",
                                                   .run = Extend_Command_Version_Exec,
                                                   .read = Extend_Command_Version_Query,
                                               },
                                               {
                                                   .name = "+INFO",
                                                   .description = "Get board information",
                                                   .run = Extend_Command_Info_Exec,
                                                   .read = Extend_Command_Info_Query,
                                               },
                                               {
                                                   .name = "+RST",
                                                   .description = "System reset",
                                                   .run = Extend_Command_Reset_Exec,
                                               },
                                               {
                                                   .name = "+WRTMEM",
                                                   .description = "Write memory",
                                                   .run = Extend_Command_Write_Memory_Exec,
                                                   .write = Extend_Command_Write_Memory_Set,
                                               },
                                               {
                                                   .name = "+RDMEM",
                                                   .description = "Read memory",
                                                   .run = Extend_Command_Read_Memory_Exec,
                                                   .write = Extend_Command_Read_Memory_Set,
                                               },
                                               {
                                                   .name = "+TIME",
                                                   .description = "Set/Get RTC time",
                                                   .run = Extend_Command_TIME_Exec,
                                                   .read = Extend_Command_TIME_Query,
                                                   .write = Extend_Command_TIME_Set,
                                               },
                                               {
                                                   .name = "+DSLEEP",
                                                   .description = "Set system to sleep",
                                                   .run = Extend_Command_Deep_Sleep_Exec,
                                                   .write = Extend_Command_Deep_Sleep_Set,
                                               },
                                               {
                                                   .name = "+DEVBUSY",
                                                   .description = "Set device to busy",
                                                   .write = Extend_Command_DEV_Busy_Set,
                                               }};

static struct cat_command_group qat_common_cmd_group = {
    .name = "QAT_COMMON",
    .cmd = qat_common_cmds,
    .cmd_num = ARRAY_SIZE(qat_common_cmds),
};

struct cat_command_group *qat_common_get_command_group(void)
{
    LOG_INF("Registering QAT common commands");
    return &qat_common_cmd_group;
}

/* Automatically register this command group with QAT */
QAT_REGISTER_CMD_GROUP(qat_common_get_command_group, "COMMON");

/*-------------------------------------------------------------------------
 * PM Notifier for SPI wakeup management
 *-----------------------------------------------------------------------*/

#ifdef CONFIG_PM_DEVICE
/* PM notifier for SPI wakeup management */
static struct pm_notifier qat_pm_notifier;

/**
 * @brief PM state entry callback
 * 
 * Called before system enters sleep state
 */
static void qat_notify_pm_state_entry(enum pm_state state)
{
    ARG_UNUSED(state);
    return;
}

/**
 * @brief PM state exit callback
 * 
 * Called after system exits sleep state
 * Handles SPI busy management and sends wakeup event to Host
 */
static void qat_notify_pm_state_exit(enum pm_state state)
{
    const struct device *spi_dev = DEVICE_DT_GET(DT_NODELABEL(qcspi));
    
    switch (state) {
    case PM_STATE_SUSPEND_TO_RAM:
    case PM_STATE_SOFT_OFF:
        
        /* Check if wakeup was triggered by external wake up pin */
        if (spi_is_ext_wakeup()) {
            /* Set SPI busy to give Host time to send control commands */
            if (device_is_ready(spi_dev)) {
                pm_device_busy_set(spi_dev);
                LOG_DBG("QAT: SPI busy set after external pin wakeup");
            }
            spi_clear_ext_wakeup_flag();
            
            /* Notify Host that device has woken up */
            QAT_Response_Str(QAT_RC_QUIET, "+EVT:wakeup\r\n");

        } else {
            LOG_DBG("QAT: Wakeup not from external pin, skip notification");
        }
        break;
    default:
        break;
    }
}

/**
 * @brief Initialize PM notifier
 * 
 * Called during system initialization to register PM callbacks
 */
static int qat_pm_notifier_init(void)
{
    qat_pm_notifier.state_entry = qat_notify_pm_state_entry;
    qat_pm_notifier.state_exit = qat_notify_pm_state_exit;

    pm_notifier_register(&qat_pm_notifier);

    LOG_INF("QAT PM notifier registered successfully");
    return 0;
}

SYS_INIT(qat_pm_notifier_init, POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* CONFIG_PM_DEVICE */
