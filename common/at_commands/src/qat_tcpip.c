/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_nm.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/net/dns_resolve.h>
#include <zephyr/net/icmp.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/net_core.h>
#include <zephyr/net/sntp.h>
#include <zephyr/posix/fcntl.h>
#include <zephyr/random/random.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/rtc.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <cat.h>
#include "qat_api.h"

LOG_MODULE_REGISTER(qat_tcpip, LOG_LEVEL_INF);

/*-------------------------------------------------------------------------
 * Definitions
 *-----------------------------------------------------------------------*/
#define MAX_CONNECTIONS 4
#define INVALID_FD -1
#define INVALID_LINKID -1
#define CIRCULAR_BUFFER_SIZE 2048
/*
 * MTU-derived data size: matches QAT_MAX_MTU_PACKET_SIZE = 1370 from FreeRTOS reference.
 * Constraint: INPUT_BUFFER_SIZE + len("\r\n+IPD:S,TCP,0,XXXX,\r\n") ≤ CONFIG_RING0_BUF_SIZE(1400)
 *   => 1370 + 22 = 1392 ≤ 1400  (8 bytes margin)
 */
#define INPUT_BUFFER_SIZE 1370
#define RECV_TIMEOUT_MS 1000
#define PING_DEFAULT_COUNT 4
#define PING_DEFAULT_DELAY_MS 500
#define PING_DEFAULT_SIZE 32
#define PING_ID 0xACAB

/* Ping context */
static struct {
    struct k_work_delayable work;
    struct net_icmp_ctx icmp;
    union {
        struct sockaddr_in addr4;
        struct sockaddr_in6 addr6;
        struct sockaddr addr;
    };
    struct net_if *iface;
    char host[128];
    uint32_t count;
    uint32_t interval;
    uint32_t sequence;
    uint32_t sent;
    uint32_t received;
    uint32_t sent_time;
    uint16_t payload_size;
    bool active;
    bool waiting_reply;
    struct k_sem done_sem;
} ping_ctx;

/*-------------------------------------------------------------------------
 * Type Definitions
 *-----------------------------------------------------------------------*/
typedef enum {
    CONN_STATE_IDLE = 0,
    CONN_STATE_CONNECTED,
    CONN_STATE_LISTENING,
} conn_state_t;

typedef enum {
    PROTOCOL_INVALID = -1,
    PROTOCOL_TCP = 0,
    PROTOCOL_UDP,
    PROTOCOL_TCPv6,
    PROTOCOL_UDPv6,
} protocol_type_t;

typedef enum {
    RECV_MODE_ACTIVE = 0,
    RECV_MODE_PASSIVE,
} recv_mode_t;

/* Circular buffer for passive receive mode */
typedef struct {
    uint8_t buffer[CIRCULAR_BUFFER_SIZE];
    size_t head;
    size_t tail;
    size_t size;
    struct k_mutex mutex;
} circular_buffer_t;

/* Queue element for passive mode notifications */
typedef struct {
    int link_id;
    size_t data_len;
} queue_elem_t;

/* Connection information */
typedef struct {
    int sock_fd;
    conn_state_t state;
    protocol_type_t type;
    char remote_ip[INET6_ADDRSTRLEN];
    uint16_t remote_port;
    uint16_t local_port;
    bool is_server;
    recv_mode_t recv_mode;
    circular_buffer_t *recv_buf;
    struct k_work_delayable recv_work;
    bool thread_quit;
    int id;
} connection_info_t;

/* Server configuration */
typedef struct {
    int mode;
    bool close_server;
    uint16_t port;
    protocol_type_t type;
    bool accept_new_peer;
} server_config_t;

/*-------------------------------------------------------------------------
 * Global Variables
 *-----------------------------------------------------------------------*/
static connection_info_t g_client_conns[MAX_CONNECTIONS];
static connection_info_t g_listen_clients[MAX_CONNECTIONS];
static connection_info_t g_listen_udp_clients[MAX_CONNECTIONS];
static K_MUTEX_DEFINE(conn_mutex);
K_MSGQ_DEFINE(client_queue, sizeof(queue_elem_t), 10, 4);
K_MSGQ_DEFINE(tcp_server_queue, sizeof(queue_elem_t), 10, 4);
K_MSGQ_DEFINE(udp_server_queue, sizeof(queue_elem_t), 10, 4);

static int tcp_listen_fd = INVALID_FD;
static int udp_listen_fd = INVALID_FD;
static bool tcp_server_running = false;
static bool udp_server_running = false;
static bool tcp_server_accept_new_client = true;
static server_config_t tcp_config = {0};
static server_config_t udp_config = {0};

static bool ipd_message_print_flag = true;
static bool server_ipd_message_print_flag = true;
static bool udp_server_ipd_message_print_flag = true;
static uint8_t is_passthrough_mode = 0;

/* Work queue thread — three recv work handlers run serially on the system work queue */
static uint8_t s_recv_buf[INPUT_BUFFER_SIZE + 1];
static char    s_recv_response[INPUT_BUFFER_SIZE + 64];

/* AT command thread — CIPSENDDATA and CIPRECVDATA are serialized; share one data buffer */
static uint8_t s_at_data_buf[INPUT_BUFFER_SIZE + 1];
static char    s_ciprecv_response[INPUT_BUFFER_SIZE + 64];

static void cipsend_prompt_work_handler(struct k_work *work)
{
    QAT_Output(3, ">\r\n");
}
static K_WORK_DEFINE(cipsend_prompt_work, cipsend_prompt_work_handler);

/* CIPMODE state */
static int cipmode = 0;  /* 0: normal mode, 1: passthrough mode */

/* IPv6 state */
static bool ipv6_enabled = false;

/* DNS client state (shared by CIPSTA set and DNSC module) */
#define DNSC_MAX_SERVERS 2
#define DNSC_SERVER_LEN  64

static struct {
    char addr[DNSC_SERVER_LEN];
    bool valid;
} dnsc_servers[DNSC_MAX_SERVERS];
/* Default pool */
#define QAT_DHCPS_DEFAULT_POOL_START "192.168.111.2"
#define QAT_DHCPS_DEFAULT_POOL_END   "192.168.111.100"
#define QAT_DHCPS_DEFAULT_LEASE_MIN  1440

/* DHCP server state (SoftAP DHCPv4 server) */
static struct {
    bool running;
    bool pool_configured;
    struct in_addr pool_start_ip;
    struct in_addr pool_end_ip;
    uint32_t lease_time_minute; /* per AT guide */
} dhcp_server_config = {
    .running = false,
    .pool_configured = false,
    .lease_time_minute = QAT_DHCPS_DEFAULT_LEASE_MIN,
};

/* Zephyr DHCPv4 server lease time is typically compile-time via Kconfig.
 * We keep lease_time_minute for AT reporting/compatibility.
 */

/* Helpers for /24 calculation used by CIPDHCPV4S (per request) */
static inline uint32_t in4_to_u32_host(const struct in_addr *a)
{
    return ntohl(a->s_addr);
}

static inline void u32_host_to_in4(uint32_t v, struct in_addr *a)
{
    a->s_addr = htonl(v);
}

static inline void dhcps_calc_server_ip_netmask_24(const struct in_addr *pool_start,
                                                   struct in_addr *server_ip,
                                                   struct in_addr *netmask)
{
    uint32_t ps = in4_to_u32_host(pool_start);
    uint32_t network = ps & 0xFFFFFF00u;

    u32_host_to_in4(network | 0x00000001u, server_ip); /* x.x.x.1 */
    u32_host_to_in4(0xFFFFFF00u, netmask);             /* 255.255.255.0 */
}

/* IPv6 prefix configuration (SoftAP IPv6 prefix, per AT guide: /64, no compression) */
#define QAT_IPV6PREFIX_LEN_MAX 19

static struct {
    char prefix_str[40];        /* e.g. "2001:db8:0:0" */
    struct in6_addr prefix;     /* parsed "::" expanded base (prefix::) */
    bool configured;
} ipv6_prefix_config = {
    .prefix_str = "2001:db8:0:0", /* keep a default like freertos demo */
    .configured = false,
};

/* Validate IPv6 prefix string per AT guide:
 * - prefix length is fixed 64
 * - prefix format doesn't allow compression (no "::")
 * - valid examples: 2001:db8:0:0 , 2001:0:0:1
 * This matches FreeRTOS demo behavior: exactly 3 ':' (4 groups), no leading/trailing ':',
 * and only hex digits and ':'.
 */
static bool cipv6prefix_is_valid_prefix(const char *str)
{
    if (!str) {
        return false;
    }

    size_t len = strlen(str);
    if (len == 0 || len > QAT_IPV6PREFIX_LEN_MAX) {
        return false;
    }

    /* no leading/trailing ':' */
    if (str[0] == ':' || str[len - 1] == ':') {
        return false;
    }

    int colon_count = 0;
    int last_colon_pos = -1;

    for (size_t i = 0; i < len; i++) {
        char c = str[i];

        if (c == ':') {
            colon_count++;
            /* disallow "::" */
            if (last_colon_pos == (int)i - 1) {
                return false;
            }
            last_colon_pos = (int)i;
        } else {
            bool is_hex = ((c >= '0' && c <= '9') ||
                           (c >= 'a' && c <= 'f') ||
                           (c >= 'A' && c <= 'F'));
            if (!is_hex) {
                return false;
            }
        }
    }

    /* require exactly 4 groups => 3 colons */
    if (colon_count != 3) {
        return false;
    }

    return true;
}

/* CIPSEND state */
static struct {
    int link_id;
    size_t max_len;
    size_t total_sent;
    bool exit_length_valid; /* false when len == 0 */
} cipsend_state = {
    .link_id = INVALID_LINKID,
    .max_len = 0,
    .total_sent = 0,
    .exit_length_valid = true,
};

/*-------------------------------------------------------------------------
 * Circular Buffer Functions
 *-----------------------------------------------------------------------*/
static circular_buffer_t *circular_buffer_create(void)
{
    circular_buffer_t *cb = k_malloc(sizeof(circular_buffer_t));
    if (!cb) {
        return NULL;
    }

    memset(cb, 0, sizeof(circular_buffer_t));
    k_mutex_init(&cb->mutex);
    return cb;
}

static void circular_buffer_destroy(circular_buffer_t *cb)
{
    if (cb) {
        k_free(cb);
    }
}

static int circular_buffer_write(circular_buffer_t *cb, const uint8_t *data, size_t len)
{
    if (!cb || !data) {
        return -EINVAL;
    }

    k_mutex_lock(&cb->mutex, K_FOREVER);

    if (cb->size + len > CIRCULAR_BUFFER_SIZE) {
        k_mutex_unlock(&cb->mutex);
        return -ENOMEM;
    }

    for (size_t i = 0; i < len; i++) {
        cb->buffer[cb->head] = data[i];
        cb->head = (cb->head + 1) % CIRCULAR_BUFFER_SIZE;
    }
    cb->size += len;

    k_mutex_unlock(&cb->mutex);
    return 0;
}

static int circular_buffer_read(circular_buffer_t *cb, uint8_t *data, size_t len)
{
    if (!cb || !data) {
        return -EINVAL;
    }

    k_mutex_lock(&cb->mutex, K_FOREVER);

    if (cb->size < len) {
        k_mutex_unlock(&cb->mutex);
        return -ENODATA;
    }

    for (size_t i = 0; i < len; i++) {
        data[i] = cb->buffer[cb->tail];
        cb->tail = (cb->tail + 1) % CIRCULAR_BUFFER_SIZE;
    }
    cb->size -= len;

    k_mutex_unlock(&cb->mutex);
    return 0;
}

/*-------------------------------------------------------------------------
 * Helper Functions
 *-----------------------------------------------------------------------*/
static const char *protocol_type_to_at_name(protocol_type_t type);
static bool queue_pop_front(struct k_msgq *queue, queue_elem_t *elem);
static bool queue_push_front(struct k_msgq *queue, const queue_elem_t *elem);
static void cleanup_server_queue_entries(protocol_type_t type, int link_id);
static void clear_msgq(struct k_msgq *queue);
static int find_free_server_slot(connection_info_t *conns);
static void cleanup_server_conn_entry(connection_info_t *conns, protocol_type_t queue_type, int link_id);

static void cleanup_client_conn(int link_id)
{
    if (link_id < 0 || link_id >= MAX_CONNECTIONS) {
        return;
    }

    g_client_conns[link_id].id = INVALID_LINKID;
    g_client_conns[link_id].sock_fd = INVALID_FD;
    g_client_conns[link_id].state = CONN_STATE_IDLE;
    g_client_conns[link_id].type = PROTOCOL_INVALID;
    g_client_conns[link_id].recv_mode = RECV_MODE_ACTIVE;
    g_client_conns[link_id].thread_quit = false;
    memset(g_client_conns[link_id].remote_ip, 0, sizeof(g_client_conns[link_id].remote_ip));
    g_client_conns[link_id].remote_port = 0;
    g_client_conns[link_id].local_port = 0;

    if (g_client_conns[link_id].recv_buf) {
        circular_buffer_destroy(g_client_conns[link_id].recv_buf);
        g_client_conns[link_id].recv_buf = NULL;
    }
}

#ifndef NT_DEV_AP_ID
#define NT_DEV_AP_ID 0
#endif
#ifndef NT_DEV_STA_ID
#define NT_DEV_STA_ID 1
#endif

/* Get the Station interface (STA) */
static struct net_if *get_sta_iface(void)
{
    return net_if_get_first_wifi();
}

/* Get the SoftAP interface (SAP) */
static struct net_if *get_ap_iface(void)
{
    return net_if_get_wifi_sap();
}

static struct net_if *get_default_iface(void)
{
    return get_sta_iface();
}

static struct net_if *get_iface_by_qat_id(int id)
{
    switch (id) {
    case NT_DEV_AP_ID:
        return get_ap_iface();
    case NT_DEV_STA_ID:
        return get_sta_iface();
    default:
        return NULL;
    }
}

/*-------------------------------------------------------------------------
 * SAP SLAAC - Router Advertisement (RA) Implementation
 *
 * When AT+CIPV6=1 and AT+CIPV6PREFIX=<prefix> are configured, the SoftAP
 * acts as an IPv6 router: it periodically sends unsolicited RA to ff02::1
 * and responds to RS from clients. Clients use the RA prefix to
 * auto-configure SLAAC addresses (RFC 4862).
 *-----------------------------------------------------------------------*/

/* ICMPv6 type 133 = Router Solicitation (not exposed in public Zephyr headers) */
#ifndef NET_ICMPV6_RS
#define NET_ICMPV6_RS 133
#endif

#define SAP_RA_INTERVAL_MS          30000   /* 30 s between unsolicited RA */
#define SAP_RA_ROUTER_LIFETIME      1800    /* 30 min router lifetime       */
#define SAP_RA_VALID_LIFETIME       2592000 /* 30 days valid lifetime       */
#define SAP_RA_PREFERRED_LIFETIME   604800  /* 7 days preferred lifetime    */
#define SAP_RA_CUR_HOP_LIMIT        64

/*
 * Forward declarations of internal Zephyr IPv6/ICMPv6 functions.
 * These are compiled into the kernel image and accessible as symbols.
 */
extern int net_ipv6_create(struct net_pkt *pkt, const struct in6_addr *src,
                           const struct in6_addr *dst);
extern int net_icmpv6_create(struct net_pkt *pkt, uint8_t icmp_type, uint8_t icmp_code);
extern int net_ipv6_finalize(struct net_pkt *pkt, uint8_t next_header_proto);

/* RA-related structures (from zephyr/subsys/net/ip/icmpv6.h) */
struct sap_nd_ra_hdr {
    uint8_t  cur_hop_limit;
    uint8_t  flags;
    uint16_t router_lifetime;
    uint32_t reachable_time;
    uint32_t retrans_timer;
} __packed;

struct sap_nd_opt_hdr {
    uint8_t type;
    uint8_t len;
} __packed;

struct sap_nd_opt_prefix_info {
    uint8_t  prefix_len;
    uint8_t  flags;
    uint32_t valid_lifetime;
    uint32_t preferred_lifetime;
    uint32_t reserved;
    uint8_t  prefix[NET_IPV6_ADDR_SIZE];
} __packed;

#define SAP_ND_HOP_LIMIT        255
#define SAP_ND_OPT_SLLAO          1
#define SAP_ND_OPT_PREFIX_INFO    3
#define SAP_ND_NET_BUF_TIMEOUT    K_MSEC(100)

static struct k_work_delayable sap_ra_work;
static struct net_icmp_ctx     sap_rs_ctx;
static bool                    sap_ra_running;

/*
 * Build and send one RA on the SAP interface to ff02::1.
 * Uses Zephyr net_pkt API (net_ipv6_create / net_icmpv6_create /
 * net_ipv6_finalize) so that the ICMPv6 checksum is computed by the
 * kernel and the packet goes through the normal TX path.
 */
