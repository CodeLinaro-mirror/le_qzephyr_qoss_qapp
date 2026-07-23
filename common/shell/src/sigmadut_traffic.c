/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket_service.h>

LOG_MODULE_REGISTER(sigmadut, LOG_LEVEL_DBG);

#define UDP_ENDMARK_ENABLE
#define MAX_ACK_RETRY                       40
#define UDP_RECEIVER_BUF_SIZE               1500

#define SIGMA_TRAFFIC_DEFAULT_PORT          7001
#define SIGMA_TRAFFIC_DEFAULT_RUNTIME       5
#define SIGMA_TRAFFIC_DEFAULT_PACKET_SIZE   1400
#define SIGMA_TRAFFIC_K                     (1000)
#define SIGMA_TRAFFIC_M                     (SIGMA_TRAFFIC_K * SIGMA_TRAFFIC_K)
#define SIGMA_TRAFFIC_DEFAULT_BAUDRATE      (30 * SIGMA_TRAFFIC_M)

#define SIGMA_THREAD_STACK_SIZE             4096
#define SIGMA_THREAD_PRIORITY               8

#define END_OF_TEST_CODE                    0xaabbccdd
#define SOCK_ID_IPV4                        0
#define SOCK_ID_IPV6                        1
#define SOCK_ID_MAX                         2

enum sigma_role {
    SIGMA_RX = 0,
    SIGMA_TX = 1,
};

struct sigmadut_stats {
    uint32_t first_time;
    uint32_t last_time;
    uint64_t bytes;
    uint64_t pkts;
    uint64_t echo_bytes;
    uint64_t echo_pkts;
};

struct sigmadut_rx_context {
    uint8_t  echo;
    struct sockaddr_in server_addr;
};

struct sigmadut_tx_context {
    uint8_t ip_tos;
    uint32_t duration;
    uint32_t interval_us;
    uint32_t payload_size;
    struct sockaddr_in host_addr;
    struct sockaddr_in remote_addr;
    struct k_thread thread_data;
    K_KERNEL_STACK_MEMBER(thread_stack, SIGMA_THREAD_STACK_SIZE);
};

struct sigma_dut_header {
    uint32_t stream_id;
    uint32_t seq;
    union {
        struct {
            uint32_t sec;
            uint32_t usec;
        };
        uint64_t timestamp;
    };
};

struct sigmadut_stats_header {
    uint32_t kbytes;
    uint64_t bytes;
    uint32_t msec;
    uint32_t numPackets;
};

struct sigmadut_end {
    uint32_t code;
    uint32_t count;
};

static struct sigmadut_stats sigmadut_rx_stats = {0};
static struct sigmadut_stats sigmadut_tx_stats = {0};
static struct zsock_pollfd sigmadut_rx_fds[SOCK_ID_MAX] = { 0 };
static bool sigmadut_tx_stop = false;
static bool sigmadut_rx_stop = true;
static bool sigmadut_tx_running = false;
static bool sigmadut_rx_running = false;
static bool sigmadut_server_echo = false;
static uint8_t udp_server_buf[UDP_RECEIVER_BUF_SIZE];

static int64_t current_time_us()
{
    return k_uptime_get() * 1000;
}

static inline char *net_addr_sprint(const struct sockaddr_in *addr)
{
#define NBUFS 3
    static char buf[NBUFS][NET_IPV6_ADDR_LEN];
    static int i;
    char *s = buf[++i % NBUFS];

    return net_addr_ntop(addr->sin_family, &addr->sin_addr, s, NET_IPV6_ADDR_LEN);
}

#ifdef UDP_ENDMARK_ENABLE

/*
 * finish_rx_and_send_ack() - called on the RX side upon receiving an EOT packet.
 * Fills a sigmadut_stats_header with the accumulated session statistics and
 * repeatedly sends it back to the TX peer.  After each send it waits up to 1 s
 * for another incoming packet; if the TX peer has stopped retransmitting EOTs
 * (i.e. it received our stats reply) the select() will time out and the loop
 * exits naturally.  Retries up to MAX_ACK_RETRY times.
 *
 * Parameters:
 *   pkt_stats  - accumulated RX session statistics (bytes, pkts, timestamps)
 *   sock_local - the bound RX socket used for both sending and draining
 *   faddr      - address of the TX peer (source of the EOT packet)
 *   addrlen    - length of faddr
 */
