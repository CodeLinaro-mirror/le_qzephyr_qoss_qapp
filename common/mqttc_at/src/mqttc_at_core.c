/*
 * Copyright (c) 2026 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/dns_resolve.h>
#include <zephyr/net/net_if.h>
#if defined(CONFIG_MQTT_LIB_TLS) && defined(CONFIG_TLS_CREDENTIALS)
#include <zephyr/net/tls_credentials.h>
#define MQTTC_AT_TLS_SUPPORTED 1
#else
#define MQTTC_AT_TLS_SUPPORTED 0
#endif
#if MQTTC_AT_TLS_SUPPORTED && defined(CONFIG_FILE_SYSTEM)
#include <zephyr/fs/fs.h>
#endif
#include "mqttc_at_handler.h"

LOG_MODULE_REGISTER(mqttc_at_core, CONFIG_MQTTC_AT_LOG_LEVEL);

#define MQTTC_AT_SESSIONS CONFIG_MQTTC_AT_MAX_SESSIONS
#define MQTTC_AT_MAX_TOPICS CONFIG_MQTTC_AT_MAX_TOPICS
#define MQTTC_AT_RECV_STACK CONFIG_MQTTC_AT_RECV_THREAD_STACK_SIZE

/* MQTT receive/keepalive poll interval */
#define MQTTC_POLL_MS 100
#define MQTTC_AT_TLS_TAG_BASE 30
#define MQTTC_AT_MAX_CERT_PATH_LEN 128

typedef enum {
    MQTT_STATE_INIT = 0,
    MQTT_STATE_CONNECTED = 1,
    MQTT_STATE_DISCONNECT = 2,
    MQTT_STATE_FORCE_DISCONNECT = 3,
} mqttc_state_t;

typedef struct {
    struct mqtt_client client;
    struct sockaddr_storage broker_addr;
    uint8_t rx_buf[512];
    uint8_t tx_buf[512];
    mqttc_state_t state;
    bool in_use;
    char host[MQTTC_AT_MAX_HOST_LEN];
    uint16_t port;
    char client_id[MQTTC_AT_MAX_CLIENT_ID_LEN];
    char username[MQTTC_AT_MAX_USERNAME_LEN];
    char password[MQTTC_AT_MAX_PASSWORD_LEN];
    bool use_ssl;
    char ca_file[MQTTC_AT_MAX_CERT_PATH_LEN];
    char cert_file[MQTTC_AT_MAX_CERT_PATH_LEN];
    char key_file[MQTTC_AT_MAX_CERT_PATH_LEN];
    sec_tag_t sec_tags[3];
    uint32_t sec_tag_count;
    uint8_t *ca_buf;
    uint8_t *cert_buf;
    uint8_t *key_buf;
    /* persistent mqtt_utf8 structs for client.user_name / client.password */
    struct mqtt_utf8 username_utf8;
    struct mqtt_utf8 password_utf8;
    /* subscriptions */
    char sub_topics[MQTTC_AT_MAX_TOPICS][MQTTC_AT_MAX_TOPIC_LEN];
    uint8_t sub_qos[MQTTC_AT_MAX_TOPICS];
    int sub_count;
    char pending_unsub_topic[MQTTC_AT_MAX_TOPIC_LEN];
    /* receive thread */
    struct k_thread recv_thread;
    k_thread_stack_t *recv_stack;
    bool recv_thread_started;
    /* mutex for MQTT client serialization */
    struct k_mutex lock;
    /* publish receive buffers (kept in BSS to avoid stack overflow in mqtt_evt_cb) */
    uint8_t payload_buf[MQTTC_AT_MAX_PAYLOAD_LEN + 1];
    char hex_buf[MQTTC_AT_MAX_PAYLOAD_LEN * 2 + 1];
} mqttc_session_t;

/* PUBRAW data-mode state (one at a time, serialized by QAT) */
typedef struct {
    bool active;
    int session_id;
    char topic[MQTTC_AT_MAX_TOPIC_LEN];
    uint8_t qos;
    uint8_t retain;
    uint8_t buf[MQTTC_AT_MAX_PAYLOAD_LEN];
    size_t expected_len;
    size_t offset;
} mqttc_pubraw_t;

static mqttc_session_t *g_sessions[MQTTC_AT_SESSIONS];
static int g_recv_mode; /* 0=string, 1=hex */
static mqttc_pubraw_t *g_pubraw;

static mqttc_at_output_cb_t g_output_cb;
static void *g_output_user_data;

/* -------------------------------------------------------------------------
 * Output helpers
 * ---------------------------------------------------------------------- */

static void mqttc_output(const char *buf)
{
    if (g_output_cb) {
        g_output_cb(buf, strlen(buf), g_output_user_data);
    }
}

static void mqttc_output_urc(const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    mqttc_output(buf);
}

static const char *mqttc_transport_scheme(const mqttc_session_t *s) { return s->use_ssl ? "SSL" : "TCP"; }

#if MQTTC_AT_TLS_SUPPORTED
static void mqttc_delete_credential_if_exists(sec_tag_t tag, enum tls_credential_type type)
{
    int rc = tls_credential_delete(tag, type);

    if (rc < 0 && rc != -ENOENT) {
        LOG_WRN("tls_credential_delete(tag=%d, type=%d) failed: %d", (int)tag, (int)type, rc);
    }
}