static int sap_send_ra(struct net_if *iface)
{
    if (!ipv6_prefix_config.configured) {
        return -ENOENT;
    }

    /* Find link-local source address on SAP iface */
    struct in6_addr src = IN6ADDR_ANY_INIT;
    struct net_if_ipv6 *ipv6 = iface->config.ip.ipv6;

    if (ipv6) {
        for (int i = 0; i < NET_IF_MAX_IPV6_ADDR; i++) {
            if (ipv6->unicast[i].is_used &&
                net_ipv6_is_ll_addr(&ipv6->unicast[i].address.in6_addr)) {
                src = ipv6->unicast[i].address.in6_addr;
                break;
            }
        }
    }

    if (net_ipv6_is_addr_unspecified(&src)) {
        LOG_WRN("SAP RA: no link-local addr on SAP iface, skip");
        return -EADDRNOTAVAIL;
    }

    /* Destination: ff02::1 (all-nodes multicast) */
    struct in6_addr dst;
    net_ipv6_addr_create_ll_allnodes_mcast(&dst);

    /* SLLAO option size: round up (type+len+addr) to 8-byte boundary */
    struct net_linkaddr *lladdr = net_if_get_link_addr(iface);
    uint8_t llao_len = 0;
    if (lladdr && lladdr->len > 0) {
        llao_len = (uint8_t)(((2U + lladdr->len + 7U) / 8U) * 8U);
    }

    /* Total ICMPv6 payload = RA body + SLLAO + Prefix Info option */
    size_t payload_len = sizeof(struct sap_nd_ra_hdr) +
                         llao_len +
                         sizeof(struct sap_nd_opt_hdr) +
                         sizeof(struct sap_nd_opt_prefix_info);

    /* Allocate packet */
    struct net_pkt *pkt = net_pkt_alloc_with_buffer(iface, payload_len,
                                                     AF_INET6, IPPROTO_ICMPV6,
                                                     SAP_ND_NET_BUF_TIMEOUT);
    if (!pkt) {
        LOG_ERR("SAP RA: failed to allocate packet");
        return -ENOMEM;
    }

    /* Hop limit = 255 (required for ND, RFC 4861 §6.1.2) */
    net_pkt_set_ipv6_hop_limit(pkt, SAP_ND_HOP_LIMIT);

    /* IPv6 header */
    if (net_ipv6_create(pkt, &src, &dst)) {
        goto drop;
    }

    /* ICMPv6 header (type=134 RA, code=0) */
    if (net_icmpv6_create(pkt, 134, 0)) {
        goto drop;
    }

    /* RA body */
    {
        struct sap_nd_ra_hdr ra = {
            .cur_hop_limit  = SAP_RA_CUR_HOP_LIMIT,
            .flags          = 0,   /* M=0, O=0: pure SLAAC */
            .router_lifetime = htons(SAP_RA_ROUTER_LIFETIME),
            .reachable_time = 0,
            .retrans_timer  = 0,
        };
        if (net_pkt_write(pkt, &ra, sizeof(ra))) {
            goto drop;
        }
    }

    /* SLLAO option */
    if (llao_len > 0) {
        struct sap_nd_opt_hdr opt = {
            .type = SAP_ND_OPT_SLLAO,
            .len  = llao_len >> 3,
        };
        if (net_pkt_write(pkt, &opt, sizeof(opt)) ||
            net_pkt_write(pkt, lladdr->addr, lladdr->len) ||
            net_pkt_memset(pkt, 0, llao_len - sizeof(opt) - lladdr->len)) {
            goto drop;
        }
    }

    /* Prefix Info option: /64, L=1 on-link, A=1 autonomous */
    {
        struct sap_nd_opt_hdr opt = {
            .type = SAP_ND_OPT_PREFIX_INFO,
            .len  = 4, /* 32 bytes */
        };
        struct sap_nd_opt_prefix_info pfx = {
            .prefix_len         = 64,
            .flags              = 0xC0, /* L=1, A=1 */
            .valid_lifetime     = htonl(SAP_RA_VALID_LIFETIME),
            .preferred_lifetime = htonl(SAP_RA_PREFERRED_LIFETIME),
            .reserved           = 0,
        };
        memcpy(pfx.prefix, ipv6_prefix_config.prefix.s6_addr, 16);
        memset(pfx.prefix + 8, 0, 8); /* keep /64 prefix, clear host part */

        if (net_pkt_write(pkt, &opt, sizeof(opt)) ||
            net_pkt_write(pkt, &pfx, sizeof(pfx))) {
            goto drop;
        }
    }

    /* Finalize: set IPv6 payload length + compute ICMPv6 checksum */
    net_pkt_cursor_init(pkt);
    if (net_ipv6_finalize(pkt, IPPROTO_ICMPV6)) {
        goto drop;
    }

    /* Send via normal TX path */
    if (net_send_data(pkt) < 0) {
        goto drop;
    }

    LOG_INF("SAP RA: sent RA prefix=%s/64", ipv6_prefix_config.prefix_str);
    return 0;

drop:
    net_pkt_unref(pkt);
    LOG_ERR("SAP RA: failed to send RA");
    return -EIO;
}

/* Periodic unsolicited RA work handler */
static void sap_ra_work_handler(struct k_work *work)
{
    struct net_if *iface = get_iface_by_qat_id(NT_DEV_AP_ID);

    if (!sap_ra_running || !ipv6_enabled || !iface ||
        !ipv6_prefix_config.configured) {
        return;
    }

    sap_send_ra(iface);
    k_work_reschedule((struct k_work_delayable *)work,
                      K_MSEC(SAP_RA_INTERVAL_MS));
}

/* RS handler: respond to Router Solicitation from clients on SAP iface */
static int handle_sap_rs_input(struct net_icmp_ctx *ctx,
                               struct net_pkt *pkt,
                               struct net_icmp_ip_hdr *hdr,
                               struct net_icmp_hdr *icmp_hdr,
                               void *user_data)
{
    struct net_if *iface = net_pkt_iface(pkt);

    if (!wifi_nm_iface_is_sap(iface) || !ipv6_enabled ||
        !ipv6_prefix_config.configured) {
        return 0;
    }

    LOG_INF("SAP RA: received RS from client, sending solicited RA");
    sap_send_ra(iface);
    return 0;
}

/* Start SAP RA: register RS handler + kick off periodic unsolicited RA */
static void sap_ra_start(void)
{
    if (sap_ra_running) {
        return;
    }

    int ret = net_icmp_init_ctx(&sap_rs_ctx, AF_INET6, NET_ICMPV6_RS, 0,
                                handle_sap_rs_input);
    if (ret < 0) {
        LOG_WRN("SAP RA: failed to register RS handler (%d)", ret);
    }

    sap_ra_running = true;
    k_work_init_delayable(&sap_ra_work, sap_ra_work_handler);
    /* Send first RA immediately, then every SAP_RA_INTERVAL_MS */
    k_work_reschedule(&sap_ra_work, K_NO_WAIT);
    LOG_INF("SAP RA: started (interval=%d ms)", SAP_RA_INTERVAL_MS);
}

/* Stop SAP RA: cancel periodic work + unregister RS handler */
static void sap_ra_stop(void)
{
    if (!sap_ra_running) {
        return;
    }

    sap_ra_running = false;
    k_work_cancel_delayable(&sap_ra_work);
    net_icmp_cleanup_ctx(&sap_rs_ctx);
    LOG_INF("SAP RA: stopped");
}

/*-------------------------------------------------------------------------
 * AT+CIPSTA - Network Configuration
 *-----------------------------------------------------------------------*/
static cat_return_state cmd_cipsta_exec(const struct cat_command *cmd)
{
    /* CIPSTA only applies to STA */
    return QAT_Response_Str(QAT_RC_OK,
        "+CIPSTA=<ip>,<gateway>,<netmask>[,<dns1>[,<dns2>]]\r\n");
}

static cat_return_state cmd_cipsta_query(const struct cat_command *cmd, uint8_t *data, size_t *data_size,
                                         const size_t max_data_size)
{
    LOG_INF("CIPSTA query START");
    *data_size = 0;

    /* Query STA interface by logical ID (1 = STA) */
    struct net_if *iface = get_iface_by_qat_id(NT_DEV_STA_ID);
    char buffer[QAT_RESPONSE_BUF_SIZE];
    int offset = 0;

    LOG_INF("Got iface: %p", iface);

    if (!iface) {
        LOG_ERR("No network interface");
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPSTA: No network interface\r\n");
    }

    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "+CIPSTA:");
    LOG_INF("Buffer initialized, offset=%d", offset);

    /* Get IPv4 address */
    struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;
    LOG_INF("Got ipv4: %p", ipv4);

    if (ipv4) {
        /* Find first valid IPv4 address */
        struct net_if_addr_ipv4 *unicast = NULL;
        LOG_INF("Searching for valid IPv4 address, max=%d", NET_IF_MAX_IPV4_ADDR);

        for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
            LOG_INF("Checking slot %d: is_used=%d, addr_state=%d",
                   i, ipv4->unicast[i].ipv4.is_used, ipv4->unicast[i].ipv4.addr_state);

            /* Check if this address slot is used and in preferred state */
            if (ipv4->unicast[i].ipv4.is_used &&
                ipv4->unicast[i].ipv4.addr_state == NET_ADDR_PREFERRED) {
                unicast = &ipv4->unicast[i];
                LOG_INF("Found valid address at slot %d", i);
                break;
            }
        }

        if (unicast) {
            char addr_str[NET_IPV4_ADDR_LEN];

            LOG_INF("Converting IP address");
            net_addr_ntop(AF_INET, &unicast->ipv4.address.in_addr,
                         addr_str, sizeof(addr_str));
            offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%s,", addr_str);
            LOG_INF("IP: %s, offset=%d", addr_str, offset);

            LOG_INF("Converting gateway");
            net_addr_ntop(AF_INET, &ipv4->gw, addr_str, sizeof(addr_str));
            offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%s,", addr_str);
            LOG_INF("GW: %s, offset=%d", addr_str, offset);

            LOG_INF("Converting netmask");
            net_addr_ntop(AF_INET, &unicast->netmask, addr_str, sizeof(addr_str));
            offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%s", addr_str);
            LOG_INF("NM: %s, offset=%d", addr_str, offset);
        } else {
            LOG_WRN("No valid unicast address found");
        }
    } else {
        LOG_WRN("No IPv4 config");
    }

    /* Append IPv6 addresses */
    struct net_if_ipv6 *ipv6 = iface->config.ip.ipv6;
    if (ipv6) {
        for (int i = 0; i < NET_IF_MAX_IPV6_ADDR; i++) {
            if (!ipv6->unicast[i].is_used ||
                ipv6->unicast[i].addr_state != NET_ADDR_PREFERRED) {
                continue;
            }
            char ip6_str[INET6_ADDRSTRLEN];
            net_addr_ntop(AF_INET6, &ipv6->unicast[i].address.in6_addr,
                          ip6_str, sizeof(ip6_str));
            offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                               ",%s/64", ip6_str);
        }
    }

    /* Append DNS servers */
    for (int i = 0; i < DNSC_MAX_SERVERS; i++) {
        if (dnsc_servers[i].valid) {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                               ",DNS%d:%s", i + 1, dnsc_servers[i].addr);
        }
    }

    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\r\n");
    LOG_INF("CIPSTA query END, returning buffer: %s", buffer);
    return QAT_Response_Str(QAT_RC_OK, buffer);
}

static cat_return_state cmd_cipsta_set(const struct cat_command *cmd,
                                       const uint8_t *data,
                                       const size_t data_size,
                                       const size_t args_num)
{
    char ip_str[NET_IPV4_ADDR_LEN];
    char gw_str[NET_IPV4_ADDR_LEN];
    char nm_str[NET_IPV4_ADDR_LEN];
    char dns1_str[NET_IPV4_ADDR_LEN] = {0};
    char dns2_str[NET_IPV4_ADDR_LEN] = {0};

    int parsed = sscanf((char *)data,
                        "%15[^,],%15[^,],%15[^,],%15[^,],%15s",
                        ip_str, gw_str, nm_str, dns1_str, dns2_str);
    if (parsed < 3) {
        return QAT_Response_Str(QAT_RC_ERROR,
                                "+CIPSTA: Invalid parameters (need IP,GW,NM)\r\n");
    }

    /* CIPSTA only applies to STA (wlan1) */
    struct net_if *iface = get_iface_by_qat_id(NT_DEV_STA_ID);
    if (!iface) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPSTA: No STA interface\r\n");
    }

    /* Parse IP address */
    struct in_addr addr, gw, netmask;
    if (zsock_inet_pton(AF_INET, ip_str, &addr) != 1) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPSTA: Invalid IP address\r\n");
    }

    /* Parse gateway */
    if (zsock_inet_pton(AF_INET, gw_str, &gw) != 1) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPSTA: Invalid gateway\r\n");
    }

    /* Parse netmask */
    if (zsock_inet_pton(AF_INET, nm_str, &netmask) != 1) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPSTA: Invalid netmask\r\n");
    }

    /* Remove existing IP addresses first to avoid conflicts */
    struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;
    if (ipv4) {
        for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
            if (ipv4->unicast[i].ipv4.is_used) {
                net_if_ipv4_addr_rm(iface, &ipv4->unicast[i].ipv4.address.in_addr);
            }
        }
    }

    /* Add new IP address */
    struct net_if_addr *if_addr = net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0);
    if (!if_addr) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPSTA: Failed to set IP\r\n");
    }

    /* Set netmask */
    net_if_ipv4_set_netmask_by_addr(iface, &addr, &netmask);

    /* Set gateway */
    net_if_ipv4_set_gw(iface, &gw);

    /* Apply DNS servers if provided */
    if (dns1_str[0] || dns2_str[0]) {
        if (dns1_str[0]) {
            snprintf(dnsc_servers[0].addr, DNSC_SERVER_LEN, "%s", dns1_str);
            dnsc_servers[0].valid = true;
        }
        if (dns2_str[0]) {
            snprintf(dnsc_servers[1].addr, DNSC_SERVER_LEN, "%s", dns2_str);
            dnsc_servers[1].valid = true;
        }
        const char *servers[DNSC_MAX_SERVERS + 1];
        int count = 0;
        for (int i = 0; i < DNSC_MAX_SERVERS; i++) {
            if (dnsc_servers[i].valid) {
                servers[count++] = dnsc_servers[i].addr;
            }
        }
        servers[count] = NULL;
        struct dns_resolve_context *ctx = dns_resolve_get_default();
        if (ctx && count > 0) {
            dns_resolve_reconfigure(ctx, servers, NULL, DNS_SOURCE_MANUAL);
        }
    }

    LOG_INF("IP configured: %s, GW: %s, NM: %s", ip_str, gw_str, nm_str);

    char out[128];
    snprintf(out, sizeof(out), "+CIPSTA:%s,%s,%s\r\n", ip_str, gw_str, nm_str);
    return QAT_Response_Str(QAT_RC_OK, out);
}

/*-------------------------------------------------------------------------
 * AT+CIPSTART - Start Connection
 *-----------------------------------------------------------------------*/
static cat_return_state cmd_cipstart_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK,
        "+CIPSTART=<link_id>,<type>,<remote_ip>,<remote_port>\r\n"
        "  type: TCP, UDP, TCPv6, UDPv6\r\n");
}

static cat_return_state cmd_cipstart_query(const struct cat_command *cmd, uint8_t *data, size_t *data_size,
                                           const size_t max_data_size)
{
    char buffer[QAT_RESPONSE_BUF_SIZE];
    int offset = 0;
    *data_size = 0;

    k_mutex_lock(&conn_mutex, K_FOREVER);

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (g_client_conns[i].state != CONN_STATE_CONNECTED) {
            continue;
        }

        const char *type_str = "UNKNOWN";
        switch (g_client_conns[i].type) {
        case PROTOCOL_TCP: type_str = "TCP"; break;
        case PROTOCOL_UDP: type_str = "UDP"; break;
        case PROTOCOL_TCPv6: type_str = "TCPv6"; break;
        case PROTOCOL_UDPv6: type_str = "UDPv6"; break;
        default: break;
        }

        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                          "+CIPSTART:C,%d,%s,%s,%d,%d\r\n",
                          i, type_str, g_client_conns[i].remote_ip,
                          g_client_conns[i].remote_port,
                          g_client_conns[i].local_port);
    }

    k_mutex_unlock(&conn_mutex);

    return QAT_Response_Str(QAT_RC_OK, buffer);
}