static void finish_rx_and_send_ack(struct sigmadut_stats *pkt_stats, int sock,
                     struct sockaddr *faddr, socklen_t addrlen)
{
    int ret;
    int retry = MAX_ACK_RETRY;
    struct sigmadut_stats_header stat = {0};
    struct timeval rcvtimeo = {
        .tv_sec  = 1,
        .tv_usec = 0,
    };

    size_t pktlen = sizeof(struct sigmadut_stats_header);
    stat.kbytes     = (uint32_t)(pkt_stats->bytes / 1024);
    stat.bytes      = pkt_stats->bytes;
    stat.msec       = pkt_stats->last_time - pkt_stats->first_time;
    stat.numPackets = pkt_stats->pkts;

    while (retry > 0) {
        ret = zsock_sendto(sock, &stat, pktlen, 0, faddr, addrlen);
        if (ret == pktlen) {
                        ret = zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo,
                                   sizeof(rcvtimeo));
            if (!ret) {
                ret = zsock_recvfrom(sock, udp_server_buf, sizeof(udp_server_buf),
                                     0, NULL, NULL);
                if (ret < 0) {
                    break;
                }
            }
        }

        k_sleep(K_MSEC(10));
        retry--;
    }
}

/*
 * finish_tx_and_wait_ack() - called on the TX side after all data packets
 * have been sent.  Builds an EOT (end-of-test) marker carrying the total
 * packet count and repeatedly sends it to the RX peer over the already-
 * connected socket.  After each send it waits up to 2 s for the RX peer
 * to reply with a sigmadut_stats_header.  Retries up to MAX_ACK_RETRY times.
 *
 * Parameters:
 *   fd   - the connected TX socket (used for both send and recv)
 *   pkts - total number of data packets sent, embedded in the EOT marker
 *
 * Returns 0 on success, -1 if no valid stats reply was received.
 */
static int finish_tx_and_wait_ack(int fd, uint32_t pkts)
{
    int ret = -1;
    int retry = MAX_ACK_RETRY;
    struct sigmadut_stats_header stat_packet;
    struct sigmadut_end eot_packet;

    eot_packet.code = htonl(END_OF_TEST_CODE);
    eot_packet.count = htonl(pkts);

    struct zsock_timeval tv = {
        .tv_sec  = 0,
        .tv_usec = 2000000,
    };

    while (retry > 0) {
        zsock_fd_set rset;

        ret = zsock_send(fd, &eot_packet, sizeof(eot_packet), 0);
        if (ret == (int)sizeof(eot_packet)) {
            ZSOCK_FD_ZERO(&rset);
            ZSOCK_FD_SET(fd, &rset);

            ret = zsock_select(fd + 1, &rset, NULL, NULL, &tv);
            if (ret > 0) {
                ret = zsock_recv(fd, &stat_packet, sizeof(stat_packet), 0);
                if (ret == (int)sizeof(stat_packet)) {
                    ret = 0;
                    break;
                }
            }
        }
        k_sleep(K_MSEC(10));
        retry--;
    }

    return ret;
}
#else
static void finish_rx_and_send_ack(struct session *pkt_stats, int sock_local,
                     struct sockaddr *faddr, socklen_t addrlen)
{
    return;
}

static int finish_tx_and_wait_ack(int fd, uint32_t pkts)
{
    return 0;
}
#endif /* UDP_ENDMARK_ENABLE */

static void sigmadut_traffic_result_print(enum sigma_role type, struct sigmadut_stats *pkt_stats)
{
    uint64_t total_bytes = 0;
    uint64_t duration = 0;
    double throughput = 0;
    uint32_t last_time  = pkt_stats->last_time;
    uint32_t first_time = pkt_stats->first_time;

    if (last_time < first_time) {
        duration = ~first_time + 1 + last_time;
    } else {
        duration = last_time - first_time;
    }
    duration = (duration + 999) / 1000;

    if (duration > 0) {
        total_bytes = pkt_stats->bytes;
        throughput = (double)total_bytes * 8 / 1000 / duration;
    }

    LOG_INF("Results for UDP %s.",
           (type == SIGMA_RX) ? "Receive" : "Transmit");
    LOG_INF("Duration:\t%llu.%03llus", duration / 1000, duration % 1000);
    LOG_INF("Num packets:\t%llu", pkt_stats->pkts);
    LOG_INF("Num Bytes:\t%llu", total_bytes);
    LOG_INF("Throughput:\t%.2f Mbps", throughput);
}