static void mqttc_unload_tls_credentials(mqttc_session_t *s)
{
    mqttc_delete_credential_if_exists(s->sec_tags[0], TLS_CREDENTIAL_CA_CERTIFICATE);
    k_free(s->ca_buf);
    s->ca_buf = NULL;

    mqttc_delete_credential_if_exists(s->sec_tags[1], TLS_CREDENTIAL_PUBLIC_CERTIFICATE);
    k_free(s->cert_buf);
    s->cert_buf = NULL;

    mqttc_delete_credential_if_exists(s->sec_tags[2], TLS_CREDENTIAL_PRIVATE_KEY);
    k_free(s->key_buf);
    s->key_buf = NULL;

    s->sec_tag_count = 0;
}
#endif

#if MQTTC_AT_TLS_SUPPORTED && defined(CONFIG_FILE_SYSTEM)
static int mqttc_read_file_to_buf(const char *path, uint8_t **buf_out, size_t *len_out)
{
    struct fs_file_t file;
    struct fs_dirent entry;
    uint8_t *buf;
    ssize_t bytes_read;
    int rc;

    rc = fs_stat(path, &entry);
    if (rc < 0) {
        LOG_ERR("fs_stat(%s) failed: %d", path, rc);
        return rc;
    }
    if (entry.type != FS_DIR_ENTRY_FILE) {
        return -EISDIR;
    }
    if (entry.size == 0) {
        return -ENODATA;
    }

    buf = k_malloc(entry.size + 1);
    if (!buf) {
        return -ENOMEM;
    }

    fs_file_t_init(&file);
    rc = fs_open(&file, path, FS_O_READ);
    if (rc < 0) {
        k_free(buf);
        return rc;
    }

    bytes_read = fs_read(&file, buf, entry.size);
    fs_close(&file);
    if (bytes_read < 0) {
        k_free(buf);
        return (int)bytes_read;
    }
    if (bytes_read != (ssize_t)entry.size) {
        k_free(buf);
        return -EIO;
    }

    buf[bytes_read] = '\0';
    *buf_out = buf;
    *len_out = (size_t)bytes_read;
    return 0;
}

static int mqttc_load_credential(const char *path, sec_tag_t tag, enum tls_credential_type type, uint8_t **buf_keep)
{
    uint8_t *buf;
    size_t len;
    int rc;

    if (!path || path[0] == '\0') {
        return -EINVAL;
    }

    rc = mqttc_read_file_to_buf(path, &buf, &len);
    if (rc < 0) {
        return rc;
    }

    mqttc_delete_credential_if_exists(tag, type);
    k_free(*buf_keep);
    *buf_keep = NULL;

    rc = tls_credential_add(tag, type, buf, (len > 0 && buf[0] == '-') ? len + 1 : len);
    if (rc < 0) {
        k_free(buf);
        return rc;
    }

    *buf_keep = buf;
    return 0;
}

static int mqttc_load_tls_credentials(mqttc_session_t *s)
{
    int rc;

    if (s->ca_file[0] == '\0') {
        return -EINVAL;
    }
    if ((s->cert_file[0] == '\0') != (s->key_file[0] == '\0')) {
        return -EINVAL;
    }

    rc = mqttc_load_credential(s->ca_file, s->sec_tags[0], TLS_CREDENTIAL_CA_CERTIFICATE, &s->ca_buf);
    if (rc < 0) {
        goto fail;
    }
    s->sec_tag_count = 1;

    if (s->cert_file[0] != '\0') {
        rc = mqttc_load_credential(s->cert_file, s->sec_tags[1], TLS_CREDENTIAL_PUBLIC_CERTIFICATE, &s->cert_buf);
        if (rc < 0) {
            goto fail;
        }
        rc = mqttc_load_credential(s->key_file, s->sec_tags[2], TLS_CREDENTIAL_PRIVATE_KEY, &s->key_buf);
        if (rc < 0) {
            goto fail;
        }
        s->sec_tag_count = 3;
    }

    return 0;

fail:
    mqttc_unload_tls_credentials(s);
    s->sec_tag_count = 0;
    return rc;
}
#endif

static int mqttc_prepare_tls(mqttc_session_t *s)
{
    if (!s->use_ssl) {
        return 0;
    }

#if !MQTTC_AT_TLS_SUPPORTED
    return -ENOTSUP;
#elif !defined(CONFIG_FILE_SYSTEM)
    return -ENOTSUP;
#else
    return mqttc_load_tls_credentials(s);
#endif
}

/* -------------------------------------------------------------------------
 * MQTT event callback
 * ---------------------------------------------------------------------- */

static int session_to_sid(const mqttc_session_t *s)
{
    for (int i = 0; i < MQTTC_AT_SESSIONS; i++) {
        if (g_sessions[i] == s) {
            return i;
        }
    }
    return -1;
}