static void client_recv_work_handler(struct k_work *work)
{
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    connection_info_t *conn = CONTAINER_OF(dwork, connection_info_t, recv_work);

    if (conn->thread_quit || conn->state != CONN_STATE_CONNECTED) {
        return;
    }

    struct zsock_pollfd fds[1];
    fds[0].fd = conn->sock_fd;
    fds[0].events = ZSOCK_POLLIN;

    ssize_t recv_len = 0;
    int ret = zsock_poll(fds, 1, 0);
    if (ret > 0 && (fds[0].revents & ZSOCK_POLLIN)) {
        recv_len = zsock_recv(conn->sock_fd, s_recv_buf, sizeof(s_recv_buf), 0);

        if (recv_len > 0) {
            if (conn->recv_mode == RECV_MODE_ACTIVE) {
                /* Active mode: print data immediately */
                const char *proto = (conn->type == PROTOCOL_TCP || conn->type == PROTOCOL_TCPv6) ? "TCP" : "UDP";

                if (is_passthrough_mode) {
                    int offset = snprintf(s_recv_response, sizeof(s_recv_response),
                                        "+IPDHEX:C,%s,%d,%zd,", proto, conn->id, recv_len);
                    memcpy(s_recv_response + offset, s_recv_buf, recv_len);
                    QAT_Output(offset + recv_len, s_recv_response);
                } else {
                    s_recv_buf[recv_len] = '\0';
                    snprintf(s_recv_response, sizeof(s_recv_response), "+IPD:C,%s,%d,%zd,%s",
                            proto, conn->id, recv_len, s_recv_buf);
                    QAT_Response_Str(QAT_RC_QUIET, s_recv_response);
                }
            } else {
                /* Passive mode: store in buffer and notify */
                if (circular_buffer_write(conn->recv_buf, s_recv_buf, recv_len) == 0) {
                    queue_elem_t elem = {.link_id = conn->id, .data_len = recv_len};
                    k_msgq_put(&client_queue, &elem, K_NO_WAIT);

                    if (ipd_message_print_flag) {
                        char response[64];
                        const char *proto = (conn->type == PROTOCOL_TCP || conn->type == PROTOCOL_TCPv6) ? "TCP" : "UDP";
                        snprintf(response, sizeof(response), "+IPD:C,%s,%d,%zd\r\n",
                                proto, conn->id, recv_len);
                        QAT_Response_Str(QAT_RC_QUIET, response);
                        ipd_message_print_flag = false;
                    }
                }
            }
        } else if (recv_len == 0 || (recv_len < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            /* Connection closed */
            char response[64];
            snprintf(response, sizeof(response), "+IPS:CLOSED:%d", conn->id);
            QAT_Response_Str(QAT_RC_QUIET, response);

            zsock_close(conn->sock_fd);
            cleanup_client_conn(conn->id);
            return;
        }
    }

    /* Reschedule work */
    k_work_reschedule(dwork, (recv_len > 0) ? K_NO_WAIT : K_MSEC(10));
}

static cat_return_state cmd_cipstart_set(const struct cat_command *cmd,
                                         const uint8_t *data,
                                         const size_t data_size,
                                         const size_t args_num)
{
    int link_id, port;
    char type_str[16];
    char ip_str[INET6_ADDRSTRLEN];
    char response[128];

    LOG_INF("CIPSTART: Received command, data='%s', size=%zu", (char *)data, data_size);

    /* Try parsing with quotes first */
    int parsed = sscanf((char *)data, "%d,\"%15[^\"]\",\"%45[^\"]\",%d",
                       &link_id, type_str, ip_str, &port);

    LOG_INF("CIPSTART: Parse with quotes result=%d", parsed);

    /* If that fails, try without quotes */
    if (parsed != 4) {
        parsed = sscanf((char *)data, "%d,%15[^,],%45[^,],%d",
                       &link_id, type_str, ip_str, &port);
        LOG_INF("CIPSTART: Parse without quotes result=%d", parsed);
    }

    if (parsed != 4) {
        LOG_ERR("CIPSTART: Failed to parse parameters, parsed=%d", parsed);
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPSTART: Invalid parameters\r\n");
    }

    LOG_INF("CIPSTART: Parsed - link_id=%d, type=%s, ip=%s, port=%d",
            link_id, type_str, ip_str, port);

    if (link_id < 0 || link_id >= MAX_CONNECTIONS) {
        LOG_ERR("CIPSTART: Invalid link_id=%d (must be 0-%d)", link_id, MAX_CONNECTIONS-1);
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPSTART: Invalid link_id\r\n");
    }

    k_mutex_lock(&conn_mutex, K_FOREVER);
    LOG_INF("CIPSTART: Acquired mutex, checking link %d state=%d",
            link_id, g_client_conns[link_id].state);

    if (g_client_conns[link_id].state == CONN_STATE_CONNECTED) {
        LOG_WRN("CIPSTART: Link %d already in use", link_id);
        k_mutex_unlock(&conn_mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPSTART: Link already in use\r\n");
    }

    /* Determine protocol type */
    protocol_type_t proto_type;
    int family, sock_type, protocol;

    LOG_INF("CIPSTART: Determining protocol type for '%s'", type_str);

    if (strcmp(type_str, "TCP") == 0) {
        proto_type = PROTOCOL_TCP;
        family = AF_INET;
        sock_type = SOCK_STREAM;
        protocol = IPPROTO_TCP;
        LOG_INF("CIPSTART: Protocol=TCP (IPv4)");
    } else if (strcmp(type_str, "UDP") == 0) {
        proto_type = PROTOCOL_UDP;
        family = AF_INET;
        sock_type = SOCK_DGRAM;
        protocol = IPPROTO_UDP;
        LOG_INF("CIPSTART: Protocol=UDP (IPv4)");
    } else if (strcmp(type_str, "TCPv6") == 0) {
        proto_type = PROTOCOL_TCPv6;
        family = AF_INET6;
        sock_type = SOCK_STREAM;
        protocol = IPPROTO_TCP;
        LOG_INF("CIPSTART: Protocol=TCPv6 (IPv6)");
    } else if (strcmp(type_str, "UDPv6") == 0) {
        proto_type = PROTOCOL_UDPv6;
        family = AF_INET6;
        sock_type = SOCK_DGRAM;
        protocol = IPPROTO_UDP;
        LOG_INF("CIPSTART: Protocol=UDPv6 (IPv6)");
    } else {
        LOG_ERR("CIPSTART: Invalid protocol type '%s'", type_str);
        k_mutex_unlock(&conn_mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPSTART: Invalid type\r\n");
    }

    /* Create socket */
    LOG_INF("CIPSTART: Creating socket (family=%d, type=%d, proto=%d)",
            family, sock_type, protocol);
    int sock_fd = zsock_socket(family, sock_type, protocol);
    if (sock_fd < 0) {
        LOG_ERR("CIPSTART: Failed to create socket, errno=%d", errno);
        k_mutex_unlock(&conn_mutex);
        snprintf(response, sizeof(response), "+IPS:FAILED:%d\r\n", link_id);
        return QAT_Response_Str(QAT_RC_ERROR, response);
    }
    LOG_INF("CIPSTART: Socket created successfully, fd=%d", sock_fd);

    /* Set non-blocking */
    int flags = zsock_fcntl(sock_fd, F_GETFL, 0);
    zsock_fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK);
    LOG_INF("CIPSTART: Socket set to non-blocking mode");

    /* Connect to remote */
    struct sockaddr_storage remote_addr;
    socklen_t addr_len;

    LOG_INF("CIPSTART: Preparing to connect to %s:%d", ip_str, port);

    if (family == AF_INET) {
        struct sockaddr_in *addr4 = (struct sockaddr_in *)&remote_addr;
        addr4->sin_family = AF_INET;
        addr4->sin_port = htons(port);
        int ret = zsock_inet_pton(AF_INET, ip_str, &addr4->sin_addr);
        if (ret != 1) {
            LOG_ERR("CIPSTART: Invalid IPv4 address '%s', ret=%d", ip_str, ret);
            zsock_close(sock_fd);
            k_mutex_unlock(&conn_mutex);
            snprintf(response, sizeof(response), "+IPS:FAILED:%d\r\n", link_id);
            return QAT_Response_Str(QAT_RC_ERROR, response);
        }
        addr_len = sizeof(struct sockaddr_in);
        LOG_INF("CIPSTART: IPv4 address parsed successfully");
    } else {
        struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&remote_addr;
        addr6->sin6_family = AF_INET6;
        addr6->sin6_port = htons(port);
        int ret = zsock_inet_pton(AF_INET6, ip_str, &addr6->sin6_addr);
        if (ret != 1) {
            LOG_ERR("CIPSTART: Invalid IPv6 address '%s', ret=%d", ip_str, ret);
            zsock_close(sock_fd);
            k_mutex_unlock(&conn_mutex);
            snprintf(response, sizeof(response), "+IPS:FAILED:%d\r\n", link_id);
            return QAT_Response_Str(QAT_RC_ERROR, response);
        }
        addr_len = sizeof(struct sockaddr_in6);
        LOG_INF("CIPSTART: IPv6 address parsed successfully");
    }

    LOG_INF("CIPSTART: Calling zsock_connect...");
    int connect_ret = zsock_connect(sock_fd, (struct sockaddr *)&remote_addr, addr_len);
    LOG_INF("CIPSTART: zsock_connect returned %d, errno=%d", connect_ret, errno);

    if (connect_ret < 0) {
        if (errno != EINPROGRESS) {
            LOG_ERR("CIPSTART: Connect failed immediately, errno=%d", errno);
            zsock_close(sock_fd);
            k_mutex_unlock(&conn_mutex);
            snprintf(response, sizeof(response), "+IPS:FAILED:%d\r\n", link_id);
            return QAT_Response_Str(QAT_RC_ERROR, response);
        }

        /* Non-blocking connect: wait for TCP handshake to complete (up to 5 s).
         * POLLOUT becomes set once the connection succeeds or fails. */
        struct zsock_pollfd cpfd = {.fd = sock_fd, .events = ZSOCK_POLLOUT};
        int poll_ret = zsock_poll(&cpfd, 1, 5000);
        if (poll_ret <= 0) {
            LOG_ERR("CIPSTART: Connect timed out, poll_ret=%d", poll_ret);
            zsock_close(sock_fd);
            k_mutex_unlock(&conn_mutex);
            snprintf(response, sizeof(response), "+IPS:FAILED:%d\r\n", link_id);
            return QAT_Response_Str(QAT_RC_ERROR, response);
        }

        /* Retrieve the async connect result via SO_ERROR. */
        int so_error = 0;
        socklen_t so_len = sizeof(so_error);
        zsock_getsockopt(sock_fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len);
        if (so_error != 0) {
            LOG_ERR("CIPSTART: Connect failed, so_error=%d", so_error);
            zsock_close(sock_fd);
            k_mutex_unlock(&conn_mutex);
            snprintf(response, sizeof(response), "+IPS:FAILED:%d\r\n", link_id);
            return QAT_Response_Str(QAT_RC_ERROR, response);
        }

        LOG_INF("CIPSTART: TCP handshake complete");
    } else {
        LOG_INF("CIPSTART: Connection established immediately");
    }

    /* Initialize connection info */
    LOG_INF("CIPSTART: Initializing connection info for link %d", link_id);
    g_client_conns[link_id].id = link_id;
    g_client_conns[link_id].sock_fd = sock_fd;
    g_client_conns[link_id].state = CONN_STATE_CONNECTED;
    g_client_conns[link_id].type = proto_type;
    g_client_conns[link_id].recv_mode = RECV_MODE_ACTIVE;
    g_client_conns[link_id].thread_quit = false;
    memcpy(g_client_conns[link_id].remote_ip, ip_str, sizeof(g_client_conns[link_id].remote_ip) - 1);
    g_client_conns[link_id].remote_ip[sizeof(g_client_conns[link_id].remote_ip) - 1] = '\0';
    g_client_conns[link_id].remote_port = port;
    LOG_INF("CIPSTART: Connection info set - id=%d, fd=%d, ip=%s, port=%d",
            link_id, sock_fd, ip_str, port);

    /* Create circular buffer for passive mode */
    g_client_conns[link_id].recv_buf = circular_buffer_create();
    if (!g_client_conns[link_id].recv_buf) {
        LOG_ERR("CIPSTART: Failed to create circular buffer");
        zsock_close(sock_fd);
        k_mutex_lock(&conn_mutex, K_FOREVER);
        cleanup_client_conn(link_id);
        k_mutex_unlock(&conn_mutex);
        snprintf(response, sizeof(response), "+IPS:FAILED:%d", link_id);
        return QAT_Response_Str(QAT_RC_ERROR, response);
    } else {
        LOG_INF("CIPSTART: Circular buffer created");
    }

    /* Start receive work — cancel any stale work item before reinitializing */
    k_work_cancel_delayable(&g_client_conns[link_id].recv_work);
    k_work_init_delayable(&g_client_conns[link_id].recv_work, client_recv_work_handler);
    k_work_reschedule(&g_client_conns[link_id].recv_work, K_MSEC(100));
    LOG_INF("CIPSTART: Receive work scheduled");

    k_mutex_unlock(&conn_mutex);
    LOG_INF("CIPSTART: Mutex released");

    snprintf(response, sizeof(response), "+IPS:CONNECTED:%d\r\n", link_id);
    LOG_INF("CIPSTART: SUCCESS - Link %d connected to %s:%d (fd=%d)",
            link_id, ip_str, port, sock_fd);
    return QAT_Response_Str(QAT_RC_OK, response);
}

/*-------------------------------------------------------------------------
 * AT+CIPCLOSE - Close Connection
 *-----------------------------------------------------------------------*/
static cat_return_state cmd_cipclose_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK, "+CIPCLOSE=<link_id>\r\n");
}

static cat_return_state cmd_cipclose_set(const struct cat_command *cmd,
                                         const uint8_t *data,
                                         const size_t data_size,
                                         const size_t args_num)
{
    int link_id;
    char response[64];

    if (sscanf((char *)data, "%d", &link_id) != 1) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPCLOSE: Invalid parameter\r\n");
    }

    if (link_id < 0 || link_id >= MAX_CONNECTIONS) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPCLOSE: Invalid link_id\r\n");
    }

    k_mutex_lock(&conn_mutex, K_FOREVER);

    if (g_client_conns[link_id].state != CONN_STATE_CONNECTED) {
        k_mutex_unlock(&conn_mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPCLOSE: Link not active\r\n");
    }

    /* Signal thread to quit and cancel work */
    g_client_conns[link_id].thread_quit = true;
    k_work_cancel_delayable(&g_client_conns[link_id].recv_work);

    /* Close socket */
    zsock_close(g_client_conns[link_id].sock_fd);

    /* Cleanup */
    cleanup_client_conn(link_id);

    k_mutex_unlock(&conn_mutex);

    snprintf(response, sizeof(response), "+IPS:CLOSED:%d", link_id);
    return QAT_Response_Str(QAT_RC_OK, response);
}

/*-------------------------------------------------------------------------
 * AT+CIPSEND - Send Data (Online Mode)
 *-----------------------------------------------------------------------*/
static void cipsend_exit_online_mode(void)
{
    cipsend_state.link_id = INVALID_LINKID;
    cipsend_state.max_len = 0;
    cipsend_state.total_sent = 0;
    cipsend_state.exit_length_valid = true;
    QAT_Transfer_Mode_set(QAT_Transfer_Mode_AT_COMMAND_E, NULL);
}

static int cipsend_data_callback(const uint8_t *data, size_t len)
{
    char response[64];
    size_t start = 0;
    size_t end = len;

    if (cipsend_state.link_id == INVALID_LINKID) {
        return -EINVAL;
    }

    /* Interactive tools may submit line fragments like:
     *   "+++\r", "\n", or "\r\n"
     * Trim leading/trailing CR/LF first so they are not forwarded as payload.
     */
    while (start < end && (data[start] == '\r' || data[start] == '\n')) {
        start++;
    }
    while (end > start && (data[end - 1] == '\r' || data[end - 1] == '\n')) {
        end--;
    }

    size_t trimmed_len = end - start;

    /* Match FreeRTOS len==0 escape behavior, but tolerate surrounding CR/LF. */
    if (!cipsend_state.exit_length_valid &&
        trimmed_len == 3 &&
        memcmp(data + start, "+++", 3) == 0) {
        cipsend_exit_online_mode();
        return 0;
    }

    /* Pure newline fragment after leaving/while in online mode: swallow it. */
    if (trimmed_len == 0) {
        return 0;
    }

    k_mutex_lock(&conn_mutex, K_FOREVER);

    if (g_client_conns[cipsend_state.link_id].state != CONN_STATE_CONNECTED) {
        k_mutex_unlock(&conn_mutex);
        snprintf(response, sizeof(response), "+IPS:SEND FAILED:%d\r\n", cipsend_state.link_id);
        QAT_Response_Str(QAT_RC_ERROR, response);
        cipsend_exit_online_mode();
        return -ENOTCONN;
    }

    int sock_fd = g_client_conns[cipsend_state.link_id].sock_fd;
    k_mutex_unlock(&conn_mutex);

    const uint8_t *payload = data + start;
    size_t send_len = trimmed_len;

    if (cipsend_state.exit_length_valid) {
        size_t remaining = cipsend_state.max_len - cipsend_state.total_sent;
        send_len = (send_len <= remaining) ? send_len : remaining;
    }

    LOG_INF("CIPSEND: link=%d raw_len=%zu trimmed_len=%zu send_len=%zu total_sent=%zu max_len=%zu fixed_len=%d payload='%.*s'",
            cipsend_state.link_id, len, trimmed_len, send_len,
            cipsend_state.total_sent, cipsend_state.max_len,
            cipsend_state.exit_length_valid ? 1 : 0,
            (int)send_len, (const char *)payload);

    /* Nothing left to send: finish immediately */
    if (send_len == 0) {
        snprintf(response, sizeof(response), "+IPS:SEND DONE:%d\r\n", cipsend_state.link_id);
        QAT_Response_Str(QAT_RC_QUIET, response);
        cipsend_exit_online_mode();
        return 0;
    }

    /* Wait for the send buffer to have space (up to 500 ms).
     * Connection is guaranteed established by this point (CIPSTART waits for
     * the TCP handshake before reporting +IPS:CONNECTED). */
    struct zsock_pollfd pfd = {.fd = sock_fd, .events = ZSOCK_POLLOUT};
    int poll_ret = zsock_poll(&pfd, 1, 500);
    if (poll_ret <= 0 || !(pfd.revents & ZSOCK_POLLOUT)) {
        LOG_ERR("CIPSEND: socket not writable, poll_ret=%d", poll_ret);
        snprintf(response, sizeof(response), "+IPS:SEND FAILED:%d\r\n", cipsend_state.link_id);
        QAT_Response_Str(QAT_RC_ERROR, response);
        cipsend_exit_online_mode();
        return -ETIMEDOUT;
    }

    /* Send all data, handling partial sends */
    size_t total = 0;
    while (total < send_len) {
        ssize_t sent = zsock_send(sock_fd, payload + total, send_len - total, 0);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                k_msleep(10);
                continue;
            }
            LOG_ERR("CIPSEND: send failed, errno=%d", errno);
            snprintf(response, sizeof(response), "+IPS:SEND FAILED:%d\r\n", cipsend_state.link_id);
            QAT_Response_Str(QAT_RC_ERROR, response);
            cipsend_exit_online_mode();
            return -errno;
        }
        total += sent;
    }

    if (cipsend_state.exit_length_valid) {
        cipsend_state.total_sent += total;
        if (cipsend_state.total_sent >= cipsend_state.max_len) {
            snprintf(response, sizeof(response), "+IPS:SEND DONE:%d\r\n", cipsend_state.link_id);
            QAT_Response_Str(QAT_RC_QUIET, response);
            cipsend_exit_online_mode();
        }
    }

    return 0;
}

static cat_return_state cmd_cipsend_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK, "+CIPSEND=<link_id>,<length>\r\n");
}

static cat_return_state cmd_cipsend_set(const struct cat_command *cmd,
                                        const uint8_t *data,
                                        const size_t data_size,
                                        const size_t args_num)
{
    int link_id, len;

    if (sscanf((char *)data, "%d,%d", &link_id, &len) != 2) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPSEND: Invalid parameters\r\n");
    }

    if (link_id < 0 || link_id >= MAX_CONNECTIONS) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPSEND: Invalid link_id\r\n");
    }

    if (len < 0) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPSEND: Invalid length\r\n");
    }

    k_mutex_lock(&conn_mutex, K_FOREVER);

    if (g_client_conns[link_id].state != CONN_STATE_CONNECTED) {
        k_mutex_unlock(&conn_mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPSEND: Link not active\r\n");
    }

    k_mutex_unlock(&conn_mutex);

    /* Initialize state; len == 0 matches FreeRTOS unlimited send mode */
    cipsend_state.link_id = link_id;
    cipsend_state.max_len = len;
    cipsend_state.total_sent = 0;
    cipsend_state.exit_length_valid = (len != 0);

    /* Enter online data mode */
    int ret = QAT_Transfer_Mode_set(QAT_Transfer_Mode_ONLINE_DATA_E, cipsend_data_callback);
    if (ret != 0) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPSEND: Failed to enter online mode\r\n");
    }

    k_work_submit(&cipsend_prompt_work);
    return QAT_Response_Str(QAT_RC_OK, NULL);
}

/*-------------------------------------------------------------------------
 * AT+CIPSENDDATA - Send Data Directly
 *-----------------------------------------------------------------------*/
static cat_return_state cmd_cipsenddata_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK, "+CIPSENDDATA=<link_id>,<length>,\"<data>\"\r\n");
}

static cat_return_state cmd_cipsenddata_set(const struct cat_command *cmd,
                                            const uint8_t *data,
                                            const size_t data_size,
                                            const size_t args_num)
{
    int link_id, len;
    char response[128];

    if (sscanf((char *)data, "%d,%d,\"%1399[^\"]\"", &link_id, &len, s_at_data_buf) != 3) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPSENDDATA: Invalid parameters\r\n");
    }

    if (link_id < 0 || link_id >= MAX_CONNECTIONS) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPSENDDATA: Invalid link_id\r\n");
    }

    k_mutex_lock(&conn_mutex, K_FOREVER);

    if (g_client_conns[link_id].state != CONN_STATE_CONNECTED) {
        k_mutex_unlock(&conn_mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPSENDDATA: Link not active\r\n");
    }

    int sock_fd = g_client_conns[link_id].sock_fd;
    k_mutex_unlock(&conn_mutex);

    int actual_len = strlen(s_at_data_buf);
    int to_send = (len < actual_len) ? len : actual_len;
    int total_sent = 0;

    LOG_INF("CIPSENDDATA: link=%d req_len=%d actual_len=%d to_send=%d payload='%.*s'",
            link_id, len, actual_len, to_send, to_send, s_at_data_buf);

    /* Wait for the send buffer to have space before entering the send loop. */
    struct zsock_pollfd pfd = {.fd = sock_fd, .events = ZSOCK_POLLOUT};
    if (zsock_poll(&pfd, 1, 500) <= 0 || !(pfd.revents & ZSOCK_POLLOUT)) {
        snprintf(response, sizeof(response), "+IPS:SEND FAILED:%d", link_id);
        return QAT_Response_Str(QAT_RC_ERROR, response);
    }

    while (total_sent < to_send) {
        ssize_t sent = zsock_send(sock_fd, s_at_data_buf + total_sent, to_send - total_sent, 0);
        if (sent < 0) {
            snprintf(response, sizeof(response), "+IPS:SEND FAILED:%d", link_id);
            return QAT_Response_Str(QAT_RC_ERROR, response);
        }
        total_sent += sent;
    }

    snprintf(response, sizeof(response), "+IPS:SEND DONE:%d\r\n", link_id);
    return QAT_Response_Str(QAT_RC_OK, response);
}

/*-------------------------------------------------------------------------
 * AT+CIPRECVTYPE - Set Receive Mode
 *-----------------------------------------------------------------------*/
static cat_return_state cmd_ciprecvtype_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK,
        "+CIPRECVTYPE=<server_flag>,<type>,<link_id>,<mode>\r\n"
        "  server_flag: C (client), S (server)\r\n"
        "  type: TCP, UDP\r\n"
        "  mode: 0 (active), 1 (passive)\r\n");
}