static void udp_svc_handler(struct net_socket_service_event *pev);
NET_SOCKET_SERVICE_SYNC_DEFINE_STATIC(sigmadut_udp_service, udp_svc_handler,
                                      SOCK_ID_MAX);

static int udp_recv_data(struct net_socket_service_event *pev)
{
    int ret = 1;
    int family, sock_error;
    struct sockaddr addr;
    bool finish = false;
    socklen_t optlen = sizeof(int);
    socklen_t addrlen = sizeof(addr);

    if (!sigmadut_rx_running) {
        return -ENOENT;
    }

    if ((pev->event.revents & ZSOCK_POLLERR) || (pev->event.revents & ZSOCK_POLLNVAL)) {
        (void)zsock_getsockopt(pev->event.fd, SOL_SOCKET, SO_DOMAIN, &family, &optlen);
        (void)zsock_getsockopt(pev->event.fd, SOL_SOCKET, SO_ERROR, &sock_error, &optlen);
        NET_ERR("UDP receiver IPv%d socket error (%d)", family == AF_INET ? 4 : 6, sock_error);
        ret = -sock_error;
        goto error;
    }

    if (!(pev->event.revents & ZSOCK_POLLIN)) {
        return 0;
    }

    while (ret > 0) {
        ret = zsock_recvfrom(pev->event.fd, udp_server_buf, sizeof(udp_server_buf), ZSOCK_MSG_DONTWAIT,
                     &addr, &addrlen);
        if ((ret < 0) && (errno == EAGAIN)) {
            ret = 0;
            break;
        }

        if (ret < 0) {
            ret = -errno;
            (void)zsock_getsockopt(pev->event.fd, SOL_SOCKET,
                           SO_DOMAIN, &family, &optlen);
            NET_ERR("recv failed on IPv%d socket (%d)",
                family == AF_INET ? 4 : 6, -ret);
            goto error;
        }

        if (sigmadut_rx_stop) {
            sigmadut_rx_stop = false;
            memset(&sigmadut_rx_stats, 0, sizeof(sigmadut_rx_stats));
            sigmadut_rx_stats.first_time = current_time_us();
        }
        sigmadut_rx_stats.pkts++;
        sigmadut_rx_stats.bytes += ret;

        if (ret == sizeof(struct sigmadut_end)) {
            struct sigmadut_end *eof = (struct sigmadut_end *)udp_server_buf;
            uint32_t code = ntohl(eof->code);
            if (code == END_OF_TEST_CODE) {
                finish = true;
                break;
            }
        }

        if (sigmadut_server_echo) {
            ret = zsock_sendto(pev->event.fd, udp_server_buf, ret, ZSOCK_MSG_DONTWAIT,
                                   (struct sockaddr *)&addr, addrlen);
            if (ret > 0) {
                sigmadut_rx_stats.echo_bytes += ret;
                sigmadut_rx_stats.echo_pkts++;
            }
        }
    }

    if (finish) {
        sigmadut_rx_stop = true;
        sigmadut_rx_stats.last_time = current_time_us();
        finish_rx_and_send_ack(&sigmadut_rx_stats, pev->event.fd, &addr, addrlen);
        sigmadut_traffic_result_print(SIGMA_RX, &sigmadut_rx_stats);
    }

error:
    return ret;
}

static void udp_receiver_cleanup(void)
{
    if (sigmadut_rx_running) {
        net_socket_service_unregister(&sigmadut_udp_service);
    }
    sigmadut_rx_running = false;

    for (int i = 0; i < ARRAY_SIZE(sigmadut_rx_fds); i++) {
        if (sigmadut_rx_fds[i].fd >= 0) {
            zsock_close(sigmadut_rx_fds[i].fd);
            sigmadut_rx_fds[i].fd = -1;
        }
    }

    if (!sigmadut_rx_stop) {
        sigmadut_rx_stop = true;
        sigmadut_rx_stats.last_time = current_time_us();
    }
    sigmadut_traffic_result_print(SIGMA_RX, &sigmadut_rx_stats);
}