static void mqtt_evt_cb(struct mqtt_client *client, const struct mqtt_evt *evt)
{
    mqttc_session_t *s = CONTAINER_OF(client, mqttc_session_t, client);
    int sid = session_to_sid(s);

    switch (evt->type) {
    case MQTT_EVT_CONNACK:
        if (evt->result == 0) {
            s->state = MQTT_STATE_CONNECTED;
            mqttc_output_urc("\r\n+EVT:MQTT_CONNECTED:%d,%s,\"%s\",%d,1\r\n", sid, mqttc_transport_scheme(s), s->host,
                             s->port);
        } else {
            s->state = MQTT_STATE_DISCONNECT;
#if MQTTC_AT_TLS_SUPPORTED
            if (s->use_ssl) {
                mqttc_unload_tls_credentials(s);
            }
#endif
            mqttc_output_urc("\r\n+EVT:MQTT_DISCONNECTED:%d\r\n", sid);
        }
        break;

    case MQTT_EVT_DISCONNECT: {
        bool was_connected = (s->state == MQTT_STATE_CONNECTED);

        s->state = MQTT_STATE_DISCONNECT;
        s->pending_unsub_topic[0] = '\0';
#if MQTTC_AT_TLS_SUPPORTED
        if (s->use_ssl) {
            mqttc_unload_tls_credentials(s);
        }
#endif
        if (was_connected) {
            mqttc_output_urc("\r\n+EVT:MQTT_DISCONNECTED:%d\r\n", sid);
        }
        break;
    }

    case MQTT_EVT_PUBLISH: {
        const struct mqtt_publish_param *p = &evt->param.publish;
        const char *topic_str = (const char *)p->message.topic.topic.utf8;
        uint16_t topic_len = p->message.topic.topic.size;
        uint32_t total_payload_len = p->message.payload.len;
        uint32_t payload_len = total_payload_len;
        char topic_copy[MQTTC_AT_MAX_TOPIC_LEN];
        uint8_t drain_buf[64];
        size_t copy_len;
        int rc;

        if (payload_len > MQTTC_AT_MAX_PAYLOAD_LEN) {
            LOG_WRN("sid %d: publish payload too large (%u), truncating", sid, payload_len);
            payload_len = MQTTC_AT_MAX_PAYLOAD_LEN;
        }

        rc = mqtt_read_publish_payload_blocking(client, s->payload_buf, payload_len);
        if (rc < 0) {
            LOG_ERR("sid %d: mqtt_read_publish_payload_blocking failed: %d", sid, rc);
            break;
        }
        if ((uint32_t)rc != payload_len) {
            LOG_WRN("sid %d: short read %d/%u, draining and dropping", sid, rc, payload_len);
            uint32_t remaining = total_payload_len - (uint32_t)rc;
            while (remaining > 0) {
                uint32_t chunk = MIN(remaining, sizeof(drain_buf));
                int drained = mqtt_read_publish_payload_blocking(client, drain_buf, chunk);
                if (drained <= 0) {
                    break;
                }
                remaining -= (uint32_t)drained;
            }
            break;
        }

        /* Drain any excess beyond truncation point to keep stream in sync */
        if (total_payload_len > payload_len) {
            uint32_t excess = total_payload_len - payload_len;
            while (excess > 0) {
                uint32_t chunk = MIN(excess, sizeof(drain_buf));
                int drained = mqtt_read_publish_payload_blocking(client, drain_buf, chunk);
                if (drained <= 0) {
                    break;
                }
                excess -= (uint32_t)drained;
            }
        }
        s->payload_buf[payload_len] = '\0';

        if (p->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
            struct mqtt_puback_param ack = {.message_id = p->message_id};
            mqtt_publish_qos1_ack(client, &ack);
        } else if (p->message.topic.qos == MQTT_QOS_2_EXACTLY_ONCE) {
            struct mqtt_pubrec_param pubrec = {.message_id = p->message_id};
            mqtt_publish_qos2_receive(client, &pubrec);
        }

        copy_len = MIN((size_t)topic_len, sizeof(topic_copy) - 1);
        memcpy(topic_copy, topic_str, copy_len);
        topic_copy[copy_len] = '\0';

        if (g_recv_mode == 0) {
            /* String mode — split output to avoid 256-byte URC buffer truncation */
            mqttc_output_urc("\r\n+EVT:MQTT_SUBRECV:%d,\"%s\",%u,", sid, topic_copy, payload_len);
            mqttc_output((const char *)s->payload_buf);
            mqttc_output("\r\n");
        } else {
            /* Hex mode — build hex in s->hex_buf (BSS, not stack) and output in pieces
             * to avoid the 256-byte mqttc_output_urc buffer truncating large payloads */
            for (uint32_t i = 0; i < payload_len; i++) {
                snprintf(&s->hex_buf[i * 2], 3, "%02X", s->payload_buf[i]);
            }
            s->hex_buf[payload_len * 2] = '\0';
            mqttc_output_urc("\r\n+EVT:MQTT_SUBRECVHEX:%d,\"%s\",%u,", sid, topic_copy, payload_len);
            mqttc_output(s->hex_buf);
            mqttc_output("\r\n");
        }
        break;
    }

    case MQTT_EVT_PUBACK:
        mqttc_output_urc("\r\n+EVT:MQTT_PUBSUC:%d,0,%s,\"%s\",%d\r\n", sid, mqttc_transport_scheme(s), s->host,
                         s->port);
        break;

    case MQTT_EVT_PUBREC: {
        const struct mqtt_pubrec_param *pubrec = &evt->param.pubrec;
        struct mqtt_pubrel_param pubrel = {.message_id = pubrec->message_id};
        mqtt_publish_qos2_release(&s->client, &pubrel);
        break;
    }

    case MQTT_EVT_PUBCOMP:
        mqttc_output_urc("\r\n+EVT:MQTT_PUBSUC:%d,0,%s,\"%s\",%d\r\n", sid, mqttc_transport_scheme(s), s->host,
                         s->port);
        break;

    case MQTT_EVT_SUBACK: {
        const struct mqtt_suback_param *sub = &evt->param.suback;

        if (sub->return_codes.len > 0 && sub->return_codes.data[0] != MQTT_SUBACK_FAILURE) {
            /* Find the topic that was just subscribed */
            if (s->sub_count > 0) {
                mqttc_output_urc("\r\n+EVT:MQTT_SUBSUC:%d,\"%s\"\r\n", sid, s->sub_topics[s->sub_count - 1]);
            }
        } else {
            if (s->sub_count > 0) {
                mqttc_output_urc("\r\n+EVT:MQTT_SUBFAIL:%d,\"%s\"\r\n", sid, s->sub_topics[s->sub_count - 1]);
                /* Remove the failed subscription */
                s->sub_count--;
            }
        }
        break;
    }

    case MQTT_EVT_UNSUBACK:
        mqttc_output_urc("\r\n+EVT:MQTT_UNSUBSUC:%d,\"%s\"\r\n", sid, s->pending_unsub_topic);
        s->pending_unsub_topic[0] = '\0';
        break;

    case MQTT_EVT_PINGRESP:
        break;

    default:
        break;
    }
}