static cat_return_state cmd_ciprecvtype_query(const struct cat_command *cmd, uint8_t *data, size_t *data_size,
                                              const size_t max_data_size)
{
    char buffer[QAT_RESPONSE_BUF_SIZE];
    int offset = 0;
    *data_size = 0;

    k_mutex_lock(&conn_mutex, K_FOREVER);

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (g_client_conns[i].state == CONN_STATE_CONNECTED) {
            const char *proto = protocol_type_to_at_name(g_client_conns[i].type);
            if (!proto) {
                continue;
            }
            offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                             "+CIPRECVTYPE:C,%s,%d,%d\r\n",
                             proto, i, g_client_conns[i].recv_mode);
        }
    }

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (g_listen_clients[i].state == CONN_STATE_CONNECTED &&
            (g_listen_clients[i].type == PROTOCOL_TCP || g_listen_clients[i].type == PROTOCOL_TCPv6)) {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                             "+CIPRECVTYPE:S,TCP,%d,%d\r\n",
                             i, g_listen_clients[i].recv_mode);
        }
    }

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (g_listen_udp_clients[i].state == CONN_STATE_CONNECTED &&
            (g_listen_udp_clients[i].type == PROTOCOL_UDP || g_listen_udp_clients[i].type == PROTOCOL_UDPv6)) {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                             "+CIPRECVTYPE:S,UDP,%d,%d\r\n",
                             i, g_listen_udp_clients[i].recv_mode);
        }
    }

    k_mutex_unlock(&conn_mutex);

    return QAT_Response_Str(QAT_RC_OK, buffer);
}

static cat_return_state cmd_ciprecvtype_set(const struct cat_command *cmd,
                                            const uint8_t *data,
                                            const size_t data_size,
                                            const size_t args_num)
{
    char server_flag[2];
    char proto[8];
    int link_id, mode;

    if (sscanf((char *)data, "%1[CS],%7[^,],%d,%d",
               server_flag, proto, &link_id, &mode) != 4) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPRECVTYPE: Invalid parameters\r\n");
    }

    if (link_id < 0 || link_id >= MAX_CONNECTIONS) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPRECVTYPE: Invalid link_id\r\n");
    }

    if (mode != RECV_MODE_ACTIVE && mode != RECV_MODE_PASSIVE) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPRECVTYPE: Invalid mode\r\n");
    }

    k_mutex_lock(&conn_mutex, K_FOREVER);

    if (server_flag[0] == 'C') {
        const char *actual_proto;

        if (g_client_conns[link_id].state != CONN_STATE_CONNECTED) {
            k_mutex_unlock(&conn_mutex);
            return QAT_Response_Str(QAT_RC_ERROR, "+CIPRECVTYPE: Link not active\r\n");
        }

        actual_proto = protocol_type_to_at_name(g_client_conns[link_id].type);
        if (!actual_proto || strcmp(proto, actual_proto) != 0) {
            k_mutex_unlock(&conn_mutex);
            return QAT_Response_Str(QAT_RC_ERROR, "+CIPRECVTYPE: Protocol type mismatch\r\n");
        }

        g_client_conns[link_id].recv_mode = mode;
    } else {
        const char *actual_proto;

        connection_info_t *server_conns = (strcmp(proto, "TCP") == 0) ?
            g_listen_clients : g_listen_udp_clients;

        if (server_conns[link_id].state != CONN_STATE_CONNECTED) {
            k_mutex_unlock(&conn_mutex);
            return QAT_Response_Str(QAT_RC_ERROR, "+CIPRECVTYPE: Server link not active\r\n");
        }

        actual_proto = protocol_type_to_at_name(server_conns[link_id].type);
        if (!actual_proto || strcmp(proto, actual_proto) != 0) {
            k_mutex_unlock(&conn_mutex);
            return QAT_Response_Str(QAT_RC_ERROR, "+CIPRECVTYPE: Protocol type mismatch\r\n");
        }

        server_conns[link_id].recv_mode = mode;
    }

    k_mutex_unlock(&conn_mutex);

    LOG_INF("Link %d recv mode set to %d", link_id, mode);
    return QAT_Response_Str(QAT_RC_OK, NULL);
}

/*-------------------------------------------------------------------------
 * AT+CIPRECVDATA - Read Received Data (Passive Mode)
 *-----------------------------------------------------------------------*/
static cat_return_state cmd_ciprecvdata_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK,
        "+CIPRECVDATA=<server_flag>,<type>,<link_id>,<length>\r\n");
}

static cat_return_state cmd_ciprecvdata_set(const struct cat_command *cmd,
                                            const uint8_t *data,
                                            const size_t data_size,
                                            const size_t args_num)
{
    char server_flag[2];
    char proto[8];
    int link_id, read_len;
    queue_elem_t elem;

    if (sscanf((char *)data, "%1[CS],%7[^,],%d,%d",
               server_flag, proto, &link_id, &read_len) != 4) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPRECVDATA: Invalid parameters\r\n");
    }

    if (link_id < 0 || link_id >= MAX_CONNECTIONS) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPRECVDATA: Invalid link_id\r\n");
    }

    if (read_len <= 0 || read_len > sizeof(s_at_data_buf)) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPRECVDATA: Invalid length\r\n");
    }

    k_mutex_lock(&conn_mutex, K_FOREVER);

    connection_info_t *conn;

    if (server_flag[0] == 'C') {
        conn = &g_client_conns[link_id];
    } else if (strcmp(proto, "TCP") == 0) {
        conn = &g_listen_clients[link_id];
    } else {
        conn = &g_listen_udp_clients[link_id];
    }

    if (conn->state != CONN_STATE_CONNECTED) {
        k_mutex_unlock(&conn_mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPRECVDATA: Link not active\r\n");
    }

    if (conn->recv_mode != RECV_MODE_PASSIVE) {
        k_mutex_unlock(&conn_mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPRECVDATA: Not in passive mode\r\n");
    }

    {
        const char *actual_proto = protocol_type_to_at_name(conn->type);

        if (!actual_proto || strcmp(proto, actual_proto) != 0) {
            k_mutex_unlock(&conn_mutex);
            return QAT_Response_Str(QAT_RC_ERROR, "+CIPRECVDATA: Protocol type mismatch\r\n");
        }
    }

    /* Get queue element */
    struct k_msgq *queue;

    if (server_flag[0] == 'C') {
        queue = &client_queue;
    } else if (strcmp(proto, "TCP") == 0) {
        queue = &tcp_server_queue;
    } else {
        queue = &udp_server_queue;
    }

    if (k_msgq_peek(queue, &elem) != 0) {
        k_mutex_unlock(&conn_mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPRECVDATA: No data available\r\n");
    }

    if (read_len > elem.data_len) {
        k_mutex_unlock(&conn_mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPRECVDATA: Length exceeds available data\r\n");
    }

    /* Read from circular buffer */
    if (circular_buffer_read(conn->recv_buf, s_at_data_buf, read_len) < 0) {
        k_mutex_unlock(&conn_mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPRECVDATA: Failed to read data\r\n");
    }

    /* Update queue */
    if (!queue_pop_front(queue, &elem)) {
        k_mutex_unlock(&conn_mutex);
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPRECVDATA: Failed to update queue\r\n");
    }

    if (read_len == elem.data_len) {
        if (server_flag[0] == 'C') {
            ipd_message_print_flag = true;
        } else if (conn->type == PROTOCOL_TCP || conn->type == PROTOCOL_TCPv6) {
            server_ipd_message_print_flag = true;
        } else {
            udp_server_ipd_message_print_flag = true;
        }
    } else {
        elem.data_len -= read_len;
        if (!queue_push_front(queue, &elem)) {
            k_mutex_unlock(&conn_mutex);
            return QAT_Response_Str(QAT_RC_ERROR, "+CIPRECVDATA: Failed to update queue\r\n");
        }
    }

    k_mutex_unlock(&conn_mutex);

    /* Format response */
    s_at_data_buf[read_len] = '\0';
    snprintf(s_ciprecv_response, sizeof(s_ciprecv_response), "+CIPRECVDATA:%c,%s,%d,%s",
             server_flag[0], proto, read_len, s_at_data_buf);

    return QAT_Response_Str(QAT_RC_OK, s_ciprecv_response);
}

/*-------------------------------------------------------------------------
 * AT+CIPPING - Ping Test
 *-----------------------------------------------------------------------*/

/* Ping helper: prepare ICMP echo request */
static void ping_prepare_echo(struct net_icmp_ping_params *params, bool is_ipv6)
{
    params->identifier = (uint16_t)sys_rand32_get();
    params->sequence = ping_ctx.sequence;
    params->tc_tos = 0;
    params->priority = 0;
    params->data = NULL;
    params->data_size = ping_ctx.payload_size;
}

/* Ping callback for IPv4 */
static int ping_recv_ipv4(struct net_icmp_ctx *ctx,
                         struct net_pkt *pkt,
                         struct net_icmp_ip_hdr *hdr,
                         struct net_icmp_hdr *icmp_hdr,
                         void *user_data)
{
    char response[256];
    struct net_ipv4_hdr *ip_hdr = hdr->ipv4;
    char ip_str[INET_ADDRSTRLEN];

    net_addr_ntop(AF_INET, &ip_hdr->src, ip_str, sizeof(ip_str));

    ping_ctx.received++;
    ping_ctx.waiting_reply = false;

    snprintf(response, sizeof(response), "+CIPPING:%s,%u,%ums\r\n",
            ip_str, ping_ctx.sequence,
            (uint32_t)(k_uptime_get_32() - ping_ctx.sent_time));

    QAT_Response_Str(QAT_RC_QUIET, response);

    if (ping_ctx.sequence >= ping_ctx.count) {
        k_sem_give(&ping_ctx.done_sem);
    }

    return 0;
}

/* Ping callback for IPv6 */
static int ping_recv_ipv6(struct net_icmp_ctx *ctx,
                         struct net_pkt *pkt,
                         struct net_icmp_ip_hdr *hdr,
                         struct net_icmp_hdr *icmp_hdr,
                         void *user_data)
{
    char response[256];
    struct net_ipv6_hdr *ip_hdr = hdr->ipv6;
    char ip_str[INET6_ADDRSTRLEN];

    net_addr_ntop(AF_INET6, &ip_hdr->src, ip_str, sizeof(ip_str));

    ping_ctx.received++;
    ping_ctx.waiting_reply = false;

    snprintf(response, sizeof(response), "+CIPPING:%s,%u,%ums\r\n",
            ip_str, ping_ctx.sequence,
            (uint32_t)(k_uptime_get_32() - ping_ctx.sent_time));

    QAT_Response_Str(QAT_RC_QUIET, response);

    if (ping_ctx.sequence >= ping_ctx.count) {
        k_sem_give(&ping_ctx.done_sem);
    }

    return 0;
}

/* Ping work handler */
static void ping_work_handler(struct k_work *work)
{
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct net_icmp_ping_params params;
    int ret;
    char response[256];

    if (!ping_ctx.active) {
        return;
    }

    /* Previous ping got no reply: report timeout before sending next */
    if (ping_ctx.waiting_reply) {
        snprintf(response, sizeof(response), "+CIPPING:Request timed out!\r\n");
        QAT_Response_Str(QAT_RC_QUIET, response);
        ping_ctx.waiting_reply = false;
    }

    ping_ctx.sequence++;

    if (ping_ctx.sequence > ping_ctx.count) {
        k_sem_give(&ping_ctx.done_sem);
        return;
    }

    /* Prepare ping parameters */
    bool is_ipv6 = (ping_ctx.addr.sa_family == AF_INET6);
    ping_prepare_echo(&params, is_ipv6);

    /* Send ping */
    ping_ctx.sent_time = k_uptime_get_32();
    ret = net_icmp_send_echo_request(&ping_ctx.icmp,
                                    ping_ctx.iface,
                                    &ping_ctx.addr,
                                    &params,
                                    &ping_ctx);

    if (ret < 0) {
        snprintf(response, sizeof(response), "+CIPPING:ping send %.100s - error\r\n",
                ping_ctx.host);
        QAT_Response_Str(QAT_RC_QUIET, response);
        k_sem_give(&ping_ctx.done_sem);
        return;
    }

    ping_ctx.sent++;
    ping_ctx.waiting_reply = true;

    /* Schedule reply timeout; last ping uses same timeout as others */
    if (ping_ctx.sequence < ping_ctx.count) {
        k_work_reschedule(dwork, K_MSEC(ping_ctx.interval));
    } else {
        k_work_reschedule(dwork, K_MSEC(RECV_TIMEOUT_MS));
    }
}

static cat_return_state cmd_cipping_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK,
        "+CIPPING=<host>[,<count>[,<delay>[,<size>]]]\r\n");
}

static cat_return_state cmd_cipping_set(const struct cat_command *cmd,
                                        const uint8_t *data,
                                        const size_t data_size,
                                        const size_t args_num)
{
    char host[128];
    int count = PING_DEFAULT_COUNT;
    int delay = PING_DEFAULT_DELAY_MS;
    int size = PING_DEFAULT_SIZE;
    char response[256];
    int ret;

    /* Try parsing with quotes first */
    int parsed = sscanf((char *)data, "\"%127[^\"]\",%d,%d,%d",
                       host, &count, &delay, &size);

    /* If that fails, try without quotes */
    if (parsed < 1) {
        parsed = sscanf((char *)data, "%127[^,],%d,%d,%d",
                       host, &count, &delay, &size);
    }

    if (parsed < 1) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPPING: Invalid parameters\r\n");
    }

    /* Check if ping is already active */
    if (ping_ctx.active) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPPING: Ping already in progress\r\n");
    }

    LOG_INF("CIPPING: host=%s, count=%d, delay=%d, size=%d", host, count, delay, size);

    /* Resolve host */
    struct zsock_addrinfo hints = {0};
    struct zsock_addrinfo *res;

    hints.ai_family = AF_UNSPEC;  /* Allow both IPv4 and IPv6 */
    hints.ai_socktype = SOCK_RAW;

    if (zsock_getaddrinfo(host, NULL, &hints, &res) != 0) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPPING: Failed to resolve host\r\n");
    }

    /* Initialize ping context */
    memset(&ping_ctx, 0, sizeof(ping_ctx));
    k_sem_init(&ping_ctx.done_sem, 0, 1);
    memcpy(ping_ctx.host, host, sizeof(ping_ctx.host) - 1);
    ping_ctx.host[sizeof(ping_ctx.host) - 1] = '\0';
    ping_ctx.count = count;
    ping_ctx.interval = delay;
    ping_ctx.payload_size = size;
    ping_ctx.sequence = 0;
    ping_ctx.sent = 0;
    ping_ctx.received = 0;
    ping_ctx.active = true;

    /* Copy address */
    if (res->ai_family == AF_INET) {
        struct sockaddr_in *addr4 = (struct sockaddr_in *)res->ai_addr;
        ping_ctx.addr4.sin_family = AF_INET;
        ping_ctx.addr4.sin_addr = addr4->sin_addr;

        /* Initialize ICMP context for IPv4 */
        ret = net_icmp_init_ctx(&ping_ctx.icmp, AF_INET, NET_ICMPV4_ECHO_REPLY,
                               0, ping_recv_ipv4);
    } else {
        struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)res->ai_addr;
        ping_ctx.addr6.sin6_family = AF_INET6;
        ping_ctx.addr6.sin6_addr = addr6->sin6_addr;

        /* Initialize ICMP context for IPv6 */
        ret = net_icmp_init_ctx(&ping_ctx.icmp, AF_INET6, NET_ICMPV6_ECHO_REPLY,
                               0, ping_recv_ipv6);
    }

    zsock_freeaddrinfo(res);

    if (ret < 0) {
        ping_ctx.active = false;
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPPING: Failed to initialize ICMP\r\n");
    }

    /* Get network interface */
    ping_ctx.iface = get_default_iface();
    if (!ping_ctx.iface) {
        net_icmp_cleanup_ctx(&ping_ctx.icmp);
        ping_ctx.active = false;
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPPING: No network interface\r\n");
    }

    /* Start ping work */
    k_work_init_delayable(&ping_ctx.work, ping_work_handler);
    k_work_schedule(&ping_ctx.work, K_NO_WAIT);

    /* Wait for completion */
    ret = k_sem_take(&ping_ctx.done_sem, K_SECONDS(count * 2 + 5));

    /* Cleanup */
    k_work_cancel_delayable(&ping_ctx.work);
    net_icmp_cleanup_ctx(&ping_ctx.icmp);

    /* Print summary */
    snprintf(response, sizeof(response), "+CIPPING:%d,%d\r\n",
            ping_ctx.sent, ping_ctx.received);
    QAT_Response_Str(QAT_RC_QUIET, response);

    ping_ctx.active = false;

    return QAT_Response_Str(QAT_RC_OK, NULL);
}

/*-------------------------------------------------------------------------
 * AT+CIPDHCPV4C - DHCP Client
 *-----------------------------------------------------------------------*/
static cat_return_state cmd_cipdhcpv4c_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK,
                            "+CIPDHCPV4C=<interface>,<action>\r\n"
                            "  interface: wlan0 (AP), wlan1 (STA)\r\n"
                            "  action: new, release\r\n");
}

static cat_return_state cmd_cipdhcpv4c_query(const struct cat_command *cmd, uint8_t *data, size_t *data_size,
                                             const size_t max_data_size)
{
    char buffer[512];
    int offset = 0;
    *data_size = 0;

    struct net_if *iface = get_iface_by_qat_id(NT_DEV_STA_ID);
    if (!iface) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPDHCPV4C: No STA interface\r\n");
    }

    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "+CIPDHCPV4C:");

    /* Check DHCP state */
    struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;
    if (ipv4) {
        /* Get current IP configuration */
        struct net_if_addr_ipv4 *unicast = NULL;
        for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
            if (ipv4->unicast[i].ipv4.is_used &&
                ipv4->unicast[i].ipv4.addr_state == NET_ADDR_PREFERRED) {
                unicast = &ipv4->unicast[i];
                break;
            }
        }

        if (unicast) {
            char ip_str[NET_IPV4_ADDR_LEN];
            char gw_str[NET_IPV4_ADDR_LEN];
            char nm_str[NET_IPV4_ADDR_LEN];

            net_addr_ntop(AF_INET, &unicast->ipv4.address.in_addr,
                         ip_str, sizeof(ip_str));
            net_addr_ntop(AF_INET, &ipv4->gw, gw_str, sizeof(gw_str));
            net_addr_ntop(AF_INET, &unicast->netmask, nm_str, sizeof(nm_str));

            offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                             "%s,%s,%s", ip_str, gw_str, nm_str);
        } else {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                             "No IP address assigned");
        }
    } else {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                         "IPv4 not configured");
    }

    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\r\n");
    return QAT_Response_Str(QAT_RC_OK, buffer);
}

