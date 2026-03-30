/*
 * Copyright (c) 2026 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 * HTTP/HTTPS client core — http_client_req() implementation.
 *
 * Uses Zephyr's native HTTP client API (CONFIG_HTTP_CLIENT) for HTTP/1.1
 * framing, header parsing, and response streaming. Socket creation, TLS
 * setup, and DNS+connect use the zsock_*() API directly — compatible with
 * Zephyr 4.x where CONFIG_NET_SOCKETS_POSIX_NAMES is removed.
 *
 * FR201366: AT command for http/https client support over ipv6/4 on zephyr
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/http/parser.h>

#include "../inc/httpc_at_core.h"
#include "../inc/httpc_at_utils.h"
#include "../inc/httpc_at_handler.h"

LOG_MODULE_REGISTER(httpc_at_core, CONFIG_HTTPC_AT_LOG_LEVEL);

/* Module-level receive buffer (allocated once in httpc_at_core_init, reused) */
static uint8_t *g_recv_buf;
static bool     g_core_initialized;

/* -------------------------------------------------------------------------
 * Response callback
 * ---------------------------------------------------------------------- */

/**
 * @brief Context passed to http_response_cb() via http_client_req() user_data.
 */
struct response_ctx {
	httpc_at_output_cb_t      output_cb;
	void                     *output_user_data;
	struct httpc_at_response  *resp;
};

/**
 * @brief http_client_req() response callback.
 *
 * Called by the Zephyr HTTP client for each received data fragment.
 * Streams body data to the AT output callback and fills the response
 * struct on the final call.
 *
 * @param rsp        HTTP response data for this fragment
 * @param final_data HTTP_DATA_FINAL when all data has been received
 * @param user_data  Pointer to struct response_ctx
 * @return 0 to continue, <0 to abort
 */
static int http_response_cb(struct http_response *rsp,
			    enum http_final_call final_data,
			    void *user_data)
{
	struct response_ctx *ctx = (struct response_ctx *)user_data;

	/* Stream body fragment to AT output */
	if (rsp->body_frag_len > 0 && ctx->output_cb) {
		ctx->output_cb((const char *)rsp->body_frag_start,
			       rsp->body_frag_len,
			       ctx->output_user_data);
	}

	/* On final call, fill response struct */
	if (final_data == HTTP_DATA_FINAL && ctx->resp) {
		ctx->resp->status_code    = (int)rsp->http_status_code;
		ctx->resp->content_length = rsp->content_length;
		ctx->resp->received_bytes = rsp->processed;
		ctx->resp->complete       = (bool)rsp->message_complete;
	}

	return 0;
}

/* -------------------------------------------------------------------------
 * Private helpers
 * ---------------------------------------------------------------------- */

/**
 * @brief Parse URL into host, path, port, and is_https flag.
 *
 * @param url        Input URL string (http:// or https://)
 * @param host       Output: hostname buffer
 * @param host_size  Size of host buffer
 * @param path       Output: path buffer (includes leading '/')
 * @param path_size  Size of path buffer
 * @param port       Output: TCP port number
 * @param is_https   Output: true if HTTPS
 * @return 0 on success, negative errno on failure
 */
static int parse_url(const char *url,
		     char *host, size_t host_size,
		     char *path, size_t path_size,
		     int *port, bool *is_https,
		     uint16_t default_http_port,
		     uint16_t default_https_port)
{
	const char *p = url;

	if (strncmp(p, "https://", 8) == 0) {
		*is_https = true;
		p += 8;
		*port = default_https_port;
	} else if (strncmp(p, "http://", 7) == 0) {
		*is_https = false;
		p += 7;
		*port = default_http_port;
	} else {
		LOG_ERR("URL must start with http:// or https://");
		return -EINVAL;
	}

	/* Find end of host (either '/', ':', or end of string) */
	const char *host_start = p;
	const char *host_end   = p;

	while (*host_end && *host_end != '/' && *host_end != ':') {
		host_end++;
	}

	size_t host_len = (size_t)(host_end - host_start);

	if (host_len == 0 || host_len >= host_size) {
		LOG_ERR("Invalid or too-long hostname in URL");
		return -EINVAL;
	}
	memcpy(host, host_start, host_len);
	host[host_len] = '\0';

	/* Optional port override */
	if (*host_end == ':') {
		const char *port_start = host_end + 1;
		char *port_end_ptr;
		long p_val = strtol(port_start, &port_end_ptr, 10);

		if (p_val <= 0 || p_val > 65535) {
			LOG_ERR("Invalid port in URL");
			return -EINVAL;
		}
		*port    = (int)p_val;
		host_end = port_end_ptr;
	}

	/* Path (everything from '/' onwards, or default '/') */
	if (*host_end == '/') {
		size_t path_len = strlen(host_end);

		if (path_len >= path_size) {
			path_len = path_size - 1;
		}
		memcpy(path, host_end, path_len);
		path[path_len] = '\0';
	} else {
		path[0] = '/';
		path[1] = '\0';
	}

	return 0;
}