/* -------------------------------------------------------------------------
 * Receive thread
 * ---------------------------------------------------------------------- */

static void mqttc_recv_thread_fn(void *arg1, void *arg2, void *arg3)
{
    mqttc_session_t *s = (mqttc_session_t *)arg1;
    int sid = session_to_sid(s);

    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    LOG_INF("sid %d: recv thread started", sid);

    while (s->state == MQTT_STATE_CONNECTED) {
        int rc;

        k_mutex_lock(&s->lock, K_FOREVER);
        rc = mqtt_input(&s->client);
        if (rc != 0 && rc != -EAGAIN) {
            LOG_ERR("sid %d: mqtt_input error: %d", sid, rc);
            s->state = MQTT_STATE_DISCONNECT;
            k_mutex_unlock(&s->lock);
            mqttc_output_urc("\r\n+EVT:MQTT_DISCONNECTED:%d\r\n", sid);
            break;
        }

        rc = mqtt_live(&s->client);
        k_mutex_unlock(&s->lock);
        if (rc != 0 && rc != -EAGAIN) {
            LOG_ERR("sid %d: mqtt_live error: %d", sid, rc);
            if (rc == -ETIMEDOUT) {
                mqttc_output_urc("\r\n+EVT:MQTT_KEEPLIVETIMEOUT:%d\r\n", sid);
            }
            s->state = MQTT_STATE_DISCONNECT;
            mqttc_output_urc("\r\n+EVT:MQTT_DISCONNECTED:%d\r\n", sid);
            break;
        }

        k_sleep(K_MSEC(MQTTC_POLL_MS));
    }

    LOG_INF("sid %d: recv thread exiting", sid);
}

/* -------------------------------------------------------------------------
 * DNS resolution helper
 * ---------------------------------------------------------------------- */

static int mqttc_resolve_host(const char *host, uint16_t port, struct sockaddr_storage *addr)
{
    struct zsock_addrinfo hints = {
        .ai_family   = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
    };
    struct zsock_addrinfo *res = NULL;
    char port_str[8];
    int rc;

    snprintf(port_str, sizeof(port_str), "%u", port);
    rc = zsock_getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0) {
        LOG_ERR("DNS resolution failed for %s: %d", host, rc);
        return -ENOENT;
    }

    memcpy(addr, res->ai_addr, res->ai_addrlen);
    zsock_freeaddrinfo(res);

    /* link-local IPv6 (fe80::/10) requires sin6_scope_id to identify the
     * outgoing interface. zsock_getaddrinfo() leaves it 0 for literal
     * addresses, so fill it from the default (WiFi) interface. */
    if (addr->ss_family == AF_INET6) {
        struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)addr;

        if (a6->sin6_scope_id == 0) {
            struct net_if *iface = net_if_get_first_wifi();

            if (!iface) {
                iface = net_if_get_default();
            }
            if (iface) {
                a6->sin6_scope_id = (uint32_t)net_if_get_by_iface(iface);
                LOG_DBG("link-local IPv6: set scope_id=%u", a6->sin6_scope_id);
            }
        }
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Public API — init
 * ---------------------------------------------------------------------- */

int mqttc_at_core_init(mqttc_at_output_cb_t output_cb, void *user_data)
{
    g_output_cb = output_cb;
    g_output_user_data = user_data;
    memset(g_sessions, 0, sizeof(g_sessions));
    g_pubraw = NULL;
    g_recv_mode = 0;

    return 0;
}

/* -------------------------------------------------------------------------
 * Session teardown helper
 *
 * Fully reclaims g_sessions[sid]: disconnects if still connected, terminates
 * the recv thread, frees its stack, unloads TLS credentials, k_free()s the
 * session struct and NULLs the slot. Shared by mqttc_at_core_destroy() and the
 * stale-slot reclaim path in mqttc_at_core_init_session().
 *
 * emit_destroyed_urc: when true, emits +EVT:MQTT_DESTROYED (explicit DESTROY);
 * when false, the slot is reclaimed silently (re-init of a stale slot).
 * ---------------------------------------------------------------------- */