static cat_return_state cmd_cipdhcpv4c_set(const struct cat_command *cmd,
                                           const uint8_t *data,
                                           const size_t data_size,
                                           const size_t args_num)
{
    char ifname[16];
    char action[16];
    char response[256];

    if (sscanf((char *)data, "%15[^,],%15[^\r\n]", ifname, action) != 2) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPDHCPV4C: Invalid parameters\r\n");
    }

    int iface_id;
    if (strcmp(ifname, "wlan0") == 0) {
        iface_id = NT_DEV_AP_ID;
    } else if (strcmp(ifname, "wlan1") == 0) {
        iface_id = NT_DEV_STA_ID;
    } else {
        return QAT_Response_Str(QAT_RC_ERROR,
                                "+CIPDHCPV4C: interface must be wlan1 (STA)\r\n");
    }

    struct net_if *iface = get_iface_by_qat_id(iface_id);
    if (!iface) {
        snprintf(response, sizeof(response),
                 "+CIPDHCPV4C: No interface found for %s\r\n", ifname);
        return QAT_Response_Str(QAT_RC_ERROR, response);
    }

    if (iface_id != NT_DEV_STA_ID) {
        return QAT_Response_Str(QAT_RC_ERROR,
                                "+CIPDHCPV4C: Only wlan1 (STA) supports DHCP client\r\n");
    }

    if (strcmp(action, "new") == 0) {
        /* Start DHCP client; detailed IP info will be reported via +EVT:dhcp_bound from qat_wlan.c */
        net_dhcpv4_start(iface);

        snprintf(response, sizeof(response),
                 "+CIPDHCPV4C: DHCP client start\r\n");
        LOG_INF("DHCP client started on STA (iface_id=%d)", iface_id);

    } else if (strcmp(action, "release") == 0) {
        /* Stop DHCP client and release IP */
        net_dhcpv4_stop(iface);

        /* Remove IP addresses */
        struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;
        if (ipv4) {
            for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
                if (ipv4->unicast[i].ipv4.is_used) {
                    net_if_ipv4_addr_rm(iface, &ipv4->unicast[i].ipv4.address.in_addr);
                }
            }
        }

        snprintf(response, sizeof(response), "+CIPDHCPV4C: DHCP released\r\n");
        LOG_INF("DHCP client stopped and IP released on STA (iface_id=%d)", iface_id);

    } else {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPDHCPV4C: Invalid action (use 'new' or 'release')\r\n");
    }

    return QAT_Response_Str(QAT_RC_OK, response);
}

/*-------------------------------------------------------------------------
 * Server Helper Functions
 *-----------------------------------------------------------------------*/
static void cleanup_server_conn_entry(connection_info_t *conns, protocol_type_t queue_type, int link_id)
{
    if (!conns || link_id < 0 || link_id >= MAX_CONNECTIONS) {
        return;
    }

    cleanup_server_queue_entries(queue_type, link_id);

    conns[link_id].sock_fd = INVALID_FD;
    conns[link_id].state = CONN_STATE_IDLE;
    conns[link_id].type = PROTOCOL_INVALID;
    conns[link_id].recv_mode = RECV_MODE_ACTIVE;
    memset(conns[link_id].remote_ip, 0, sizeof(conns[link_id].remote_ip));
    conns[link_id].remote_port = 0;
    conns[link_id].local_port = 0;
    conns[link_id].is_server = false;
    conns[link_id].thread_quit = false;
    conns[link_id].id = INVALID_LINKID;

    if (conns[link_id].recv_buf) {
        circular_buffer_destroy(conns[link_id].recv_buf);
        conns[link_id].recv_buf = NULL;
    }
}

static const char *protocol_type_to_at_name(protocol_type_t type)
{
    switch (type) {
    case PROTOCOL_TCP:
    case PROTOCOL_TCPv6:
        return "TCP";
    case PROTOCOL_UDP:
    case PROTOCOL_UDPv6:
        return "UDP";
    default:
        return NULL;
    }
}

static bool queue_pop_front(struct k_msgq *queue, queue_elem_t *elem)
{
    return k_msgq_get(queue, elem, K_NO_WAIT) == 0;
}

static bool queue_push_front(struct k_msgq *queue, const queue_elem_t *elem)
{
    queue_elem_t items[10];
    size_t count = 0;
    queue_elem_t tmp;

    while (count < ARRAY_SIZE(items) && k_msgq_get(queue, &tmp, K_NO_WAIT) == 0) {
        items[count++] = tmp;
    }

    if (k_msgq_put(queue, elem, K_NO_WAIT) != 0) {
        for (size_t i = 0; i < count; i++) {
            (void)k_msgq_put(queue, &items[i], K_NO_WAIT);
        }
        return false;
    }

    for (size_t i = 0; i < count; i++) {
        if (k_msgq_put(queue, &items[i], K_NO_WAIT) != 0) {
            return false;
        }
    }

    return true;
}

static void queue_remove_link_entries(struct k_msgq *queue, int link_id)
{
    queue_elem_t items[10];
    size_t count = 0;
    queue_elem_t elem;

    while (count < ARRAY_SIZE(items) && k_msgq_get(queue, &elem, K_NO_WAIT) == 0) {
        if (elem.link_id != link_id) {
            items[count++] = elem;
        }
    }

    for (size_t i = 0; i < count; i++) {
        (void)k_msgq_put(queue, &items[i], K_NO_WAIT);
    }
}

static void cleanup_server_queue_entries(protocol_type_t type, int link_id)
{
    if (type == PROTOCOL_TCP || type == PROTOCOL_TCPv6) {
        queue_remove_link_entries(&tcp_server_queue, link_id);
    } else if (type == PROTOCOL_UDP || type == PROTOCOL_UDPv6) {
        queue_remove_link_entries(&udp_server_queue, link_id);
    }
}

static void clear_msgq(struct k_msgq *queue)
{
    queue_elem_t elem;

    while (k_msgq_get(queue, &elem, K_NO_WAIT) == 0) {
    }
}

static int find_free_server_slot(connection_info_t *conns)
{
    if (!conns) {
        return -1;
    }

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (conns[i].state == CONN_STATE_IDLE) {
            return i;
        }
    }
    return -1;
}

/*-------------------------------------------------------------------------
 * TCP Server Thread
 *-----------------------------------------------------------------------*/
static void tcp_server_work_handler(struct k_work *work)
{
    if (tcp_config.mode == 0 && tcp_config.close_server) {
        if (tcp_listen_fd != INVALID_FD) {
            zsock_close(tcp_listen_fd);
            tcp_listen_fd = INVALID_FD;
        }

        for (int i = 0; i < MAX_CONNECTIONS; i++) {
            if (g_listen_clients[i].sock_fd != INVALID_FD) {
                zsock_close(g_listen_clients[i].sock_fd);
                cleanup_server_conn_entry(g_listen_clients, PROTOCOL_TCP, i);
            }
        }

        clear_msgq(&tcp_server_queue);
        tcp_server_running = false;
        tcp_server_accept_new_client = true;
        server_ipd_message_print_flag = true;
        LOG_INF("TCP server stopped");
        return;
    }

    /* Prepare poll fds */
    struct zsock_pollfd fds[MAX_CONNECTIONS + 1];
    int nfds = 0;

    /* Add listen socket */
    if (tcp_server_accept_new_client && tcp_listen_fd != INVALID_FD) {
        fds[nfds].fd = tcp_listen_fd;
        fds[nfds].events = ZSOCK_POLLIN;
        nfds++;
    }

    /* Add client sockets */
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (g_listen_clients[i].sock_fd != INVALID_FD) {
            fds[nfds].fd = g_listen_clients[i].sock_fd;
            fds[nfds].events = ZSOCK_POLLIN;
            nfds++;
        }
    }

    int ret = zsock_poll(fds, nfds, 0);
    if (ret > 0) {
        /* Check listen socket for new connections */
        if (tcp_server_accept_new_client && tcp_listen_fd != INVALID_FD && (fds[0].revents & ZSOCK_POLLIN)) {
            struct sockaddr_storage client_addr;
            socklen_t addr_len = sizeof(client_addr);

            int client_fd = zsock_accept(tcp_listen_fd, (struct sockaddr *)&client_addr, &addr_len);
            if (client_fd >= 0) {
                int slot = find_free_server_slot(g_listen_clients);
                if (slot >= 0) {
                    /* Set non-blocking */
                    int flags = zsock_fcntl(client_fd, F_GETFL, 0);
                    zsock_fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

                    /* Store client info */
                    g_listen_clients[slot].sock_fd = client_fd;
                    g_listen_clients[slot].state = CONN_STATE_CONNECTED;
                    g_listen_clients[slot].type = tcp_config.type;
                    g_listen_clients[slot].recv_mode = RECV_MODE_ACTIVE;
                    g_listen_clients[slot].id = slot;
                    g_listen_clients[slot].is_server = true;
                    g_listen_clients[slot].recv_buf = circular_buffer_create();

                    /* Get remote address */
                    if (client_addr.ss_family == AF_INET) {
                        struct sockaddr_in *addr4 = (struct sockaddr_in *)&client_addr;
                        zsock_inet_ntop(AF_INET, &addr4->sin_addr,
                                      g_listen_clients[slot].remote_ip,
                                      sizeof(g_listen_clients[slot].remote_ip));
                        g_listen_clients[slot].remote_port = ntohs(addr4->sin_port);
                    }

                    LOG_INF("TCP server accepted client on slot %d", slot);
                } else {
                    zsock_close(client_fd);
                    LOG_WRN("TCP server: no free slots");
                }
            }
        }

        /* Check client sockets for data */
        for (int i = 0; i < MAX_CONNECTIONS; i++) {
            if (g_listen_clients[i].sock_fd == INVALID_FD) {
                continue;
            }

            /* Find this socket in poll results */
            bool has_data = false;
            for (int j = 0; j < nfds; j++) {
                if (fds[j].fd == g_listen_clients[i].sock_fd &&
                    (fds[j].revents & ZSOCK_POLLIN)) {
                    has_data = true;
                    break;
                }
            }

            if (!has_data) {
                continue;
            }

            ssize_t recv_len = zsock_recv(g_listen_clients[i].sock_fd, s_recv_buf, sizeof(s_recv_buf), 0);

            if (recv_len > 0) {
                if (g_listen_clients[i].recv_mode == RECV_MODE_ACTIVE) {
                    /* Active mode: print immediately */
                    const char *server_proto =
                        (g_listen_clients[i].type == PROTOCOL_TCPv6) ? "TCPv6" : "TCP";

                    if (is_passthrough_mode) {
                        int offset = snprintf(s_recv_response, sizeof(s_recv_response),
                                            "+IPDHEX:S,%s,%d,%zd,", server_proto, i, recv_len);
                        memcpy(s_recv_response + offset, s_recv_buf, recv_len);
                        QAT_Output(offset + recv_len, s_recv_response);
                    } else {
                        s_recv_buf[recv_len] = '\0';
                        snprintf(s_recv_response, sizeof(s_recv_response), "+IPD:S,%s,%d,%zd,%s",
                                server_proto, i, recv_len, s_recv_buf);
                        QAT_Response_Str(QAT_RC_QUIET, s_recv_response);
                    }
                } else {
                    /* Passive mode: store in buffer */
                    if (circular_buffer_write(g_listen_clients[i].recv_buf, s_recv_buf, recv_len) == 0) {
                        queue_elem_t elem = {.link_id = i, .data_len = recv_len};
                        k_msgq_put(&tcp_server_queue, &elem, K_NO_WAIT);

                        if (server_ipd_message_print_flag) {
                            char response[64];
                            const char *server_proto =
                                (g_listen_clients[i].type == PROTOCOL_TCPv6) ? "TCPv6" : "TCP";
                            snprintf(response, sizeof(response), "+IPD:S,%s,%d,%zd\r\n", server_proto, i, recv_len);
                            QAT_Response_Str(QAT_RC_QUIET, response);
                            server_ipd_message_print_flag = false;
                        }
                    }
                }
            } else if (recv_len == 0 || (recv_len < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                /* Client disconnected */
                char response[64];

                snprintf(response, sizeof(response), "+IPS:CLOSED:%d", i);
                QAT_Response_Str(QAT_RC_QUIET, response);

                zsock_close(g_listen_clients[i].sock_fd);
                cleanup_server_conn_entry(g_listen_clients, PROTOCOL_TCP, i);
                LOG_INF("TCP server client %d disconnected", i);
            }
        }
    }

    /* Reschedule if server is still running */
    if (tcp_server_running) {
        k_work_reschedule((struct k_work_delayable *)work, K_MSEC(50));
    }
}

static K_WORK_DELAYABLE_DEFINE(tcp_server_work, tcp_server_work_handler);

/*-------------------------------------------------------------------------
 * AT+CIPSERVER - TCP Server
 *-----------------------------------------------------------------------*/
static cat_return_state cmd_cipserver_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK,
        "+CIPSERVER=<mode>,<param2>\r\n"
        "  mode: 0 (stop), 1 (start)\r\n"
        "  param2: 0/1 when mode=0, port when mode=1\r\n");
}

static cat_return_state cmd_cipserver_query(const struct cat_command *cmd, uint8_t *data, size_t *data_size,
                                            const size_t max_data_size)
{
    char buffer[QAT_RESPONSE_BUF_SIZE];
    int offset = 0;
    *data_size = 0;

    k_mutex_lock(&conn_mutex, K_FOREVER);

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (g_listen_clients[i].state != CONN_STATE_CONNECTED) {
            continue;
        }

        char local_ip[INET6_ADDRSTRLEN] = {0};
        uint16_t local_port = tcp_config.port;
        const char *type_str = (g_listen_clients[i].type == PROTOCOL_TCPv6) ? "TCPv6" : "TCP";
        struct sockaddr_storage local_addr;
        socklen_t local_addr_len = sizeof(local_addr);

        if (zsock_getsockname(g_listen_clients[i].sock_fd, (struct sockaddr *)&local_addr, &local_addr_len) == 0) {
            if (local_addr.ss_family == AF_INET) {
                struct sockaddr_in *addr4 = (struct sockaddr_in *)&local_addr;
                zsock_inet_ntop(AF_INET, &addr4->sin_addr, local_ip, sizeof(local_ip));
                local_port = ntohs(addr4->sin_port);
            } else if (local_addr.ss_family == AF_INET6) {
                struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&local_addr;
                zsock_inet_ntop(AF_INET6, &addr6->sin6_addr, local_ip, sizeof(local_ip));
                local_port = ntohs(addr6->sin6_port);
            }
        }

        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                          "+CIPSERVER:S,%d,%s,%s,%d,%s,%d\r\n",
                          i, type_str, g_listen_clients[i].remote_ip,
                          g_listen_clients[i].remote_port,
                          local_ip[0] ? local_ip : "0.0.0.0",
                          local_port);
    }

    k_mutex_unlock(&conn_mutex);

    return QAT_Response_Str(QAT_RC_OK, buffer);
}

