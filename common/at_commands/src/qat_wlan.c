/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/pm/pm.h>
#include <zephyr/pm/policy.h>
#include <zephyr/pm/device.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <cat.h>
#include "qat_api.h"
#include <qcom_wifi_mgmt.h>
#include <zephyr/net/net_ip.h>
#ifdef CONFIG_QAT_WIFI_CRED
#include <zephyr/fs/fs.h>
#endif

LOG_MODULE_REGISTER(qat_wlan, LOG_LEVEL_DBG);

/*-------------------------------------------------------------------------
 * Definitions
 *-----------------------------------------------------------------------*/
#define WLAN_RESPONSE_BUFFER_LENGTH 128
#define WLAN_STR_BUFFER_LENGTH      1500

/* BCMC filter configuration (AT command managed) */
#define AT_BCMC_WHITELIST_LEN   4
#define AT_WIFI_MAC_HDR_LEN     24
#define AT_LLC_SNAP_HDR_LEN     8

/**
Enumeration that identifies the device concurrency mode.
*/
typedef enum {
    DEV_MODE_STATION_E = 0x01, /**< Station mode */
    DEV_MODE_AP_E = 0x10,      /**< SoftAP mode */
    DEV_MODE_AP_STA_E = 0x11,  /**< AP_STA Concurrency */
    DEV_MODE_NO_CONC_E,        /**< Concurrency Off. */
} qat_WLAN_DEV_Mode_e;


typedef struct {
    struct k_mutex mutex;
    bool connected;
    bool wlan_enabled;
    char ssid[WIFI_SSID_MAX_LEN + 1];
    uint8_t ssid_len;
    uint16_t channel;
    struct net_if *iface;
    bool ap_active;               /* SoftAP is currently enabled */
    char op_mode;
    /* Scan tracking */
    uint16_t scan_result_count;
    bool scan_in_progress;
    /* Security parameters - stored from +CWWPA and +CWPWD */
    uint32_t auth_mode;  /* From +CWWPA */
    uint32_t cipher;     /* From +CWWPA */
    char passphrase[65]; /* From +CWPWD */
    uint8_t passphrase_len;
    bool security_set;   /* Flag to indicate if security params are set */
} wifi_context_t;

/*-------------------------------------------------------------------------
 * Global Variables
 *-----------------------------------------------------------------------*/
static wifi_context_t g_wifi_ctx = {
    .op_mode = DEV_MODE_STATION_E,
};
static bool enable_event_reporting = true;
static uint32_t at_udp_whitelist_arr[AT_BCMC_WHITELIST_LEN] = {7777, 0, 0, 0};

/*-------------------------------------------------------------------------
 * Forward Declarations
 *-----------------------------------------------------------------------*/
static cat_return_state cmd_wlan_wifisp_exec(const struct cat_command *cmd);
static cat_return_state cmd_wlan_enable_exec(const struct cat_command *cmd);
static cat_return_state cmd_wlan_disable_exec(const struct cat_command *cmd);
static cat_return_state cmd_wlan_scan_exec(const struct cat_command *cmd);
static cat_return_state cmd_wlan_scan_set(const struct cat_command *cmd, const uint8_t *data,
                                          const size_t data_size, const size_t args_num);
static cat_return_state cmd_wlan_connect_exec(const struct cat_command *cmd);
static cat_return_state cmd_wlan_connect_set(const struct cat_command *cmd, const uint8_t *data,
                                             const size_t data_size, const size_t args_num);
static cat_return_state cmd_wlan_connect_query(const struct cat_command *cmd, uint8_t *data,
                                               size_t *data_size, const size_t max_data_size);
static cat_return_state cmd_wlan_disconnect_exec(const struct cat_command *cmd);
static cat_return_state cmd_wlan_ap_enable_set(const struct cat_command *cmd, const uint8_t *data,
                                               const size_t data_size, const size_t args_num);
static cat_return_state cmd_wlan_ap_enable_exec(const struct cat_command *cmd);
static cat_return_state cmd_wlan_reg_domain_set(const struct cat_command *cmd, const uint8_t *data,
                                                const size_t data_size, const size_t args_num);
static cat_return_state cmd_wlan_reg_domain_query(const struct cat_command *cmd, uint8_t *data,
                                                  size_t *data_size, const size_t max_data_size);
static cat_return_state cmd_wlan_wpa_params_set(const struct cat_command *cmd, const uint8_t *data,
                                                const size_t data_size, const size_t args_num);
static cat_return_state cmd_wlan_wpa_params_exec(const struct cat_command *cmd);
static cat_return_state cmd_wlan_wpa_passphrase_set(const struct cat_command *cmd, const uint8_t *data,
                                                    const size_t data_size, const size_t args_num);
static cat_return_state cmd_wlan_wpa_passphrase_exec(const struct cat_command *cmd);
static cat_return_state cmd_wlan_mode_set(const struct cat_command *cmd, const uint8_t *data,
                                          const size_t data_size, const size_t args_num);
static cat_return_state cmd_wlan_mode_query(const struct cat_command *cmd, uint8_t *data,
                                            size_t *data_size, const size_t max_data_size);
static cat_return_state cmd_wlan_mode_exec(const struct cat_command *cmd);
static cat_return_state cmd_wlan_phy_mode_exec(const struct cat_command *cmd);
static cat_return_state cmd_wlan_antiinf_exec(const struct cat_command *cmd);
static cat_return_state cmd_wlan_edca_exec(const struct cat_command *cmd);
static cat_return_state cmd_wlan_edcca_exec(const struct cat_command *cmd);
static cat_return_state cmd_wlan_bmiss_exec(const struct cat_command *cmd);
static cat_return_state cmd_ps_set(const struct cat_command *cmd, const uint8_t *data,
                                          const size_t data_size, const size_t args_num);
static cat_return_state cmd_ps_exec(const struct cat_command *cmd);
static cat_return_state cmd_ps_wlan_inactivity_time_exec(const struct cat_command *cmd);
static cat_return_state cmd_ps_wlan_inactivity_time_set(const struct cat_command *cmd, const uint8_t *data,
                                                   const size_t data_size, const size_t args_num);
static cat_return_state cmd_ps_wlan_ignore_bcmc_exec(const struct cat_command *cmd);
static cat_return_state cmd_ps_wlan_ignore_bcmc_set(const struct cat_command *cmd, const uint8_t *data,
                                                      const size_t data_size, const size_t args_num);
static cat_return_state cmd_ps_wlan_bcmc_filter_exec(const struct cat_command *cmd);
static cat_return_state cmd_ps_wlan_bcmc_filter_set(const struct cat_command *cmd, const uint8_t *data,
                                                 const size_t data_size, const size_t args_num);
static cat_return_state cmd_ps_wlan_bcmc_list_exec(const struct cat_command *cmd);
static cat_return_state cmd_ps_wlan_bcmc_list_set(const struct cat_command *cmd, const uint8_t *data,
                                               const size_t data_size, const size_t args_num);
static cat_return_state cmd_ps_wlan_bcmc_list_query(const struct cat_command *cmd, uint8_t *data,
                                                 size_t *data_size, const size_t max_data_size);
static cat_return_state cmd_wlan_listen_interval_exec(const struct cat_command *cmd);
static cat_return_state cmd_wlan_listen_interval_set(const struct cat_command *cmd, const uint8_t *data,
                                                    const size_t data_size, const size_t args_num);
static cat_return_state cmd_wlan_listen_interval_query(const struct cat_command *cmd, uint8_t *data,
                                                      size_t *data_size, const size_t max_data_size);

#ifdef CONFIG_QAT_WIFI_CRED
typedef enum {
    WIFI_CRED_OK         =  0,
    WIFI_CRED_ERR_NOFILE = -1,  /* fs_open failed — no saved file */
    WIFI_CRED_ERR_IO     = -2,  /* short read */
    WIFI_CRED_ERR_MAGIC  = -3,  /* bad magic or version */
    WIFI_CRED_ERR_LEN    = -4,  /* ssid/passphrase length out of range */
} wifi_cred_err_t;

static wifi_cred_err_t load_wifi_cred_from_flash(void);
#endif /* CONFIG_QAT_WIFI_CRED */

/*-------------------------------------------------------------------------
 * WiFi Event Handler
 *-----------------------------------------------------------------------*/
static struct net_mgmt_event_callback wifi_mgmt_cb;
static struct net_mgmt_event_callback net_mgmt_cb;
static bool callbacks_registered = false;

/* Work queue for delayed scan complete event */
static struct k_work_delayable scan_complete_work;

/*
 * DHCP work contexts - each embeds the target iface so that start and stop
 * always operate on the same interface and there is no shared global state.
 * This is important in concurrent (STA+AP) mode where two interfaces exist.
 */
struct dhcp_start_ctx {
	struct k_work_delayable work;
	struct net_if *iface;
};

struct dhcp_stop_ctx {
	struct k_work work;
	struct net_if *iface;
};

static struct dhcp_start_ctx dhcp_start_work;
static struct dhcp_stop_ctx  dhcp_stop_work;

/* power save timeout timer */
static void ps_timeout_callback(struct k_timer *timer);
K_TIMER_DEFINE(ps_timeout_timer, ps_timeout_callback, NULL);

void qat_ps_exit(void);


/**
 * @brief power save timeout callback - disables power save and acquires PM lock
 *
 * This callback is invoked when the power save timeout timer expires. It:
 * 1. Disables power save mode
 * 2. Acquires the PM lock to prevent system from entering low power state
 * 3. Stops the timer
 */
/**
 * Execute AT+PS=0 actions: stop timer, disable power save and RX filter,
 * acquire PM lock, send "+PS: exit." notification.
 * Called from ps_timeout_callback and qat_notify_pm_state_exit.
 */
void qat_ps_exit(void)
{
    struct qcom_wifi_pm_bmps_params bmps_params;
    int ret;

    k_timer_stop(&ps_timeout_timer);

    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
    if (!g_wifi_ctx.wlan_enabled || !g_wifi_ctx.iface) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return;
    }
    
    /* Disable power save */
    bmps_params.enable = 0;
    ret = net_mgmt(NET_REQUEST_WIFI_PM_QCOM_SET_BMPS_ENABLE, g_wifi_ctx.iface,
                   &bmps_params, sizeof(bmps_params));
    k_mutex_unlock(&g_wifi_ctx.mutex);
    if (ret) {
        LOG_ERR("Failed to disable power save on PS exit: %d", ret);
    }

    /* Acquire PM lock to prevent re-entering low power state */
    pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_RAM, PM_ALL_SUBSTATES);
    LOG_INF("Power save exited, PM lock acquired");

    QAT_Response_Str(QAT_RC_QUIET, "+PS: exit.\r\n");
}

static void ps_timeout_callback(struct k_timer *timer)
{
    LOG_INF("Power save timeout expired");
    qat_ps_exit();
}

static void dhcp_start_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct dhcp_start_ctx *ctx = CONTAINER_OF(dwork, struct dhcp_start_ctx, work);
	struct net_if *iface = ctx->iface;
	struct wifi_iface_status status = {0};

	if (!iface) {
		LOG_ERR("No interface available for DHCP start");
		return;
	}

	/* SoftAP interface uses a static IP - do not run DHCP client on it */
	if (net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &status, sizeof(status)) == 0) {
		if (status.iface_mode == WIFI_MODE_AP) {
			LOG_DBG("SoftAP mode active, skipping DHCP client start (static IP)");
			return;
		}
	}

	LOG_INF("Starting DHCP client (delayed)");
	net_dhcpv4_start(iface);
}

static void dhcp_stop_work_handler(struct k_work *work)
{
	struct dhcp_stop_ctx *ctx = CONTAINER_OF(work, struct dhcp_stop_ctx, work);
	struct net_if *iface = ctx->iface;
	struct wifi_iface_status status = {0};

	if (!iface) {
		LOG_ERR("No interface available for DHCP stop");
		return;
	}

	/* SoftAP interface uses a static IP - do not run DHCP client on it */
	if (net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &status, sizeof(status)) == 0) {
		if (status.iface_mode == WIFI_MODE_AP) {
			LOG_DBG("SoftAP mode active, skipping DHCP client stop (static IP)");
			return;
		}
	}

	LOG_INF("Stopping DHCP client (delayed)");
	net_dhcpv4_stop(iface);
}

static void scan_complete_work_handler(struct k_work *work)
{
    /* Send scan complete event */
    QAT_Response_Str(QAT_RC_QUIET, "+EVT:wlan_scancmplt\r\n");
    LOG_INF("Scan complete event sent (delayed)");
    
    /* Reset scan tracking */
    g_wifi_ctx.scan_result_count = 0;
    g_wifi_ctx.scan_in_progress = false;
}