static void mqttc_session_teardown(int sid, bool emit_destroyed_urc)
{
    mqttc_session_t *s = g_sessions[sid];
    struct mqtt_disconnect_param disc_param = {0};

    if (s == NULL) {
        return;
    }

    if (s->state == MQTT_STATE_CONNECTED) {
        k_mutex_lock(&s->lock, K_FOREVER);
        mqtt_disconnect(&s->client, &disc_param);
        mqtt_abort(&s->client);
        k_mutex_unlock(&s->lock);
        s->state = MQTT_STATE_DISCONNECT;
    }

    if (s->recv_thread_started) {
        /* Try graceful join first (thread checks state each 100ms loop) */
        if (k_thread_join(&s->recv_thread, K_MSEC(500)) != 0) {
            k_thread_abort(&s->recv_thread);
        }
        s->recv_thread_started = false;
    }
    /* Plan B: free recv stack */
    if (s->recv_stack) {
        k_thread_stack_free(s->recv_stack);
        s->recv_stack = NULL;
    }

#if MQTTC_AT_TLS_SUPPORTED
    if (s->use_ssl) {
        mqttc_unload_tls_credentials(s);
    }
#endif

    /* Plan A: free session struct */
    if (emit_destroyed_urc) {
        mqttc_output_urc("\r\n+EVT:MQTT_DESTROYED:%d\r\n", sid);
    }
    k_free(s);
    g_sessions[sid] = NULL;
}

/* -------------------------------------------------------------------------
 * Public API — session operations
 * ---------------------------------------------------------------------- */

int mqttc_at_core_init_session(int sid, bool use_ssl, const char *ca_file, const char *cert_file, const char *key_file)
{
    mqttc_session_t *s;
    sec_tag_t base_tag;

    if (sid < 0 || sid >= MQTTC_AT_SESSIONS) {
        return -EINVAL;
    }

    if (g_sessions[sid] != NULL) {
        /* A slot is already allocated. If the session is actively CONNECTED,
         * reject the re-init (caller must DESTROY first). Otherwise the slot is
         * stale (INIT/DISCONNECT after a dropped connection or a
         * disconnect-without-destroy): reclaim it silently so the re-init
         * succeeds idempotently instead of returning -EALREADY. (CR 4563318) */
        if (g_sessions[sid]->state == MQTT_STATE_CONNECTED) {
            return -EALREADY;
        }
        mqttc_session_teardown(sid, false);
    }

    if (ca_file && strlen(ca_file) >= MQTTC_AT_MAX_CERT_PATH_LEN) {
        return -ENAMETOOLONG;
    }
    if (cert_file && strlen(cert_file) >= MQTTC_AT_MAX_CERT_PATH_LEN) {
        return -ENAMETOOLONG;
    }
    if (key_file && strlen(key_file) >= MQTTC_AT_MAX_CERT_PATH_LEN) {
        return -ENAMETOOLONG;
    }

    s = k_malloc(sizeof(mqttc_session_t));
    if (!s) {
        LOG_ERR("sid %d: failed to alloc session (%zu B)", sid, sizeof(mqttc_session_t));
        return -ENOMEM;
    }
    memset(s, 0, sizeof(*s));
    g_sessions[sid] = s;

    s->state = MQTT_STATE_INIT;
    s->in_use = true;
    s->use_ssl = use_ssl;
    base_tag = MQTTC_AT_TLS_TAG_BASE + sid * 3;
    s->sec_tags[0] = base_tag;
    s->sec_tags[1] = base_tag + 1;
    s->sec_tags[2] = base_tag + 2;
    if (ca_file) {
        strlcpy(s->ca_file, ca_file, sizeof(s->ca_file));
    }
    if (cert_file) {
        strlcpy(s->cert_file, cert_file, sizeof(s->cert_file));
    }
    if (key_file) {
        strlcpy(s->key_file, key_file, sizeof(s->key_file));
    }
    k_mutex_init(&s->lock);
    return 0;
}