static void udp_svc_handler(struct net_socket_service_event *pev)
{
    int ret;

    ret = udp_recv_data(pev);
    if (ret < 0) {
        udp_receiver_cleanup();
    }
}

static int sigmadut_udp_rx_init(struct sigmadut_rx_context *rx_ctx)
{
    int ret;
    const struct sockaddr_in *addr = &rx_ctx->server_addr;

    for (int i = 0; i < ARRAY_SIZE(sigmadut_rx_fds); i++) {
        sigmadut_rx_fds[i].fd = -1;
    }

    sigmadut_rx_fds[SOCK_ID_IPV4].fd = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sigmadut_rx_fds[SOCK_ID_IPV4].fd < 0) {
        ret = -errno;
        LOG_ERR("Cannot create IPv4 network socket.");
        goto error;
    }

    LOG_INF("Binding to %s", net_addr_sprint(addr));

    ret = zsock_bind(sigmadut_rx_fds[SOCK_ID_IPV4].fd, (struct sockaddr *)addr, sizeof(struct sockaddr_in));
    if (ret < 0) {
        LOG_ERR("Cannot bind IPv4 UDP port %d (%d)", ntohs(addr->sin_port), errno);
        goto error;
    }
    LOG_INF("Listening on port %d", ntohs(addr->sin_port));

    sigmadut_rx_fds[SOCK_ID_IPV4].events = ZSOCK_POLLIN;
    ret = net_socket_service_register(&sigmadut_udp_service, sigmadut_rx_fds, ARRAY_SIZE(sigmadut_rx_fds), NULL);
    if (ret < 0) {
        LOG_ERR("Cannot register socket service handler (%d)", ret);
    }

    if (!ret) {
        sigmadut_rx_running = true;
    }

error:
    return ret;
}

static void sigmadut_udp_tx(void *arg, void *p2, void *p3)
{
    int err = 0;
    int fd = -1;
    struct sigmadut_stats pkt_stats = {0};
    struct sigmadut_tx_context *sigma_tx_context = arg;
    struct sockaddr_in *server_addr = &sigma_tx_context->remote_addr;
    struct sockaddr_in *host_addr = &sigma_tx_context->host_addr;

    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    sigmadut_tx_running = true;

    uint32_t payload_size = sigma_tx_context->payload_size;
    uint8_t *buf = k_calloc(1, payload_size);
    if (!buf) {
        LOG_ERR("malloc fail");
        goto clean_up;
    }

    LOG_INF("Remote port is %d", ntohs(server_addr->sin_port));
    LOG_INF("Connecting to %s", net_addr_sprint(server_addr));
    LOG_INF("Duration: %ds", sigma_tx_context->duration);
    LOG_INF("Packet size: %d bytes", sigma_tx_context->payload_size);

    fd = zsock_socket(server_addr->sin_family, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        LOG_ERR("Socket creation failed");
        goto error;
    }

    if (host_addr->sin_port != 0) {
        if (zsock_bind(fd, (struct sockaddr *)host_addr, sizeof(*host_addr)) != 0) {
            LOG_ERR("Socket bind failed");
            goto error;
        }
    }

    if (sigma_tx_context->ip_tos > 0) {
        zsock_setsockopt(fd, IPPROTO_IP, IP_TOS,
                         &sigma_tx_context->ip_tos, sizeof(int));
    }

    LOG_INF("Connecting...");
    err = zsock_connect(fd, (const struct sockaddr *)server_addr, sizeof(*server_addr));
    if (err < 0) {
        LOG_ERR("Connection failed");
        goto error;
    }

    uint32_t id = 0;
    int64_t ts_start = current_time_us();
    int64_t pkt_duration = sigma_tx_context->interval_us;
    int64_t expired_ts = sigma_tx_context->duration * 1000000 + ts_start;
    pkt_stats.first_time = ts_start;

    while (!sigmadut_tx_stop) {
        int64_t ts = current_time_us();
        if (ts >= expired_ts) {
            break;
        }

        struct sigma_dut_header *hdr = (struct sigma_dut_header *)buf;
        hdr->seq = htonl(id);
        hdr->sec = htonl(ts / 1000);
        hdr->usec = htonl(ts % 1000);

retry_send:
        err = zsock_send(fd, buf, payload_size, 0);
        if (err < 0) {
            if (errno == EAGAIN) {
                goto retry_send;
            }
            if (errno == ENOMEM || errno == EINPROGRESS) {
                k_usleep(1);
                goto retry_send;
            }

            err = -errno;
            int family, optlen = sizeof(int);
            zsock_getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &family, &optlen);
            LOG_ERR("send failed on IPv%d socket (%d)", family == AF_INET ? 4 : 6, -err);
            goto error;
        }

        pkt_stats.bytes += err;
        pkt_stats.pkts++;
        id++;

        int64_t t = current_time_us() - ts;
        if (t > 0 && t < pkt_duration) {
            k_usleep(pkt_duration - t);
        }
    }

    pkt_stats.last_time = current_time_us();
    sigmadut_traffic_result_print(SIGMA_TX, &pkt_stats);
    finish_tx_and_wait_ack(fd, (uint32_t)pkt_stats.pkts);
    memcpy(&sigmadut_tx_stats, &pkt_stats, sizeof(sigmadut_tx_stats));