static void wifi_mgmt_event_handler(struct net_mgmt_event_callback *cb,
                                   uint64_t mgmt_event, struct net_if *iface)
{
    char buffer[WLAN_RESPONSE_BUFFER_LENGTH];
    
    /* Debug: Log all events received */
    LOG_DBG("wifi_mgmt_event_handler: event=0x%llx, enable_reporting=%d", 
            mgmt_event, enable_event_reporting);
    
    if (!enable_event_reporting) {
        LOG_WRN("Event reporting disabled, ignoring event 0x%llx", mgmt_event);
        return;
    }

    switch (mgmt_event) {
    case NET_EVENT_WIFI_SCAN_RESULT: {
        const struct wifi_scan_result *entry = (const struct wifi_scan_result *)cb->info;
        
        LOG_DBG("SCAN_RESULT received: count=%d", g_wifi_ctx.scan_result_count + 1);
        
        /* First result - send scan result start event */
        if (g_wifi_ctx.scan_result_count == 0) {
            /* Note: We don't know total count yet, so we'll send it in SCAN_DONE */
            g_wifi_ctx.scan_in_progress = true;
            LOG_INF("First scan result, marking scan in progress");
        }
        
        g_wifi_ctx.scan_result_count++;
        
        snprintf(buffer, sizeof(buffer),
                "+CWLAP:\"%s\",%02x:%02x:%02x:%02x:%02x:%02x,%d,%d",
                entry->ssid,
                entry->mac[0], entry->mac[1], entry->mac[2],
                entry->mac[3], entry->mac[4], entry->mac[5],
                entry->channel, entry->rssi);
        
        QAT_Response_Str(QAT_RC_QUIET, buffer);
        
        /* Add small delay to prevent UART buffer overflow when many APs are found */
        k_msleep(10);
        break;
    }
    
    case NET_EVENT_WIFI_SCAN_DONE:
        LOG_INF("SCAN_DONE received: scan_in_progress=%d, count=%d", 
                g_wifi_ctx.scan_in_progress, g_wifi_ctx.scan_result_count);
        
        /* Schedule scan complete event with delay to ensure all scan results are output
         * Use work queue to avoid blocking the event handler
         * Delay = 10ms per result + 50ms buffer
         */
        if (g_wifi_ctx.scan_result_count > 0) {
            k_work_schedule(&scan_complete_work, 
                           K_MSEC(g_wifi_ctx.scan_result_count * 10 + 50));
        } else {
            /* No results, send immediately */
            k_work_schedule(&scan_complete_work, K_NO_WAIT);
        }
        break;
        
    case NET_EVENT_WIFI_CONNECT_RESULT: {
        const struct wifi_status *status = (const struct wifi_status *)cb->info;
        
        if (status->status == 0) {
            struct wifi_iface_status iface_status = {0};
            int ret;

            k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
            g_wifi_ctx.connected = true;
            /* Update iface from callback parameter */
            if (!g_wifi_ctx.iface) {
                g_wifi_ctx.iface = iface;
            }
            k_mutex_unlock(&g_wifi_ctx.mutex);
            
            /* Get interface status to retrieve BSSID */
            ret = net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &iface_status, 
                          sizeof(iface_status));
            if (ret == 0) {
                snprintf(buffer, sizeof(buffer),
                        "+EVT:wlan_conned:%02x:%02x:%02x:%02x:%02x:%02x",
                        (uint8_t)iface_status.bssid[0], (uint8_t)iface_status.bssid[1], 
                        (uint8_t)iface_status.bssid[2], (uint8_t)iface_status.bssid[3], 
                        (uint8_t)iface_status.bssid[4], (uint8_t)iface_status.bssid[5]);
            } else {
                snprintf(buffer, sizeof(buffer), "+EVT:wlan_conned");
            }
            QAT_Response_Str(QAT_RC_QUIET, buffer);
            
            /* DHCP start is now driven by NET_EVENT_IF_UP in if_event_handler */
        } else {
            snprintf(buffer, sizeof(buffer), "+EVT:wlan_conn_failed:%d", status->status);
            QAT_Response_Str(QAT_RC_QUIET, buffer);
        }
        break;
    }
    
    case NET_EVENT_WIFI_DISCONNECT_RESULT: {
        k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
        g_wifi_ctx.connected = false;
        k_mutex_unlock(&g_wifi_ctx.mutex);
        
        QAT_Response_Str(QAT_RC_QUIET, "+EVT:wlan_disconn");
        
        /* DHCP stop is now driven by NET_EVENT_IF_DOWN in if_event_handler */
        break;
    }
    
    case NET_EVENT_WIFI_AP_ENABLE_RESULT: {
        const struct wifi_status *status = (const struct wifi_status *)cb->info;
        
        if (status && status->status == 0) {
            char buffer[WLAN_RESPONSE_BUFFER_LENGTH];
            struct wifi_iface_status iface_status = {0};
            int ret;
            int freq = 0;
            
            /* Try to get interface status for complete information */
            ret = net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &iface_status, 
                          sizeof(iface_status));
            
            /* Calculate frequency from channel */
            k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
            if (ret == 0 && iface_status.channel > 0) {
                /* Use channel from iface_status if available */
                if (iface_status.channel >= 1 && iface_status.channel <= 14) {
                    /* 2.4GHz: channel 1-14 */
                    freq = 2407 + iface_status.channel * 5;
                } else if (iface_status.channel >= 36) {
                    /* 5GHz: channel 36+ */
                    freq = 5000 + iface_status.channel * 5;
                }
            } else if (g_wifi_ctx.channel > 0) {
                /* Fallback to saved channel */
                if (g_wifi_ctx.channel >= 1 && g_wifi_ctx.channel <= 14) {
                    freq = 2407 + g_wifi_ctx.channel * 5;
                } else if (g_wifi_ctx.channel >= 36) {
                    freq = 5000 + g_wifi_ctx.channel * 5;
                }
            }
            
            if (g_wifi_ctx.ssid_len > 0) {
                snprintf(buffer, sizeof(buffer),
                        "+EVT:wlan_switchchan:1,%d,%s",
                        freq,
                        g_wifi_ctx.ssid);
                k_mutex_unlock(&g_wifi_ctx.mutex);
                g_wifi_ctx.ap_active = true;
                QAT_Response_Str(QAT_RC_QUIET, buffer);

                LOG_INF("AP enabled on freq %d MHz, SSID: %s", freq, g_wifi_ctx.ssid);
            } else {
                k_mutex_unlock(&g_wifi_ctx.mutex);
                g_wifi_ctx.ap_active = true;
                QAT_Response_Str(QAT_RC_QUIET, "+EVT:wlan_ap_enabled");
            }
        } else {
            LOG_ERR("AP enable failed: status=%d", status ? status->status : -1);
        }
        break;
    }
    
    case NET_EVENT_WIFI_AP_STA_CONNECTED: {
        const struct wifi_ap_sta_info *sta_info = (const struct wifi_ap_sta_info *)cb->info;
        char buffer[WLAN_RESPONSE_BUFFER_LENGTH];
        struct wifi_iface_status iface_status = {0};
        int ret;
        int freq = 0;
        
        if (sta_info) {
            /* Get interface status for SSID and channel */
            ret = net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &iface_status, 
                          sizeof(iface_status));
            
            /* Calculate frequency from channel */
            if (ret == 0 && iface_status.channel > 0) {
                if (iface_status.channel >= 1 && iface_status.channel <= 14) {
                    freq = 2407 + iface_status.channel * 5;
                } else if (iface_status.channel >= 36) {
                    freq = 5000 + iface_status.channel * 5;
                }
            }
            
            /* Send station connected event */
            if (ret == 0 && freq > 0) {
                snprintf(buffer, sizeof(buffer),
                        "+EVT:wlan_conned:1,%02x:%02x:%02x:%02x:%02x:%02x,%d,%s,%d,0",
                        sta_info->mac[0], sta_info->mac[1], sta_info->mac[2],
                        sta_info->mac[3], sta_info->mac[4], sta_info->mac[5],
                        freq,
                        iface_status.ssid,
                        sta_info->link_mode);  /* Using link_mode as assoc_id */
            } else {
                /* Fallback if we can't get full info */
                k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
                if (g_wifi_ctx.channel > 0) {
                    if (g_wifi_ctx.channel >= 1 && g_wifi_ctx.channel <= 14) {
                        freq = 2407 + g_wifi_ctx.channel * 5;
                    } else if (g_wifi_ctx.channel >= 36) {
                        freq = 5000 + g_wifi_ctx.channel * 5;
                    }
                }
                snprintf(buffer, sizeof(buffer),
                        "+EVT:wlan_conned:1,%02x:%02x:%02x:%02x:%02x:%02x,%d,%s,0,0",
                        sta_info->mac[0], sta_info->mac[1], sta_info->mac[2],
                        sta_info->mac[3], sta_info->mac[4], sta_info->mac[5],
                        freq,
                        g_wifi_ctx.ssid_len > 0 ? g_wifi_ctx.ssid : "unknown");
                k_mutex_unlock(&g_wifi_ctx.mutex);
            }
            QAT_Response_Str(QAT_RC_QUIET, buffer);
            
            LOG_INF("Station connected to AP: %02x:%02x:%02x:%02x:%02x:%02x at %d MHz",
                    sta_info->mac[0], sta_info->mac[1], sta_info->mac[2],
                    sta_info->mac[3], sta_info->mac[4], sta_info->mac[5], freq);
        }
        break;
    }
    
    case NET_EVENT_WIFI_AP_STA_DISCONNECTED: {
        const struct wifi_ap_sta_info *sta_info = (const struct wifi_ap_sta_info *)cb->info;
        char buffer[WLAN_RESPONSE_BUFFER_LENGTH];
        
        if (sta_info) {
            snprintf(buffer, sizeof(buffer),
                    "+EVT:wlan_disconn:1,%02x:%02x:%02x:%02x:%02x:%02x",
                    sta_info->mac[0], sta_info->mac[1], sta_info->mac[2],
                    sta_info->mac[3], sta_info->mac[4], sta_info->mac[5]);
            QAT_Response_Str(QAT_RC_QUIET, buffer);
            
            LOG_INF("Station disconnected from AP: %02x:%02x:%02x:%02x:%02x:%02x",
                    sta_info->mac[0], sta_info->mac[1], sta_info->mac[2],
                    sta_info->mac[3], sta_info->mac[4], sta_info->mac[5]);
        }
        break;
    }
    
    default:
        LOG_DBG("Unhandled wifi_mgmt event: 0x%llx", mgmt_event);
        break;
    }
}

static void net_mgmt_event_handler(struct net_mgmt_event_callback *cb,
                                  uint64_t mgmt_event, struct net_if *iface)
{
    LOG_DBG("net_mgmt_event_handler called: event=0x%llx", mgmt_event);

    switch (mgmt_event) {
        case NET_EVENT_IPV4_DHCP_BOUND: {
            char ip_buffer[128];
            struct net_if_config *cfg;
            
            LOG_INF("DHCP bound event received, sending +EVT:dhcp_bound");
            
            if (enable_event_reporting) {
                /* Get IP address using net_if API */
                cfg = net_if_get_config(iface);
                if (cfg && cfg->ip.ipv4) {
                    struct in_addr *addr = &cfg->ip.ipv4->unicast[0].ipv4.address.in_addr;
                    if (addr->s_addr != 0) {
                        char ip_str[NET_IPV4_ADDR_LEN];
                        net_addr_ntop(AF_INET, addr, ip_str, sizeof(ip_str));
                        
                        snprintf(ip_buffer, sizeof(ip_buffer), 
                                "+EVT:dhcp_bound,ip=%s\r\n", ip_str);
                        QAT_Response_Str(QAT_RC_QUIET, ip_buffer);
                    } else {
                        QAT_Response_Str(QAT_RC_QUIET, "+EVT:dhcp_bound");
                    }
                } else {
                    QAT_Response_Str(QAT_RC_QUIET, "+EVT:dhcp_bound");
                }
            }
            break;
        }

        default:
            LOG_DBG("Unhandled net_mgmt event: 0x%llx", mgmt_event);
            break;
    }
}

/*-------------------------------------------------------------------------
 * Command Implementations
 *-----------------------------------------------------------------------*/
/* AT+WIFISP - Probe WiFi capability */
static cat_return_state cmd_wlan_wifisp_exec(const struct cat_command *cmd)
{
    if (g_wifi_ctx.wlan_enabled) {
        return QAT_Response_Str(QAT_RC_OK, "+WIFISP: Supported");
    }

    /* Temporarily probe WiFi interface availability */
    struct net_if *iface = net_if_get_first_wifi();
    if (!iface) {
        return QAT_Response_Str(QAT_RC_ERROR, "+WIFISP:get wlan mode fail");
    }

    return QAT_Response_Str(QAT_RC_OK, "+WIFISP: Supported");
}

/* AT+CWENABLE - Enable WiFi */
static cat_return_state cmd_wlan_enable_exec(const struct cat_command *cmd)
{
    struct qcom_wifi_set_op_mode_params params;
    int ret;
    
    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
    
    if (g_wifi_ctx.wlan_enabled) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_OK, NULL);
    }
    
    /* Get default WiFi interface */
    g_wifi_ctx.iface = net_if_get_first_wifi();
    if (!g_wifi_ctx.iface) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+CWENABLE:No WiFi interface found");
    }
    
    /* Register event callbacks (similar to qapi_WLAN_Set_Callback) */
    if (!callbacks_registered) {
        /* Register WiFi management event callbacks */
        net_mgmt_init_event_callback(&wifi_mgmt_cb, wifi_mgmt_event_handler,
                                    NET_EVENT_WIFI_SCAN_RESULT |
                                    NET_EVENT_WIFI_SCAN_DONE |
                                    NET_EVENT_WIFI_CONNECT_RESULT |
                                    NET_EVENT_WIFI_DISCONNECT_RESULT |
                                    NET_EVENT_WIFI_AP_ENABLE_RESULT |
                                    NET_EVENT_WIFI_AP_STA_CONNECTED |
                                    NET_EVENT_WIFI_AP_STA_DISCONNECTED);
        net_mgmt_add_event_callback(&wifi_mgmt_cb);
        
        /* Register network management event callbacks.
         * NET_EVENT_IF_UP/DOWN drive DHCP client start/stop; the handler
         * checks the interface mode so SoftAP interfaces are skipped.
         */
        net_mgmt_init_event_callback(&net_mgmt_cb, net_mgmt_event_handler,
                                    NET_EVENT_IPV4_DHCP_BOUND
                                    );
        net_mgmt_add_event_callback(&net_mgmt_cb);

        callbacks_registered = true;
        LOG_INF("Event callbacks registered successfully");
        LOG_INF("Registered events: SCAN_RESULT=0x%llx, SCAN_DONE=0x%llx", 
                (uint64_t)NET_EVENT_WIFI_SCAN_RESULT, (uint64_t)NET_EVENT_WIFI_SCAN_DONE);
    } else {
        LOG_DBG("Event callbacks already registered");
    }
    
    /* Bring up the interface */
    net_if_up(g_wifi_ctx.iface);
    
    g_wifi_ctx.wlan_enabled = true;
    /* Set default active device to STA */
    k_mutex_unlock(&g_wifi_ctx.mutex);

    /* Set default operation mode to station (similar to reference code) */
    params.opmode = "station";
    params.hidden_ssid = "";
    ret = net_mgmt(NET_REQUEST_WIFI_QCOM_SET_OPERATION_MODE, g_wifi_ctx.iface,
                   &params, sizeof(params));
    if (ret) {
        LOG_WRN("Failed to set default station mode: %d", ret);
        /* Don't fail enable if mode setting fails */
    }

    LOG_INF("WiFi enabled");
    
    /* Send enable event (similar to QAPI_WLAN_ENABLE_CB_E) */
    if (enable_event_reporting) {
        QAT_Response_Str(QAT_RC_QUIET, "+CWENABLE:wlan enabled");
    }
    
    return QAT_Response_Str(QAT_RC_OK, NULL);
}

/* AT+CWQABLE - Disable WiFi */
static cat_return_state cmd_wlan_disable_exec(const struct cat_command *cmd)
{
    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);

    if (!g_wifi_ctx.wlan_enabled) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_OK, "+CWQABLE:WiFi already disabled");
    }

    if (g_wifi_ctx.connected && g_wifi_ctx.iface) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        int ret = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, g_wifi_ctx.iface, NULL, 0);
        if (ret) {
            LOG_WRN("Disconnect before disable failed: %d", ret);
        }
        k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
    }

    if (g_wifi_ctx.ap_active) {
        struct net_if *ap_iface = net_if_get_wifi_sap();

        k_mutex_unlock(&g_wifi_ctx.mutex);
        if (ap_iface) {
            int ret = net_mgmt(NET_REQUEST_WIFI_AP_DISABLE, ap_iface, NULL, 0);
            if (ret) {
                LOG_WRN("AP disable before wifi disable failed: %d", ret);
            }
        }
        k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
        g_wifi_ctx.ap_active = false;
    }

    if (g_wifi_ctx.iface) {
        net_if_down(g_wifi_ctx.iface);
    }

    g_wifi_ctx.wlan_enabled = false;
    g_wifi_ctx.connected = false;
    k_mutex_unlock(&g_wifi_ctx.mutex);
    
    /* Unregister event callbacks when disabling WiFi */
    if (callbacks_registered) {
        net_mgmt_del_event_callback(&wifi_mgmt_cb);
        net_mgmt_del_event_callback(&net_mgmt_cb);
        callbacks_registered = false;
        LOG_INF("Event callbacks unregistered");
    }
    
    LOG_INF("WiFi disabled");
    
    /* Send disable event (similar to QAPI_WLAN_DISABLE_CB_E) */
    if (enable_event_reporting) {
        QAT_Response_Str(QAT_RC_QUIET, "+EVT:wlan_disabled");
    }
    
    return QAT_Response_Str(QAT_RC_OK, NULL);
}

/* AT+CWLAP - Scan for WiFi networks */
static cat_return_state cmd_wlan_scan_exec(const struct cat_command *cmd)
{
    struct wifi_scan_params params = {0};
    int ret;
    
    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
    
    if (!g_wifi_ctx.wlan_enabled || !g_wifi_ctx.iface) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+CWLAP:Enable WiFi first");
    }
    
    /* Reset scan tracking */
    g_wifi_ctx.scan_result_count = 0;
    g_wifi_ctx.scan_in_progress = false;
    
    k_mutex_unlock(&g_wifi_ctx.mutex);
    
    /* Send scan start event (similar to QAPI_WLAN_SCAN_START_CB_E) */
    if (enable_event_reporting) {
        QAT_Response_Str(QAT_RC_QUIET, "+EVT:wlan_scanstart");
        LOG_INF("Scan start event sent, callbacks_registered=%d", callbacks_registered);
    }
    
    /* Set scan_type to ACTIVE (required by qcom driver) */
    params.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    /* Leave bands = 0 (driver will scan all bands) */
    
    /* Start scan with params (not NULL) */
    ret = net_mgmt(NET_REQUEST_WIFI_SCAN, g_wifi_ctx.iface, &params, sizeof(params));
    if (ret) {
        LOG_ERR("Scan request failed: %d", ret);
        return QAT_Response_Str(QAT_RC_ERROR, "+CWLAP:Scan failed");
    }
    
    return QAT_Response_Str(QAT_RC_OK, NULL);
}