int mqttc_at_core_connect(int sid, const char *host, uint16_t port, const char *client_id, const char *username,
                          const char *password, uint16_t keepalive, bool clean_session)
{
    mqttc_session_t *s;
    struct mqtt_utf8 client_id_utf8;
    int rc;

    if (sid < 0 || sid >= MQTTC_AT_SESSIONS) {
        return -EINVAL;
    }

    s = g_sessions[sid];
    if (s == NULL) {
        return -ENODEV;
    }
    if (s->state == MQTT_STATE_CONNECTED) {
        return -EALREADY;
    }

    /* Ensure previous recv thread is fully terminated before reusing k_thread.
     * If join times out the thread may still be in a Zephyr wait queue; abort
     * to remove it before the k_thread struct is reused.
     */
    if (s->recv_thread_started) {
        if (k_thread_join(&s->recv_thread, K_MSEC(500)) != 0) {
            k_thread_abort(&s->recv_thread);
        }
        s->recv_thread_started = false;
        /* Plan B: free stack from previous connection */
        if (s->recv_stack) {
            k_thread_stack_free(s->recv_stack);
            s->recv_stack = NULL;
        }
    }

    /* Resolve host */
    rc = mqttc_resolve_host(host, port, &s->broker_addr);
    if (rc < 0) {
        return rc;
    }

    /* Store connection params */
    strlcpy(s->host, host, sizeof(s->host));
    s->port = port;
    strlcpy(s->client_id, client_id, sizeof(s->client_id));
    strlcpy(s->username, username, sizeof(s->username));
    strlcpy(s->password, password, sizeof(s->password));

    /* Configure mqtt_client */
    mqtt_client_init(&s->client);

    s->client.broker = (struct sockaddr *)&s->broker_addr;
    s->client.evt_cb = mqtt_evt_cb;
    s->client.protocol_version = MQTT_VERSION_3_1_1;
    s->client.clean_session = clean_session ? 1 : 0;
    s->client.keepalive = keepalive;

    client_id_utf8.utf8 = (const uint8_t *)s->client_id;
    client_id_utf8.size = strlen(s->client_id);
    s->client.client_id = client_id_utf8;

    if (s->username[0] != '\0') {
        s->username_utf8.utf8 = (const uint8_t *)s->username;
        s->username_utf8.size = strlen(s->username);
        s->client.user_name = &s->username_utf8;
    }

    if (s->password[0] != '\0') {
        s->password_utf8.utf8 = (const uint8_t *)s->password;
        s->password_utf8.size = strlen(s->password);
        s->client.password = &s->password_utf8;
    }

    s->client.rx_buf = s->rx_buf;
    s->client.rx_buf_size = sizeof(s->rx_buf);
    s->client.tx_buf = s->tx_buf;
    s->client.tx_buf_size = sizeof(s->tx_buf);

    rc = mqttc_prepare_tls(s);
    if (rc < 0) {
        LOG_ERR("sid %d: TLS credential setup failed: %d", sid, rc);
        return rc;
    }

    /* Plan B: allocate recv thread stack from heap */
    s->recv_stack = k_thread_stack_alloc(MQTTC_AT_RECV_STACK, 0);
    if (!s->recv_stack) {
        LOG_ERR("sid %d: failed to alloc recv stack (%u B)", sid, MQTTC_AT_RECV_STACK);
#if MQTTC_AT_TLS_SUPPORTED
        if (s->use_ssl) {
            mqttc_unload_tls_credentials(s);
        }
#endif
        return -ENOMEM;
    }

    if (s->use_ssl) {
#if defined(CONFIG_MQTT_LIB_TLS)
        s->client.transport.type = MQTT_TRANSPORT_SECURE;
        s->client.transport.tls.config.peer_verify = TLS_PEER_VERIFY_REQUIRED;
        s->client.transport.tls.config.sec_tag_list = s->sec_tags;
        s->client.transport.tls.config.sec_tag_count = s->sec_tag_count;
        s->client.transport.tls.config.hostname = s->host;
#else
        k_thread_stack_free(s->recv_stack);
        s->recv_stack = NULL;
        return -ENOTSUP;
#endif
    } else {
        s->client.transport.type = MQTT_TRANSPORT_NON_SECURE;
    }

    rc = mqtt_connect(&s->client);
    if (rc < 0) {
        LOG_ERR("sid %d: mqtt_connect failed: %d", sid, rc);
        k_thread_stack_free(s->recv_stack);
        s->recv_stack = NULL;
#if MQTTC_AT_TLS_SUPPORTED
        if (s->use_ssl) {
            mqttc_unload_tls_credentials(s);
        }
#endif
        return rc;
    }

    /* Start receive thread — CONNACK will update state to CONNECTED */
    s->state = MQTT_STATE_CONNECTED; /* optimistic; evt_cb corrects on failure */
    k_thread_create(&s->recv_thread, s->recv_stack, MQTTC_AT_RECV_STACK, mqttc_recv_thread_fn, s, NULL, NULL,
                    K_PRIO_COOP(7), 0, K_NO_WAIT);
    s->recv_thread_started = true;

    return 0;
}

int mqttc_at_core_subscribe(int sid, const char *topic, uint8_t qos)
{
    mqttc_session_t *s;
    struct mqtt_topic sub_topic;
    struct mqtt_subscription_list sub_list;
    static uint16_t msg_id = 1;
    int rc;

    if (sid < 0 || sid >= MQTTC_AT_SESSIONS) {
        return -EINVAL;
    }

    s = g_sessions[sid];
    if (s == NULL || s->state != MQTT_STATE_CONNECTED) {
        return -ENOTCONN;
    }

    k_mutex_lock(&s->lock, K_FOREVER);

    if (s->sub_count >= MQTTC_AT_MAX_TOPICS) {
        k_mutex_unlock(&s->lock);
        return -ENOMEM;
    }

    /* Check for duplicate */
    for (int i = 0; i < s->sub_count; i++) {
        if (strcmp(s->sub_topics[i], topic) == 0) {
            k_mutex_unlock(&s->lock);
            mqttc_output_urc("\r\n+EVT:MQTT_SUBALREADY:\"%s\"\r\n", topic);
            return 0;
        }
    }

    strlcpy(s->sub_topics[s->sub_count], topic, MQTTC_AT_MAX_TOPIC_LEN);
    s->sub_qos[s->sub_count] = qos;
    s->sub_count++;

    sub_topic.topic.utf8 = (const uint8_t *)topic;
    sub_topic.topic.size = strlen(topic);
    sub_topic.qos = qos;

    sub_list.list = &sub_topic;
    sub_list.list_count = 1;
    sub_list.message_id = msg_id++;

    rc = mqtt_subscribe(&s->client, &sub_list);
    if (rc < 0) {
        LOG_ERR("sid %d: mqtt_subscribe failed: %d", sid, rc);
        s->sub_count--;
        k_mutex_unlock(&s->lock);
        return rc;
    }

    k_mutex_unlock(&s->lock);
    return 0;
}