static sa_family_t detect_literal_host_family(const char *host)
{
	struct in_addr addr4;
	struct in6_addr addr6;

	if (!host || host[0] == '\0') {
		return AF_UNSPEC;
	}

	if (zsock_inet_pton(AF_INET, host, &addr4) == 1) {
		return AF_INET;
	}

	if (zsock_inet_pton(AF_INET6, host, &addr6) == 1) {
		return AF_INET6;
	}

	return AF_UNSPEC;
}

/**
 * @brief Create and configure a TCP or TLS socket.
 *
 * For HTTPS, applies TLS sec_tags based on auth_type and sets peer
 * verification mode. SNI hostname check is bypassed (NULL hostname).
 *
 * @param is_https   True for TLS socket, false for plain TCP
 * @param auth_type  TLS authentication scheme (used only when is_https)
 * @param hostname   Server hostname (for TLS SNI, currently bypassed)
 * @return Socket fd on success, negative errno on failure
 */
static int create_http_socket(bool is_https,
			      sa_family_t ip_family,
			      httpc_at_auth_type_t auth_type,
			      const char *hostname)
{
	int sock;
	int proto = is_https ? IPPROTO_TLS_1_2 : IPPROTO_TCP;
	sa_family_t family = (ip_family == AF_INET6) ? AF_INET6 : AF_INET;

	sock = zsock_socket(family, SOCK_STREAM, proto);
	if (sock < 0) {
		int err = errno;

		LOG_ERR("zsock_socket(family=%d) failed: %d", family, err);
		return -err;
	}

	if (is_https) {
		/*
		 * Build the sec_tag list based on auth_type.
		 * HTTPC_AT_AUTH_NONE:   empty list → no peer verification
		 * HTTPC_AT_AUTH_SERVER: CA cert tag only
		 * HTTPC_AT_AUTH_CLIENT: client cert + key tags
		 * HTTPC_AT_AUTH_MUTUAL: all three tags
		 */
		sec_tag_t sec_tags[3];
		int tag_count = 0;

		switch (auth_type) {
		case HTTPC_AT_AUTH_NONE:
			/* No tags: TLS without certificate verification */
			break;
		case HTTPC_AT_AUTH_SERVER:
			sec_tags[tag_count++] = HTTPC_AT_TLS_TAG_CA_CERT;
			break;
		case HTTPC_AT_AUTH_CLIENT:
			sec_tags[tag_count++] = HTTPC_AT_TLS_TAG_CLIENT_CERT;
			sec_tags[tag_count++] = HTTPC_AT_TLS_TAG_CLIENT_KEY;
			break;
		case HTTPC_AT_AUTH_MUTUAL:
			sec_tags[tag_count++] = HTTPC_AT_TLS_TAG_CA_CERT;
			sec_tags[tag_count++] = HTTPC_AT_TLS_TAG_CLIENT_CERT;
			sec_tags[tag_count++] = HTTPC_AT_TLS_TAG_CLIENT_KEY;
			break;
		default:
			break;
		}

		if (tag_count > 0) {
			socklen_t optlen = (socklen_t)(tag_count * sizeof(sec_tag_t));

			LOG_DBG("TLS_SEC_TAG_LIST: tag_count=%d sizeof(sec_tag_t)=%zu optlen=%u",
				tag_count, sizeof(sec_tag_t), (unsigned int)optlen);
			for (int i = 0; i < tag_count; i++) {
				LOG_DBG("TLS_SEC_TAG_LIST: sec_tags[%d]=%d",
					i, (int)sec_tags[i]);
			}

			int ret = zsock_setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST,
						   sec_tags,
						   (socklen_t)(tag_count * sizeof(sec_tag_t)));
			if (ret < 0) {
				int err = errno;

				LOG_ERR("zsock_setsockopt(TLS_SEC_TAG_LIST) failed: %d", err);
				zsock_close(sock);
				return -err;
			}
		}

		/* Peer verification */
		int verify = (auth_type == HTTPC_AT_AUTH_NONE)
			     ? TLS_PEER_VERIFY_NONE
			     : TLS_PEER_VERIFY_REQUIRED;
		int ret = zsock_setsockopt(sock, SOL_TLS, TLS_PEER_VERIFY,
					   &verify, sizeof(verify));

		if (ret < 0) {
			int err = errno;

			LOG_ERR("zsock_setsockopt(TLS_PEER_VERIFY) failed: %d", err);
			zsock_close(sock);
			return -err;
		}

		/* SNI hostname — bypass hostname check */
		if (hostname && strlen(hostname) > 0) {
			ret = zsock_setsockopt(sock, SOL_TLS, TLS_HOSTNAME,
					       NULL, 0);
			if (ret < 0) {
				int err = errno;

				LOG_ERR("zsock_setsockopt(TLS_HOSTNAME) failed: %d", err);
				zsock_close(sock);
				return -err;
			}
		}
	}

	return sock;
}