/* AT+CWLAP=<ssid> - Scan for specific SSID */
static cat_return_state cmd_wlan_scan_set(const struct cat_command *cmd, const uint8_t *data,
                                          const size_t data_size, const size_t args_num)
{
    struct wifi_scan_params params = {0};
    static char ssid_filter[WIFI_SSID_MAX_LEN + 1];
    int ret;
    bool has_ssid_filter = false;
    
    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
    
    if (!g_wifi_ctx.wlan_enabled || !g_wifi_ctx.iface) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+CWLAP:Enable WiFi first");
    }
    
    /* Reset scan tracking */
    g_wifi_ctx.scan_result_count = 0;
    g_wifi_ctx.scan_in_progress = false;
    
    k_mutex_unlock(&g_wifi_ctx.mutex);
    
    /* Send scan start event (similar to QAPI_WLAN_SCAN_START_CB_E) */
    if (enable_event_reporting) {
        QAT_Response_Str(QAT_RC_QUIET, "+EVT:wlan_scanstart");
    }
    
    /* Parse SSID from data */
    if (data_size > 0 && data_size <= WIFI_SSID_MAX_LEN) {
        memcpy(ssid_filter, data, data_size);
        ssid_filter[data_size] = '\0';
        params.ssids[0] = ssid_filter;
        has_ssid_filter = true;
    }
    
    /* Note: Qcom driver doesn't support bands/dwell_time/max_bss_cnt/band_chan parameters
     * The driver will scan all bands (2.4G and 5G) automatically
     * Only SSID filter is supported via params.ssids[0] */
    
    /* Start scan - pass params only if we have SSID filter, otherwise pass NULL */
    if (has_ssid_filter) {
        /* Set scan_type to ACTIVE to avoid driver check failure */
        params.scan_type = WIFI_SCAN_TYPE_ACTIVE;
        ret = net_mgmt(NET_REQUEST_WIFI_SCAN, g_wifi_ctx.iface, &params, sizeof(params));
    } else {
        /* No filter - pass NULL for full scan */
        ret = net_mgmt(NET_REQUEST_WIFI_SCAN, g_wifi_ctx.iface, NULL, 0);
    }
    
    if (ret) {
        LOG_ERR("Scan request failed: %d", ret);
        return QAT_Response_Str(QAT_RC_ERROR, "+CWLAP:Scan failed");
    }
    
    return QAT_Response_Str(QAT_RC_OK, NULL);
}

/* AT+CWJAP=<ssid>[,<bssid>] - Connect to WiFi */
static cat_return_state cmd_wlan_connect_set(const struct cat_command *cmd, const uint8_t *data,
                                             const size_t data_size, const size_t args_num)
{
    char ssid[WIFI_SSID_MAX_LEN + 1] = {0};
    char bssid_str[18] = {0};  /* Format: xx:xx:xx:xx:xx:xx */
    uint8_t bssid[6] = {0};
    char buffer[WLAN_RESPONSE_BUFFER_LENGTH];
    int ret;
    char *comma;
    size_t ssid_len;
    bool has_bssid = false;
    
    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);

    if (!g_wifi_ctx.wlan_enabled || !g_wifi_ctx.iface) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+CWJAP:Enable WiFi first");
    }

    if (g_wifi_ctx.op_mode == DEV_MODE_AP_E) {
        
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+CWJAP:Not allowed in AP mode");
    }

    k_mutex_unlock(&g_wifi_ctx.mutex);

#ifdef CONFIG_QAT_WIFI_CRED
    /* AT+CWJAP=@saved  →  connect using flash-stored credentials */
    if (data_size == 6 && memcmp(data, "@saved", 6) == 0) {
        wifi_cred_err_t load_err = load_wifi_cred_from_flash();
        if (load_err != WIFI_CRED_OK) {
            const char *msg = (load_err == WIFI_CRED_ERR_NOFILE)
                              ? "+CWJAP: no saved credentials"
                              : "+CWJAP: saved credentials corrupt";
            return QAT_Response_Str(QAT_RC_ERROR, msg);
        }
        /* Credentials loaded into g_wifi_ctx — build connect params */
        struct wifi_connect_req_params params = {0};
        k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
        params.ssid        = g_wifi_ctx.ssid;
        params.ssid_length = g_wifi_ctx.ssid_len;
        params.channel     = WIFI_CHANNEL_ANY;
        params.timeout     = SYS_FOREVER_MS;
        if (g_wifi_ctx.security_set && g_wifi_ctx.passphrase_len > 0) {
            if (g_wifi_ctx.auth_mode == 0x02) {
                params.security = WIFI_SECURITY_TYPE_WPA_PSK;
            } else if (g_wifi_ctx.auth_mode == 0x04) {
                params.security = WIFI_SECURITY_TYPE_PSK;
            } else if (g_wifi_ctx.auth_mode == 0x400) {
                params.security = WIFI_SECURITY_TYPE_SAE;
            } else if (g_wifi_ctx.auth_mode == 0x404 || g_wifi_ctx.auth_mode == 0x406) {
                params.security = WIFI_SECURITY_TYPE_PSK_SHA256;
            } else {
                params.security = WIFI_SECURITY_TYPE_PSK;
            }
            params.psk        = (const uint8_t *)g_wifi_ctx.passphrase;
            params.psk_length = g_wifi_ctx.passphrase_len;
        } else {
            params.security = WIFI_SECURITY_TYPE_NONE;
        }
        char buf[WLAN_RESPONSE_BUFFER_LENGTH];
        snprintf(buf, sizeof(buf), "+CWJAP:connecting to ssid %s (@saved)", g_wifi_ctx.ssid);
        k_mutex_unlock(&g_wifi_ctx.mutex);
        QAT_Response_Str(QAT_RC_QUIET, buf);
        int r = net_mgmt(NET_REQUEST_WIFI_CONNECT, g_wifi_ctx.iface, &params, sizeof(params));
        if (r) {
            return QAT_Response_Str(QAT_RC_ERROR, "+CWJAP:Connect failed");
        }
        return QAT_Response_Str(QAT_RC_OK, NULL);
    }
#endif /* CONFIG_QAT_WIFI_CRED */

    /* Parse SSID and optional BSSID: AT+CWJAP=<ssid>[,<bssid>] */
    comma = strchr((const char *)data, ',');
    if (!comma) {
        /* Only SSID provided */
        if (data_size > WIFI_SSID_MAX_LEN) {
            return QAT_Response_Str(QAT_RC_ERROR, "+CWJAP:SSID too long");
        }
        memcpy(ssid, data, data_size);
        ssid[data_size] = '\0';
        ssid_len = data_size;
    } else {
        /* SSID and BSSID provided */
        ssid_len = comma - (const char *)data;
        size_t bssid_len = data_size - ssid_len - 1;
        
        if (ssid_len > WIFI_SSID_MAX_LEN) {
            return QAT_Response_Str(QAT_RC_ERROR, "+CWJAP:SSID too long");
        }
        if (bssid_len > sizeof(bssid_str) - 1) {
            return QAT_Response_Str(QAT_RC_ERROR, "+CWJAP:BSSID too long");
        }
        
        memcpy(ssid, data, ssid_len);
        ssid[ssid_len] = '\0';
        memcpy(bssid_str, comma + 1, bssid_len);
        bssid_str[bssid_len] = '\0';
        
        /* Parse BSSID string (format: xx:xx:xx:xx:xx:xx or xx-xx-xx-xx-xx-xx) */
        if (sscanf(bssid_str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                   &bssid[0], &bssid[1], &bssid[2], &bssid[3], &bssid[4], &bssid[5]) == 6 ||
            sscanf(bssid_str, "%hhx-%hhx-%hhx-%hhx-%hhx-%hhx",
                   &bssid[0], &bssid[1], &bssid[2], &bssid[3], &bssid[4], &bssid[5]) == 6) {
            has_bssid = true;
        } else {
            return QAT_Response_Str(QAT_RC_ERROR, "+CWJAP:Invalid BSSID format");
        }
    }
    

    /* Now use standard WiFi connect with SSID */
    struct wifi_connect_req_params params = {0};
    params.ssid = ssid;
    params.ssid_length = ssid_len;
    params.channel = WIFI_CHANNEL_ANY;
    params.timeout = SYS_FOREVER_MS;
    
    /* Set BSSID if provided (driver checks if bssid is non-zero) */
    if (has_bssid) {
        memcpy(params.bssid, bssid, 6);
    } else {
        /* Clear BSSID to indicate it's not set */
        memset(params.bssid, 0, 6);
    }
    
    /* Use security parameters from context (set by +CWWPA and +CWPWD) */
    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
    if (g_wifi_ctx.security_set && g_wifi_ctx.passphrase_len > 0) {
        /* Map auth_mode to security type */
        if (g_wifi_ctx.auth_mode == 0x02) {
            /* WPA-PSK */
            params.security = WIFI_SECURITY_TYPE_WPA_PSK;
        } else if (g_wifi_ctx.auth_mode == 0x04) {
            /* WPA2-PSK */
            params.security = WIFI_SECURITY_TYPE_PSK;
        } else if (g_wifi_ctx.auth_mode == 0x400) {
            /* WPA3-SAE */
            params.security = WIFI_SECURITY_TYPE_SAE;
        } else if (g_wifi_ctx.auth_mode == 0x404 || g_wifi_ctx.auth_mode == 0x406) {
            /* WPA2/WPA3 mixed - use PSK_SHA256 or SAE */
            params.security = WIFI_SECURITY_TYPE_PSK_SHA256;
        } else {
            /* Default to WPA2-PSK */
            params.security = WIFI_SECURITY_TYPE_PSK;
        }
        
        /* Set passphrase */
        params.psk = (const uint8_t *)g_wifi_ctx.passphrase;
        params.psk_length = g_wifi_ctx.passphrase_len;
        
        LOG_INF("Using stored security: auth_mode=0x%x, security=%d, psk_len=%d",
                g_wifi_ctx.auth_mode, params.security, params.psk_length);
    } else {
        /* No security parameters set, assume open network */
        params.security = WIFI_SECURITY_TYPE_NONE;
        LOG_INF("No security parameters set, using open network");
    }
    
    /* Save SSID to context */
    memcpy(g_wifi_ctx.ssid, ssid, ssid_len);
    g_wifi_ctx.ssid[ssid_len] = '\0';
    g_wifi_ctx.ssid_len = ssid_len;
    k_mutex_unlock(&g_wifi_ctx.mutex);
    
    snprintf(buffer, sizeof(buffer), "+CWJAP:connecting to ssid %s", ssid);
    QAT_Response_Str(QAT_RC_QUIET, buffer);
    
    /* Connect using qapi_WLAN_Commit equivalent (net_mgmt CONNECT) */
    ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, g_wifi_ctx.iface, &params, sizeof(params));
    if (ret) {
        LOG_ERR("Connection request failed: %d", ret);
        return QAT_Response_Str(QAT_RC_ERROR, "+CWJAP:Connect failed");
    }
    
    LOG_INF("Connecting to %s with security type %d", ssid, params.security);
    return QAT_Response_Str(QAT_RC_OK, NULL);
}

/* AT+CWJAP - Show usage */
static cat_return_state cmd_wlan_connect_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK, "+CWJAP=<ssid>[,<bssid>]");
}

/* AT+CWJAP? - Query connection status */
static cat_return_state cmd_wlan_connect_query(const struct cat_command *cmd, uint8_t *data,
                                               size_t *data_size, const size_t max_data_size)
{
    char buffer[WLAN_STR_BUFFER_LENGTH];
    struct wifi_iface_status status = {0};
    struct qcom_wifi_get_operation_mode_params mode_params;
    struct qcom_wifi_get_phy_mode_params phy_params;
    int ret;
    int offset = 0;
    const char *mode_str;
    const char *phy_str;
    
    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
    
    if (!g_wifi_ctx.wlan_enabled || !g_wifi_ctx.iface) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+CWJAP:WiFi not enabled");
    }
    
    /* Format: +CWJAP:<ssid>,<channel>,<rssi>,<power_mode>,<mac>,<op_mode>,<phy_mode> */
    
    if (g_wifi_ctx.connected) {
        /* Get WiFi status */
        ret = net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, g_wifi_ctx.iface, &status, sizeof(status));
        if (ret) {
            k_mutex_unlock(&g_wifi_ctx.mutex);
            return QAT_Response_Str(QAT_RC_ERROR, "+CWJAP:Failed to get status");
        }
        
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "+CWJAP:%s,%d,",
                          g_wifi_ctx.ssid, status.channel);
    } else {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "+CWJAP:NA,NA,");
    }
    
    k_mutex_unlock(&g_wifi_ctx.mutex);
    
    /* Get RSSI */
    if (g_wifi_ctx.connected) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%d,", status.rssi);
    } else {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "NA,");
    }
    
    /* Power mode - simplified, just report "Max Perf" for now */
    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "Max Perf,");
    
    /* Get MAC address from BSSID */
    if (g_wifi_ctx.connected) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, 
                          "%02x-%02x-%02x-%02x-%02x-%02x,",
                          (uint8_t)status.bssid[0], (uint8_t)status.bssid[1], (uint8_t)status.bssid[2],
                          (uint8_t)status.bssid[3], (uint8_t)status.bssid[4], (uint8_t)status.bssid[5]);
    } else {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "NA,");
    }
    
    /* Get operating mode */
    ret = net_mgmt(NET_REQUEST_WIFI_QCOM_GET_OPERATION_MODE, g_wifi_ctx.iface,
                   &mode_params, sizeof(mode_params));
    if (ret == 0) {
        switch (mode_params.opmode) {
        case DEV_MODE_STATION_E:
            mode_str = "station";
            break;
        case DEV_MODE_AP_E:
            mode_str = "softap";
            break;
        case DEV_MODE_AP_STA_E:
            mode_str = "concurrency mode";
            break;
        case DEV_MODE_NO_CONC_E:
            mode_str = "non_softap+station";
            break;
        default:
            mode_str = "unknown";
            break;
        }
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%s,", mode_str);
    } else {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "NA,");
    }
    
    /* Get PHY mode */
    ret = net_mgmt(NET_REQUEST_WIFI_QCOM_GET_PHY_MODE, g_wifi_ctx.iface,
                   &phy_params, sizeof(phy_params));
    if (ret == 0) {
        /* Map phy_mode to string - correct enum values from wlan_base.h */
        switch (phy_params.phy_mode) {
        case 0: /* QAPI_WLAN_11B_MODE_E = 0x0 */
            phy_str = "b";
            break;
        case 1: /* QAPI_WLAN_11G_MODE_E = 0x1 */
            phy_str = "g";
            break;
        case 2: /* QAPI_WLAN_11NG_HT20_MODE_E = 0x2 */
            phy_str = "ng";
            break;
        case 3: /* QAPI_WLAN_11A_MODE_E = 0x3 */
            phy_str = "a";
            break;
        case 4: /* QAPI_WLAN_11A_HT20_MODE_E = 0x4 */
            phy_str = "a_ht20";
            break;
        case 5: /* QAPI_WLAN_11ABGN_HT20_MODE_E = 0x5 */
            phy_str = "abgn";
            break;
        default:
            phy_str = "unknown";
            break;
        }
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%s", phy_str);
    } else {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "NA");
    }
    
    return QAT_Response_Str(QAT_RC_OK, buffer);
}