error:
    zsock_close(fd);
    k_free(buf);

clean_up:
    k_free(sigma_tx_context);
    sigmadut_tx_stop = false;
    sigmadut_tx_running = false;
}

/*
 * Parse an optional size suffix: plain number = bytes, K = *1024, M = *1024*1024.
 * Returns the value in bytes, or 0 on parse error.
 */
static uint32_t parse_size_suffix(const char *str)
{
    char *end;
    uint32_t val = (uint32_t)strtoul(str, &end, 0);

    if (end == str) {
        return 0;
    }
    if (*end == 'K' || *end == 'k') {
        val *= SIGMA_TRAFFIC_K;
    } else if (*end == 'M' || *end == 'm') {
        val *= SIGMA_TRAFFIC_M;
    }
    return val;
}

/*
 * Derive the inter-packet delay (us) from a target baud-rate in bytes/sec
 * and a packet size in bytes.  Clamp to at least 1 us.
 */
static uint32_t baudrate_to_interval_us(uint32_t baudrate_bps, uint32_t packet_size)
{
    if (baudrate_bps == 0 || packet_size == 0) {
        return 1;
    }
    uint64_t sum = (uint64_t)packet_size * 8 * SIGMA_TRAFFIC_M;
    uint32_t us = (uint32_t)(sum / (uint64_t)baudrate_bps);
    return us > 0 ? us : 1;
}

/*
 * sigmadut_upload_cmd() - shell handler for "wificert upload".
 *
 * Parses options and positional arguments, fills a sigma_tx_context with the
 * resulting configuration, and spawns the sigmadut_udp_tx thread to run the test.
 *
 * Usage: wificert upload [-S <tos>] <dest_ip> [<dest_port> [<duration>
 *                        [<packet_size>[K] [<baud_rate>[K|M]]]]]
 */