static cat_return_state cmd_cipserver_set(const struct cat_command *cmd,
                                          const uint8_t *data,
                                          const size_t data_size,
                                          const size_t args_num)
{
    int mode, value;
    char type_str[16] = {0};
    int parsed = sscanf((char *)data, "%d,%d,%15[^,\r\n]", &mode, &value, type_str);

    if (parsed < 2) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPSERVER: Invalid parameters\r\n");
    }

    if (mode == 0) {
        /* Stop server */
        if (value < 0 || value > 1) {
            return QAT_Response_Str(QAT_RC_ERROR, "+CIPSERVER: param2 must be 0 or 1\r\n");
        }

        tcp_config.mode = 0;
        tcp_config.close_server = (value == 1);

        if (tcp_server_running) {
            if (tcp_config.close_server) {
                k_work_cancel_delayable(&tcp_server_work);
                tcp_server_work_handler(&tcp_server_work.work);
            } else {
                tcp_server_accept_new_client = false;
                tcp_config.mode = 1;
                tcp_config.close_server = false;
                if (tcp_listen_fd != INVALID_FD) {
                    zsock_close(tcp_listen_fd);
                    tcp_listen_fd = INVALID_FD;
                }
            }
        }

        return QAT_Response_Str(QAT_RC_OK, NULL);

    } else if (mode == 1) {
        /* Start server */
        if (value < 0 || value > 65535) {
            return QAT_Response_Str(QAT_RC_ERROR, "+CIPSERVER: Invalid port\r\n");
        }

        if (tcp_server_running) {
            return QAT_Response_Str(QAT_RC_ERROR, "+CIPSERVER: Server already running\r\n");
        }

        protocol_type_t server_type = PROTOCOL_TCP;
        int family = AF_INET;

        if (parsed >= 3 && type_str[0] != '\0') {
            if (strcmp(type_str, "TCP") == 0) {
                server_type = PROTOCOL_TCP;
                family = AF_INET;
            } else if (strcmp(type_str, "TCPv6") == 0) {
                server_type = PROTOCOL_TCPv6;
                family = AF_INET6;
            } else {
                return QAT_Response_Str(QAT_RC_ERROR, "+CIPSERVER: Invalid type\r\n");
            }
        }

        /* Create listen socket */
        tcp_listen_fd = zsock_socket(family, SOCK_STREAM, IPPROTO_TCP);
        if (tcp_listen_fd < 0) {
            return QAT_Response_Str(QAT_RC_ERROR, "+CIPSERVER: Failed to create socket\r\n");
        }

        /* Set socket options */
        int opt = 1;
        zsock_setsockopt(tcp_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        /* Bind to port */
        if (family == AF_INET) {
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(value);

            if (zsock_bind(tcp_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                zsock_close(tcp_listen_fd);
                tcp_listen_fd = INVALID_FD;
                return QAT_Response_Str(QAT_RC_ERROR, "+CIPSERVER: Failed to bind\r\n");
            }
        } else {
            struct sockaddr_in6 addr6;
            memset(&addr6, 0, sizeof(addr6));
            addr6.sin6_family = AF_INET6;
            addr6.sin6_addr = in6addr_any;
            addr6.sin6_port = htons(value);

            if (zsock_bind(tcp_listen_fd, (struct sockaddr *)&addr6, sizeof(addr6)) < 0) {
                zsock_close(tcp_listen_fd);
                tcp_listen_fd = INVALID_FD;
                return QAT_Response_Str(QAT_RC_ERROR, "+CIPSERVER: Failed to bind\r\n");
            }
        }

        /* Listen */
        if (zsock_listen(tcp_listen_fd, MAX_CONNECTIONS) < 0) {
            zsock_close(tcp_listen_fd);
            tcp_listen_fd = INVALID_FD;
            return QAT_Response_Str(QAT_RC_ERROR, "+CIPSERVER: Failed to listen\r\n");
        }

        /* Set non-blocking */
        int flags = zsock_fcntl(tcp_listen_fd, F_GETFL, 0);
        zsock_fcntl(tcp_listen_fd, F_SETFL, flags | O_NONBLOCK);

        /* Start server work */
        tcp_config.mode = 1;
        tcp_config.port = value;
        tcp_config.type = server_type;
        tcp_config.close_server = false;
        tcp_config.accept_new_peer = true;
        tcp_server_running = true;
        tcp_server_accept_new_client = true;

        k_work_reschedule(&tcp_server_work, K_MSEC(100));

        LOG_INF("TCP server started on port %d", value);
        return QAT_Response_Str(QAT_RC_OK, NULL);

    } else {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPSERVER: Invalid mode\r\n");
    }
}

/*-------------------------------------------------------------------------
 * UDP Server Thread
 *-----------------------------------------------------------------------*/
static void udp_server_work_handler(struct k_work *work)
{
    if (udp_config.mode == 0 && udp_config.close_server) {
        for (int i = 0; i < MAX_CONNECTIONS; i++) {
            cleanup_server_conn_entry(g_listen_udp_clients, PROTOCOL_UDP, i);
        }

        if (udp_listen_fd != INVALID_FD) {
            zsock_close(udp_listen_fd);
            udp_listen_fd = INVALID_FD;
        }

        clear_msgq(&udp_server_queue);
        udp_server_running = false;
        udp_server_ipd_message_print_flag = true;
        LOG_INF("UDP server stopped");
        return;
    }

    /* Poll UDP socket */
    struct zsock_pollfd fds[1];
    fds[0].fd = udp_listen_fd;
    fds[0].events = ZSOCK_POLLIN;

    int ret = zsock_poll(fds, 1, 0);
    if (ret > 0 && (fds[0].revents & ZSOCK_POLLIN)) {
        struct sockaddr_storage from_addr;
        socklen_t from_len = sizeof(from_addr);

        ssize_t recv_len = zsock_recvfrom(udp_listen_fd, s_recv_buf, sizeof(s_recv_buf), 0,
                                         (struct sockaddr *)&from_addr, &from_len);

        if (recv_len > 0) {
            /* Find or create client slot */
            int slot = -1;
            char from_ip[INET6_ADDRSTRLEN];
            uint16_t from_port;

            if (from_addr.ss_family == AF_INET) {
                struct sockaddr_in *addr4 = (struct sockaddr_in *)&from_addr;
                zsock_inet_ntop(AF_INET, &addr4->sin_addr, from_ip, sizeof(from_ip));
                from_port = ntohs(addr4->sin_port);
            } else {
                struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&from_addr;
                zsock_inet_ntop(AF_INET6, &addr6->sin6_addr, from_ip, sizeof(from_ip));
                from_port = ntohs(addr6->sin6_port);
            }

            /* Find existing client */
            for (int i = 0; i < MAX_CONNECTIONS; i++) {
                if (g_listen_udp_clients[i].state == CONN_STATE_CONNECTED &&
                    strcmp(g_listen_udp_clients[i].remote_ip, from_ip) == 0 &&
                    g_listen_udp_clients[i].remote_port == from_port) {
                    slot = i;
                    break;
                }
            }

            /* Create new client if not found */
            if (slot < 0 && udp_config.accept_new_peer) {
                slot = find_free_server_slot(g_listen_udp_clients);
                if (slot >= 0) {
                    g_listen_udp_clients[slot].sock_fd = udp_listen_fd;
                    g_listen_udp_clients[slot].state = CONN_STATE_CONNECTED;
                    g_listen_udp_clients[slot].type = udp_config.type;
                    g_listen_udp_clients[slot].recv_mode = RECV_MODE_ACTIVE;
                    g_listen_udp_clients[slot].id = slot;
                    g_listen_udp_clients[slot].is_server = true;
                    memcpy(g_listen_udp_clients[slot].remote_ip, from_ip, sizeof(g_listen_udp_clients[slot].remote_ip) - 1);
                    g_listen_udp_clients[slot].remote_ip[sizeof(g_listen_udp_clients[slot].remote_ip) - 1] = '\0';
                    g_listen_udp_clients[slot].remote_port = from_port;
                    g_listen_udp_clients[slot].recv_buf = circular_buffer_create();
                }
            }

            if (slot >= 0) {
                if (g_listen_udp_clients[slot].recv_mode == RECV_MODE_ACTIVE) {
                    /* Active mode */
                    if (is_passthrough_mode) {
                        int offset = snprintf(s_recv_response, sizeof(s_recv_response),
                                            "+IPDHEX:S,UDP,%d,%zd,", slot, recv_len);
                        memcpy(s_recv_response + offset, s_recv_buf, recv_len);
                        QAT_Output(offset + recv_len, s_recv_response);
                    } else {
                        s_recv_buf[recv_len] = '\0';
                        snprintf(s_recv_response, sizeof(s_recv_response), "+IPD:S,UDP,%d,%zd,%s",
                                slot, recv_len, s_recv_buf);
                        QAT_Response_Str(QAT_RC_QUIET, s_recv_response);
                    }
                } else {
                    /* Passive mode */
                    if (circular_buffer_write(g_listen_udp_clients[slot].recv_buf, s_recv_buf, recv_len) == 0) {
                        queue_elem_t elem = {.link_id = slot, .data_len = recv_len};
                        k_msgq_put(&udp_server_queue, &elem, K_NO_WAIT);

                        if (udp_server_ipd_message_print_flag) {
                            char response[64];
                            snprintf(response, sizeof(response), "+IPD:S,UDP,%d,%zd\r\n", slot, recv_len);
                            QAT_Response_Str(QAT_RC_QUIET, response);
                            udp_server_ipd_message_print_flag = false;
                        }
                    }
                }
            }
        }
    }

    /* Reschedule if server is still running */
    if (udp_server_running) {
        k_work_reschedule((struct k_work_delayable *)work, K_MSEC(100));
    }
}

static K_WORK_DELAYABLE_DEFINE(udp_server_work, udp_server_work_handler);

/*-------------------------------------------------------------------------
 * AT+CIPMODE - Passthrough Mode
 *-----------------------------------------------------------------------*/
static cat_return_state cmd_cipmode_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK,
        "+CIPMODE=<mode>\r\n"
        "  mode: 0 (normal), 1 (passthrough)\r\n");
}

static cat_return_state cmd_cipmode_query(const struct cat_command *cmd, uint8_t *data, size_t *data_size,
                                          const size_t max_data_size)
{
    char buffer[64];
    *data_size = 0;
    snprintf(buffer, sizeof(buffer), "+CIPMODE:%d\r\n", cipmode);
    return QAT_Response_Str(QAT_RC_OK, buffer);
}

static cat_return_state cmd_cipmode_set(const struct cat_command *cmd,
                                        const uint8_t *data,
                                        const size_t data_size,
                                        const size_t args_num)
{
    int mode;

    if (sscanf((char *)data, "%d", &mode) != 1) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPMODE: Invalid parameter\r\n");
    }

    if (mode != 0 && mode != 1) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPMODE: Mode must be 0 or 1\r\n");
    }

    cipmode = mode;
    is_passthrough_mode = (mode == 1) ? 1 : 0;

    LOG_INF("CIPMODE set to %d (%s)", mode, mode ? "passthrough" : "normal");
    return QAT_Response_Str(QAT_RC_OK, NULL);
}

/*-------------------------------------------------------------------------
 * AT+CIPV6 - IPv6 Enable/Disable
 *-----------------------------------------------------------------------*/
static cat_return_state cmd_cipv6_exec(const struct cat_command *cmd)
{
    /* Align with FreeRTOS: only enable/disable switch */
    return QAT_Response_Str(QAT_RC_OK, "AT+CIPV6=<enable>\r\n");
}

static cat_return_state cmd_cipv6_query(const struct cat_command *cmd, uint8_t *data, size_t *data_size,
                                        const size_t max_data_size)
{
    char buffer[32];
    *data_size = 0;
    snprintf(buffer, sizeof(buffer), "+CIPV6:%d\r\n", ipv6_enabled ? 1 : 0);
    return QAT_Response_Str(QAT_RC_OK, buffer);
}

static cat_return_state cmd_cipv6_set(const struct cat_command *cmd,
                                      const uint8_t *data,
                                      const size_t data_size,
                                      const size_t args_num)
{
    int enable;

    /* Align with FreeRTOS: only one parameter */
    if (sscanf((char *)data, "%d", &enable) != 1) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPV6:Invalid input parameter!\r\n");
    }

    if (enable != 0 && enable != 1) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPV6:enable parameter can only be 0 or 1!\r\n");
    }

    /* CIPV6 is a global IPv6 switch for AT usage. Apply to both default iface and SoftAP iface (if exists)
     * to avoid STA/AP role switch issues.
     */
    struct net_if *ifaces[2] = {0};
    ifaces[0] = get_default_iface();
    ifaces[1] = get_ap_iface();
    if (ifaces[0] == ifaces[1]) {
        ifaces[1] = NULL;
    }

    if (!ifaces[0] && !ifaces[1]) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPV6: No network interface\r\n");
    }

    if (enable == 0) {
        for (int n = 0; n < 2; n++) {
            struct net_if *iface = ifaces[n];
            if (!iface) {
                continue;
            }

            struct net_if_ipv6 *ipv6 = iface->config.ip.ipv6;
            if (ipv6) {
                for (int i = 0; i < NET_IF_MAX_IPV6_ADDR; i++) {
                    if (ipv6->unicast[i].is_used) {
                        net_if_ipv6_addr_rm(iface, &ipv6->unicast[i].address.in6_addr);
                    }
                }
            }

            /* If user had configured CIPV6PREFIX, also remove that manual address prefix::1 */
            if (ipv6_prefix_config.configured) {
                struct in6_addr manual_addr = ipv6_prefix_config.prefix; /* this is prefix::1 */
                net_if_ipv6_addr_rm(iface, &manual_addr);
            }

            /* Stop RS on STA iface */
            if (!wifi_nm_iface_is_sap(iface)) {
                net_if_stop_rs(iface);
                LOG_INF("CIPV6: stopped RS on STA iface");
            }
        }

        ipv6_enabled = false;

        /* Stop SAP RA (router advertisement) */
        sap_ra_stop();

        LOG_INF("CIPV6: disabled (IPv6 addresses cleared)");
        return QAT_Response_Str(QAT_RC_OK, NULL);
    }

    ipv6_enabled = true;

    for (int n = 0; n < 2; n++) {
        struct net_if *iface = ifaces[n];
        if (!iface) {
            continue;
        }

        net_if_up(iface);

        /* Ensure link-local exists */
        struct net_if_ipv6 *ipv6 = iface->config.ip.ipv6;
        bool has_ll = false;
        if (ipv6) {
            for (int i = 0; i < NET_IF_MAX_IPV6_ADDR; i++) {
                if (ipv6->unicast[i].is_used && net_ipv6_is_ll_addr(&ipv6->unicast[i].address.in6_addr)) {
                    has_ll = true;
                    break;
                }
            }
        }

        if (!has_ll) {
            struct in6_addr ll_addr;
            net_ipv6_addr_create_iid(&ll_addr, net_if_get_link_addr(iface));
            ll_addr.s6_addr[0] = 0xfe;
            ll_addr.s6_addr[1] = 0x80;

            if (!net_if_ipv6_addr_add(iface, &ll_addr, NET_ADDR_AUTOCONF, 0)) {
                LOG_WRN("CIPV6: failed to add link-local addr");
            } else {
                LOG_INF("CIPV6: link-local addr added");
            }
        }

        /* Re-apply CIPV6PREFIX configured address (prefix::1) on SoftAP iface only */
        if (ipv6_prefix_config.configured && wifi_nm_iface_is_sap(iface)) {
            struct in6_addr manual_addr = ipv6_prefix_config.prefix; /* prefix::1 */
            if (!net_if_ipv6_addr_add(iface, &manual_addr, NET_ADDR_MANUAL, 0)) {
                LOG_WRN("CIPV6: failed to re-add CIPV6PREFIX addr");
            }
        }

        /* For STA iface: send RS only when iface is up (connected to AP).
         * Zephyr will process the RA and configure SLAAC address via
         * handle_prefix_autonomous() -> net_if_ipv6_addr_add(..., NET_ADDR_AUTOCONF)
         */
        if (!wifi_nm_iface_is_sap(iface)) {
            if (net_if_is_up(iface)) {
                net_if_start_rs(iface);
                LOG_INF("CIPV6: started RS on STA iface, SLAAC will auto-configure on RA");
            } else {
                LOG_INF("CIPV6: STA iface is down, skip RS");
            }
        }
    }

    /* Start SAP RA if prefix is already configured */
    {
        struct net_if *sap_iface = get_ap_iface();
        LOG_INF("CIPV6: sap_iface=%p prefix_configured=%d",
                sap_iface, ipv6_prefix_config.configured);
        if (ipv6_prefix_config.configured && sap_iface) {
            sap_ra_start();
        } else if (!sap_iface) {
            LOG_WRN("CIPV6: no SAP iface found, RA not started");
        } else {
            LOG_INF("CIPV6: prefix not configured yet, RA will start after AT+CIPV6PREFIX");
        }
    }

    LOG_INF("CIPV6: enabled");
    return QAT_Response_Str(QAT_RC_OK, NULL);
}

/*-------------------------------------------------------------------------
 * AT+CIPDHCPV4S - DHCP Server
 *-----------------------------------------------------------------------*/
static cat_return_state cmd_cipdhcpv4s_exec(const struct cat_command *cmd)
{
    /* iface_id: 0 = AP, 1 = STA. DHCPv4 server is only valid on AP (0). */
    return QAT_Response_Str(QAT_RC_OK,
        "AT+CIPDHCPV4S=<iface_id>,<action>[,<start_ip>[,<end_ip>[,<lease_time_minute>]]]\r\n"
        "  iface_id: 0 (AP)\r\n"
        "  action: start | stop | pool\r\n");
}

static cat_return_state cmd_cipdhcpv4s_query(const struct cat_command *cmd, uint8_t *data, size_t *data_size,
                                             const size_t max_data_size)
{
    char buffer[256];
    *data_size = 0;

    if (!dhcp_server_config.pool_configured) {
        snprintf(buffer, sizeof(buffer), "+CIPDHCPV4S:%s,POOL=DEFAULT\r\n",
                 dhcp_server_config.running ? "RUNNING" : "STOPPED");
        return QAT_Response_Str(QAT_RC_OK, buffer);
    }

    char start_ip[INET_ADDRSTRLEN];
    char end_ip[INET_ADDRSTRLEN];
    zsock_inet_ntop(AF_INET, &dhcp_server_config.pool_start_ip, start_ip, sizeof(start_ip));
    zsock_inet_ntop(AF_INET, &dhcp_server_config.pool_end_ip, end_ip, sizeof(end_ip));

    snprintf(buffer, sizeof(buffer), "+CIPDHCPV4S:%s,%s,%s,%u\r\n",
             dhcp_server_config.running ? "RUNNING" : "STOPPED",
             start_ip, end_ip, (unsigned)dhcp_server_config.lease_time_minute);

    return QAT_Response_Str(QAT_RC_OK, buffer);
}

static cat_return_state cmd_cipdhcpv4s_set(const struct cat_command *cmd,
                                          const uint8_t *data,
                                          const size_t data_size,
                                          const size_t args_num)
{
    int iface_id = -1;
    char ifname[16] = {0};
    char action[16] = {0};

    /* Parse either:
     *   <iface_id>,<action>[,...]   e.g. 0,start
     * or
     *   <ifname>,<action>[,...]     e.g. wlan0,pool,...
     */
    if (sscanf((char *)data, "%d,%15[^,\r\n]", &iface_id, action) >= 2) {
        /* numeric iface_id parsed */
    } else if (sscanf((char *)data, "%15[^,],%15[^,\r\n]", ifname, action) >= 2) {
        if (strcmp(ifname, "wlan0") == 0) {
            iface_id = NT_DEV_AP_ID;
        } else if (strcmp(ifname, "wlan1") == 0) {
            iface_id = NT_DEV_STA_ID;
        } else {
            return QAT_Response_Str(QAT_RC_ERROR,
                                    "+CIPDHCPV4S:Invalid input parameter!\r\n");
        }
    } else {
        return QAT_Response_Str(QAT_RC_ERROR,
                                "+CIPDHCPV4S:Invalid input parameter!\r\n");
    }

    /* DHCPv4 server is only allowed on AP (iface_id = 0 / wlan0) */
    if (iface_id != NT_DEV_AP_ID) {
        return QAT_Response_Str(QAT_RC_ERROR,
                                "+CIPDHCPV4S:iface_id must be 0 (AP) for DHCP server\r\n");
    }

    struct net_if *iface = get_iface_by_qat_id(NT_DEV_AP_ID);
    if (!iface) {
        /* Keep behavior tolerant: allow config but warn when applying start/stop */
        LOG_WRN("CIPDHCPV4S: no SoftAP iface found");
    }

    if (strcmp(action, "pool") == 0) {
        char start_ip_str[INET_ADDRSTRLEN] = {0};
        char end_ip_str[INET_ADDRSTRLEN] = {0};
        unsigned lease_min = dhcp_server_config.lease_time_minute;

        /* Locate tail after "<iface>,pool," so both "0,pool,..." and
         * "wlan0,pool,..." formats are supported.
         */
        char *p = strchr((char *)data, ',');
        if (!p) {
            LOG_WRN("CIPDHCPV4S pool: no first comma in '%s'", (char *)data);
            return QAT_Response_Str(QAT_RC_ERROR,
                                    "+CIPDHCPV4S:Invalid input parameter!\r\n");
        }
        p++; /* now at "pool,..." */

        if (strncmp(p, "pool,", 5) == 0) {
            p += 5; /* now at start_ip */
        } else {
            /* Already know action == 'pool', but be defensive */
            char *comma = strchr(p, ',');
            if (!comma) {
                LOG_WRN("CIPDHCPV4S pool: malformed action segment in '%s'", (char *)data);
                return QAT_Response_Str(QAT_RC_ERROR,
                                        "+CIPDHCPV4S:Invalid input parameter!\r\n");
            }
            p = comma + 1; /* now at start_ip */
        }

        int parsed = sscanf(p, "%15[^,],%15[^,],%u",
                            start_ip_str, end_ip_str, &lease_min);
        if (parsed < 3) {
            LOG_WRN("CIPDHCPV4S pool: sscanf on tail failed (parsed=%d)", parsed);
            return QAT_Response_Str(QAT_RC_ERROR,
                                    "+CIPDHCPV4S:Invalid input parameter!\r\n");
        }

        struct in_addr start_ip, end_ip;
        if (zsock_inet_pton(AF_INET, start_ip_str, &start_ip) != 1) {
            return QAT_Response_Str(QAT_RC_ERROR, "+CIPDHCPV4S:Invalid start IP\r\n");
        }
        if (zsock_inet_pton(AF_INET, end_ip_str, &end_ip) != 1) {
            return QAT_Response_Str(QAT_RC_ERROR, "+CIPDHCPV4S:Invalid end IP\r\n");
        }

        if (lease_min == 0 || lease_min > (7 * 24 * 60)) {
            return QAT_Response_Str(QAT_RC_ERROR,
                                    "+CIPDHCPV4S:Invalid lease time\r\n");
        }

        uint32_t s = in4_to_u32_host(&start_ip);
        uint32_t e = in4_to_u32_host(&end_ip);
        if ((s & 0xFFFFFF00u) != (e & 0xFFFFFF00u) || s >= e) {
            return QAT_Response_Str(QAT_RC_ERROR,
                                    "+CIPDHCPV4S:Invalid pool range (/24 or order)\r\n");
        }

        dhcp_server_config.pool_start_ip = start_ip;
        dhcp_server_config.pool_end_ip = end_ip;
        dhcp_server_config.lease_time_minute = lease_min;
        dhcp_server_config.pool_configured = true;

        /* Keep one summary log for visibility */
        LOG_INF("CIPDHCPV4S: pool=%s-%s lease=%u min", start_ip_str, end_ip_str, lease_min);

        return QAT_Response_Str(QAT_RC_OK, NULL);
    }

    if (strcmp(action, "stop") == 0) {
        if (iface) {
            int ret = net_dhcpv4_server_stop(iface);
            if (ret < 0 && ret != -ENOENT) {
                LOG_WRN("CIPDHCPV4S stop failed: %d", ret);
            }
        }

        dhcp_server_config.running = false;
        LOG_INF("CIPDHCPV4S stopped");
        return QAT_Response_Str(QAT_RC_OK, NULL);
    }

    if (strcmp(action, "start") == 0) {
        struct in_addr pool_start, pool_end;
        struct in_addr server_ip, netmask;

        if (dhcp_server_config.pool_configured) {
            pool_start = dhcp_server_config.pool_start_ip;
            pool_end = dhcp_server_config.pool_end_ip;
        } else {
            if (zsock_inet_pton(AF_INET, QAT_DHCPS_DEFAULT_POOL_START, &pool_start) != 1 ||
                zsock_inet_pton(AF_INET, QAT_DHCPS_DEFAULT_POOL_END, &pool_end) != 1) {
                return QAT_Response_Str(QAT_RC_ERROR, "+CIPDHCPV4S:Internal IP parse error\r\n");
            }
        }

        /* Infer server_ip = same /24 .1 and netmask=/24 */
        dhcps_calc_server_ip_netmask_24(&pool_start, &server_ip, &netmask);

        /* Validate pool range is in same /24 and ordered */
        uint32_t s = in4_to_u32_host(&pool_start);
        uint32_t e = in4_to_u32_host(&pool_end);
        if ((s & 0xFFFFFF00u) != (e & 0xFFFFFF00u) || s >= e) {
            return QAT_Response_Str(QAT_RC_ERROR,
                                    "+CIPDHCPV4S:Invalid pool range (/24 or order)\r\n");
        }

        if (!iface) {
            /* Keep behavior tolerant: allow config but warn when applying start/stop */
            return QAT_Response_Str(QAT_RC_ERROR,
                            "+CIPDHCPV4S:No SoftAP interface\r\n");
        }

        /* Apply IPv4 to SoftAP iface
         * Important: remove stale IPv4 addresses first; otherwise Zephyr DHCPv4 server can reject
         * the pool with "Address pool does not belong to the interface subnet."
         */
        struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;
        if (ipv4) {
            for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
                if (ipv4->unicast[i].ipv4.is_used) {
                    net_if_ipv4_addr_rm(iface, &ipv4->unicast[i].ipv4.address.in_addr);
                }
            }
        }

        (void)net_if_ipv4_addr_add(iface, &server_ip, NET_ADDR_MANUAL, 0);
        net_if_ipv4_set_netmask_by_addr(iface, &server_ip, &netmask);
        net_if_ipv4_set_gw(iface, &server_ip);

        /* Start Zephyr DHCPv4 server with base=pool_start (pool size controlled by Kconfig ADDR_COUNT) */
        int ret = net_dhcpv4_server_start(iface, &pool_start);
        if (ret < 0 && ret != -EALREADY) {
            LOG_WRN("CIPDHCPV4S start failed: %d", ret);
            return QAT_Response_Str(QAT_RC_ERROR, "+CIPDHCPV4S:DHCP server start failed\r\n");
        }

        dhcp_server_config.running = true;

        /* Keep a concise summary log when server starts */
        char server_ip_str[NET_IPV4_ADDR_LEN];
        net_addr_ntop(AF_INET, &server_ip, server_ip_str, sizeof(server_ip_str));
        LOG_INF("CIPDHCPV4S: started server=%s", server_ip_str);

        return QAT_Response_Str(QAT_RC_OK, NULL);
    }

    return QAT_Response_Str(QAT_RC_ERROR,
                            "+CIPDHCPV4S:Invalid action (start/stop/pool)\r\n");
}