/* AT+CWQAP - Disconnect from WiFi */
static cat_return_state cmd_wlan_disconnect_exec(const struct cat_command *cmd)
{
    int ret;
    
    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
    
    if (!g_wifi_ctx.wlan_enabled || !g_wifi_ctx.iface) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+CWQAP:WiFi not enabled");
    }
    
    if (!g_wifi_ctx.connected) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_OK, "+CWQAP:Not connected");
    }
    
    k_mutex_unlock(&g_wifi_ctx.mutex);
    
    /* Disconnect */
    ret = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, g_wifi_ctx.iface, NULL, 0);
    if (ret) {
        LOG_ERR("Disconnect request failed: %d", ret);
        return QAT_Response_Str(QAT_RC_ERROR, "+CWQAP:Disconnect failed");
    }
    
    LOG_INF("Disconnected");
    return QAT_Response_Str(QAT_RC_OK, NULL);
}

/* AT+CWSOFTAP=<ht_config>,<channel>,<ssid>[,<security>[,<passphrase>]] - Enable AP mode */
static cat_return_state cmd_wlan_ap_enable_set(const struct cat_command *cmd, const uint8_t *data,
                                               const size_t data_size, const size_t args_num)
{
    struct wifi_connect_req_params params = {0};
    struct net_if *ap_iface = NULL;
    char *token, *saveptr;
    char data_copy[256];
    char ht_config[16] = {0};
    char ssid[WIFI_SSID_MAX_LEN + 1] = {0};
    char passphrase[65] = {0};
    int channel = 0;
    int security = WIFI_SECURITY_TYPE_NONE;
    int ret;
    char buffer[WLAN_RESPONSE_BUFFER_LENGTH];
    bool is_sta_connected = false;
    
    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);

    if (!g_wifi_ctx.wlan_enabled || !g_wifi_ctx.iface) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+CWSOFTAP:Enable WiFi first");
    }

    if (g_wifi_ctx.op_mode == DEV_MODE_STATION_E) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+CWSOFTAP:Not allowed in station mode");
    }
    
    /* Check if STA is connected - need to use second interface for AP */
    is_sta_connected = g_wifi_ctx.connected;
    
    k_mutex_unlock(&g_wifi_ctx.mutex);
    
    /* Parse parameters: ht_config,channel,ssid */
    if (data_size >= sizeof(data_copy)) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CWSOFTAP:Parameters too long");
    }
    
    memcpy(data_copy, data, data_size);
    data_copy[data_size] = '\0';
    
    /* Parse ht_config (disable/ht20) */
    token = strtok_r(data_copy, ",", &saveptr);
    if (!token) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CWSOFTAP:Missing ht_config parameter");
    }
    strlcpy(ht_config, token, sizeof(ht_config));
    
    /* Check if it's just "disable" command */
    if (strcmp(ht_config, "disable") == 0 && !strchr((const char *)data, ',')) {
        /* Disable AP mode */
        ret = net_mgmt(NET_REQUEST_WIFI_AP_DISABLE, g_wifi_ctx.iface, NULL, 0);
        if (ret) {
            LOG_ERR("AP disable failed: %d", ret);
            return QAT_Response_Str(QAT_RC_ERROR, "+CWSOFTAP:Failed to disable AP");
        }
        LOG_INF("AP mode disabled");
        g_wifi_ctx.ap_active = false;
        return QAT_Response_Str(QAT_RC_OK, "+CWSOFTAP:disabled ap");
    }
    
    /* Parse channel (1-14, 36-165) */
    token = strtok_r(NULL, ",", &saveptr);
    if (!token) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CWSOFTAP:Missing channel parameter");
    }
    channel = atoi(token);
    
    /* Parse SSID */
    token = strtok_r(NULL, ",", &saveptr);
    if (!token) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CWSOFTAP:Missing SSID parameter");
    }
    strlcpy(ssid, token, sizeof(ssid));

    /* Parse optional security type and passphrase */
    token = strtok_r(NULL, ",", &saveptr);
    if (token) {
        security = atoi(token);
        token = strtok_r(NULL, ",", &saveptr);
        if (token) {
            strlcpy(passphrase, token, sizeof(passphrase));
        }
    }

    if (security != WIFI_SECURITY_TYPE_NONE && passphrase[0] == '\0') {
        return QAT_Response_Str(QAT_RC_ERROR, "+CWSOFTAP:Passphrase required for encrypted AP");
    }

    /* Validate security type: only 0 (OPEN) and 1 (WPA2-PSK) are supported for AP */
    if (security > WIFI_SECURITY_TYPE_PSK) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CWSOFTAP:Invalid security type (0=OPEN, 1=WPA2-PSK)");
    }

    /* Get the SAP (SoftAP) interface using dedicated API */
    ap_iface = net_if_get_wifi_sap();

    if (!ap_iface) {
        LOG_ERR("Failed to get WiFi SAP interface");
        return QAT_Response_Str(QAT_RC_ERROR, "+CWSOFTAP:No SAP interface available");
    }

    LOG_INF("Using second WiFi interface for AP: %p", ap_iface);

    /* If STA is connected, disconnect it first and switch to concurrent mode */
    if (is_sta_connected) {
        LOG_INF("STA connected, disconnecting before switching to AP interface");

        /* Disconnect STA */
        ret = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, g_wifi_ctx.iface, NULL, 0);
        if (ret) {
            LOG_WRN("Failed to disconnect STA: %d (continuing anyway)", ret);
        } else {
            LOG_INF("STA disconnected successfully");
            /* Update context */
            k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
            g_wifi_ctx.connected = false;
            k_mutex_unlock(&g_wifi_ctx.mutex);
        }

    }

    /* Setup AP parameters using standard WiFi API */
    params.ssid = ssid;
    params.ssid_length = strlen(ssid);
    params.channel = channel;
    params.security = security;
    if (security != WIFI_SECURITY_TYPE_NONE) {
        params.psk = passphrase;
        params.psk_length = strlen(passphrase);
    }
    
    /* Enable AP mode on the second interface */
    ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, ap_iface, &params, sizeof(params));
    if (ret) {
        LOG_ERR("AP enable failed: %d", ret);
        snprintf(buffer, sizeof(buffer), "+CWSOFTAP:Failed to enable AP (ret=%d)", ret);
        return QAT_Response_Str(QAT_RC_ERROR, buffer);
    }
    
    /* Save SSID to context */
    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
    memcpy(g_wifi_ctx.ssid, ssid, params.ssid_length);
    g_wifi_ctx.ssid[params.ssid_length] = '\0';
    g_wifi_ctx.ssid_len = params.ssid_length;
    g_wifi_ctx.channel = channel;
    k_mutex_unlock(&g_wifi_ctx.mutex);
    
    LOG_INF("AP mode enabled: ht=%s, channel=%d, ssid=%s, security=%d", ht_config, channel, ssid,
            security);
    snprintf(buffer, sizeof(buffer), "+CWSOFTAP:%s,channel=%d,ht20=%d,security=%d",
             ssid, channel, (strcmp(ht_config, "ht20") == 0) ? 1 : 0, security);
    return QAT_Response_Str(QAT_RC_OK, buffer);
}

/* AT+CWSOFTAP - Show usage */
static cat_return_state cmd_wlan_ap_enable_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK,
        "+CWSOFTAP=<ht_config>,<channel>,<ssid>[,<security>[,<passphrase>]]\r\n"
        "  ht_config: disable | ht20\r\n"
        "  channel: 1-14 or 36-165 (0=auto)\r\n"
        "  security: 0=open, 1=WPA2-PSK\r\n"
        "  passphrase: required when security>0 (optional)");
}

/* AT+CWCOUNTRY=<country_code> - Set regulatory domain */
static cat_return_state cmd_wlan_reg_domain_set(const struct cat_command *cmd, const uint8_t *data,
                                                const size_t data_size, const size_t args_num)
{
    struct wifi_reg_domain reg_domain = {0};
    struct wifi_reg_chan_info chan_info_buf[MAX_REG_CHAN_NUM];
    int ret;

    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);

    if (!g_wifi_ctx.wlan_enabled) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+CWCOUNTRY:Enable WiFi first");
    }

    k_mutex_unlock(&g_wifi_ctx.mutex);

    struct net_if *iface = net_if_get_wifi_sap();
    if (!iface) {
        iface = net_if_get_wifi_sta();
    }
    if (!iface) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CWCOUNTRY:No wifi interface");
    }

    /* Country code should be 2 characters */
    if (data_size != 2) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CWCOUNTRY:Invalid country code (use 2-letter code)");
    }

    /* Setup regulatory domain - convert to uppercase */
    reg_domain.country_code[0] = toupper((unsigned char)data[0]);
    reg_domain.country_code[1] = toupper((unsigned char)data[1]);

    /* Provide chan_info buffer for the driver */
    reg_domain.chan_info = chan_info_buf;
    reg_domain.oper = WIFI_MGMT_SET;

    /* Set regulatory domain */
    ret = net_mgmt(NET_REQUEST_WIFI_REG_DOMAIN, iface, &reg_domain, sizeof(reg_domain));
    if (ret) {
        LOG_ERR("Set regulatory domain failed: %d", ret);
        return QAT_Response_Str(QAT_RC_ERROR, "+CWCOUNTRY:Failed to set country code");
    }
    
    LOG_INF("Regulatory domain set to: %s", reg_domain.country_code);
    return QAT_Response_Str(QAT_RC_OK, NULL);
}

/* AT+CWCOUNTRY? - Query regulatory domain */
static cat_return_state cmd_wlan_reg_domain_query(const struct cat_command *cmd, uint8_t *data,
                                                  size_t *data_size, const size_t max_data_size)
{
    struct wifi_reg_domain reg_domain = {0};
    struct wifi_reg_chan_info chan_info_buf[MAX_REG_CHAN_NUM];
    char buffer[WLAN_RESPONSE_BUFFER_LENGTH];
    int ret;
    
    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);

    if (!g_wifi_ctx.wlan_enabled) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+CWCOUNTRY:WiFi not enabled");
    }

    k_mutex_unlock(&g_wifi_ctx.mutex);

    struct net_if *iface = net_if_get_wifi_sap();
    if (!iface) {
        iface = net_if_get_wifi_sta();
    }
    if (!iface) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CWCOUNTRY:No wifi interface");
    }

    /* Provide chan_info buffer for the driver */
    reg_domain.chan_info = chan_info_buf;
    reg_domain.oper = WIFI_MGMT_GET;

    /* Get regulatory domain */
    ret = net_mgmt(NET_REQUEST_WIFI_REG_DOMAIN, iface, &reg_domain, sizeof(reg_domain));
    if (ret) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CWCOUNTRY:Failed to get country code");
    }
    
    /* Ensure null termination and format output with only 2 characters */
    snprintf(buffer, sizeof(buffer), "+CWCOUNTRY:%.2s", reg_domain.country_code);
    return QAT_Response_Str(QAT_RC_OK, buffer);
}

/* AT+CWCOUNTRY - Show usage */
static cat_return_state cmd_wlan_reg_domain_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK, "+CWCOUNTRY=<countrycode>, e.g. US/CN");
}


/*-------------------------------------------------------------------------
 * Advanced WiFi Configuration Commands
 *-----------------------------------------------------------------------*/
/* AT+CWPHYMODE - Show usage */
static cat_return_state cmd_wlan_phy_mode_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK, "+CWPHYMODE=a/b/g/ng/abgn");
}

/* AT+CWPHYMODE=<mode> - Set PHY mode */
static cat_return_state cmd_wlan_phy_mode_set(const struct cat_command *cmd, const uint8_t *data,
                                              const size_t data_size, const size_t args_num)
{
    struct qcom_wifi_set_phy_mode_params params;
    char wmode[16];
    int ret;
    
    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
    
    if (!g_wifi_ctx.wlan_enabled || !g_wifi_ctx.iface) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+CWPHYMODE:Enable WiFi first");
    }
    
    k_mutex_unlock(&g_wifi_ctx.mutex);
    
    /* Parse PHY mode string */
    if (data_size == 0 || data_size >= sizeof(wmode)) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CWPHYMODE:Invalid parameter");
    }
    
    memcpy(wmode, data, data_size);
    wmode[data_size] = '\0';
    
    /* Map string to PHY mode enum - correct enum values from wlan_base.h */
    if (strcmp(wmode, "b") == 0) {
        params.phy_mode = 0;  /* QAPI_WLAN_11B_MODE_E = 0x0 */
    } else if (strcmp(wmode, "g") == 0) {
        params.phy_mode = 1;  /* QAPI_WLAN_11G_MODE_E = 0x1 */
    } else if (strcmp(wmode, "ng") == 0) {
        params.phy_mode = 2;  /* QAPI_WLAN_11NG_HT20_MODE_E = 0x2 */
    } else if (strcmp(wmode, "a") == 0) {
        params.phy_mode = 3;  /* QAPI_WLAN_11A_MODE_E = 0x3 */
    } else if (strcmp(wmode, "abgn") == 0) {
        params.phy_mode = 5;  /* QAPI_WLAN_11ABGN_HT20_MODE_E = 0x5 */
    } else {
        return QAT_Response_Str(QAT_RC_ERROR, "+CWPHYMODE:Unknown wmode, only support b/g/ng/a/abgn");
    }
    
    /* Set PHY mode via net_mgmt */
    ret = net_mgmt(NET_REQUEST_WIFI_QCOM_SET_PHY_MODE, g_wifi_ctx.iface, 
                   &params, sizeof(params));
    if (ret) {
        LOG_ERR("Set PHY mode failed: %d", ret);
        return QAT_Response_Str(QAT_RC_ERROR, "+CWPHYMODE:Failed to set PHY mode");
    }
    
    LOG_INF("PHY mode set to %s (%u)", wmode, params.phy_mode);
    return QAT_Response_Str(QAT_RC_OK, NULL);
}

/* AT+CWPHYMODE? - Query PHY mode */
static cat_return_state cmd_wlan_phy_mode_query(const struct cat_command *cmd, uint8_t *data,
                                                size_t *data_size, const size_t max_data_size)
{
    struct qcom_wifi_get_phy_mode_params params;
    char buffer[WLAN_RESPONSE_BUFFER_LENGTH];
    const char *phy_str;
    int ret;
    int offset = 0;
    
    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
    
    if (!g_wifi_ctx.wlan_enabled || !g_wifi_ctx.iface) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+CWPHYMODE:WiFi not enabled");
    }
    
    k_mutex_unlock(&g_wifi_ctx.mutex);
    
    /* Get PHY mode via net_mgmt */
    ret = net_mgmt(NET_REQUEST_WIFI_QCOM_GET_PHY_MODE, g_wifi_ctx.iface,
                   &params, sizeof(params));
    if (ret) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CWPHYMODE:Failed to get PHY mode");
    }
    
    /* Map phy_mode enum to string - correct enum values from wlan_base.h */
    switch (params.phy_mode) {
    case 0: /* QAPI_WLAN_11B_MODE_E = 0x0 */
        phy_str = "b";
        break;
    case 1: /* QAPI_WLAN_11G_MODE_E = 0x1 */
        phy_str = "g";
        break;
    case 2: /* QAPI_WLAN_11NG_HT20_MODE_E = 0x2 */
        phy_str = "ng";
        break;
    case 3: /* QAPI_WLAN_11A_MODE_E = 0x3 */
        phy_str = "a";
        break;
    case 4: /* QAPI_WLAN_11A_HT20_MODE_E = 0x4 */
        phy_str = "a_ht20";
        break;
    case 5: /* QAPI_WLAN_11ABGN_HT20_MODE_E = 0x5 */
        phy_str = "abgn";
        break;
    default:
        /* Unknown mode - return numeric value */
        snprintf(buffer, sizeof(buffer), "+CWPHYMODE:FAIL, %u", params.phy_mode);
        return QAT_Response_Str(QAT_RC_ERROR, buffer);
    }
    
    /* Format output: +CWPHYMODE:<mode_string> */
    offset = snprintf(buffer, sizeof(buffer), "+CWPHYMODE:%s", phy_str);
    
    return QAT_Response_Str(QAT_RC_OK, buffer);
}

/* AT+ANTIINF - Show usage */
static cat_return_state cmd_wlan_antiinf_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK, 
        "AT+ANTIINF=<0: 1M RTS|1:6M RTS|2: 12M RTS>\r\n"
        "AT+ANTIINF?: get ANTIINF");
}