/**
 * @brief DNS-resolve hostname and connect socket to host:port.
 *
 * @param sock  Socket fd (already created)
 * @param host  Hostname or IP address string
 * @param port  TCP port number
 * @return 0 on success, negative errno on failure
 */
static int resolve_and_connect(int sock, const char *host, int port, sa_family_t ip_family)
{
	char port_str[8];
	struct zsock_addrinfo hints;
	struct zsock_addrinfo *res = NULL;
	struct sockaddr_in addr4;
	struct sockaddr_in6 addr6;
	int ret;

	memset(&addr4, 0, sizeof(addr4));
	memset(&addr6, 0, sizeof(addr6));

	if (zsock_inet_pton(AF_INET, host, &addr4.sin_addr) == 1) {
		addr4.sin_family = AF_INET;
		addr4.sin_port = htons((uint16_t)port);

		ret = zsock_connect(sock, (struct sockaddr *)&addr4, sizeof(addr4));
		if (ret < 0) {
			int err = errno;

			LOG_ERR("zsock_connect() to IPv4 literal %s:%d failed: %d", host, port, err);
			return -err;
		}

		LOG_DBG("Connected to IPv4 literal %s:%d", host, port);
		return 0;
	}

	if (zsock_inet_pton(AF_INET6, host, &addr6.sin6_addr) == 1) {
		addr6.sin6_family = AF_INET6;
		addr6.sin6_port = htons((uint16_t)port);

		ret = zsock_connect(sock, (struct sockaddr *)&addr6, sizeof(addr6));
		if (ret < 0) {
			int err = errno;

			LOG_ERR("zsock_connect() to IPv6 literal [%s]:%d failed: %d", host, port, err);
			return -err;
		}

		LOG_DBG("Connected to IPv6 literal [%s]:%d", host, port);
		return 0;
	}

	snprintf(port_str, sizeof(port_str), "%d", port);

	memset(&hints, 0, sizeof(hints));
	hints.ai_family   = (ip_family == AF_INET6) ? AF_INET6 : AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	ret = zsock_getaddrinfo(host, port_str, &hints, &res);
	if (ret != 0) {
		LOG_ERR("zsock_getaddrinfo(%s:%s) failed: %d", host, port_str, ret);
		return -EHOSTUNREACH;
	}

	ret = zsock_connect(sock, res->ai_addr, res->ai_addrlen);
	zsock_freeaddrinfo(res);

	if (ret < 0) {
		int err = errno;

		LOG_ERR("zsock_connect() to %s:%d failed: %d", host, port, err);
		return -err;
	}

	LOG_DBG("Connected to %s:%d", host, port);
	return 0;
}

/**
 * @brief Send exactly len bytes to a socket with timeout.
 */