/*-------------------------------------------------------------------------
 * AT+CIPV6PREFIX - IPv6 Prefix Configuration
 *-----------------------------------------------------------------------*/
static cat_return_state cmd_cipv6prefix_exec(const struct cat_command *cmd)
{
    /* Per AT Commands User Guide 4.17: AT+CIPV6PREFIX=<prefix> (prefix length fixed 64, no compression) */
    return QAT_Response_Str(QAT_RC_OK, "AT+CIPV6PREFIX=<prefix>\r\n");
}

static cat_return_state cmd_cipv6prefix_query(const struct cat_command *cmd,
                                              uint8_t *data,
                                              size_t *data_size,
                                              const size_t max_data_size)
{
    char buffer[128];
    *data_size = 0;

    /* Per AT guide: +CIPV6PREFIX:<prefix> */
    if (!ipv6_prefix_config.configured) {
        /* If not configured, return current stored/default prefix as well (more script-friendly) */
        snprintf(buffer, sizeof(buffer), "+CIPV6PREFIX:%s\r\n", ipv6_prefix_config.prefix_str);
        return QAT_Response_Str(QAT_RC_OK, buffer);
    }

    snprintf(buffer, sizeof(buffer), "+CIPV6PREFIX:%s\r\n", ipv6_prefix_config.prefix_str);
    return QAT_Response_Str(QAT_RC_OK, buffer);
}

static cat_return_state cmd_cipv6prefix_set(const struct cat_command *cmd,
                                            const uint8_t *data,
                                            const size_t data_size,
                                            const size_t args_num)
{
    char prefix_in[40] = {0};

    /* Per AT guide: only one parameter <prefix> */
    if (sscanf((char *)data, "%39s", prefix_in) != 1) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPV6PREFIX:Invalid input parameter!\r\n");
    }

    if (!cipv6prefix_is_valid_prefix(prefix_in)) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPV6PREFIX:IPv6 Prefix format error\r\n");
    }

    /* Verify parseability by appending ::1 (same trick as FreeRTOS demo) */
    char ip_str[64];
    snprintf(ip_str, sizeof(ip_str), "%s::1", prefix_in);

    struct in6_addr addr;
    if (zsock_inet_pton(AF_INET6, ip_str, &addr) != 1) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPV6PREFIX:IPv6 Prefix format error\r\n");
    }

    /* Must be global unicast prefix (2000::/3) */
    if ((addr.s6_addr[0] & 0xE0) != 0x20) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPV6PREFIX:IPv6 Prefix format error\r\n");
    }

    /* Save config */
    snprintf(ipv6_prefix_config.prefix_str, sizeof(ipv6_prefix_config.prefix_str), "%s", prefix_in);
    ipv6_prefix_config.prefix = addr;
    ipv6_prefix_config.configured = true;

    LOG_INF("CIPV6PREFIX set to %s (/64)", ipv6_prefix_config.prefix_str);

    /* Apply to SoftAP iface */
    struct net_if *iface = get_iface_by_qat_id(NT_DEV_AP_ID);
    if (!iface) {
        LOG_WRN("CIPV6PREFIX: no SoftAP iface found");
        return QAT_Response_Str(QAT_RC_OK, NULL);
    }

    if (ipv6_enabled) {
        /* Ensure address ends with ::1 (already) */
        struct net_if_addr *ifaddr = net_if_ipv6_addr_add(iface, &addr, NET_ADDR_MANUAL, 0);
        if (!ifaddr) {
            LOG_WRN("CIPV6PREFIX: failed to add IPv6 addr to SoftAP iface");
            /* Keep config even if apply fails, to match expected AT behavior */
        }

        /* (Re)start SAP RA with the new prefix */
        sap_ra_stop();
        sap_ra_start();
    }

    return QAT_Response_Str(QAT_RC_OK, NULL);
}

/*-------------------------------------------------------------------------
 * AT+CIPUDPSERVER - UDP Server
 *-----------------------------------------------------------------------*/
static cat_return_state cmd_cipudpserver_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK,
        "+CIPUDPSERVER=<mode>,<param2>\r\n"
        "  mode: 0 (stop), 1 (start)\r\n"
        "  param2: 0/1 when mode=0, port when mode=1\r\n");
}

static cat_return_state cmd_cipudpserver_query(const struct cat_command *cmd, uint8_t *data, size_t *data_size,
                                               const size_t max_data_size)
{
    char buffer[QAT_RESPONSE_BUF_SIZE];
    int offset = 0;
    *data_size = 0;

    k_mutex_lock(&conn_mutex, K_FOREVER);

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (g_listen_udp_clients[i].state != CONN_STATE_CONNECTED ||
            (g_listen_udp_clients[i].type != PROTOCOL_UDP &&
             g_listen_udp_clients[i].type != PROTOCOL_UDPv6)) {
            continue;
        }

        char local_ip[INET6_ADDRSTRLEN] = {0};
        uint16_t local_port = udp_config.port;
        const char *type_str = (g_listen_udp_clients[i].type == PROTOCOL_UDPv6) ? "UDPv6" : "UDP";
        struct sockaddr_storage local_addr;
        socklen_t local_addr_len = sizeof(local_addr);

        if (zsock_getsockname(g_listen_udp_clients[i].sock_fd, (struct sockaddr *)&local_addr, &local_addr_len) == 0) {
            if (local_addr.ss_family == AF_INET) {
                struct sockaddr_in *addr4 = (struct sockaddr_in *)&local_addr;
                zsock_inet_ntop(AF_INET, &addr4->sin_addr, local_ip, sizeof(local_ip));
                local_port = ntohs(addr4->sin_port);
            } else if (local_addr.ss_family == AF_INET6) {
                struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&local_addr;
                zsock_inet_ntop(AF_INET6, &addr6->sin6_addr, local_ip, sizeof(local_ip));
                local_port = ntohs(addr6->sin6_port);
            }
        }

        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                          "+CIPUDPSERVER:S,%d,%s,%s,%d,%s,%d\r\n",
                          i, type_str, g_listen_udp_clients[i].remote_ip,
                          g_listen_udp_clients[i].remote_port,
                          local_ip[0] ? local_ip : "0.0.0.0",
                          local_port);
    }

    k_mutex_unlock(&conn_mutex);

    return QAT_Response_Str(QAT_RC_OK, buffer);
}

static cat_return_state cmd_cipudpserver_set(const struct cat_command *cmd,
                                             const uint8_t *data,
                                             const size_t data_size,
                                             const size_t args_num)
{
    int mode, value;
    char type_str[16] = {0};
    int parsed = sscanf((char *)data, "%d,%d,%15[^,\r\n]", &mode, &value, type_str);

    if (parsed < 2) {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPUDPSERVER: Invalid parameters\r\n");
    }

    if (mode == 0) {
        /* Stop server */
        if (value < 0 || value > 1) {
            return QAT_Response_Str(QAT_RC_ERROR, "+CIPUDPSERVER: param2 must be 0 or 1\r\n");
        }

        udp_config.mode = 0;
        udp_config.close_server = (value == 1);
        udp_config.accept_new_peer = false;

        if (udp_server_running && udp_config.close_server) {
            k_work_cancel_delayable(&udp_server_work);
            udp_server_work_handler(&udp_server_work.work);
        }

        return QAT_Response_Str(QAT_RC_OK, NULL);

    } else if (mode == 1) {
        /* Start server */
        if (value < 0 || value > 65535) {
            return QAT_Response_Str(QAT_RC_ERROR, "+CIPUDPSERVER: Invalid port\r\n");
        }

        if (udp_server_running) {
            return QAT_Response_Str(QAT_RC_ERROR, "+CIPUDPSERVER: Server already running\r\n");
        }

        protocol_type_t server_type = PROTOCOL_UDP;
        int family = AF_INET;

        if (parsed >= 3 && type_str[0] != '\0') {
            if (strcmp(type_str, "UDP") == 0) {
                server_type = PROTOCOL_UDP;
                family = AF_INET;
            } else if (strcmp(type_str, "UDPv6") == 0) {
                server_type = PROTOCOL_UDPv6;
                family = AF_INET6;
            } else {
                return QAT_Response_Str(QAT_RC_ERROR, "+CIPUDPSERVER: Invalid type\r\n");
            }
        }

        /* Create UDP socket */
        udp_listen_fd = zsock_socket(family, SOCK_DGRAM, IPPROTO_UDP);
        if (udp_listen_fd < 0) {
            return QAT_Response_Str(QAT_RC_ERROR, "+CIPUDPSERVER: Failed to create socket\r\n");
        }

        /* Bind to port */
        if (family == AF_INET) {
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(value);

            if (zsock_bind(udp_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                zsock_close(udp_listen_fd);
                udp_listen_fd = INVALID_FD;
                return QAT_Response_Str(QAT_RC_ERROR, "+CIPUDPSERVER: Failed to bind\r\n");
            }
        } else {
            struct sockaddr_in6 addr6;
            memset(&addr6, 0, sizeof(addr6));
            addr6.sin6_family = AF_INET6;
            addr6.sin6_addr = in6addr_any;
            addr6.sin6_port = htons(value);

            if (zsock_bind(udp_listen_fd, (struct sockaddr *)&addr6, sizeof(addr6)) < 0) {
                zsock_close(udp_listen_fd);
                udp_listen_fd = INVALID_FD;
                return QAT_Response_Str(QAT_RC_ERROR, "+CIPUDPSERVER: Failed to bind\r\n");
            }
        }

        /* Set non-blocking */
        int flags = zsock_fcntl(udp_listen_fd, F_GETFL, 0);
        zsock_fcntl(udp_listen_fd, F_SETFL, flags | O_NONBLOCK);

        /* Start server work */
        udp_config.mode = 1;
        udp_config.port = value;
        udp_config.type = server_type;
        udp_config.close_server = false;
        udp_config.accept_new_peer = true;
        udp_server_running = true;

        k_work_reschedule(&udp_server_work, K_MSEC(1));

        LOG_INF("UDP server started on port %d", value);
        return QAT_Response_Str(QAT_RC_OK, NULL);

    } else {
        return QAT_Response_Str(QAT_RC_ERROR, "+CIPUDPSERVER: Invalid mode\r\n");
    }
}

/*-------------------------------------------------------------------------
 * AT+DNSC - DNS Client Management
 *-----------------------------------------------------------------------*/

static cat_return_state cmd_dnsc_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK,
        "\nAT+DNSC:  show the usage of command\n\r"
        "AT+DNSC?: show the current list of dns servers\n\r"
        "AT+DNSC=addsvr,<ip>: add a DNS server \n\r"
        "AT+DNSC=delsvr,<ip>: delete a DNS server \n\r"
        "AT+DNSC=gethostbyname,<hostname>: resolve a hostname (string) into an IP address \n\r"
        "AT+DNSC=gethostbyname2,<hostname>,<iptype>: like dns_gethostbyname, but returned address type can be controlled\n\r"
        "                                       iptype: v4, v6 , v4v6, v6v4 \n\r");
}

static cat_return_state cmd_dnsc_query(const struct cat_command *cmd,
                                       uint8_t *data, size_t *data_size,
                                       const size_t max_data_size)
{
    char buffer[256];
    int offset = 0;

    *data_size = 0;

    /* Per AT guide: +DNSC:<index>,<ip> and index range is 0-1 */
    for (int i = 0; i < DNSC_MAX_SERVERS; i++) {
        if (dnsc_servers[i].valid) {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                               "+DNSC:%d,%s\r\n", i, dnsc_servers[i].addr);
        }
    }

    return QAT_Response_Str(QAT_RC_OK, buffer);
}

static cat_return_state cmd_dnsc_set(const struct cat_command *cmd,
                                     const uint8_t *data,
                                     const size_t data_size,
                                     const size_t args_num)
{
    char subcmd[32];
    char param[DNSC_SERVER_LEN];
    char response[256];

    if (sscanf((char *)data, "%31[^,],%63s", subcmd, param) < 1) {
        return QAT_Response_Str(QAT_RC_ERROR, "+DNSC: Invalid parameters\r\n");
    }

    if (strcmp(subcmd, "addsvr") == 0) {
        /* Check duplicate */
        for (int i = 0; i < DNSC_MAX_SERVERS; i++) {
            if (dnsc_servers[i].valid && strcmp(dnsc_servers[i].addr, param) == 0) {
                return QAT_Response_Str(QAT_RC_ERROR, "+DNSC: IP already exists\r\n");
            }
        }
        /* Find empty slot */
        for (int i = 0; i < DNSC_MAX_SERVERS; i++) {
            if (!dnsc_servers[i].valid) {
                snprintf(dnsc_servers[i].addr, DNSC_SERVER_LEN, "%s", param);
                dnsc_servers[i].valid = true;
                LOG_INF("DNSC: added server[%d]=%s", i, param);
                goto dnsc_reconfigure;
            }
        }
        return QAT_Response_Str(QAT_RC_ERROR, "+DNSC: DNS server list is full\r\n");

    } else if (strcmp(subcmd, "delsvr") == 0) {
        for (int i = 0; i < DNSC_MAX_SERVERS; i++) {
            if (dnsc_servers[i].valid && strcmp(dnsc_servers[i].addr, param) == 0) {
                dnsc_servers[i].valid = false;
                LOG_INF("DNSC: removed server[%d]=%s", i, param);
                goto dnsc_reconfigure;
            }
        }
        return QAT_Response_Str(QAT_RC_ERROR, "+DNSC: DNS server not found\r\n");

    } else if (strcmp(subcmd, "gethostbyname") == 0) {
        /* Per AT guide: response OK then event +EVT:dns_get:<hostname>,<ip_host> or +EVT:dns_fail:<hostname> */
        struct zsock_addrinfo hints = {0};
        struct zsock_addrinfo *res = NULL;

        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        int ret = zsock_getaddrinfo(param, NULL, &hints, &res);
        if (ret != 0) {
            snprintf(response, sizeof(response), "+EVT:dns_fail:%s\r\n", param);
            QAT_Response_Str(QAT_RC_QUIET, response);
            return QAT_Response_Str(QAT_RC_OK, NULL);
        }

        char ip_str[INET6_ADDRSTRLEN];
        if (res->ai_family == AF_INET) {
            struct sockaddr_in *a4 = (struct sockaddr_in *)res->ai_addr;
            zsock_inet_ntop(AF_INET, &a4->sin_addr, ip_str, sizeof(ip_str));
        } else {
            struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)res->ai_addr;
            zsock_inet_ntop(AF_INET6, &a6->sin6_addr, ip_str, sizeof(ip_str));
        }
        zsock_freeaddrinfo(res);

        snprintf(response, sizeof(response), "+EVT:dns_get:%s,%s\r\n", param, ip_str);
        QAT_Response_Str(QAT_RC_QUIET, response);
        return QAT_Response_Str(QAT_RC_OK, NULL);

    } else if (strcmp(subcmd, "gethostbyname2") == 0) {
        /* Parse: gethostbyname2,<hostname>,<iptype> ; iptype: v4, v6 , v4v6, v6v4 */
        char hostname[DNSC_SERVER_LEN];
        char iptype[16] = {0};

        if (sscanf((char *)data, "gethostbyname2,%63[^,],%15s", hostname, iptype) < 2) {
            return QAT_Response_Str(QAT_RC_ERROR,
                                    "+DNSC:AT+DNSC=gethostbyname2,<hostname>,<iptype>: like dns_gethostbyname, but returned address type can be controlled\r\n"
                                    "                                       iptype: v4, v6 , v4v6, v6v4 \r\n");
        }

        int family = AF_UNSPEC;
        if (strcmp(iptype, "v4") == 0) {
            family = AF_INET;
        } else if (strcmp(iptype, "v6") == 0) {
            family = AF_INET6;
        } else if (strcmp(iptype, "v4v6") == 0 || strcmp(iptype, "v6v4") == 0) {
            family = AF_UNSPEC;
        } else {
            return QAT_Response_Str(QAT_RC_ERROR, "+DNSC:invalid type.\r\n");
        }

        struct zsock_addrinfo hints = {0};
        struct zsock_addrinfo *res = NULL;
        hints.ai_family = family;
        hints.ai_socktype = SOCK_STREAM;

        int ret = zsock_getaddrinfo(hostname, NULL, &hints, &res);
        if (ret != 0) {
            snprintf(response, sizeof(response), "+EVT:dns_fail:%s\r\n", hostname);
            QAT_Response_Str(QAT_RC_QUIET, response);
            return QAT_Response_Str(QAT_RC_OK, NULL);
        }

        /* Pick address based on preference order for v4v6/v6v4 */
        struct zsock_addrinfo *pick = res;

        if (strcmp(iptype, "v4v6") == 0) {
            for (struct zsock_addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
                if (ai->ai_family == AF_INET) {
                    pick = ai;
                    break;
                }
            }
        } else if (strcmp(iptype, "v6v4") == 0) {
            for (struct zsock_addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
                if (ai->ai_family == AF_INET6) {
                    pick = ai;
                    break;
                }
            }
        }

        char ip_str[INET6_ADDRSTRLEN];
        if (pick->ai_family == AF_INET) {
            struct sockaddr_in *a4 = (struct sockaddr_in *)pick->ai_addr;
            zsock_inet_ntop(AF_INET, &a4->sin_addr, ip_str, sizeof(ip_str));
        } else {
            struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)pick->ai_addr;
            zsock_inet_ntop(AF_INET6, &a6->sin6_addr, ip_str, sizeof(ip_str));
        }
        zsock_freeaddrinfo(res);

        snprintf(response, sizeof(response), "+EVT:dns_get:%s,%s\r\n", hostname, ip_str);
        QAT_Response_Str(QAT_RC_QUIET, response);
        return QAT_Response_Str(QAT_RC_OK, NULL);

    } else {
        return QAT_Response_Str(QAT_RC_ERROR, "+DNSC: command not supported\r\n");
    }