/* AT+ANTIINF=<rts_rate> - Set Anti-interference (0:1Mbps, 1:6Mbps, 2:12Mbps) */
static cat_return_state cmd_wlan_antiinf_set(const struct cat_command *cmd, const uint8_t *data,
                                             const size_t data_size, const size_t args_num)
{
    struct qcom_wifi_set_rts_cts_params rts_params;
    struct qcom_wifi_set_rts_rate_params rate_params;
    struct qcom_wifi_set_edca_param_cfg_params edca_params;
    struct qcom_wifi_set_threshold_params threshold_params;
    struct qcom_wifi_set_ba_win_timing_params ba_params;
    struct qcom_wifi_set_slot_time_params slot_params;
    char buffer[32];
    int ret;
    
    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
    
    if (!g_wifi_ctx.wlan_enabled || !g_wifi_ctx.iface) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+ANTIINF:Enable WiFi first");
    }
    
    k_mutex_unlock(&g_wifi_ctx.mutex);
    
    /* Parse RTS rate parameter (0: 1Mbps, 1: 6Mbps, 2: 12Mbps) */
    if (data_size == 0 || data_size >= sizeof(buffer)) {
        return QAT_Response_Str(QAT_RC_ERROR, "+ANTIINF:Invalid parameter");
    }
    
    memcpy(buffer, data, data_size);
    buffer[data_size] = '\0';
    rate_params.rts_rate = (uint32_t)atoi(buffer);
    
    /* Step 1: Enable RTS/CTS via net_mgmt */
    rts_params.enable = 1;
    ret = net_mgmt(NET_REQUEST_WIFI_QCOM_SET_RTS_CTS, g_wifi_ctx.iface,
                   &rts_params, sizeof(rts_params));
    if (ret) {
        return QAT_Response_Str(QAT_RC_ERROR, "+ANTIINF:Enable RTS/CTS fail");
    }
    
    /* Step 2: Set RTS rate via net_mgmt */
    ret = net_mgmt(NET_REQUEST_WIFI_QCOM_SET_RTS_RATE, g_wifi_ctx.iface,
                   &rate_params, sizeof(rate_params));
    if (ret) {
        return QAT_Response_Str(QAT_RC_ERROR, "+ANTIINF:fix RTS rate fail (0:1Mbps 1:6Mbps 2:12Mbps)");
    }
    
    /* Step 3: Set EDCA parameters (optimized for anti-interference) via net_mgmt */
    edca_params.qid = 0xff;  /* All queues (0-7) */
    edca_params.aifsn = 0x3;
    edca_params.cw_min = 0x2;  /* cwmin = 2^2 - 1 = 3 */
    edca_params.cw_max = 0x4;  /* cwmax = 2^4 - 1 = 15 */
    edca_params.txop_limit = 200;
    
    ret = net_mgmt(NET_REQUEST_WIFI_QCOM_SET_EDCA_PARAM_CFG, g_wifi_ctx.iface,
                   &edca_params, sizeof(edca_params));
    if (ret) {
        return QAT_Response_Str(QAT_RC_ERROR, "+ANTIINF:set edca param fail");
    }
    
    /* Step 4: Set PER upper threshold via net_mgmt */
    threshold_params.threshold = 60;
    ret = net_mgmt(NET_REQUEST_WIFI_QCOM_SET_THRESHOLD, g_wifi_ctx.iface,
                   &threshold_params, sizeof(threshold_params));
    if (ret) {
        return QAT_Response_Str(QAT_RC_ERROR, "+ANTIINF:set per upper threshold fail");
    }
    
    /* Step 5: Set BA window timing via net_mgmt */
    ba_params.ack_timeout = 128;  /* 128us, should be less than 4096 */
    ba_params.delay = 10;         /* 10 * 2 * SM clock cycles, should be less than 64 */
    
    ret = net_mgmt(NET_REQUEST_WIFI_QCOM_SET_BA_WIN_TIMING, g_wifi_ctx.iface,
                   &ba_params, sizeof(ba_params));
    if (ret) {
        return QAT_Response_Str(QAT_RC_ERROR, "+ANTIINF:set ba window fail");
    }
    
    /* Step 6: Set slot time via net_mgmt */
    slot_params.slot_time = 20;  /* 20us */
    ret = net_mgmt(NET_REQUEST_WIFI_QCOM_SET_SLOT_TIME, g_wifi_ctx.iface,
                   &slot_params, sizeof(slot_params));
    if (ret) {
        return QAT_Response_Str(QAT_RC_ERROR, "+ANTIINF:set slot time fail");
    }
    
    LOG_INF("Anti-interference configured: RTS rate=%u", rate_params.rts_rate);
    return QAT_Response_Str(QAT_RC_OK, NULL);
}

/* AT+ANTIINF? - Query antiinf status */
static cat_return_state cmd_wlan_antiinf_query(const struct cat_command *cmd, uint8_t *data,
                                               size_t *data_size, const size_t max_data_size)
{
    struct qcom_wifi_get_rts_cts_params rts_params;
    struct qcom_wifi_get_rts_rate_params rate_params;
    struct qcom_wifi_get_edca_param_cfg_params edca_params;
    struct qcom_wifi_get_threshold_params threshold_params;
    struct qcom_wifi_get_ba_win_timing_params ba_params;
    struct qcom_wifi_get_slot_time_params slot_params;
    char buffer[WLAN_STR_BUFFER_LENGTH];
    int ret;
    int offset = 0;
    
    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
    
    if (!g_wifi_ctx.wlan_enabled || !g_wifi_ctx.iface) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+ANTIINF:WiFi not enabled");
    }
    
    k_mutex_unlock(&g_wifi_ctx.mutex);
    
    /* Get RTS/CTS enable status via net_mgmt */
    ret = net_mgmt(NET_REQUEST_WIFI_QCOM_GET_RTS_CTS, g_wifi_ctx.iface,
                   &rts_params, sizeof(rts_params));
    if (ret) {
        return QAT_Response_Str(QAT_RC_ERROR, "+ANTIINF:Failed to get RTS/CTS status");
    }
    
    /* Get RTS rate via net_mgmt */
    ret = net_mgmt(NET_REQUEST_WIFI_QCOM_GET_RTS_RATE, g_wifi_ctx.iface,
                   &rate_params, sizeof(rate_params));
    if (ret) {
        return QAT_Response_Str(QAT_RC_ERROR, "+ANTIINF:Failed to get RTS rate");
    }
    
    /* Get EDCA parameters (queue 0xff for all queues) via net_mgmt */
    edca_params.qid = 0xff;
    ret = net_mgmt(NET_REQUEST_WIFI_QCOM_GET_EDCA_PARAM_CFG, g_wifi_ctx.iface,
                   &edca_params, sizeof(edca_params));
    if (ret) {
        return QAT_Response_Str(QAT_RC_ERROR, "+ANTIINF:Failed to get EDCA parameters");
    }
    
    /* Get PER threshold via net_mgmt */
    ret = net_mgmt(NET_REQUEST_WIFI_QCOM_GET_THRESHOLD, g_wifi_ctx.iface,
                   &threshold_params, sizeof(threshold_params));
    if (ret) {
        return QAT_Response_Str(QAT_RC_ERROR, "+ANTIINF:Failed to get threshold");
    }
    
    /* Get BA window timing via net_mgmt */
    ret = net_mgmt(NET_REQUEST_WIFI_QCOM_GET_BA_WIN_TIMING, g_wifi_ctx.iface,
                   &ba_params, sizeof(ba_params));
    if (ret) {
        return QAT_Response_Str(QAT_RC_ERROR, "+ANTIINF:Failed to get BA window timing");
    }
    
    /* Get slot time via net_mgmt */
    ret = net_mgmt(NET_REQUEST_WIFI_QCOM_GET_SLOT_TIME, g_wifi_ctx.iface,
                   &slot_params, sizeof(slot_params));
    if (ret) {
        return QAT_Response_Str(QAT_RC_ERROR, "+ANTIINF:Failed to get slot time");
    }
    
    /* Format output to match reference: enable/disable,rts_rate,qid,cw_min,cw_max,threshold,ack_timeout,delay*2,slot_time */
    /* Check if RTS is enabled: if rate is non-zero, consider it enabled */
    const char *enable_str = (rts_params.enable == 1 || rate_params.rts_rate != 0) ? "enable" : "disable";
    
    offset = snprintf(buffer, sizeof(buffer), "+ANTIINF:%s,%u,%u,%u,%u,%u,%u,%u,%u",
                     enable_str,
                     rate_params.rts_rate,
                     edca_params.qid,
                     edca_params.cw_min,
                     edca_params.cw_max,
                     threshold_params.threshold,
                     ba_params.ack_timeout,
                     2 * ba_params.delay,  // delay * 2 as in reference
                     slot_params.slot_time);
    
    return QAT_Response_Str(QAT_RC_OK, buffer);
}

/* AT+EDCA - Show usage */
static cat_return_state cmd_wlan_edca_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK,
        "AT+EDCA=setparam,<qtid:0~7 or 255>,<aifsn>,<cwmin:exp>,<cwmax:exp>,<txop_limit>\r\n"
        "AT+EDCA=getparam,<qtid:0~7 or 255>");
}

/* AT+EDCA=setparam,<qid>,<aifsn>,<cwmin>,<cwmax>,<txop>
 * AT+EDCA=getparam,<qid> */
static cat_return_state cmd_wlan_edca_set(const struct cat_command *cmd, const uint8_t *data,
                                          const size_t data_size, const size_t args_num)
{
    char *token, *saveptr;
    char data_copy[128];
    char buffer[100];
    int ret;

    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);

    if (!g_wifi_ctx.wlan_enabled || !g_wifi_ctx.iface) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+EDCA:Enable WiFi first");
    }

    k_mutex_unlock(&g_wifi_ctx.mutex);

    if (data_size >= sizeof(data_copy)) {
        return QAT_Response_Str(QAT_RC_ERROR, "+EDCA:Parameters too long");
    }

    memcpy(data_copy, data, data_size);
    data_copy[data_size] = '\0';

    /* First token: "setparam" or "getparam" */
    token = strtok_r(data_copy, ",", &saveptr);
    if (!token) {
        return QAT_Response_Str(QAT_RC_ERROR,
            "+EDCA:AT+EDCA=setparam,<qtid:0~7 or 255>,<aifsn>,<cwmin:exp>,<cwmax:exp>,<txop_limit>");
    }

    if (strncasecmp(token, "setparam", 8) == 0) {
        struct qcom_wifi_set_edca_param_cfg_params params;

        token = strtok_r(NULL, ",", &saveptr);
        if (!token) return QAT_Response_Str(QAT_RC_ERROR, "+EDCA:Missing qid");
        params.qid = (uint8_t)atoi(token);

        token = strtok_r(NULL, ",", &saveptr);
        if (!token) return QAT_Response_Str(QAT_RC_ERROR, "+EDCA:Missing aifsn");
        params.aifsn = (uint8_t)atoi(token);

        token = strtok_r(NULL, ",", &saveptr);
        if (!token) return QAT_Response_Str(QAT_RC_ERROR, "+EDCA:Missing cw_min");
        params.cw_min = (uint16_t)atoi(token);

        token = strtok_r(NULL, ",", &saveptr);
        if (!token) return QAT_Response_Str(QAT_RC_ERROR, "+EDCA:Missing cw_max");
        params.cw_max = (uint16_t)atoi(token);

        token = strtok_r(NULL, ",", &saveptr);
        if (!token) return QAT_Response_Str(QAT_RC_ERROR, "+EDCA:Missing txop_limit");
        params.txop_limit = (uint16_t)atoi(token);

        ret = net_mgmt(NET_REQUEST_WIFI_QCOM_SET_EDCA_PARAM_CFG, g_wifi_ctx.iface,
                       &params, sizeof(params));
        if (ret) {
            LOG_ERR("Set EDCA parameters failed: %d", ret);
            return QAT_Response_Str(QAT_RC_ERROR,
                "+EDCA:set edca param fail, check the wlan connection\r\n"
                "set qid=0xff for all queues; set qid=0-7 for single queue");
        }
        LOG_INF("EDCA set: qid=%u aifsn=%u cw_min=%u cw_max=%u txop=%u",
                params.qid, params.aifsn, params.cw_min, params.cw_max, params.txop_limit);
        return QAT_Response_Str(QAT_RC_OK, NULL);

    } else if (strncasecmp(token, "getparam", 8) == 0) {
        struct qcom_wifi_get_edca_param_cfg_params params;

        token = strtok_r(NULL, ",", &saveptr);
        if (!token) {
            return QAT_Response_Str(QAT_RC_ERROR, "+EDCA:AT+EDCA=getparam,<qtid:0~7 or 255>");
        }
        params.qid = (uint8_t)atoi(token);

        ret = net_mgmt(NET_REQUEST_WIFI_QCOM_GET_EDCA_PARAM_CFG, g_wifi_ctx.iface,
                       &params, sizeof(params));
        if (ret) {
            snprintf(buffer, sizeof(buffer), "+EDCA:get edca param fail for qid %u", params.qid);
            return QAT_Response_Str(QAT_RC_ERROR, buffer);
        }
        snprintf(buffer, sizeof(buffer), "+EDCA:%u,%u,%u,%u,%u",
                 params.qid, params.aifsn, params.cw_min, params.cw_max, params.txop_limit);
        return QAT_Response_Str(QAT_RC_OK, buffer);

    } else {
        return QAT_Response_Str(QAT_RC_ERROR,
            "+EDCA:Unknown subcommand. Use setparam or getparam");
    }
}

/* AT+EDCCATHR - Show usage */
static cat_return_state cmd_wlan_edcca_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK,
        "AT+EDCCATHR=<EDCCA value, equals real value plus 100>\r\n"
        "AT+EDCCATHR?: get EDCCATHR");
}

/* AT+EDCCATHR=<value> - Set EDCCA threshold */
static cat_return_state cmd_wlan_edcca_set(const struct cat_command *cmd, const uint8_t *data,
                                               const size_t data_size, const size_t args_num)
{
    struct qcom_wifi_set_threshold_params params;
    char buffer[32];
    int ret;
    
    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
    
    if (!g_wifi_ctx.wlan_enabled || !g_wifi_ctx.iface) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+EDCCATHR:Enable WiFi first");
    }
    
    k_mutex_unlock(&g_wifi_ctx.mutex);
    
    /* Parse threshold value */
    if (data_size == 0 || data_size >= sizeof(buffer)) {
        return QAT_Response_Str(QAT_RC_ERROR, "+EDCCATHR:Invalid parameter");
    }
    
    memcpy(buffer, data, data_size);
    buffer[data_size] = '\0';
    params.threshold = (uint32_t)atoi(buffer);
    
    /* Set threshold via net_mgmt */
    ret = net_mgmt(NET_REQUEST_WIFI_QCOM_SET_THRESHOLD, g_wifi_ctx.iface,
                   &params, sizeof(params));
    if (ret) {
        LOG_ERR("Set threshold failed: %d", ret);
        return QAT_Response_Str(QAT_RC_ERROR, "+EDCCATHR:Failed to set threshold");
    }
    
    LOG_INF("Threshold set to %u", params.threshold);
    return QAT_Response_Str(QAT_RC_OK, NULL);
}

/* AT+EDCCATHR? - Query EDCCA threshold */
static cat_return_state cmd_wlan_edcca_query(const struct cat_command *cmd, uint8_t *data,
                                                 size_t *data_size, const size_t max_data_size)
{
    struct qcom_wifi_get_threshold_params params;
    char buffer[WLAN_RESPONSE_BUFFER_LENGTH];
    int ret;
    
    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
    
    if (!g_wifi_ctx.wlan_enabled || !g_wifi_ctx.iface) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+EDCCATHR:WiFi not enabled");
    }
    
    k_mutex_unlock(&g_wifi_ctx.mutex);
    
    /* Get threshold via net_mgmt */
    ret = net_mgmt(NET_REQUEST_WIFI_QCOM_GET_THRESHOLD, g_wifi_ctx.iface,
                   &params, sizeof(params));
    if (ret) {
        return QAT_Response_Str(QAT_RC_ERROR, "+EDCCATHR:Failed to get threshold");
    }
    
    snprintf(buffer, sizeof(buffer), "+EDCCATHR:%u", params.threshold);
    return QAT_Response_Str(QAT_RC_OK, buffer);
}