static int send_all_timeout(int sock, const uint8_t *data, size_t len, int32_t timeout_ms)
{
	size_t offset = 0;

	while (offset < len) {
		struct zsock_pollfd pfd = {
			.fd = sock,
			.events = ZSOCK_POLLOUT,
		};
		int ret = zsock_poll(&pfd, 1, timeout_ms);

		if (ret == 0) {
			return -ETIMEDOUT;
		}
		if (ret < 0) {
			return -errno;
		}
		if (pfd.revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL)) {
			return -EIO;
		}

		ssize_t sent = zsock_send(sock, data + offset, len - offset, 0);
		if (sent < 0) {
			int err = errno;

			if (err == EINTR || err == EAGAIN || err == EWOULDBLOCK) {
				continue;
			}
			return -err;
		}
		if (sent == 0) {
			return -ECONNRESET;
		}

		offset += (size_t)sent;
	}

	return 0;
}

/**
 * @brief Send a NULL-terminated string to socket.
 */
static int send_str_timeout(int sock, const char *s, int32_t timeout_ms)
{
	if (!s || s[0] == '\0') {
		return 0;
	}

	return send_all_timeout(sock, (const uint8_t *)s, strlen(s), timeout_ms);
}

struct stream_rsp_parse_ctx {
	httpc_at_output_cb_t output_cb;
	void *output_user_data;
	struct httpc_at_response *resp;
	size_t body_bytes;
	bool message_complete;
};

static int stream_rsp_on_headers_complete(struct http_parser *parser)
{
	struct stream_rsp_parse_ctx *ctx = (struct stream_rsp_parse_ctx *)parser->data;

	if (!ctx || !ctx->resp) {
		return 0;
	}

	ctx->resp->status_code = (int)parser->status_code;

	if ((parser->flags & F_CONTENTLENGTH) && parser->content_length <= SIZE_MAX) {
		ctx->resp->content_length = (size_t)parser->content_length;
	} else {
		ctx->resp->content_length = 0;
	}

	return 0;
}

static int stream_rsp_on_body(struct http_parser *parser, const char *at, size_t length)
{
	struct stream_rsp_parse_ctx *ctx = (struct stream_rsp_parse_ctx *)parser->data;

	if (!ctx) {
		return -EINVAL;
	}

	if (length > 0 && ctx->output_cb) {
		ctx->output_cb(at, length, ctx->output_user_data);
	}

	ctx->body_bytes += length;
	return 0;
}

static int stream_rsp_on_message_complete(struct http_parser *parser)
{
	struct stream_rsp_parse_ctx *ctx = (struct stream_rsp_parse_ctx *)parser->data;

	if (!ctx) {
		return -EINVAL;
	}

	ctx->message_complete = true;

	if (ctx->resp) {
		ctx->resp->complete = true;
		ctx->resp->received_bytes = ctx->body_bytes;
		if (ctx->resp->status_code == 0) {
			ctx->resp->status_code = (int)parser->status_code;
		}
	}

	return 0;
}

/**
 * @brief Receive and parse one HTTP response from socket.
 */