static int sigmadut_upload_cmd(const struct shell *sh, size_t argc, char **argv)
{
    int err = 0;
    const char *opt = NULL;

    struct sigmadut_tx_context *sigma_tx_context = k_calloc(1, sizeof(*sigma_tx_context));
    if (sigma_tx_context == NULL) {
        shell_error(sh, "Sigma UDP context memory alloc failed");
        return -ENOMEM;
    }

    sigma_tx_context->remote_addr.sin_family = AF_INET;
    sigma_tx_context->remote_addr.sin_port = htons(SIGMA_TRAFFIC_DEFAULT_PORT);
    sigma_tx_context->payload_size = SIGMA_TRAFFIC_DEFAULT_PACKET_SIZE;
    sigma_tx_context->duration = SIGMA_TRAFFIC_DEFAULT_RUNTIME;
    uint32_t bps = SIGMA_TRAFFIC_DEFAULT_BAUDRATE;

    size_t index = 1;
    while (index < argc && argv[index][0] == '-') {
        opt = argv[index++];
        if (strcmp(opt, "-S") == 0) {
            if (index >= argc) {
                shell_error(sh, "Missing argument for -S");
                err = -EINVAL;
                goto end;
            }
            int32_t ip_tos = (int32_t)strtol(argv[index++], NULL, 0);
            if (ip_tos < 0 || ip_tos > 255) {
                shell_error(sh, "Invalid TOS value (0-255)");
                err = -EINVAL;
                goto end;
            }
            sigma_tx_context->ip_tos = ip_tos;
        } else {
            shell_error(sh, "Unknown option '%s'", opt);
            err = -EINVAL;
            goto end;
        }
    }

    if (index >= argc) {
        shell_error(sh, "Missing destination IP address");
        err = -EINVAL;
        goto end;
    }
    opt = argv[index++];
    if (zsock_inet_pton(AF_INET, opt, &sigma_tx_context->remote_addr.sin_addr) != 1) {
        shell_error(sh, "Invalid destination IP address (%s)", opt);
        err = -EINVAL;
        goto end;
    }

    if (index < argc) {
        opt = argv[index++];
        uint32_t port = (uint32_t)strtoul(opt, NULL, 0);
        if (port == 0 || port > 65535) {
            shell_error(sh, "Invalid destination port (%s)", opt);
            err = -EINVAL;
            goto end;
        }
        sigma_tx_context->remote_addr.sin_port = htons(port);
    }

    if (index < argc) {
        opt = argv[index++];
        uint32_t duration = (uint32_t)strtoul(opt, NULL, 0);
        if (duration == 0) {
            shell_error(sh, "Invalid duration (%s)", opt);
            err = -EINVAL;
            goto end;
        }
        sigma_tx_context->duration = duration;
    }

    if (index < argc) {
        opt = argv[index++];
        uint32_t size = parse_size_suffix(opt);
        if (size < sizeof(struct sigma_dut_header)) {
            shell_error(sh, "Invalid packet size (%s)", opt);
            err = -EINVAL;
            goto end;
        }
        sigma_tx_context->payload_size = size;
    }

    if (index < argc) {
        opt = argv[index++];
        bps = parse_size_suffix(opt);
        if (bps == 0) {
            shell_error(sh, "Invalid baud rate (%s)", opt);
            err = -EINVAL;
            goto end;
        }
    }

    sigma_tx_context->interval_us =
            baudrate_to_interval_us(bps, sigma_tx_context->payload_size);

    k_tid_t tid = k_thread_create(&sigma_tx_context->thread_data,
                                  sigma_tx_context->thread_stack,
                                  SIGMA_THREAD_STACK_SIZE,
                                  sigmadut_udp_tx,
                                  sigma_tx_context, NULL, NULL,
                                  SIGMA_THREAD_PRIORITY, 0, K_NO_WAIT);
    if (tid == NULL) {
        shell_error(sh, "Sigma UDP upload task creation failed");
        err = -EINVAL;
        goto end;
    }
    k_thread_name_set(tid, "sigmadut_udp_tx");

    return 0;

end:
    k_free(sigma_tx_context);
    return err;
}

static atomic_t in_sigmadut_config = {0};

/*
 * sigmadut_download_cmd() - shell handler for "wificert download".
 *
 * Parses an optional echo flag (-e), an optional listen port, and an optional
 * bind address, then calls sigmadut_udp_rx_init() to open the socket and
 * register it with the Zephyr socket service.
 *
 * Usage: wificert download [-e] [<port> [<host>]]
 */