/* AT+BMISSTHR - Show usage */
static cat_return_state cmd_wlan_bmiss_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK,
        "AT+BMISSTHR=<bmiss_threshold: 0~255>\r\n"
        "AT+BMISSTHR?: get BMISSTHR");
}

/* AT+BMISSTHR=<threshold> - Set beacon miss threshold */
static cat_return_state cmd_wlan_bmiss_set(const struct cat_command *cmd, const uint8_t *data,
                                           const size_t data_size, const size_t args_num)
{
    struct qcom_wifi_set_bmiss_threshold_params params;
    char buffer[32];
    int ret;
    
    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
    
    if (!g_wifi_ctx.wlan_enabled || !g_wifi_ctx.iface) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+BMISSTHR:Enable WiFi first");
    }
    
    k_mutex_unlock(&g_wifi_ctx.mutex);
    
    /* Parse threshold value */
    if (data_size == 0 || data_size >= sizeof(buffer)) {
        return QAT_Response_Str(QAT_RC_ERROR, "+BMISSTHR:Invalid parameter");
    }
    
    memcpy(buffer, data, data_size);
    buffer[data_size] = '\0';
    params.threshold = (uint32_t)atoi(buffer);
    
    /* Set beacon miss threshold via net_mgmt */
    ret = net_mgmt(NET_REQUEST_WIFI_QCOM_SET_BMISS_THRESHOLD, g_wifi_ctx.iface,
                   &params, sizeof(params));
    if (ret) {
        LOG_ERR("Set beacon miss threshold failed: %d", ret);
        return QAT_Response_Str(QAT_RC_ERROR, "+BMISSTHR:Failed to set threshold");
    }
    
    LOG_INF("Beacon miss threshold set to %u", params.threshold);
    return QAT_Response_Str(QAT_RC_OK, NULL);
}

/* AT+BMISSTHR? - Query beacon miss threshold */
static cat_return_state cmd_wlan_bmiss_query(const struct cat_command *cmd, uint8_t *data,
                                             size_t *data_size, const size_t max_data_size)
{
    struct qcom_wifi_get_bmiss_threshold_params params;
    char buffer[WLAN_RESPONSE_BUFFER_LENGTH];
    int ret;
    
    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
    
    if (!g_wifi_ctx.wlan_enabled || !g_wifi_ctx.iface) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+BMISSTHR:WiFi not enabled");
    }
    
    k_mutex_unlock(&g_wifi_ctx.mutex);
    
    /* Get beacon miss threshold via net_mgmt */
    ret = net_mgmt(NET_REQUEST_WIFI_QCOM_GET_BMISS_THRESHOLD, g_wifi_ctx.iface,
                   &params, sizeof(params));
    if (ret) {
        return QAT_Response_Str(QAT_RC_ERROR, "+BMISSTHR:Failed to get threshold");
    }
    
    snprintf(buffer, sizeof(buffer), "+BMISSTHR:%u", params.threshold);
    return QAT_Response_Str(QAT_RC_OK, buffer);
}

/* AT+DTIMINTERVAL - Show usage */
static cat_return_state cmd_wlan_listen_interval_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK,
        "AT+DTIMINTERVAL=<val:0~65535>\r\n"
        "AT+DTIMINTERVAL?: get current listen interval");
}

/* AT+DTIMINTERVAL=<val> - Set STA listen interval (0~65535 beacon intervals) */
static cat_return_state cmd_wlan_listen_interval_set(const struct cat_command *cmd, const uint8_t *data,
                                                    const size_t data_size, const size_t args_num)
{
    struct wifi_ps_params params = {0};
    char buf[16];
    long interval;
    int ret;

    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);

    if (!g_wifi_ctx.wlan_enabled || !g_wifi_ctx.iface) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+DTIMINTERVAL:Enable WiFi first");
    }

    k_mutex_unlock(&g_wifi_ctx.mutex);

    if (data_size == 0 || data_size >= sizeof(buf)) {
        return QAT_Response_Str(QAT_RC_ERROR, "+DTIMINTERVAL:Invalid parameter");
    }

    memcpy(buf, data, data_size);
    buf[data_size] = '\0';
    interval = atol(buf);

    if (interval < WIFI_LISTEN_INTERVAL_MIN || interval > WIFI_LISTEN_INTERVAL_MAX) {
        return QAT_Response_Str(QAT_RC_ERROR,
            "+DTIMINTERVAL:Out of range (0~65535)");
    }

    params.listen_interval = (uint16_t)interval;
    params.type = WIFI_PS_PARAM_LISTEN_INTERVAL;

    ret = net_mgmt(NET_REQUEST_WIFI_PS, g_wifi_ctx.iface, &params, sizeof(params));
    if (ret) {
        LOG_ERR("Set listen interval failed: %d, reason: %s", ret,
                wifi_ps_get_config_err_code_str(params.fail_reason));
        return QAT_Response_Str(QAT_RC_ERROR, "+DTIMINTERVAL:Failed to set");
    }

    LOG_INF("Listen interval set to %hu", params.listen_interval);
    return QAT_Response_Str(QAT_RC_OK, NULL);
}

/* AT+DTIMINTERVAL? - Query current listen interval */
static cat_return_state cmd_wlan_listen_interval_query(const struct cat_command *cmd, uint8_t *data,
                                                      size_t *data_size, const size_t max_data_size)
{
    struct wifi_ps_config config = {0};
    char buffer[WLAN_RESPONSE_BUFFER_LENGTH];
    int ret;

    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);

    if (!g_wifi_ctx.wlan_enabled || !g_wifi_ctx.iface) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+DTIMINTERVAL:WiFi not enabled");
    }

    k_mutex_unlock(&g_wifi_ctx.mutex);

    ret = net_mgmt(NET_REQUEST_WIFI_PS_CONFIG, g_wifi_ctx.iface, &config, sizeof(config));
    if (ret) {
        return QAT_Response_Str(QAT_RC_ERROR, "+DTIMINTERVAL:Failed to get config");
    }

    snprintf(buffer, sizeof(buffer), "+DTIMINTERVAL:%hu", config.ps_params.listen_interval);
    return QAT_Response_Str(QAT_RC_OK, buffer);
}

/* AT+WEVT=<enable> - Enable/Disable event reporting */
static cat_return_state cmd_wlan_event_set(const struct cat_command *cmd, const uint8_t *data,
                                           const size_t data_size, const size_t args_num)
{
    char buffer[32];
    int enable;
    
    /* Parse enable parameter */
    if (data_size == 0 || data_size >= sizeof(buffer)) {
        return QAT_Response_Str(QAT_RC_ERROR, "+WEVT:Invalid parameter");
    }
    
    memcpy(buffer, data, data_size);
    buffer[data_size] = '\0';
    enable = atoi(buffer);
    
    if (enable != 0 && enable != 1) {
        return QAT_Response_Str(QAT_RC_ERROR, "+WEVT:Parameter must be 0 or 1");
    }
    
    enable_event_reporting = (enable != 0);
    
    LOG_INF("Event reporting %s", enable_event_reporting ? "enabled" : "disabled");
    return QAT_Response_Str(QAT_RC_OK, NULL);
}

/* AT+WEVT? - Query event reporting status */
static cat_return_state cmd_wlan_event_query(const struct cat_command *cmd, uint8_t *data,
                                             size_t *data_size, const size_t max_data_size)
{
    char buffer[WLAN_RESPONSE_BUFFER_LENGTH];
    
    snprintf(buffer, sizeof(buffer), "+WEVT:%d", enable_event_reporting ? 1 : 0);
    return QAT_Response_Str(QAT_RC_OK, buffer);
}

/* AT+WEVT - Show usage */
static cat_return_state cmd_wlan_event_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK, "+WEVT=0/1 (0=disable, 1=enable event reporting)");
}

/*-------------------------------------------------------------------------
 * WPA Configuration Commands
 *-----------------------------------------------------------------------*/

/* AT+CWWPA=<wpa_ver>,<ucipher>,<mcipher> - Set WPA parameters */
static cat_return_state cmd_wlan_wpa_params_set(const struct cat_command *cmd, const uint8_t *data,
                                                const size_t data_size, const size_t args_num)
{
    char *token, *saveptr;
    char data_copy[128];
    char *wpa_ver;
    char *ucipher = NULL;
    char buffer[WLAN_RESPONSE_BUFFER_LENGTH];
    uint32_t auth_mode;
    uint32_t cipher;
    
    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
    
    if (!g_wifi_ctx.wlan_enabled || !g_wifi_ctx.iface) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+CWWPA:Enable WiFi first");
    }
    
    k_mutex_unlock(&g_wifi_ctx.mutex);
    
    /* Parse parameters: wpa_ver,ucipher,mcipher */
    if (data_size >= sizeof(data_copy)) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CWWPA:Parameters too long");
    }
    
    memcpy(data_copy, data, data_size);
    data_copy[data_size] = '\0';
    
    /* Parse WPA version */
    token = strtok_r(data_copy, ",", &saveptr);
    if (!token) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CWWPA:Missing WPA version");
    }
    wpa_ver = token;
    
    /* Convert to uppercase for comparison */
    for (char *p = wpa_ver; *p; p++) {
        *p = toupper((unsigned char)*p);
    }
    
    /* Map WPA version to auth_mode */
    if (strcmp(wpa_ver, "WPA") == 0) {
        auth_mode = 0x02;  /* WPA-PSK */
    } else if (strcmp(wpa_ver, "WPA2") == 0) {
        auth_mode = 0x04;  /* WPA2-PSK */
    } else if (strcmp(wpa_ver, "SAE") == 0) {
        auth_mode = 0x400;  /* WPA3-SAE */
    } else if (strcmp(wpa_ver, "SAE_WPA2") == 0) {
        auth_mode = 0x404;  /* WPA2/WPA3 mixed */
    } else if (strcmp(wpa_ver, "SAE_WPA2_WPA") == 0) {
        auth_mode = 0x406;  /* WPA/WPA2/WPA3 mixed */
    } else {
        snprintf(buffer, sizeof(buffer), "+CWWPA:FAIL, %s", wpa_ver);
        return QAT_Response_Str(QAT_RC_ERROR, buffer);
    }
    
    /* For mixed SAE modes, cipher is auto (0) */
    if ((strcmp(wpa_ver, "SAE_WPA2") == 0) || 
        (strcmp(wpa_ver, "SAE_WPA2_WPA") == 0)) {
        cipher = 0;  /* Auto cipher */
    } else {
        /* Parse ucipher */
        token = strtok_r(NULL, ",", &saveptr);
        if (!token) {
            return QAT_Response_Str(QAT_RC_ERROR, "+CWWPA:Missing ucipher");
        }
        ucipher = token;
        
        /* Convert ucipher to uppercase */
        for (char *p = ucipher; *p; p++) {
            *p = toupper((unsigned char)*p);
        }
        
        /* Parse mcipher */
        token = strtok_r(NULL, ",", &saveptr);
        if (!token) {
            return QAT_Response_Str(QAT_RC_ERROR, "+CWWPA:Missing mcipher");
        }
        char *mcipher = token;
        
        /* Convert mcipher to uppercase */
        for (char *p = mcipher; *p; p++) {
            *p = toupper((unsigned char)*p);
        }
        
        /* Verify ucipher and mcipher are the same */
        if (strcmp(ucipher, mcipher) != 0) {
            return QAT_Response_Str(QAT_RC_ERROR, "+CWWPA:invalid ucipher mcipher, should be same");
        }
        
        /* Map cipher type to QAPI enum values */
        if (strcmp(ucipher, "TKIP") == 0) {
            cipher = 2;  /* QAPI_WLAN_CRYPT_TKIP_CRYPT_E = 2 */
        } else if (strcmp(ucipher, "CCMP") == 0) {
            cipher = 3;  /* QAPI_WLAN_CRYPT_AES_CRYPT_E = 3 */
        } else {
            return QAT_Response_Str(QAT_RC_ERROR, "+CWWPA:invalid ucipher mcipher, should be TKIP or CCMP");
        }
    }
    
    /* Store security parameters directly in context (no net_mgmt call) */
    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
    g_wifi_ctx.auth_mode = auth_mode;
    g_wifi_ctx.cipher = cipher;
    g_wifi_ctx.security_set = true;
    k_mutex_unlock(&g_wifi_ctx.mutex);
    
    LOG_INF("WPA parameters saved to context: auth_mode=0x%x, cipher=0x%x", auth_mode, cipher);
    return QAT_Response_Str(QAT_RC_OK, NULL);
}

/* AT+CWWPA - Show usage */
static cat_return_state cmd_wlan_wpa_params_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK, 
        "+CWWPA=WPA/WPA2/SAE,CCMP,CCMP/TKIP,TKIP. For mix mode, \r\n+CWWPA=SAE_WPA2/SAE_WPA2_WPA,CCMP and TKIP are not required");
}

/* AT+CWPWD=<passphrase> - Set WPA passphrase */
static cat_return_state cmd_wlan_wpa_passphrase_set(const struct cat_command *cmd, const uint8_t *data,
                                                    const size_t data_size, const size_t args_num)
{
    char buffer[WLAN_RESPONSE_BUFFER_LENGTH];
    
    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
    
    if (!g_wifi_ctx.wlan_enabled || !g_wifi_ctx.iface) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+CWPWD:Enable WiFi first");
    }
    
    /* Validate passphrase length (8-64 characters) */
    if (data_size < 8 || data_size > 64) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        snprintf(buffer, sizeof(buffer), "+CWPWD:FAIL,%zu", data_size);
        return QAT_Response_Str(QAT_RC_ERROR, buffer);
    }
    
    /* If length is 64, verify it's hexadecimal */
    if (data_size == 64) {
        for (size_t i = 0; i < data_size; i++) {
            if (!isxdigit((int)data[i])) {
                k_mutex_unlock(&g_wifi_ctx.mutex);
                return QAT_Response_Str(QAT_RC_ERROR, 
                    "+CWPWD:passphrase in hex, please enter [0-9] or [A-F]");
            }
        }
    }
    
    /* Store passphrase directly in context (no net_mgmt call) */
    memcpy(g_wifi_ctx.passphrase, data, data_size);
    g_wifi_ctx.passphrase[data_size] = '\0';
    g_wifi_ctx.passphrase_len = data_size;
    
    k_mutex_unlock(&g_wifi_ctx.mutex);
    
    LOG_INF("WPA passphrase saved to context (length: %zu)", data_size);
    return QAT_Response_Str(QAT_RC_OK, NULL);
}

/* AT+CWPWD - Show usage */
static cat_return_state cmd_wlan_wpa_passphrase_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK, 
        "+CWPWD=<PASSWORD>(The PASSWORD length should be between 8 and 64)");
}

/*-------------------------------------------------------------------------
 * Operating Mode Commands
 *-----------------------------------------------------------------------*/

/* AT+CWMODE - Show usage */
static cat_return_state cmd_wlan_mode_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK, "AT+CWMODE=<station|ap|ap_sta>");
}