static int recv_http_response(int sock, int32_t timeout_ms,
			      struct httpc_at_response *resp,
			      httpc_at_output_cb_t output_cb,
			      void *output_user_data)
{
	struct http_parser parser;
	struct http_parser_settings settings;
	struct stream_rsp_parse_ctx parse_ctx = {
		.output_cb = output_cb,
		.output_user_data = output_user_data,
		.resp = resp,
		.body_bytes = 0,
		.message_complete = false,
	};

	http_parser_init(&parser, HTTP_RESPONSE);
	http_parser_settings_init(&settings);
	parser.data = &parse_ctx;

	settings.on_headers_complete = stream_rsp_on_headers_complete;
	settings.on_body = stream_rsp_on_body;
	settings.on_message_complete = stream_rsp_on_message_complete;

	while (!parse_ctx.message_complete) {
		struct zsock_pollfd pfd = {
			.fd = sock,
			.events = ZSOCK_POLLIN,
		};
		int poll_ret = zsock_poll(&pfd, 1, timeout_ms);

		if (poll_ret == 0) {
			return -ETIMEDOUT;
		}
		if (poll_ret < 0) {
			return -errno;
		}
		if (pfd.revents & (ZSOCK_POLLERR | ZSOCK_POLLNVAL)) {
			return -EIO;
		}
		if (!(pfd.revents & ZSOCK_POLLIN)) {
			if (pfd.revents & ZSOCK_POLLHUP) {
				(void)http_parser_execute(&parser, &settings, "", 0);
				if (!parse_ctx.message_complete) {
					return -ECONNRESET;
				}
				break;
			}
			continue;
		}

		ssize_t n = zsock_recv(sock, g_recv_buf, HTTPC_AT_RECV_BUF_SIZE, 0);
		if (n < 0) {
			int err = errno;

			if (err == EINTR || err == EAGAIN || err == EWOULDBLOCK) {
				continue;
			}
			return -err;
		}
		if (n == 0) {
			(void)http_parser_execute(&parser, &settings, "", 0);
			if (!parse_ctx.message_complete) {
				return -ECONNRESET;
			}
			break;
		}

		size_t parsed = http_parser_execute(&parser, &settings,
						    (const char *)g_recv_buf,
						    (size_t)n);
		if (parsed != (size_t)n) {
			enum http_errno hpe = HTTP_PARSER_ERRNO(&parser);

			LOG_ERR("HTTP response parse failed: %s (%s)",
				http_errno_name(hpe),
				http_errno_description(hpe));
			return -EBADMSG;
		}
	}

	if (resp) {
		resp->received_bytes = parse_ctx.body_bytes;
		resp->complete = parse_ctx.message_complete;
		if (resp->status_code == 0) {
			resp->status_code = (int)parser.status_code;
		}
	}

	return parse_ctx.message_complete ? 0 : -EIO;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

int httpc_at_core_init(void)
{
	if (g_core_initialized) {
		return 0;
	}

	g_recv_buf = k_malloc(HTTPC_AT_RECV_BUF_SIZE);
	if (!g_recv_buf) {
		LOG_ERR("Failed to allocate recv buffer (%d bytes)",
			HTTPC_AT_RECV_BUF_SIZE);
		return -ENOMEM;
	}

	g_core_initialized = true;
	LOG_INF("httpc_at_core initialized (recv=%d bytes)", HTTPC_AT_RECV_BUF_SIZE);
	return 0;
}

void httpc_at_core_deinit(void)
{
	if (!g_core_initialized) {
		return;
	}

	if (g_recv_buf) {
		k_free(g_recv_buf);
		g_recv_buf = NULL;
	}

	g_core_initialized = false;
	LOG_INF("httpc_at_core deinitialized");
}

int httpc_at_execute(const struct httpc_at_request *req,
		     struct httpc_at_response *resp,
		     httpc_at_output_cb_t output_cb,
		     void *output_user_data)
{
	char host[128];
	char path[512];
	char port_str[8];
	int  port;
	bool is_https;
	int  sock;
	int  ret;
	uint16_t default_http_port;
	uint16_t default_https_port;
	sa_family_t ip_family;

	if (!req || !req->url) {
		return -EINVAL;
	}

	if (!g_core_initialized) {
		LOG_ERR("httpc_at_core not initialized");
		return -ENODEV;
	}

	if (req->http_port == 0 && req->https_port == 0) {
		default_http_port = HTTPC_AT_DEFAULT_HTTP_PORT;
		default_https_port = HTTPC_AT_DEFAULT_HTTPS_PORT;
	} else {
		default_http_port = req->http_port;
		default_https_port = req->https_port;
	}
	ip_family = (req->ip_family == AF_INET6) ? AF_INET6 : AF_INET;

	/* --- Parse URL --- */
	ret = parse_url(req->url,
			host, sizeof(host),
			path, sizeof(path),
			&port, &is_https,
			default_http_port,
			default_https_port);
	if (ret < 0) {
		LOG_ERR("Failed to parse URL: %s", req->url);
		return ret;
	}

	snprintf(port_str, sizeof(port_str), "%d", port);

	LOG_DBG("URL parsed: host=%s path=%s port=%d https=%d",
		host, path, port, (int)is_https);

	sa_family_t connect_family = ip_family;
	sa_family_t literal_family = detect_literal_host_family(host);

	if (literal_family != AF_UNSPEC) {
		connect_family = literal_family;
		LOG_DBG("Host %s is a numeric IP literal, using family=%d and bypassing DNS",
			host, connect_family);
	}

	/*
	 * TLS credential ownership model:
	 * - caller loads credentials before calling httpc_at_execute()
	 * - caller unloads credentials after httpc_at_execute() returns
	 *
	 * Do NOT call httpc_at_ssl_load_certs() here — doing so causes a
	 * double-registration that leaves the TLS credential store in an
	 * inconsistent state, making setsockopt(TLS_SEC_TAG_LIST) fail
	 * with EINVAL.
	 */

	/*
	 * Create socket and connect — with retry for Zephyr 4.3.0 TCP
	 * net_context race condition.
	 *
	 * Root cause:
	 *   net_context_unref() clears NET_CONTEXT_IN_USE immediately when
	 *   zsock_close() is called, before the TCP background thread finishes
	 *   the FIN handshake. The context is then available for reuse by
	 *   net_context_get(). When the TCP background thread later calls
	 *   net_context_put() (after receiving FIN+ACK), it decrements the
	 *   ref count of the newly-reused context, freeing it and clearing
	 *   NET_CONTEXT_IN_USE. A subsequent zsock_connect() on the freed
	 *   context returns ENOENT.
	 *
	 * Workaround:
	 *   If zsock_connect() returns ENOENT, close the socket, wait
	 *   (attempt * 100) ms (0, 100, 200, ... 800 ms) and retry with a
	 *   new socket. Up to 10 attempts total (max wait ~4500 ms cumulative).
	 *   The increasing delay gives the TCP background thread progressively
	 *   more time to finish the FIN handshake and release the context.
	 *
	 * NOTE: SO_RCVTIMEO is intentionally NOT set. In Zephyr 4.3.0,
	 *   setsockopt(SO_RCVTIMEO) on a TCP socket returns ENOPROTOOPT and
	 *   the failure path also calls net_context_put(), triggering the
	 *   same ENOENT symptom. The timeout is passed to http_client_req()
	 *   instead, which handles it internally.
	 */
	sock = -1;
	{
		int _attempt;

		for (_attempt = 0; _attempt < 10; _attempt++) {
			sock = create_http_socket(is_https, connect_family, req->auth_type, host);
			if (sock < 0) {
				LOG_ERR("Failed to create socket: %d", sock);
				return sock;
			}

			ret = resolve_and_connect(sock, host, port, connect_family);
			if (ret == 0) {
				break; /* Connected successfully */
			}

			zsock_close(sock);
			sock = -1;

			if (ret != -ENOENT || _attempt == 9) {
				LOG_ERR("Failed to connect to %s:%d: %d",
					host, port, ret);
				return ret;
			}

			LOG_WRN("zsock_connect() ENOENT (TCP context race), "
				"retry %d/10 after %d ms",
				_attempt + 1, 100 * _attempt);
			k_sleep(K_MSEC(100 * _attempt));
		}
	}

	/* --- Map httpc_at_method_t to Zephyr enum http_method --- */
	enum http_method zephyr_method;

	switch (req->method) {
	case HTTPC_AT_METHOD_HEAD: zephyr_method = HTTP_HEAD; break;
	case HTTPC_AT_METHOD_GET:  zephyr_method = HTTP_GET;  break;
	case HTTPC_AT_METHOD_POST: zephyr_method = HTTP_POST; break;
	case HTTPC_AT_METHOD_PUT:  zephyr_method = HTTP_PUT;  break;
	default:
		LOG_ERR("Unknown HTTP method: %d", (int)req->method);
		zsock_close(sock);
		return -EINVAL;
	}

	/*
	 * Build NULL-terminated optional_headers pointer array.
	 *
	 * http_client_req() expects a NULL-terminated const char ** for
	 * optional_headers. Stack-allocate the pointer array (max 10 + NULL).
	 * The strings themselves are owned by the caller and remain valid
	 * for the duration of this function.
	 */
	const char *opt_headers[HTTPC_AT_MAX_HEADER_FIELDS + 1];
	uint8_t hdr_count = 0;

	for (uint8_t i = 0; i < req->extra_header_count &&
			    i < HTTPC_AT_MAX_HEADER_FIELDS; i++) {
		if (req->extra_headers[i] &&
		    strlen(req->extra_headers[i]) > 0) {
			opt_headers[hdr_count++] = req->extra_headers[i];
		}
	}
	opt_headers[hdr_count] = NULL;

	/* --- Set up response callback context --- */
	struct response_ctx rsp_ctx = {
		.output_cb        = output_cb,
		.output_user_data = output_user_data,
		.resp             = resp,
	};

	/* Initialize response struct */
	if (resp) {
		resp->status_code    = 0;
		resp->content_length = 0;
		resp->received_bytes = 0;
		resp->complete       = false;
	}

	/* --- Build http_request --- */
	struct http_request http_req;

	memset(&http_req, 0, sizeof(http_req));
	http_req.method           = zephyr_method;
	http_req.url              = path;
	http_req.host             = host;
	http_req.port             = port_str;
	http_req.protocol         = "HTTP/1.1";
	http_req.response         = http_response_cb;
	http_req.recv_buf         = g_recv_buf;
	http_req.recv_buf_len     = HTTPC_AT_RECV_BUF_SIZE;
	http_req.optional_headers = (hdr_count > 0) ? opt_headers : NULL;

	/* POST/PUT body */
	if ((req->method == HTTPC_AT_METHOD_POST ||
	     req->method == HTTPC_AT_METHOD_PUT) &&
	    req->body && req->body_len > 0) {
		http_req.payload     = (const char *)req->body;
		http_req.payload_len = req->body_len;
	}

	/* --- Execute HTTP request --- */
	int32_t timeout_ms = (req->timeout_ms > 0)
			     ? req->timeout_ms
			     : HTTPC_AT_DEFAULT_TIMEOUT_MS;

	ret = http_client_req(sock, &http_req, timeout_ms, &rsp_ctx);

	zsock_close(sock);

	if (ret < 0) {
		LOG_ERR("http_client_req() failed: %d", ret);
		return ret;
	}

	if (resp) {
		LOG_DBG("HTTP request complete: status=%d body=%zu bytes",
			resp->status_code, resp->received_bytes);
	}

	return 0;
}

int httpc_at_stream_begin(const struct httpc_at_request *req,
			  struct httpc_at_stream_ctx *ctx)
{
	char host[128];
	char path[512];
	int port;
	bool is_https;
	int sock = -1;
	int ret;
	uint16_t default_http_port;
	uint16_t default_https_port;
	sa_family_t ip_family;
	const char *method_str;
	char line[640];
	int32_t timeout_ms;

	if (!req || !req->url || !ctx) {
		return -EINVAL;
	}

	if (!g_core_initialized) {
		LOG_ERR("httpc_at_core not initialized");
		return -ENODEV;
	}

	if (req->method != HTTPC_AT_METHOD_POST &&
	    req->method != HTTPC_AT_METHOD_PUT) {
		return -EINVAL;
	}

	if (ctx->active) {
		return -EALREADY;
	}

	if (req->http_port == 0 && req->https_port == 0) {
		default_http_port = HTTPC_AT_DEFAULT_HTTP_PORT;
		default_https_port = HTTPC_AT_DEFAULT_HTTPS_PORT;
	} else {
		default_http_port = req->http_port;
		default_https_port = req->https_port;
	}
	ip_family = (req->ip_family == AF_INET6) ? AF_INET6 : AF_INET;

	ret = parse_url(req->url,
			host, sizeof(host),
			path, sizeof(path),
			&port, &is_https,
			default_http_port,
			default_https_port);
	if (ret < 0) {
		return ret;
	}

	sa_family_t connect_family = ip_family;
	sa_family_t literal_family = detect_literal_host_family(host);

	if (literal_family != AF_UNSPEC) {
		connect_family = literal_family;
		LOG_DBG("Host %s is a numeric IP literal, using family=%d and bypassing DNS",
			host, connect_family);
	}

	timeout_ms = (req->timeout_ms > 0) ? req->timeout_ms : HTTPC_AT_DEFAULT_TIMEOUT_MS;

	sock = create_http_socket(is_https, connect_family, req->auth_type, host);
	if (sock < 0) {
		return sock;
	}

	ret = resolve_and_connect(sock, host, port, connect_family);
	if (ret < 0) {
		zsock_close(sock);
		return ret;
	}

	method_str = (req->method == HTTPC_AT_METHOD_POST) ? "POST" : "PUT";

	/* Request line */
	snprintf(line, sizeof(line), "%s %s HTTP/1.1\r\n", method_str, path);
	ret = send_str_timeout(sock, line, timeout_ms);
	if (ret < 0) {
		zsock_close(sock);
		return ret;
	}

	/* Mandatory headers */
	snprintf(line, sizeof(line), "Host: %s\r\n", host);
	ret = send_str_timeout(sock, line, timeout_ms);
	if (ret < 0) {
		zsock_close(sock);
		return ret;
	}

	ret = send_str_timeout(sock, "Connection: close\r\n", timeout_ms);
	if (ret < 0) {
		zsock_close(sock);
		return ret;
	}

	snprintf(line, sizeof(line), "Content-Length: %zu\r\n", req->body_len);
	ret = send_str_timeout(sock, line, timeout_ms);
	if (ret < 0) {
		zsock_close(sock);
		return ret;
	}

	/* Optional headers */
	for (uint8_t i = 0; i < req->extra_header_count; i++) {
		const char *hdr = req->extra_headers ? req->extra_headers[i] : NULL;
		size_t hdr_len;

		if (!hdr || hdr[0] == '\0') {
			continue;
		}

		hdr_len = strlen(hdr);
		ret = send_all_timeout(sock, (const uint8_t *)hdr, hdr_len, timeout_ms);
		if (ret < 0) {
			zsock_close(sock);
			return ret;
		}

		if (hdr_len < 2 ||
		    hdr[hdr_len - 2] != '\r' ||
		    hdr[hdr_len - 1] != '\n') {
			ret = send_str_timeout(sock, "\r\n", timeout_ms);
			if (ret < 0) {
				zsock_close(sock);
				return ret;
			}
		}
	}

	/* End of headers */
	ret = send_str_timeout(sock, "\r\n", timeout_ms);
	if (ret < 0) {
		zsock_close(sock);
		return ret;
	}

	ctx->sock = sock;
	ctx->active = true;
	ctx->method = req->method;
	ctx->content_length = req->body_len;
	ctx->sent_bytes = 0;
	ctx->timeout_ms = timeout_ms;

	/* Optional immediate payload send */
	if (req->body && req->body_len > 0) {
		ret = httpc_at_stream_send(ctx, req->body, req->body_len);
		if (ret < 0) {
			httpc_at_stream_abort(ctx);
			return ret;
		}
	}

	return 0;
}

int httpc_at_stream_send(struct httpc_at_stream_ctx *ctx,
			 const uint8_t *data,
			 size_t len)
{
	int ret;

	if (!ctx || !ctx->active || ctx->sock < 0) {
		return -EINVAL;
	}

	if (len == 0) {
		return 0;
	}

	if (!data) {
		return -EINVAL;
	}

	if (ctx->sent_bytes + len > ctx->content_length) {
		return -EMSGSIZE;
	}

	ret = send_all_timeout(ctx->sock, data, len, ctx->timeout_ms);
	if (ret < 0) {
		return ret;
	}

	ctx->sent_bytes += len;
	return 0;
}

int httpc_at_stream_finish(struct httpc_at_stream_ctx *ctx,
			   struct httpc_at_response *resp,
			   httpc_at_output_cb_t output_cb,
			   void *output_user_data)
{
	int sock;
	int ret;

	if (!ctx || !ctx->active || ctx->sock < 0) {
		return -EINVAL;
	}

	if (ctx->sent_bytes != ctx->content_length) {
		httpc_at_stream_abort(ctx);
		return -EMSGSIZE;
	}

	if (resp) {
		resp->status_code = 0;
		resp->content_length = 0;
		resp->received_bytes = 0;
		resp->complete = false;
	}

	sock = ctx->sock;
	ret = recv_http_response(sock, ctx->timeout_ms, resp, output_cb, output_user_data);

	zsock_close(sock);
	ctx->sock = -1;
	ctx->active = false;

	return ret;
}

void httpc_at_stream_abort(struct httpc_at_stream_ctx *ctx)
{
	if (!ctx) {
		return;
	}

	if (ctx->active && ctx->sock >= 0) {
		zsock_close(ctx->sock);
	}

	ctx->sock = -1;
	ctx->active = false;
	ctx->content_length = 0;
	ctx->sent_bytes = 0;
	ctx->timeout_ms = 0;
	ctx->method = HTTPC_AT_METHOD_POST;
}