dnsc_reconfigure: {
        /* Rebuild server list and reconfigure Zephyr DNS resolver */
        const char *servers[DNSC_MAX_SERVERS + 1];
        int count = 0;
        for (int i = 0; i < DNSC_MAX_SERVERS; i++) {
            if (dnsc_servers[i].valid) {
                servers[count++] = dnsc_servers[i].addr;
            }
        }
        servers[count] = NULL;

        struct dns_resolve_context *ctx = dns_resolve_get_default();
        if (ctx && count > 0) {
            int ret = dns_resolve_reconfigure(ctx, servers, NULL, DNS_SOURCE_MANUAL);
            if (ret < 0) {
                LOG_WRN("DNSC: dns_resolve_reconfigure failed: %d", ret);
            } else {
                LOG_INF("DNSC: DNS resolver updated with %d server(s)", count);
            }
        }
        return QAT_Response_Str(QAT_RC_OK, NULL);
    }
}

/*-------------------------------------------------------------------------
 * AT+SNTPC - SNTP Client
 *-----------------------------------------------------------------------*/
#define SNTPC_MAX_SERVERS    2
#define SNTPC_SERVER_LEN     64

typedef struct {
    char name[SNTPC_SERVER_LEN];      /* domain name if configured as name */
    char ip[INET6_ADDRSTRLEN];        /* textual IP if configured as IP */
    bool valid;
    bool kod;                         /* keep for future KOD extension */
} sntpc_server_entry_t;

static sntpc_server_entry_t sntpc_servers[SNTPC_MAX_SERVERS];

static struct {
    bool started;                     /* SNTP client started or not */
    int  op_mode;                     /* 0: unicast, 1: broadcast */
} sntpc_state = {
    .started = false,
    .op_mode = 0,
};

static cat_return_state cmd_sntpc_exec(const struct cat_command *cmd)
{
    return QAT_Response_Str(QAT_RC_OK,
        "\n\nAT+SNTPC: show usage of command\n\r"
        "AT+SNTPC?: show status of SNTP\n\r"
        "AT+SNTPC=[start|stop]\n\r"
        "AT+SNTPC=setOpMode,<0(unicast)|1(broadcast)>\n\r"
        "AT+SNTPC=setServer,<IP addr|name>,[id]\n\r");
}

static cat_return_state cmd_sntpc_query(const struct cat_command *cmd,
                                        uint8_t *data, size_t *data_size,
                                        const size_t max_data_size)
{
    char buffer[256];
    int offset = 0;

    *data_size = 0;

    /* First line: +SNTPC:started/stopped */
    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "+SNTPC:%s\r\n",
                       sntpc_state.started ? "started" : "stopped");

    /* Per FreeRTOS demo: +SNTPC:<id>,<name>,<ip>[,KOD] */
    for (int i = 0; i < SNTPC_MAX_SERVERS; i++) {
        if (!sntpc_servers[i].valid) {
            continue;
        }

        const char *name = (sntpc_servers[i].name[0] != '\0')
                               ? sntpc_servers[i].name
                               : "****";
        const char *ip   = (sntpc_servers[i].ip[0] != '\0')
                               ? sntpc_servers[i].ip
                               : "****";

        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                           "+SNTPC:%d,%s,%s", i, name, ip);

        if (sntpc_servers[i].kod) {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                               ",KOD");
        }

        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\r\n");
    }

    return QAT_Response_Str(QAT_RC_OK, buffer);
}

static cat_return_state cmd_sntpc_set(const struct cat_command *cmd,
                                      const uint8_t *data,
                                      const size_t data_size,
                                      const size_t args_num)
{
    char subcmd[32];

    if (sscanf((const char *)data, "%31[^,\r\n]", subcmd) != 1) {
        return QAT_Response_Str(QAT_RC_ERROR, "+SNTPC: Invalid parameters\r\n");
    }

    /* AT+SNTPC=start */
    if (strncasecmp(subcmd, "start", 5) == 0) {
        const char *server = NULL;

        /* Prefer configured servers (id 0..N), pick first valid name/ip */
        for (int i = 0; i < SNTPC_MAX_SERVERS; i++) {
            if (!sntpc_servers[i].valid) {
                continue;
            }

            if (sntpc_servers[i].name[0] != '\0') {
                server = sntpc_servers[i].name;  /* domain name */
                break;
            } else if (sntpc_servers[i].ip[0] != '\0') {
                server = sntpc_servers[i].ip;    /* numeric IP string */
                break;
            }
        }

        if (!server) {
            return QAT_Response_Str(QAT_RC_ERROR,
                "+SNTPC:no SNTP server configured, use AT+SNTPC=setServer first\r\n");
        }

        struct sntp_time ts;
        int ret = sntp_simple(server, 5000, &ts); /* 5s timeout */

        if (ret == 0) {
            sntpc_state.started = true;

            LOG_INF("SNTPC: synced from %s, unix_ts=%llu",
                    server, (unsigned long long)ts.seconds);

            /* Convert SNTP unix seconds (UTC) to struct tm and program RTC,
             * so that existing AT+TIME? can read the synchronized time.
             *
             * NOTE:
             * - sntp_simple() returns time in UTC.
             * - rtc_set_time() is called with this UTC value directly.
             * - AT+TIME? currently reports the raw RTC time without any
             *   additional time zone offset.

             * If needs local time (e.g. UTC+8 for China),
             * the host or higher layer should apply the time zone offset
             * when interpreting the AT+TIME? result, or a separate
             * time zone handling should be implemented on top.
             */
            time_t unix_ts = (time_t)ts.seconds;
            struct tm tm_utc;

            if (gmtime_r(&unix_ts, &tm_utc) == NULL) {
                LOG_WRN("SNTPC: gmtime_r failed for %llu",
                        (unsigned long long)ts.seconds);
            } else {
                /* Use the same RTC device alias as qat_common (+TIME)
                 * so that AT+SNTPC updates the time source used by AT+TIME?.
                 */
                const struct device *rtc_dev = DEVICE_DT_GET(DT_ALIAS(rtc));

                if (!device_is_ready(rtc_dev)) {
                    LOG_WRN("SNTPC: RTC device not ready");
                } else {
                    struct rtc_time rtc_utc = {
                        .tm_sec  = tm_utc.tm_sec,
                        .tm_min  = tm_utc.tm_min,
                        .tm_hour = tm_utc.tm_hour,
                        .tm_mday = tm_utc.tm_mday,
                        .tm_mon  = tm_utc.tm_mon,
                        .tm_year = tm_utc.tm_year,
                        .tm_wday = tm_utc.tm_wday,
                        .tm_yday = tm_utc.tm_yday,
                        .tm_isdst = -1,
                        .tm_nsec = 0,
                    };
                    int rtc_ret = rtc_set_time(rtc_dev, &rtc_utc);
                    if (rtc_ret != 0) {
                        LOG_WRN("SNTPC: rtc_set_time failed (%d)", rtc_ret);
                    } else {
                        LOG_INF("SNTPC: RTC time updated via SNTP");
                    }
                }
            }

            char evt[64];
            snprintf(evt, sizeof(evt), "+SNTPC:synced,%llu\r\n",
                     (unsigned long long)ts.seconds);
            QAT_Response_Str(QAT_RC_QUIET, evt);

            return QAT_Response_Str(QAT_RC_OK, NULL);
        } else {
            char resp[64];
            snprintf(resp, sizeof(resp), "+SNTPC:sync failed,err=%d\r\n", ret);
            LOG_ERR("SNTPC: sync from %s failed, err=%d", server, ret);
            return QAT_Response_Str(QAT_RC_ERROR, resp);
        }
    }

    /* AT+SNTPC=stop */
    if (strncasecmp(subcmd, "stop", 4) == 0) {
        sntpc_state.started = false;
        LOG_INF("SNTPC: client stopped (Zephyr)");
        return QAT_Response_Str(QAT_RC_OK, NULL);
    }

    /* AT+SNTPC=setOpMode,<0|1> */
    if (strncasecmp(subcmd, "setOpMode", 9) == 0) {
        int opMode;
        int parsed = sscanf((const char *)data, "setOpMode,%d", &opMode);
        if (parsed != 1) {
            return QAT_Response_Str(QAT_RC_ERROR,
                "+SNTPC:AT+SNTPC=setOpMode,<0(unicast)|1(broadcast)>\r\n");
        }

        if (opMode != 0 && opMode != 1) {
            return QAT_Response_Str(QAT_RC_ERROR,
                "+SNTPC:the operating class for SNTP client should be 0 or 1\r\n");
        }

        if (sntpc_state.started) {
            return QAT_Response_Str(QAT_RC_ERROR,
                "+SNTPC:Operating mode must not be set while SNTP client is running\r\n");
        }

        sntpc_state.op_mode = opMode;
        LOG_INF("SNTPC: op_mode=%d", opMode);
        return QAT_Response_Str(QAT_RC_OK, NULL);
    }

    /* AT+SNTPC=setServer,<IP addr|name>,[id] */
    if (strncasecmp(subcmd, "setServer", 9) == 0) {
        char addr[SNTPC_SERVER_LEN];
        int id = 0;
        int parsed = sscanf((const char *)data, "setServer,%63[^,],%d", addr, &id);

        if (parsed < 1) {
            return QAT_Response_Str(QAT_RC_ERROR,
                "+SNTPC:AT+SNTPC=setServer,<IP addr|name>,[id]\r\n");
        }

        if (strlen(addr) > 64) {
            return QAT_Response_Str(QAT_RC_ERROR,
                "+SNTPC:Domain name or address cannot be more then 64 bytes\r\n");
        }

        if (parsed == 1) {
            id = 0; /* default id 0 like demo */
        }

        if (id < 0 || id >= SNTPC_MAX_SERVERS) {
            return QAT_Response_Str(QAT_RC_ERROR,
                "+SNTPC:id exceed the max number of SNTP servers\r\n");
        }

        /* Determine if addr is IP or hostname */
        struct sockaddr_in  a4;
        struct sockaddr_in6 a6;
        memset(&a4, 0, sizeof(a4));
        memset(&a6, 0, sizeof(a6));

        bool is_ipv4 = (zsock_inet_pton(AF_INET, addr, &a4.sin_addr) == 1);
        bool is_ip   = is_ipv4 || (zsock_inet_pton(AF_INET6, addr, &a6.sin6_addr) == 1);

        memset(&sntpc_servers[id], 0, sizeof(sntpc_servers[id]));
        sntpc_servers[id].valid = true;

        if (is_ip) {
            if (is_ipv4) {
                zsock_inet_ntop(AF_INET, &a4.sin_addr,
                                sntpc_servers[id].ip, sizeof(sntpc_servers[id].ip));
            } else {
                zsock_inet_ntop(AF_INET6, &a6.sin6_addr,
                                sntpc_servers[id].ip, sizeof(sntpc_servers[id].ip));
            }
            sntpc_servers[id].name[0] = '\0';
        } else {
            snprintf(sntpc_servers[id].name,
                     sizeof(sntpc_servers[id].name), "%s", addr);
            sntpc_servers[id].ip[0] = '\0';
        }

        LOG_INF("SNTPC: server[%d]=%s (is_ip=%d)", id, addr, is_ip);
        /* If later a real SNTP library is hooked, call sntp_setserver()/sntp_setservername() here. */
        return QAT_Response_Str(QAT_RC_OK, NULL);
    }

    /* Unknown subcommand: keep same error text as FreeRTOS demo */
    return QAT_Response_Str(QAT_RC_ERROR,
        "+SNTPC:command not found, AT+SNTPC for help\r\n");
}

/*-------------------------------------------------------------------------
 * Command List
 *-----------------------------------------------------------------------*/
static struct cat_command qat_tcpip_cmds[] = {
    /* Network Configuration */
    {
        .name = "+CIPSTA",
        .description = "Set/Query IP configuration",
        .run = cmd_cipsta_exec,
        .read = cmd_cipsta_query,
        .write = cmd_cipsta_set,
    },

    /* Connection Management */
    {
        .name = "+CIPSTART",
        .description = "Start TCP/UDP connection",
        .run = cmd_cipstart_exec,
        .read = cmd_cipstart_query,
        .write = cmd_cipstart_set,
    },
    {
        .name = "+CIPCLOSE",
        .description = "Close connection",
        .run = cmd_cipclose_exec,
        .write = cmd_cipclose_set,
    },

    /* Server Mode */
    {
        .name = "+CIPSERVER",
        .description = "TCP server",
        .run = cmd_cipserver_exec,
        .read = cmd_cipserver_query,
        .write = cmd_cipserver_set,
    },
    {
        .name = "+CIPUDPSERVER",
        .description = "UDP server",
        .run = cmd_cipudpserver_exec,
        .read = cmd_cipudpserver_query,
        .write = cmd_cipudpserver_set,
    },

    /* Data Transfer */
    {
        .name = "+CIPSEND",
        .description = "Send data (online mode)",
        .run = cmd_cipsend_exec,
        .write = cmd_cipsend_set,
    },
    {
        .name = "+CIPSENDDATA",
        .description = "Send data directly",
        .run = cmd_cipsenddata_exec,
        .write = cmd_cipsenddata_set,
    },
    {
        .name = "+CIPRECVTYPE",
        .description = "Set receive mode",
        .run = cmd_ciprecvtype_exec,
        .read = cmd_ciprecvtype_query,
        .write = cmd_ciprecvtype_set,
    },
    {
        .name = "+CIPRECVDATA",
        .description = "Read received data",
        .run = cmd_ciprecvdata_exec,
        .write = cmd_ciprecvdata_set,
    },

    /* Network Tools */
    {
        .name = "+CIPPING",
        .description = "Ping test",
        .run = cmd_cipping_exec,
        .write = cmd_cipping_set,
    },

    {
        .name = "+CIPDHCPV4C",
        .description = "DHCP client",
        .run = cmd_cipdhcpv4c_exec,
        .read = cmd_cipdhcpv4c_query,
        .write = cmd_cipdhcpv4c_set,
    },

    /* Mode and Configuration */
    {
        .name = "+CIPMODE",
        .description = "Set passthrough mode",
        .run = cmd_cipmode_exec,
        .read = cmd_cipmode_query,
        .write = cmd_cipmode_set,
    },
    {
        .name = "+CIPV6",
        .description = "IPv6 enable/disable",
        .run = cmd_cipv6_exec,
        .read = cmd_cipv6_query,
        .write = cmd_cipv6_set,
    },
    {
        .name = "+CIPDHCPV4S",
        .description = "DHCP server",
        .run = cmd_cipdhcpv4s_exec,
        .read = cmd_cipdhcpv4s_query,
        .write = cmd_cipdhcpv4s_set,
    },
    {
        .name = "+CIPV6PREFIX",
        .description = "IPv6 prefix configuration",
        .run = cmd_cipv6prefix_exec,
        .read = cmd_cipv6prefix_query,
        .write = cmd_cipv6prefix_set,
    },

    /* DNS Client */
    {
        .name = "+DNSC",
        .description = "DNS client management",
        .run = cmd_dnsc_exec,
        .read = cmd_dnsc_query,
        .write = cmd_dnsc_set,
    },

    /* SNTP Client */
    {
        .name = "+SNTPC",
        .description = "SNTP client time sync",
        .run = cmd_sntpc_exec,
        .read = cmd_sntpc_query,
        .write = cmd_sntpc_set,
    },
};

/*-------------------------------------------------------------------------
 * Initialization
 *-----------------------------------------------------------------------*/
static int qat_tcpip_init(void)
{
    /* Initialize connection pools */
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        cleanup_client_conn(i);
        memset(&g_listen_clients[i], 0, sizeof(connection_info_t));
        g_listen_clients[i].sock_fd = INVALID_FD;
        g_listen_clients[i].state = CONN_STATE_IDLE;
        g_listen_clients[i].type = PROTOCOL_INVALID;
        g_listen_clients[i].id = INVALID_LINKID;

        memset(&g_listen_udp_clients[i], 0, sizeof(connection_info_t));
        g_listen_udp_clients[i].sock_fd = INVALID_FD;
        g_listen_udp_clients[i].state = CONN_STATE_IDLE;
        g_listen_udp_clients[i].type = PROTOCOL_INVALID;
        g_listen_udp_clients[i].id = INVALID_LINKID;
    }

    LOG_INF("TCP/IP AT commands initialized");
    return 0;
}

static struct cat_command_group qat_tcpip_cmd_group = {
    .name = "QAT_TCPIP",
    .cmd = qat_tcpip_cmds,
    .cmd_num = ARRAY_SIZE(qat_tcpip_cmds),
};

struct cat_command_group *qat_tcpip_get_command_group(void)
{
    qat_tcpip_init();
    return &qat_tcpip_cmd_group;
}

/* Automatically register this command group with QAT */
QAT_REGISTER_CMD_GROUP(qat_tcpip_get_command_group, "TCPIP");