/* AT+CWMODE=<station|ap|ap_sta> - Set WiFi operating mode */
static cat_return_state cmd_wlan_mode_set(const struct cat_command *cmd, const uint8_t *data,
                                            const size_t data_size, const size_t args_num)
{
    struct qcom_wifi_set_op_mode_params params;
    char mode_str[16];
    char buffer[WLAN_RESPONSE_BUFFER_LENGTH];
    struct net_if *iface;
    int ret;
    char dev_mode;

    if (data_size == 0 || data_size >= sizeof(mode_str)) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CWMODE:Invalid parameter");
    }

    memcpy(mode_str, data, data_size);
    mode_str[data_size] = '\0';

    if (strcmp(mode_str, "station") == 0) {
        iface = net_if_get_wifi_sta();
        dev_mode = DEV_MODE_STATION_E;
    } else if (strcmp(mode_str, "ap") == 0 ) {
        iface = net_if_get_wifi_sap();
        dev_mode = DEV_MODE_AP_E;
    } else if (strcmp(mode_str, "ap_sta") == 0) {
        iface = net_if_get_wifi_sap();
        dev_mode = DEV_MODE_AP_STA_E;
    } else {
        snprintf(buffer, sizeof(buffer), "+CWMODE:unknown mode %s", mode_str);
        return QAT_Response_Str(QAT_RC_ERROR, buffer);
    }

    if (!iface) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CWMODE:Failed to get WiFi interface");
    }

    /* ap_sta requires the SAP device to already be in AP mode. Transition: station -> ap -> ap_sta */
    if (strcmp(mode_str, "ap_sta") == 0) {
        params.opmode = "ap";
        params.hidden_ssid = "0";
        ret = net_mgmt(NET_REQUEST_WIFI_QCOM_SET_OPERATION_MODE, iface,
                       &params, sizeof(params));
        if (ret) {
            LOG_ERR("Pre-set ap mode for ap_sta failed: %d", ret);
            return QAT_Response_Str(QAT_RC_ERROR, "+CWMODE:Failed to set operating mode");
        }
    }

    params.opmode = mode_str;
    params.hidden_ssid = "0";

    ret = net_mgmt(NET_REQUEST_WIFI_QCOM_SET_OPERATION_MODE, iface,
                   &params, sizeof(params));
    if (ret) {
        LOG_ERR("Set op mode to %s failed: %d", mode_str, ret);
        return QAT_Response_Str(QAT_RC_ERROR, "+CWMODE:Failed to set operating mode");
    }

    g_wifi_ctx.op_mode = dev_mode;
    LOG_INF("Operating mode set to %s", mode_str);
    return QAT_Response_Str(QAT_RC_OK, NULL);
}

/* AT+CWMODE? - Query operating mode */
static cat_return_state cmd_wlan_mode_query(const struct cat_command *cmd, uint8_t *data,
                                              size_t *data_size, const size_t max_data_size)
{
    char buffer[WLAN_RESPONSE_BUFFER_LENGTH];
    char *mode_str;

    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);

    if (!g_wifi_ctx.wlan_enabled || !g_wifi_ctx.iface) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+CWMODE:WiFi not enabled");
    }

    k_mutex_unlock(&g_wifi_ctx.mutex);

    switch(g_wifi_ctx.op_mode) {
    case DEV_MODE_STATION_E:
        mode_str="STATION";
        break;
    case DEV_MODE_AP_E:
        mode_str="AP";
        break;
    case DEV_MODE_AP_STA_E:
        mode_str="AP_STA";
        break;
    default:
        mode_str="unknown";
        break;
    }
    snprintf(buffer, sizeof(buffer), "+CWMODE:%s", mode_str);
    return QAT_Response_Str(QAT_RC_OK, buffer);
}

/*-------------------------------------------------------------------------
 * Power Save Commands
 *-----------------------------------------------------------------------*/

/* AT+PS - Show usage */
static cat_return_state cmd_ps_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK,
        "AT+PS=<enable>,<timeout_ms>\r\n"
        "  enable: 1=enable BMPS, 0=disable BMPS\r\n"
        "  timeout_ms: idle timeout in milliseconds (0 means never timeout)");
}

/* AT+PS=<enable>,<timeout_ms> - Enable/Disable power save */
static cat_return_state cmd_ps_set(const struct cat_command *cmd, const uint8_t *data,
                                          const size_t data_size, const size_t args_num)
{
    struct qcom_wifi_pm_bmps_params bmps_params;
    struct qcom_wifi_pm_rx_filter_params rx_filter_params;
    char buf[64];
    char *token, *saveptr;
    int enable;
    uint32_t timeout_ms = 0;
    int ret;

    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);

    if (!g_wifi_ctx.wlan_enabled || !g_wifi_ctx.iface) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+PS:Enable WiFi first");
    }

    k_mutex_unlock(&g_wifi_ctx.mutex);

    if (data_size == 0 || data_size >= sizeof(buf)) {
        return QAT_Response_Str(QAT_RC_ERROR, "+PS:Invalid parameter");
    }
    memcpy(buf, data, data_size);
    buf[data_size] = '\0';

    /* Parse enable parameter */
    token = strtok_r(buf, ",", &saveptr);
    if (!token) {
        return QAT_Response_Str(QAT_RC_ERROR, "+PS:Missing enable parameter");
    }
    enable = atoi(token);

    if (enable != 0 && enable != 1) {
        return QAT_Response_Str(QAT_RC_ERROR, "+PS:Enable must be 0 or 1");
    }

    if (enable) {
        /* Parse timeout parameter */
        token = strtok_r(NULL, ",", &saveptr);
        if (!token) {
            return QAT_Response_Str(QAT_RC_ERROR, "+PS:Missing timeout parameter");
        }
        timeout_ms = (uint32_t)atoi(token);
    }

    /* Step 1: Start/Stop timeout timer */
    if (timeout_ms > 0) {
        /* Start timer for power save timeout */
        k_timer_start(&ps_timeout_timer, K_MSEC(timeout_ms), K_NO_WAIT);
        LOG_INF("Power save timeout timer started: %u ms", timeout_ms);
    }
    
    /* Step 2: Set RX filter (similar to reference implementation) */
    rx_filter_params.enable = (uint8_t)enable;
    rx_filter_params.bmps_rx_filter_cb = NULL;  /* No custom filter callback */
    ret = net_mgmt(NET_REQUEST_WIFI_PM_QCOM_SET_RX_FILTER_IN_BMPS, g_wifi_ctx.iface,
                   &rx_filter_params, sizeof(rx_filter_params));
    if (ret) {
        LOG_ERR("Set RX filter failed: %d", ret);
        return QAT_Response_Str(QAT_RC_ERROR, "+PS:Failed to set RX filter");
    }
    
    /* Step 3: Enable/Disable power save */
    bmps_params.enable = (uint8_t)enable;
    ret = net_mgmt(NET_REQUEST_WIFI_PM_QCOM_SET_BMPS_ENABLE, g_wifi_ctx.iface,
                   &bmps_params, sizeof(bmps_params));
    if (ret) {
        LOG_ERR("Set BMPS enable failed: %d", ret);
        return QAT_Response_Str(QAT_RC_ERROR, "+PS:Failed to set BMPS enable");
    }

    /* Step 4: Release PM lock if enabling power save and lock is active */
    if (enable && pm_policy_state_lock_is_active(PM_STATE_SUSPEND_TO_RAM, PM_ALL_SUBSTATES)) {
        pm_policy_state_lock_put(PM_STATE_SUSPEND_TO_RAM, PM_ALL_SUBSTATES);
        LOG_INF("PM_STATE_SUSPEND_TO_RAM lock released to allow low power state");
    }

    /* Step 5: Clear WiFi device busy flag so S2RAM can proceed.
     * PM_DEVICE_ACTION_RESUME sets pm_device_busy to defer sleep until WiFi
     * re-initializes. By the time AT+PS=1 is processed, WiFi has fully resumed.
     * BMPS beacon cycles are reported as PM_WLAN_ACTIVITY_ACTIVE, so the
     * activity callback never fires IDLE to clear this flag automatically. */
    if (enable) {
        const struct device *wifi_dev = net_if_get_device(g_wifi_ctx.iface);
        if (wifi_dev && pm_device_is_busy(wifi_dev)) {
            pm_device_busy_clear(wifi_dev);
            LOG_INF("WiFi device busy cleared for S2RAM entry");
        }
    }

    LOG_INF("Power save %s, idle_timeout=%u ms", enable ? "enabled" : "disabled", timeout_ms);

    if (enable) {
        return QAT_Response_Str(QAT_RC_OK, "+PS: entry.");
    }
    return QAT_Response_Str(QAT_RC_OK, NULL);
}

/*-------------------------------------------------------------------------
 * BCMC RX Filter Callback (used by AT+BCMCFLT)
 *-----------------------------------------------------------------------*/
static bool at_wakeup_cb_bcmc_filter(uint16_t type, bool bm_cast, void *wifi_frame, uint16_t len)
{
    if (bm_cast) {
        if (len < (AT_WIFI_MAC_HDR_LEN + AT_LLC_SNAP_HDR_LEN)) {
            return false;
        }

        const uint8_t *llc_snap_header = (const uint8_t *)wifi_frame + AT_WIFI_MAC_HDR_LEN;

        if (llc_snap_header[6] != 0x08 || llc_snap_header[7] != 0x00) {
            return true;
        }

        const uint8_t *ip_frame = (const uint8_t *)wifi_frame + AT_WIFI_MAC_HDR_LEN + AT_LLC_SNAP_HDR_LEN;
        const struct net_ipv4_hdr *ip = (const struct net_ipv4_hdr *)ip_frame;

        if (ip->proto != IPPROTO_UDP) {
            return true;
        }

        const struct net_udp_hdr *udp = (const struct net_udp_hdr *)(ip_frame + NET_IPV4H_LEN);
        uint16_t dst_port = ntohs(udp->dst_port);

        for (int i = 0; i < AT_BCMC_WHITELIST_LEN; i++) {
            if (at_udp_whitelist_arr[i] && dst_port == (uint16_t)at_udp_whitelist_arr[i]) {
                return true;
            }
        }
        return false;
    }
    return true;
}

/*-------------------------------------------------------------------------
 * Power Save Inactivity Time Command (AT+PSINACTIVTYTIME)
 *-----------------------------------------------------------------------*/

/* AT+PSINACTIVTYTIME - Show usage */
static cat_return_state cmd_ps_wlan_inactivity_time_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK,
        "AT+PSINACTIVTYTIME=<idle_time_ms>\r\n"
        "  Set max idle time before entering BMPS(DTIM) sleep");
}

/* AT+PSINACTIVTYTIME=<ms> - Set power save inactivity timeout */
static cat_return_state cmd_ps_wlan_inactivity_time_set(const struct cat_command *cmd, const uint8_t *data,
                                                   const size_t data_size, const size_t args_num)
{
    struct wifi_ps_params params = {0};
    char buf[32];
    uint32_t idle_timeout;

    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
    if (!g_wifi_ctx.wlan_enabled || !g_wifi_ctx.iface) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+PSIDLT:Enable WiFi first");
    }
    k_mutex_unlock(&g_wifi_ctx.mutex);

    if (data_size == 0 || data_size >= sizeof(buf)) {
        return QAT_Response_Str(QAT_RC_ERROR, "+PSIDLT:Invalid parameter");
    }

    memcpy(buf, data, data_size);
    buf[data_size] = '\0';
    idle_timeout = (uint32_t)atoi(buf);

    if (idle_timeout == 0) {
        return QAT_Response_Str(QAT_RC_ERROR, "+PSIDLT:idle_time_ms cannot be 0");
    }

    params.type = WIFI_PS_PARAM_TIMEOUT;
    params.timeout_ms = idle_timeout;
    net_mgmt(NET_REQUEST_WIFI_PS, g_wifi_ctx.iface, &params, sizeof(params));

    LOG_INF("BMPS idle timeout set to %u ms", idle_timeout);
    return QAT_Response_Str(QAT_RC_OK, NULL);
}

/*-------------------------------------------------------------------------
 * Power Save Ignore BC/MC Command (AT+PSIGBC)
 *-----------------------------------------------------------------------*/

/* AT+PSIGBC - Show usage */
static cat_return_state cmd_ps_wlan_ignore_bcmc_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK,
        "AT+PSIGBC=<0/1>\r\n"
        "  1: ignore BC/MC frames in BMPS, 0: do not ignore");
}

/* AT+PSIGBC=<0/1> - Set power save ignore BC/MC */
static cat_return_state cmd_ps_wlan_ignore_bcmc_set(const struct cat_command *cmd, const uint8_t *data,
                                                      const size_t data_size, const size_t args_num)
{
    struct qcom_wifi_pm_ignore_bc_mc_params params = {0};
    char buf[8];

    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
    if (!g_wifi_ctx.wlan_enabled || !g_wifi_ctx.iface) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+PSIGBC:Enable WiFi first");
    }
    k_mutex_unlock(&g_wifi_ctx.mutex);

    if (data_size == 0 || data_size >= sizeof(buf)) {
        return QAT_Response_Str(QAT_RC_ERROR, "+PSIGBC:Invalid parameter");
    }

    memcpy(buf, data, data_size);
    buf[data_size] = '\0';
    params.enable = (uint8_t)atoi(buf);

    net_mgmt(NET_REQUEST_WIFI_PM_QCOM_IGNORE_BC_MC_IN_BMPS, g_wifi_ctx.iface,
             &params, sizeof(params));

    LOG_INF("BMPS ignore BC/MC set to %u", params.enable);
    return QAT_Response_Str(QAT_RC_OK, NULL);
}

/*-------------------------------------------------------------------------
 * BCMC RX Filter Enable Command (AT+BCMCFLT)
 *-----------------------------------------------------------------------*/

/* AT+BCMCFLT - Show usage */
static cat_return_state cmd_ps_wlan_bcmc_filter_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK,
        "AT+PSBCMCFLT=<0/1>\r\n"
        "  1: enable BCMC RX filter with UDP whitelist, 0: disable");
}

/* AT+BCMCFLT=<0/1> - Disable/Enable BCMC RX filter */
static cat_return_state cmd_ps_wlan_bcmc_filter_set(const struct cat_command *cmd, const uint8_t *data,
                                                 const size_t data_size, const size_t args_num)
{
    struct qcom_wifi_pm_rx_filter_params rx_filter = {0};
    char buf[8];

    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
    if (!g_wifi_ctx.wlan_enabled || !g_wifi_ctx.iface) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+PSBCMCFLT:Enable WiFi first");
    }
    k_mutex_unlock(&g_wifi_ctx.mutex);

    if (data_size == 0 || data_size >= sizeof(buf)) {
        return QAT_Response_Str(QAT_RC_ERROR, "+PSBCMCFLT:Invalid parameter");
    }

    memcpy(buf, data, data_size);
    buf[data_size] = '\0';
    rx_filter.enable = (uint8_t)atoi(buf);
    rx_filter.bmps_rx_filter_cb = rx_filter.enable ? at_wakeup_cb_bcmc_filter : NULL;

    net_mgmt(NET_REQUEST_WIFI_PM_QCOM_SET_RX_FILTER_IN_BMPS, g_wifi_ctx.iface,
             &rx_filter, sizeof(rx_filter));

    LOG_INF("BCMC RX filter %s", rx_filter.enable ? "enabled" : "disabled");
    return QAT_Response_Str(QAT_RC_OK, NULL);
}

/*-------------------------------------------------------------------------
 * BCMC UDP Port Whitelist Command (AT+BCMCLST)
 *-----------------------------------------------------------------------*/

/* AT+BCMCLST - Show usage */
static cat_return_state cmd_ps_wlan_bcmc_list_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK,
        "AT+PSBCMCLST=<add>,<port>\r\n"
        "  add: 1=add UDP dst port to whitelist, 0=remove\r\n"
        "  port: destination UDP port number (e.g. 7777)\r\n"
        "AT+PSBCMCLST?: query current whitelist");
}