static int sigmadut_download_cmd(const struct shell *sh, size_t argc, char **argv)
{
    int err = 0;
    struct sigmadut_rx_context sigmadut_rx_param = {0};

    if (!atomic_cas(&in_sigmadut_config, 0, 1)) {
        shell_error(sh, "In configuration, please retry it.");
        return -EAGAIN;
    }

    if (sigmadut_rx_running) {
        shell_error(sh, "UDP server already started!");
        err = -EINVAL;
        goto end;
    }

    struct sockaddr_in *addr_in = &sigmadut_rx_param.server_addr;
    addr_in->sin_family = AF_INET;
    addr_in->sin_port = htons(SIGMA_TRAFFIC_DEFAULT_PORT);
    sigmadut_server_echo = false;

    size_t index = 1;
    while (index < argc && argv[index][0] == '-') {
        const char *opt = argv[index++];
        if (strcmp(opt, "-e") == 0) {
            sigmadut_server_echo = true;
            break;
        }
    }

    if (index < argc) {
        uint32_t port = (uint32_t)strtoul(argv[index++], NULL, 0);
        if (port == 0 || port > 65535) {
            shell_error(sh, "Invalid port (%s)", argv[index]);
            err = -EINVAL;
            goto end;
        }
        addr_in->sin_port = htons(port);
    }

    if (index < argc) {
        if (zsock_inet_pton(AF_INET, argv[index++], &addr_in->sin_addr) != 1) {
            shell_error(sh, "Invalid bind address (%s)", argv[index]);
            err = -EINVAL;
            goto end;
        }
    }

    err = sigmadut_udp_rx_init(&sigmadut_rx_param);

end:
    atomic_set(&in_sigmadut_config, 0);

    return err;
}

static int sigmadut_upload_stop_cmd(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(sh);
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    if (sigmadut_tx_running) {
        sigmadut_tx_stop = true;
    }

    if (!sigmadut_tx_running) {
        sigmadut_traffic_result_print(SIGMA_TX, &sigmadut_tx_stats);
    }

    return 0;
}

static int sigmadut_download_stop_cmd(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(sh);
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    udp_receiver_cleanup();

    return 0;
}

#define UPLOAD_HELP_TEXT                                                                                               \
    "upload [<options>] <dest ip> [<dest port> [<duration> [<packet size>[K|M] [<baud rate>[K|M]]]]]\n"                \
    "  <dest ip>     Destination IPv4 address\n"                                                                       \
    "  <dest port>   Destination UDP port          (default " STRINGIFY(SIGMA_TRAFFIC_DEFAULT_PORT) ")\n"              \
    "  <duration>    Test duration in seconds      (default " STRINGIFY(SIGMA_TRAFFIC_DEFAULT_RUNTIME) ")\n"           \
    "  <packet size> UDP payload in bytes, K=*1000, M=*1000000 (default " STRINGIFY(SIGMA_TRAFFIC_DEFAULT_PACKET_SIZE) ")\n" \
    "  <baud rate>   Target TX rate,      K=*1000, M=*1000000 (default 30M)\n"                                         \
    "Available options:\n"                                                                                             \
    "  -S <tos>      IPv4 Type-of-Service field value (0-255)\n"                                                       \
    "Examples:\n"                                                                                                      \
    "  wificert upload 10.0.0.1\n"                                                                                     \
    "  wificert upload 10.0.0.1 7001 5 1400 30M\n"                                                                     \

#define DOWNLOAD_HELP_TEXT                                                                                             \
    "download [<options>] [<port> [<host>]]\n"                                                                         \
    "  <port>        UDP port to listen on          (default " STRINGIFY(SIGMA_TRAFFIC_DEFAULT_PORT) ")\n"             \
    "  <host>        Local IPv4 address to bind to  (default: any)\n"                                                  \
    "Available options:\n"                                                                                             \
    "  -e            Echo each received packet back to the sender\n"                                                   \
    "Examples:\n"                                                                                                      \
    "  wificert download\n"                                                                                            \
    "  wificert download 7001\n"                                                                                       \
    "  wificert download 7001 10.0.0.1\n"                                                                              \

SHELL_STATIC_SUBCMD_SET_CREATE(cmd_tcp_upload,
    SHELL_CMD(stop, NULL, "Stop TCP server\n", sigmadut_upload_stop_cmd),
    SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(cmd_tcp_download,
    SHELL_CMD(stop, NULL, "Stop TCP server\n", sigmadut_download_stop_cmd),
    SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(wificert_cmds,
    SHELL_CMD_ARG(upload, &cmd_tcp_upload, UPLOAD_HELP_TEXT, sigmadut_upload_cmd, 2, 8),
    SHELL_CMD_ARG(download, &cmd_tcp_download, DOWNLOAD_HELP_TEXT, sigmadut_download_cmd, 0, 0),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(wificert, &wificert_cmds, "wificert commands", NULL);