int mqttc_at_core_publish(int sid, const char *topic, uint8_t qos, const uint8_t *payload, size_t payload_len,
                          uint8_t retain)
{
    mqttc_session_t *s;
    struct mqtt_publish_param pub;
    static uint16_t msg_id = 1;
    int rc;

    if (sid < 0 || sid >= MQTTC_AT_SESSIONS) {
        return -EINVAL;
    }

    s = g_sessions[sid];
    if (s == NULL || s->state != MQTT_STATE_CONNECTED) {
        return -ENOTCONN;
    }

    memset(&pub, 0, sizeof(pub));
    pub.message.topic.topic.utf8 = (const uint8_t *)topic;
    pub.message.topic.topic.size = strlen(topic);
    pub.message.topic.qos = qos;
    pub.message.payload.data = (uint8_t *)payload;
    pub.message.payload.len = payload_len;
    pub.message_id = (qos > 0) ? msg_id++ : 0;
    pub.retain_flag = retain;
    pub.dup_flag = 0;

    k_mutex_lock(&s->lock, K_FOREVER);
    rc = mqtt_publish(&s->client, &pub);
    k_mutex_unlock(&s->lock);
    if (rc < 0) {
        LOG_ERR("sid %d: mqtt_publish failed: %d", sid, rc);
        mqttc_output_urc("\r\n+EVT:MQTT_PUBFAIL:%d,%s,\"%s\",%d\r\n", sid, mqttc_transport_scheme(s), s->host, s->port);
        return rc;
    }

    /* QoS 0: no PUBACK, emit success immediately */
    if (qos == MQTT_QOS_0_AT_MOST_ONCE) {
        LOG_INF("publish QoS0 success, sending PUBSUC");
        mqttc_output_urc("\r\n+EVT:MQTT_PUBSUC:%d,%zu,%s,\"%s\",%d\r\n", sid, payload_len, mqttc_transport_scheme(s),
                         s->host, s->port);
    }

    return 0;
}

int mqttc_at_core_unsubscribe(int sid, const char *topic)
{
    mqttc_session_t *s;
    struct mqtt_topic unsub_topic;
    struct mqtt_subscription_list unsub_list;
    static uint16_t msg_id = 1;
    int rc;
    int found = -1;

    if (sid < 0 || sid >= MQTTC_AT_SESSIONS) {
        return -EINVAL;
    }

    s = g_sessions[sid];
    if (s == NULL || s->state != MQTT_STATE_CONNECTED) {
        return -ENOTCONN;
    }

    k_mutex_lock(&s->lock, K_FOREVER);

    if (s->pending_unsub_topic[0] != '\0') {
        k_mutex_unlock(&s->lock);
        return -EBUSY;
    }

    for (int i = 0; i < s->sub_count; i++) {
        if (strcmp(s->sub_topics[i], topic) == 0) {
            found = i;
            break;
        }
    }

    unsub_topic.topic.utf8 = (const uint8_t *)topic;
    unsub_topic.topic.size = strlen(topic);
    unsub_topic.qos = 0;

    unsub_list.list = &unsub_topic;
    unsub_list.list_count = 1;
    unsub_list.message_id = msg_id++;
    strlcpy(s->pending_unsub_topic, topic, sizeof(s->pending_unsub_topic));

    rc = mqtt_unsubscribe(&s->client, &unsub_list);
    if (rc < 0) {
        s->pending_unsub_topic[0] = '\0';
        k_mutex_unlock(&s->lock);
        LOG_ERR("sid %d: mqtt_unsubscribe failed: %d", sid, rc);
        mqttc_output_urc("\r\n+EVT:MQTT_UNSUBFAIL:%d,\"%s\"\r\n", sid, topic);
        return rc;
    }

    /* Remove from local list */
    if (found >= 0) {
        for (int i = found; i < s->sub_count - 1; i++) {
            memcpy(s->sub_topics[i], s->sub_topics[i + 1], MQTTC_AT_MAX_TOPIC_LEN);
            s->sub_qos[i] = s->sub_qos[i + 1];
        }
        s->sub_count--;
    }

    k_mutex_unlock(&s->lock);
    return 0;
}

int mqttc_at_core_disconnect(int sid)
{
    mqttc_session_t *s;
    struct mqtt_disconnect_param disc_param = {0};

    if (sid < 0 || sid >= MQTTC_AT_SESSIONS) {
        return -EINVAL;
    }

    s = g_sessions[sid];
    if (s == NULL) {
        return -ENODEV;
    }
    if (s->state != MQTT_STATE_CONNECTED) {
        return -ENOTCONN;
    }

    k_mutex_lock(&s->lock, K_FOREVER);
    mqtt_disconnect(&s->client, &disc_param);
    mqtt_abort(&s->client);
    k_mutex_unlock(&s->lock);
    s->state = MQTT_STATE_DISCONNECT;
    s->pending_unsub_topic[0] = '\0';

    /* Wait for recv thread to notice state change and exit */
    if (s->recv_thread_started) {
        k_thread_join(&s->recv_thread, K_MSEC(2000));
        s->recv_thread_started = false;
    }
    /* Plan B: free recv stack */
    if (s->recv_stack) {
        k_thread_stack_free(s->recv_stack);
        s->recv_stack = NULL;
    }
#if MQTTC_AT_TLS_SUPPORTED
    if (s->use_ssl) {
        mqttc_unload_tls_credentials(s);
    }
#endif

    mqttc_output_urc("\r\n+EVT:MQTT_DISCONNECTED:%d\r\n", sid);
    return 0;
}