/* AT+BCMCLST=<add>,<port> - Add/remove UDP dst port from BCMC whitelist */
static cat_return_state cmd_ps_wlan_bcmc_list_set(const struct cat_command *cmd, const uint8_t *data,
                                               const size_t data_size, const size_t args_num)
{
    char buf[32];
    char *token, *saveptr;
    int add;
    uint32_t port;

    if (data_size == 0 || data_size >= sizeof(buf)) {
        return QAT_Response_Str(QAT_RC_ERROR, "+PSBCMCLST:Invalid parameter");
    }

    memcpy(buf, data, data_size);
    buf[data_size] = '\0';

    token = strtok_r(buf, ",", &saveptr);
    if (!token) {
        return QAT_Response_Str(QAT_RC_ERROR, "+PSBCMCLST:Missing add parameter");
    }
    add = atoi(token);

    token = strtok_r(NULL, ",", &saveptr);
    if (!token) {
        return QAT_Response_Str(QAT_RC_ERROR, "+PSBCMCLST:Missing port parameter");
    }
    port = (uint32_t)atoi(token);

    if (add) {
        for (int i = 0; i < AT_BCMC_WHITELIST_LEN; i++) {
            if (at_udp_whitelist_arr[i] == port) {
                return QAT_Response_Str(QAT_RC_OK, NULL);
            }
        }
        for (int i = 0; i < AT_BCMC_WHITELIST_LEN; i++) {
            if (at_udp_whitelist_arr[i] == 0) {
                at_udp_whitelist_arr[i] = port;
                LOG_INF("Added UDP port %u to BCMC whitelist[%d]", port, i);
                return QAT_Response_Str(QAT_RC_OK, NULL);
            }
        }
        return QAT_Response_Str(QAT_RC_ERROR, "+PSBCMCLST:Whitelist full");
    } else {
        for (int i = 0; i < AT_BCMC_WHITELIST_LEN; i++) {
            if (at_udp_whitelist_arr[i] == port) {
                at_udp_whitelist_arr[i] = 0;
                LOG_INF("Removed UDP port %u from BCMC whitelist[%d]", port, i);
                return QAT_Response_Str(QAT_RC_OK, NULL);
            }
        }
        return QAT_Response_Str(QAT_RC_ERROR, "+PSBCMCLST:Port not found");
    }
}

/* AT+BCMCLST? - Query BCMC UDP whitelist */
static cat_return_state cmd_ps_wlan_bcmc_list_query(const struct cat_command *cmd, uint8_t *data,
                                                 size_t *data_size, const size_t max_data_size)
{
    char buffer[WLAN_RESPONSE_BUFFER_LENGTH];
    int offset = 0;

    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "+PSBCMCLST:");
    for (int i = 0; i < AT_BCMC_WHITELIST_LEN; i++) {
        if (i > 0) {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset, ",");
        }
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%u", at_udp_whitelist_arr[i]);
    }
    return QAT_Response_Str(QAT_RC_OK, buffer);
}



/*-------------------------------------------------------------------------
 * WiFi Credential Persistence (AT+CWSAVE / AT+CWLOAD)
 *-----------------------------------------------------------------------*/
#ifdef CONFIG_QAT_WIFI_CRED

#define WIFI_CRED_PATH   "/lfs/wifi.conf"
#define WIFI_CRED_MAGIC  "WCFG"
#define WIFI_CRED_VER    1

typedef struct {
    uint8_t  magic[4];
    uint8_t  version;
    uint8_t  ssid_len;
    uint8_t  passphrase_len;
    uint8_t  _pad;
    uint32_t auth_mode;
    uint32_t cipher;
    char     ssid[WIFI_SSID_MAX_LEN + 1];
    char     passphrase[65];
} wifi_cred_t;

/*
 * Load WiFi credentials from /lfs/wifi.conf into g_wifi_ctx.
 * Returns WIFI_CRED_OK on success, or a negative wifi_cred_err_t on failure.
 * Caller must NOT hold g_wifi_ctx.mutex.
 */
static wifi_cred_err_t load_wifi_cred_from_flash(void)
{
    wifi_cred_t cred;
    struct fs_file_t file;

    fs_file_t_init(&file);
    int ret = fs_open(&file, WIFI_CRED_PATH, FS_O_READ);
    if (ret != 0) {
        return WIFI_CRED_ERR_NOFILE;
    }
    ret = fs_read(&file, &cred, sizeof(cred));
    fs_close(&file);
    if (ret != (int)sizeof(cred)) {
        return WIFI_CRED_ERR_IO;
    }
    if (memcmp(cred.magic, WIFI_CRED_MAGIC, 4) != 0 || cred.version != WIFI_CRED_VER) {
        return WIFI_CRED_ERR_MAGIC;
    }
    if (cred.ssid_len > WIFI_SSID_MAX_LEN || cred.passphrase_len > 64) {
        return WIFI_CRED_ERR_LEN;
    }

    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
    g_wifi_ctx.ssid_len       = cred.ssid_len;
    memcpy(g_wifi_ctx.ssid, cred.ssid, cred.ssid_len);
    g_wifi_ctx.ssid[cred.ssid_len] = '\0';
    g_wifi_ctx.passphrase_len = cred.passphrase_len;
    memcpy(g_wifi_ctx.passphrase, cred.passphrase, cred.passphrase_len);
    g_wifi_ctx.passphrase[cred.passphrase_len] = '\0';
    g_wifi_ctx.auth_mode    = cred.auth_mode;
    g_wifi_ctx.cipher       = cred.cipher;
    g_wifi_ctx.security_set = (cred.passphrase_len > 0);
    k_mutex_unlock(&g_wifi_ctx.mutex);

    LOG_INF("load_wifi_cred_from_flash: loaded SSID=%.*s", cred.ssid_len, cred.ssid);
    return WIFI_CRED_OK;
}

static cat_return_state cmd_wlan_cwsave_exec(const struct cat_command *cmd)
{
    wifi_cred_t cred;
    struct fs_file_t file;
    int ret;

    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);

    if (g_wifi_ctx.ssid_len == 0) {
        k_mutex_unlock(&g_wifi_ctx.mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+CWSAVE: No SSID configured\r\n");
    }

    memset(&cred, 0, sizeof(cred));
    memcpy(cred.magic, WIFI_CRED_MAGIC, 4);
    cred.version = WIFI_CRED_VER;
    cred.ssid_len = g_wifi_ctx.ssid_len;
    cred.passphrase_len = g_wifi_ctx.passphrase_len;
    cred.auth_mode = g_wifi_ctx.auth_mode;
    cred.cipher = g_wifi_ctx.cipher;
    memcpy(cred.ssid, g_wifi_ctx.ssid, g_wifi_ctx.ssid_len);
    memcpy(cred.passphrase, g_wifi_ctx.passphrase, g_wifi_ctx.passphrase_len);

    k_mutex_unlock(&g_wifi_ctx.mutex);

    fs_file_t_init(&file);
    ret = fs_open(&file, WIFI_CRED_PATH, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
    if (ret != 0) {
        LOG_ERR("CWSAVE: fs_open failed: %d", ret);
        return QAT_Response_Str(QAT_RC_ERROR, "+CWSAVE: Failed to open file\r\n");
    }

    ret = fs_write(&file, &cred, sizeof(cred));
    fs_close(&file);

    if (ret != (int)sizeof(cred)) {
        LOG_ERR("CWSAVE: fs_write failed: %d", ret);
        return QAT_Response_Str(QAT_RC_ERROR, "+CWSAVE: Failed to write credentials\r\n");
    }

    LOG_INF("CWSAVE: saved SSID=%.*s to %s", cred.ssid_len, cred.ssid, WIFI_CRED_PATH);
    return QAT_Response_Str(QAT_RC_OK, NULL);
}

static cat_return_state cmd_wlan_cwload_exec(const struct cat_command *cmd)
{
    /*
     * Intentionally does not check wlan_enabled: CWLOAD only writes to
     * g_wifi_ctx memory and does not touch hardware or iface. This allows
     * pre-populating credentials before AT+CWENABLE, so the host can call
     * CWENABLE -> CWJAP without re-sending all parameters after a reboot.
     */
    wifi_cred_err_t err = load_wifi_cred_from_flash();
    if (err != WIFI_CRED_OK) {
        const char *msg =
            (err == WIFI_CRED_ERR_NOFILE) ? "+CWLOAD: No saved credentials\r\n" :
            (err == WIFI_CRED_ERR_LEN)    ? "+CWLOAD: Credential file corrupted\r\n" :
                                            "+CWLOAD: Invalid credential file\r\n";
        return QAT_Response_Str(QAT_RC_ERROR, msg);
    }

    char response[WIFI_SSID_MAX_LEN + 32];
    k_mutex_lock(&g_wifi_ctx.mutex, K_FOREVER);
    snprintf(response, sizeof(response), "+CWLOAD: SSID=%.*s\r\n",
             g_wifi_ctx.ssid_len, g_wifi_ctx.ssid);
    k_mutex_unlock(&g_wifi_ctx.mutex);
    return QAT_Response_Str(QAT_RC_OK, response);
}

#endif /* CONFIG_QAT_WIFI_CRED */

/*-------------------------------------------------------------------------
 * Command List
 *-----------------------------------------------------------------------*/
static struct cat_command qat_wlan_cmds[] = {
    {
        .name = "+WIFISP",
        .description = "Probe WiFi capability",
        .run = cmd_wlan_wifisp_exec,
    },
    {
        .name = "+CWENABLE",
        .description = "Enable WiFi",
        .run = cmd_wlan_enable_exec,
    },
    {
        .name = "+CWQABLE",
        .description = "Disable WiFi",
        .run = cmd_wlan_disable_exec,
    },
    {
        .name = "+CWMODE",
        .description = "Set/Query WiFi operating mode (station/ap/ap_sta)",
        .run = cmd_wlan_mode_exec,
        .read = cmd_wlan_mode_query,
        .write = cmd_wlan_mode_set,
    },
    {
        .name = "+CWLAP",
        .description = "Scan WiFi networks",
        .run = cmd_wlan_scan_exec,
        .write = cmd_wlan_scan_set,
    },
    {
        .name = "+CWWPA",
        .description = "Set WPA parameters",
        .run = cmd_wlan_wpa_params_exec,
        .write = cmd_wlan_wpa_params_set,
    },
    {
        .name = "+CWPWD",
        .description = "Set WPA passphrase",
        .run = cmd_wlan_wpa_passphrase_exec,
        .write = cmd_wlan_wpa_passphrase_set,
    },
    {
        .name = "+CWJAP",
        .description = "Connect to WiFi",
        .run = cmd_wlan_connect_exec,
        .read = cmd_wlan_connect_query,
        .write = cmd_wlan_connect_set,
    },
    {
        .name = "+CWQAP",
        .description = "Disconnect from WiFi",
        .run = cmd_wlan_disconnect_exec,
    },
    {
        .name = "+CWSOFTAP",
        .description = "Enable AP mode",
        .run = cmd_wlan_ap_enable_exec,
        .write = cmd_wlan_ap_enable_set,
    },
    {
        .name = "+WEVT",
        .description = "Enable/Disable event reporting",
        .run = cmd_wlan_event_exec,
        .read = cmd_wlan_event_query,
        .write = cmd_wlan_event_set,
    },
    /* Advanced WiFi Configuration Commands */
    {
        .name = "+CWPHYMODE",
        .description = "Set/Query PHY mode",
        .run = cmd_wlan_phy_mode_exec,
        .read = cmd_wlan_phy_mode_query,
        .write = cmd_wlan_phy_mode_set,
    },
    {
        .name = "+CWCOUNTRY",
        .description = "Set/Query regulatory domain",
        .run = cmd_wlan_reg_domain_exec,
        .read = cmd_wlan_reg_domain_query,
        .write = cmd_wlan_reg_domain_set,
    },
    {
        .name = "+ANTIINF",
        .description = "Set/Query Anti-interference",
        .run = cmd_wlan_antiinf_exec,
        .read = cmd_wlan_antiinf_query,
        .write = cmd_wlan_antiinf_set,
    },
    {
        .name = "+EDCA",
        .description = "Set/Query EDCA parameters",
        .run = cmd_wlan_edca_exec,
        .write = cmd_wlan_edca_set,
    },
    {
        .name = "+EDCCATHR",
        .description = "Set/Query EDCCA threshold",
        .run = cmd_wlan_edcca_exec,
        .read = cmd_wlan_edcca_query,
        .write = cmd_wlan_edcca_set,
    },
    {
        .name = "+BMISSTHR",
        .description = "Set/Query beacon miss threshold",
        .run = cmd_wlan_bmiss_exec,
        .read = cmd_wlan_bmiss_query,
        .write = cmd_wlan_bmiss_set,
    },
    {
        .name = "+DTIMINTERVAL",
        .description = "Set/Query STA listen interval (0~65535 beacon intervals)",
        .run = cmd_wlan_listen_interval_exec,
        .read = cmd_wlan_listen_interval_query,
        .write = cmd_wlan_listen_interval_set,
    },
    {
        .name = "+PS",
        .description = "Enable/Disable power save",
        .run = cmd_ps_exec,
        .write = cmd_ps_set,
    },
    {
        .name = "+PSINACTIVTYTIME",
        .description = "Set PS wlan inactivity time(in ms).",
        .run = cmd_ps_wlan_inactivity_time_exec,
        .write = cmd_ps_wlan_inactivity_time_set,
    },
    {
        .name = "+PSIGBC",
        .description = "Set PS ignore BC/MC",
        .run = cmd_ps_wlan_ignore_bcmc_exec,
        .write = cmd_ps_wlan_ignore_bcmc_set,
    },
    {
        .name = "+PSBCMCFLT",
        .description = "Enable/Disable PS BCMC RX filter",
        .run = cmd_ps_wlan_bcmc_filter_exec,
        .write = cmd_ps_wlan_bcmc_filter_set,
    },
    {
        .name = "+PSBCMCLST",
        .description = "Manage PS BCMC UDP port whitelist",
        .run = cmd_ps_wlan_bcmc_list_exec,
        .read = cmd_ps_wlan_bcmc_list_query,
        .write = cmd_ps_wlan_bcmc_list_set,
    },
#ifdef CONFIG_QAT_WIFI_CRED
    {
        .name = "+CWSAVE",
        .description = "Save WiFi credentials (SSID/passphrase/auth) to /lfs/wifi.conf",
        .run = cmd_wlan_cwsave_exec,
    },
    {
        .name = "+CWLOAD",
        .description = "Load WiFi credentials from /lfs/wifi.conf into context",
        .run = cmd_wlan_cwload_exec,
    },
#endif
};

static struct cat_command_group qat_wlan_cmd_group = {
    .name = "QAT_WLAN",
    .cmd = qat_wlan_cmds,
    .cmd_num = ARRAY_SIZE(qat_wlan_cmds),
};

/*-------------------------------------------------------------------------
 * Initialization
 *-----------------------------------------------------------------------*/
struct cat_command_group *qat_wlan_get_command_group(void)
{
    LOG_DBG("Registering QAT WiFi commands");
    return &qat_wlan_cmd_group;
}

static int qat_wlan_init(void)
{
    /* Initialize context */
    k_mutex_init(&g_wifi_ctx.mutex);
    g_wifi_ctx.wlan_enabled = false;
    g_wifi_ctx.connected = false;
    g_wifi_ctx.iface = NULL;
    g_wifi_ctx.scan_result_count = 0;
    g_wifi_ctx.scan_in_progress = false;

    /* Initialize delayed work for DHCP start and stop */
    k_work_init_delayable(&dhcp_start_work.work, dhcp_start_work_handler);
    k_work_init(&dhcp_stop_work.work, dhcp_stop_work_handler);
    
    /* Initialize delayed work for scan complete event */
    k_work_init_delayable(&scan_complete_work, scan_complete_work_handler);

    /* Note: Event callbacks are now registered dynamically in cmd_wlan_enable_exec
     * This matches the QAPI pattern where qapi_WLAN_Set_Callback is called in Enable command
     */
    callbacks_registered = false;

    LOG_INF("QAT WiFi module initialized");
    return 0;
}

/* Automatically register this command group with QAT */
QAT_REGISTER_CMD_GROUP(qat_wlan_get_command_group, "WLAN");

/* Initialize at system startup */
SYS_INIT(qat_wlan_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