int mqttc_at_core_destroy(int sid)
{
    if (sid < 0 || sid >= MQTTC_AT_SESSIONS) {
        return -EINVAL;
    }

    if (g_sessions[sid] == NULL) {
        return -ENODEV;
    }

    mqttc_session_teardown(sid, true);
    return 0;
}

int mqttc_at_core_query_conn(int sid, char *buf, size_t buf_len)
{
    mqttc_session_t *s;

    if (sid < 0 || sid >= MQTTC_AT_SESSIONS) {
        return -EINVAL;
    }

    s = g_sessions[sid];
    if (s == NULL) {
        snprintf(buf, buf_len, "+MQTTCONN:%d,%d,TCP,\"\",0\r\n", sid, MQTT_STATE_INIT);
    } else {
        snprintf(buf, buf_len, "+MQTTCONN:%d,%d,%s,\"%s\",%d\r\n", sid, (int)s->state, mqttc_transport_scheme(s),
                 s->host, s->port);
    }
    return 0;
}

int mqttc_at_core_query_sub(int sid, char *buf, size_t buf_len)
{
    mqttc_session_t *s;
    int written = 0;

    if (sid < 0 || sid >= MQTTC_AT_SESSIONS) {
        return -EINVAL;
    }

    s = g_sessions[sid];
    if (s == NULL || s->sub_count == 0) {
        written = snprintf(buf, buf_len, "+MQTTSUB:%d,%d,\"\",0\r\n", sid, (int)s->state);
        return written;
    }

    for (int i = 0; i < s->sub_count && written < (int)buf_len - 1; i++) {
        int ret = snprintf(buf + written, buf_len - written, "+MQTTSUB:%d,%d,\"%s\",%d\r\n", sid, (int)s->state,
                           s->sub_topics[i], s->sub_qos[i]);
        if (ret < 0 || ret >= (int)(buf_len - written)) {
            break;
        }
        written += ret;
    }
    return written;
}

void mqttc_at_core_set_recv_mode(int mode) { g_recv_mode = mode; }

/* -------------------------------------------------------------------------
 * PUBRAW data mode
 * ---------------------------------------------------------------------- */

bool mqttc_at_core_is_in_data_mode(void) { return g_pubraw != NULL; }

void mqttc_at_core_cancel_data_mode(void)
{
    k_free(g_pubraw);
    g_pubraw = NULL;
}

int mqttc_at_core_start_pubraw(int sid, const char *topic, size_t length, uint8_t qos, uint8_t retain)
{
    mqttc_session_t *s;

    if (sid < 0 || sid >= MQTTC_AT_SESSIONS) {
        return -EINVAL;
    }

    s = g_sessions[sid];
    if (s == NULL || s->state != MQTT_STATE_CONNECTED) {
        return -ENOTCONN;
    }

    if (g_pubraw != NULL) {
        return -EBUSY;
    }
    if (length == 0) {
        return -EINVAL;
    }
    if (length > MQTTC_AT_MAX_PAYLOAD_LEN) {
        return -E2BIG;
    }

    g_pubraw = k_malloc(sizeof(mqttc_pubraw_t));
    if (!g_pubraw) {
        return -ENOMEM;
    }
    memset(g_pubraw, 0, sizeof(*g_pubraw));
    g_pubraw->active = true;
    g_pubraw->session_id = sid;
    strlcpy(g_pubraw->topic, topic, sizeof(g_pubraw->topic));
    g_pubraw->qos = qos;
    g_pubraw->retain = retain;
    g_pubraw->expected_len = length;
    g_pubraw->offset = 0;
    return 0;
}

int mqttc_at_core_data_mode_input(const uint8_t *data, size_t len)
{
    size_t remaining;
    size_t to_copy;

    if (g_pubraw == NULL) {
        LOG_INF("data_mode_input: not active, ignoring %zu bytes", len);
        return -EINVAL;
    }

    remaining = g_pubraw->expected_len - g_pubraw->offset;
    to_copy = MIN(len, remaining);
    LOG_INF("data_mode_input: got %zu bytes, copying %zu, offset %zu/%zu", len, to_copy, g_pubraw->offset,
            g_pubraw->expected_len);
    memcpy(g_pubraw->buf + g_pubraw->offset, data, to_copy);
    g_pubraw->offset += to_copy;

    if (g_pubraw->offset >= g_pubraw->expected_len) {
        int sid = g_pubraw->session_id;
        const char *topic = g_pubraw->topic;
        uint8_t qos = g_pubraw->qos;
        uint8_t retain = g_pubraw->retain;
        size_t expected_len = g_pubraw->expected_len;
        uint8_t *buf = g_pubraw->buf;

        int rc = mqttc_at_core_publish(sid, topic, qos, buf, expected_len, retain);
        k_free(g_pubraw);
        g_pubraw = NULL;
        LOG_INF("data_mode_input: publish rc=%d", rc);
        if (rc < 0) {
            return rc;
        }
    }

    return (int)len;
}
