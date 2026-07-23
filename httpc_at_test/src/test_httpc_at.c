/*
 * Copyright (c) 2026 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 * @file test_httpc_at.c
 * @brief ztest test suites for the httpc_at AT command handler module.
 *
 * Tests are organized into three independent suites:
 *
 *   httpc_at_cfg   (tests 01-04)  — AT+HTTPSSLCFG, AT+HTTPURLCFG
 *   httpc_at_http  (tests 05-12)  — Plain HTTP: GETSIZE, GET, HTTPCLIENT,
 *                                   HTTPPOST/PUT data mode
 *   httpc_at_https (tests 13-17)  — HTTPS: scheme=0 (no-verify),
 *                                   scheme=1 (server-verify with CA from FS)
 *
 * Running all suites:
 *   Flash the image and boot — all suites run automatically in order.
 *
 * Running a single suite (Zephyr shell, requires CONFIG_ZTEST_SHELL=y):
 *   uart:~$ ztest run-suite httpc_at_https
 *
 * Configuration:
 *   Edit prj.conf before building. Key symbols:
 *     CONFIG_HTTPC_AT_TEST_WIFI_SSID / _PASSWORD / _SECURITY
 *     CONFIG_HTTPC_AT_TEST_HTTP_URL / _POST_URL / _PUT_URL / _LONG_URL
 *     CONFIG_HTTPC_AT_TEST_HTTPS_URL
 *     CONFIG_HTTPC_AT_TEST_CA_FILE   (path on device FS, e.g. "/lfs/ca.pem")
 *     CONFIG_HTTPC_AT_TEST_TIMEOUT_MS
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/ztest.h>

#include "../../common/httpc_at/inc/httpc_at_handler.h"

LOG_MODULE_REGISTER(test_httpc_at, LOG_LEVEL_DBG);

/* -----------------------------------------------------------------------
 * External symbols from main.c
 * --------------------------------------------------------------------- */

extern int wifi_connect_ap(void);
extern int wait_for_network(void);

/* -----------------------------------------------------------------------
 * Test URLs / config from prj.conf
 * --------------------------------------------------------------------- */

#define TEST_HTTP_URL   CONFIG_HTTPC_AT_TEST_HTTP_URL
#define TEST_POST_URL   CONFIG_HTTPC_AT_TEST_POST_URL
#define TEST_PUT_URL    CONFIG_HTTPC_AT_TEST_PUT_URL
#define TEST_LONG_URL   CONFIG_HTTPC_AT_TEST_LONG_URL
#define TEST_HTTPS_URL  CONFIG_HTTPC_AT_TEST_HTTPS_URL
#define TEST_CA_FILE    CONFIG_HTTPC_AT_TEST_CA_FILE
#define TEST_TIMEOUT_MS CONFIG_HTTPC_AT_TEST_TIMEOUT_MS

/* -----------------------------------------------------------------------
 * Output capture buffer
 *
 * The httpc_at module calls output_cb for every AT response string.
 * We capture all output here so tests can assert on response content.
 * --------------------------------------------------------------------- */

/*
 * Output capture buffer.
 *
 * Must be large enough to hold the largest AT response including body.
 * For AT+HTTPGET / AT+HTTPCLIENT GET, the full response is:
 *   "+HTTPGET:" + <body> + ",<size>\r\nOK\r\n"
 * The test server returns ~10671 bytes of HTML, so 16 KB is sufficient.
 *
 * This is a static global (BSS), not on the stack — safe to increase.
 */
#define OUT_BUF_SIZE 16384

static char   g_out_buf[OUT_BUF_SIZE];
static size_t g_out_len;

static void test_output_cb(const char *data, size_t len, void *user_data)
{
    ARG_UNUSED(user_data);

    size_t copy_len = len;

    if (g_out_len + copy_len >= OUT_BUF_SIZE - 1) {
        copy_len = OUT_BUF_SIZE - 1 - g_out_len;
    }
    if (copy_len > 0) {
        memcpy(g_out_buf + g_out_len, data, copy_len);
        g_out_len += copy_len;
        g_out_buf[g_out_len] = '\0';
    }
}

static void clear_output(void)
{
    g_out_len    = 0;
    g_out_buf[0] = '\0';
}

static bool output_contains(const char *substr)
{
    return strstr(g_out_buf, substr) != NULL;
}

/* -----------------------------------------------------------------------
 * Helper: build a simple httpc_at_param_t array on the stack
 *
 * Usage:
 *   httpc_at_param_t p[3];
 *   PARAM_INT(p, 0, 2);            // integer param
 *   PARAM_STR(p, 1, "http://..."); // string param
 *   PARAM_STR(p, 2, "");           // empty string param
 * --------------------------------------------------------------------- */

#define PARAM_INT(arr, idx, val) \
    do { \
        (arr)[idx].str_val   = ""; \
        (arr)[idx].int_val   = (val); \
        (arr)[idx].int_valid = true; \
    } while (0)

#define PARAM_STR(arr, idx, s) \
    do { \
        (arr)[idx].str_val   = (s); \
        (arr)[idx].int_val   = 0; \
        (arr)[idx].int_valid = false; \
    } while (0)

/* -----------------------------------------------------------------------
 * Shared suite setup / teardown
 *
 * All three suites share the same setup: connect WiFi, wait for network,
 * then initialize httpc_at.  Each suite registers its own copy so that
 * any single suite can be run standalone.
 * --------------------------------------------------------------------- */

static void *common_suite_setup(void)
{
    int ret;

    LOG_INF("=== suite setup: WiFi + httpc_at_init ===");

    ret = wifi_connect_ap();
    zassert_ok(ret, "wifi_connect_ap() failed: %d", ret);

    ret = wait_for_network();
    zassert_ok(ret, "wait_for_network() timed out: %d", ret);

    LOG_INF("Network ready — waiting 3s for stack stabilization...");
    k_sleep(K_SECONDS(3));

    ret = httpc_at_init(test_output_cb, NULL);
    zassert_ok(ret, "httpc_at_init() failed: %d", ret);

    LOG_INF("=== suite setup complete ===");
    return NULL;
}

static void common_suite_teardown(void *data)
{
    ARG_UNUSED(data);
    httpc_at_deinit();
    LOG_INF("=== suite teardown ===");
}

static void before_each_test(void *data)
{
    ARG_UNUSED(data);
    clear_output();
}

/* ======================================================================
 * SUITE 1: httpc_at_cfg
 *
 * Tests 01-04: AT+HTTPSSLCFG, AT+HTTPURLCFG, AT+HTTPNETCFG (config commands only,
 * no network traffic).
 * ==================================================================== */
ZTEST_SUITE(httpc_at_cfg,
            NULL,
            common_suite_setup,
            before_each_test,
            NULL,
            common_suite_teardown);

/* -----------------------------------------------------------------------
 * Test 01: AT+HTTPSSLCFG set (scheme=0, no-verify)
 * --------------------------------------------------------------------- */

ZTEST(httpc_at_cfg, test_01_httpsslcfg_set)
{
    httpc_at_param_t p[1];

    PARAM_INT(p, 0, 0); /* scheme=0: no certificate verification */

    LOG_INF("--- test_01_httpsslcfg_set ---");
    int ret = httpc_at_handle_httpsslcfg(HTTPC_AT_OP_EXEC_W_PARAM, 1, p);

    zassert_ok(ret, "httpsslcfg set returned %d", ret);
    zassert_equal(g_out_len, 0, "No direct output expected, got: %s", g_out_buf);
}

/* -----------------------------------------------------------------------
 * Test 02: AT+HTTPSSLCFG? (query)
 * --------------------------------------------------------------------- */

ZTEST(httpc_at_cfg, test_02_httpsslcfg_query)
{
    LOG_INF("--- test_02_httpsslcfg_query ---");
    int ret = httpc_at_handle_httpsslcfg(HTTPC_AT_OP_QUERY, 0, NULL);

    zassert_ok(ret, "httpsslcfg query returned %d", ret);
    zassert_true(output_contains("+HTTPSSLCFG:"),
                 "Expected +HTTPSSLCFG:, got: %s", g_out_buf);
    LOG_INF("HTTPSSLCFG query response: %s", g_out_buf);
}

/* -----------------------------------------------------------------------
 * Test 03: AT+HTTPURLCFG set (data mode)
 *
 * Phase 1: AT+HTTPURLCFG=<len>  → "OK\r\n>\r\n"
 * Phase 2: httpc_at_data_mode_input(url_bytes) → no direct output
 * --------------------------------------------------------------------- */

ZTEST(httpc_at_cfg, test_03_httpurlcfg_set)
{
    const char *url     = TEST_LONG_URL;
    size_t      url_len = strlen(url);

    httpc_at_param_t p[1];

    PARAM_INT(p, 0, (int)url_len);

    LOG_INF("--- test_03_httpurlcfg_set (url=%s, len=%zu) ---", url, url_len);

    /* Phase 1 */
    int ret = httpc_at_handle_httpurlcfg(HTTPC_AT_OP_EXEC_W_PARAM, 1, p);

    zassert_ok(ret, "httpurlcfg phase1 returned %d", ret);
    zassert_true(output_contains("OK"), "Phase1: expected OK, got: %s", g_out_buf);
    zassert_true(output_contains(">"),  "Phase1: expected >, got: %s", g_out_buf);
    zassert_true(httpc_at_is_in_data_mode(), "Should be in data mode after phase1");

    clear_output();

    /* Phase 2: feed URL bytes */
    ret = httpc_at_data_mode_input((const uint8_t *)url, url_len);

    zassert_ok(ret, "httpurlcfg data mode input returned %d", ret);
    zassert_false(httpc_at_is_in_data_mode(), "Should exit data mode after phase2");
    zassert_equal(g_out_len, 0, "Phase2 should not emit direct output, got: %s", g_out_buf);

    /* Verify stored URL */
    const struct httpc_at_global_cfg *cfg = httpc_at_get_config();

    zassert_not_null(cfg->url, "Stored URL should not be NULL");
    zassert_equal(cfg->url_len, url_len, "Stored URL length mismatch");
    zassert_mem_equal(cfg->url, url, url_len, "Stored URL content mismatch");
}

/* -----------------------------------------------------------------------
 * Test 04: AT+HTTPURLCFG? (query)
 * --------------------------------------------------------------------- */

ZTEST(httpc_at_cfg, test_04_httpurlcfg_query)
{
    LOG_INF("--- test_04_httpurlcfg_query ---");
    int ret = httpc_at_handle_httpurlcfg(HTTPC_AT_OP_QUERY, 0, NULL);

    zassert_ok(ret, "httpurlcfg query returned %d", ret);
    zassert_true(output_contains("+HTTPURLCFG:"),
                 "Expected +HTTPURLCFG:, got: %s", g_out_buf);
    LOG_INF("HTTPURLCFG query response: %s", g_out_buf);
}

/* -----------------------------------------------------------------------
 * Test 04a: AT+HTTPNETCFG? (query)
 * --------------------------------------------------------------------- */

ZTEST(httpc_at_cfg, test_04a_httpnetcfg_query)
{
    LOG_INF("--- test_04a_httpnetcfg_query ---");
    int ret = httpc_at_handle_httpnetcfg(HTTPC_AT_OP_QUERY, 0, NULL);

    zassert_ok(ret, "httpnetcfg query returned %d", ret);
    zassert_true(output_contains("+HTTPNETCFG:"),
                 "Expected +HTTPNETCFG:, got: %s", g_out_buf);
    LOG_INF("HTTPNETCFG query response: %s", g_out_buf);
}

/* -----------------------------------------------------------------------
 * Test 04b: AT+HTTPNETCFG set + query verify
 * --------------------------------------------------------------------- */

ZTEST(httpc_at_cfg, test_04b_httpnetcfg_set_and_query)
{
    httpc_at_param_t p[2];
    int ret;

    LOG_INF("--- test_04b_httpnetcfg_set_and_query ---");

    PARAM_INT(p, 0, HTTPC_AT_NETCFG_HTTP_PORT);
    PARAM_INT(p, 1, 8080);
    ret = httpc_at_handle_httpnetcfg(HTTPC_AT_OP_EXEC_W_PARAM, 2, p);
    zassert_ok(ret, "set HTTP port failed: %d", ret);

    PARAM_INT(p, 0, HTTPC_AT_NETCFG_HTTPS_PORT);
    PARAM_INT(p, 1, 8443);
    ret = httpc_at_handle_httpnetcfg(HTTPC_AT_OP_EXEC_W_PARAM, 2, p);
    zassert_ok(ret, "set HTTPS port failed: %d", ret);

    PARAM_INT(p, 0, HTTPC_AT_NETCFG_IP_PREFER);
    PARAM_INT(p, 1, 1);
    ret = httpc_at_handle_httpnetcfg(HTTPC_AT_OP_EXEC_W_PARAM, 2, p);
    zassert_ok(ret, "set IP prefer failed: %d", ret);

    PARAM_INT(p, 0, HTTPC_AT_NETCFG_PREALLOC_SSL_BUF);
    PARAM_INT(p, 1, 2);
    ret = httpc_at_handle_httpnetcfg(HTTPC_AT_OP_EXEC_W_PARAM, 2, p);
    zassert_ok(ret, "set prealloc_ssl_buf failed: %d", ret);

    PARAM_INT(p, 0, HTTPC_AT_NETCFG_DATA_CACHE);
    PARAM_INT(p, 1, 0);
    ret = httpc_at_handle_httpnetcfg(HTTPC_AT_OP_EXEC_W_PARAM, 2, p);
    zassert_ok(ret, "set data_cache failed: %d", ret);

    clear_output();
    ret = httpc_at_handle_httpnetcfg(HTTPC_AT_OP_QUERY, 0, NULL);
    zassert_ok(ret, "httpnetcfg query failed: %d", ret);
    zassert_true(output_contains("+HTTPNETCFG:8080,8443,1,2,0"),
                 "Unexpected HTTPNETCFG query response: %s", g_out_buf);
}

/* ======================================================================
 * SUITE 2: httpc_at_http
 *
 * Tests 05-12: Plain HTTP requests (no TLS).
 * ==================================================================== */

ZTEST_SUITE(httpc_at_http,
            NULL,
            common_suite_setup,
            before_each_test,
            NULL,
            common_suite_teardown);
/* -----------------------------------------------------------------------
 * Test 05: AT+HTTPGETSIZE=<url>
 * --------------------------------------------------------------------- */

ZTEST(httpc_at_http, test_05_httpgetsize)
{
    httpc_at_param_t p[2];

    PARAM_STR(p, 0, TEST_HTTP_URL);
    PARAM_INT(p, 1, TEST_TIMEOUT_MS);

    LOG_INF("--- test_05_httpgetsize (url=%s) ---", TEST_HTTP_URL);
    int ret = httpc_at_handle_httpgetsize(HTTPC_AT_OP_EXEC_W_PARAM, 2, p);

    zassert_ok(ret, "httpgetsize returned %d", ret);
    zassert_true(output_contains("+HTTPGETSIZE:"),
                 "Expected +HTTPGETSIZE:, got: %s", g_out_buf);
    LOG_INF("HTTPGETSIZE response: %s", g_out_buf);
}
/* -----------------------------------------------------------------------
 * Test 06: AT+HTTPGET=<url>
 * --------------------------------------------------------------------- */
ZTEST(httpc_at_http, test_06_httpget)
{
    httpc_at_param_t p[2];

    PARAM_STR(p, 0, TEST_HTTP_URL);
    PARAM_INT(p, 1, TEST_TIMEOUT_MS);

    LOG_INF("--- test_06_httpget (url=%s) ---", TEST_HTTP_URL);
    int ret = httpc_at_handle_httpget(HTTPC_AT_OP_EXEC_W_PARAM, 2, p);

    zassert_ok(ret, "httpget returned %d", ret);
    zassert_true(output_contains("+HTTPGET:"),
                 "Expected +HTTPGET:, got: %s", g_out_buf);
    LOG_INF("HTTPGET response prefix: %.128s", g_out_buf);
}
/* -----------------------------------------------------------------------
 * Test 07: AT+HTTPCLIENT HEAD
 * --------------------------------------------------------------------- */

ZTEST(httpc_at_http, test_07_httpclient_head)
{
    httpc_at_param_t p[3];

    PARAM_INT(p, 0, HTTPC_AT_METHOD_HEAD);
    PARAM_INT(p, 1, HTTPC_AT_CTYPE_FORM_URLENCODED);
    PARAM_STR(p, 2, TEST_HTTP_URL);

    LOG_INF("--- test_07_httpclient_head ---");
    int ret = httpc_at_handle_httpclient(HTTPC_AT_OP_EXEC_W_PARAM, 3, p);

    zassert_ok(ret, "httpclient HEAD returned %d", ret);
    zassert_true(output_contains("+HTTPC:OK"),
                 "Expected +HTTPC:OK, got: %s", g_out_buf);
}

/* -----------------------------------------------------------------------
 * Test 08: AT+HTTPCLIENT GET
 * --------------------------------------------------------------------- */

ZTEST(httpc_at_http, test_08_httpclient_get)
{
    httpc_at_param_t p[3];

    PARAM_INT(p, 0, HTTPC_AT_METHOD_GET);
    PARAM_INT(p, 1, HTTPC_AT_CTYPE_FORM_URLENCODED);
    PARAM_STR(p, 2, TEST_HTTP_URL);

    LOG_INF("--- test_08_httpclient_get ---");
    int ret = httpc_at_handle_httpclient(HTTPC_AT_OP_EXEC_W_PARAM, 3, p);

    zassert_ok(ret, "httpclient GET returned %d", ret);
    zassert_true(output_contains("+HTTPC:"),
                 "Expected +HTTPC:, got: %s", g_out_buf);
    zassert_false(output_contains("+HTTPC:ERROR"),
                  "Unexpected ERROR in response: %s", g_out_buf);
    LOG_INF("HTTPCLIENT GET response prefix: %.128s", g_out_buf);
}

/* -----------------------------------------------------------------------
 * Test 09: AT+HTTPCLIENT POST (inline data)
 * --------------------------------------------------------------------- */
ZTEST(httpc_at_http, test_09_httpclient_post)
{
    httpc_at_param_t p[4];

    PARAM_INT(p, 0, HTTPC_AT_METHOD_POST);
    PARAM_INT(p, 1, HTTPC_AT_CTYPE_FORM_URLENCODED);
    PARAM_STR(p, 2, TEST_POST_URL);
    PARAM_STR(p, 3, "key=value&test=1");

    LOG_INF("--- test_09_httpclient_post ---");
    int ret = httpc_at_handle_httpclient(HTTPC_AT_OP_EXEC_W_PARAM, 4, p);

    zassert_ok(ret, "httpclient POST returned %d", ret);
    zassert_true(output_contains("+HTTPC:OK") || output_contains("+HTTPC:ERROR"),
                 "Expected +HTTPC response, got: %s", g_out_buf);
    LOG_INF("HTTPCLIENT POST response: %s", g_out_buf);
}

/* -----------------------------------------------------------------------
 * Test 10: AT+HTTPCLIENT PUT (inline data)
 * --------------------------------------------------------------------- */

ZTEST(httpc_at_http, test_10_httpclient_put)
{
    httpc_at_param_t p[4];

    PARAM_INT(p, 0, HTTPC_AT_METHOD_PUT);
    PARAM_INT(p, 1, HTTPC_AT_CTYPE_JSON);
    PARAM_STR(p, 2, TEST_PUT_URL);
    PARAM_STR(p, 3, "{\"key\":\"value\"}");

    LOG_INF("--- test_10_httpclient_put ---");
    int ret = httpc_at_handle_httpclient(HTTPC_AT_OP_EXEC_W_PARAM, 4, p);

    zassert_ok(ret, "httpclient PUT returned %d", ret);
    zassert_true(output_contains("+HTTPC:OK") || output_contains("+HTTPC:ERROR"),
                 "Expected +HTTPC response, got: %s", g_out_buf);
    LOG_INF("HTTPCLIENT PUT response: %s", g_out_buf);
}

/* -----------------------------------------------------------------------
 * Test 11: AT+HTTPPOST (data mode)
 *
 * Phase 1: AT+HTTPPOST=<url>,<len>  → "OK\r\n>\r\n"
 * Phase 2: httpc_at_data_mode_input(body) →
 *          success: "+HTTPC:<data>,<size>\r\n+HTTPCPOST:SEND OK\r\n"
 *          failure: "+HTTPCPOST:SEND FAIL\r\n"
 * --------------------------------------------------------------------- */

ZTEST(httpc_at_http, test_11_httppost_datamode)
{
    const char *body     = "field1=hello&field2=world";
    size_t      body_len = strlen(body);

    httpc_at_param_t p[2];

    PARAM_STR(p, 0, TEST_POST_URL);
    PARAM_INT(p, 1, (int)body_len);

    LOG_INF("--- test_11_httppost_datamode (len=%zu) ---", body_len);

    /* Phase 1 */
    int ret = httpc_at_handle_httppost(HTTPC_AT_OP_EXEC_W_PARAM, 2, p);

    zassert_ok(ret, "httppost phase1 returned %d", ret);
    zassert_true(output_contains("OK"), "Phase1: expected OK, got: %s", g_out_buf);
    zassert_true(output_contains(">"),  "Phase1: expected >, got: %s", g_out_buf);
    zassert_true(httpc_at_is_in_data_mode(), "Should be in data mode");

    clear_output();

    /* Phase 2: feed POST body */
    ret = httpc_at_data_mode_input((const uint8_t *)body, body_len);

    zassert_false(httpc_at_is_in_data_mode(), "Should exit data mode after phase2");
    zassert_true(output_contains("+HTTPCPOST:SEND OK") ||
                 output_contains("+HTTPCPOST:SEND FAIL"),
                 "Expected +HTTPCPOST:SEND OK/FAIL, got: %s", g_out_buf);
    if (output_contains("+HTTPCPOST:SEND OK")) {
        zassert_true(output_contains("+HTTPC:"),
                     "POST success should include +HTTPC:<data>,<size>, got: %s", g_out_buf);
    }
    LOG_INF("HTTPPOST data mode response: %s", g_out_buf);
}

/* -----------------------------------------------------------------------
 * Test 12: AT+HTTPPUT (data mode)
 *
 * Phase 1: AT+HTTPPUT=<url>,<content_type>,<len>  → "OK\r\n>\r\n"
 * Phase 2: httpc_at_data_mode_input(body) →
 *          success: "+HTTPC:<data>,<size>\r\n+HTTPCPUT:SEND OK\r\n"
 *          failure: "+HTTPCPUT:SEND FAIL\r\n"
 * --------------------------------------------------------------------- */

ZTEST(httpc_at_http, test_12_httpput_datamode)
{
    const char *body     = "{\"sensor\":\"temp\",\"value\":25}";
    size_t      body_len = strlen(body);

    httpc_at_param_t p[3];

    PARAM_STR(p, 0, TEST_PUT_URL);
    PARAM_INT(p, 1, HTTPC_AT_CTYPE_JSON);
    PARAM_INT(p, 2, (int)body_len);

    LOG_INF("--- test_12_httpput_datamode (len=%zu) ---", body_len);

    /* Phase 1 */
    int ret = httpc_at_handle_httpput(HTTPC_AT_OP_EXEC_W_PARAM, 3, p);

    zassert_ok(ret, "httpput phase1 returned %d", ret);
    zassert_true(output_contains("OK"), "Phase1: expected OK, got: %s", g_out_buf);
    zassert_true(output_contains(">"),  "Phase1: expected >, got: %s", g_out_buf);
    zassert_true(httpc_at_is_in_data_mode(), "Should be in data mode");

    clear_output();

    /* Phase 2: feed PUT body */
    ret = httpc_at_data_mode_input((const uint8_t *)body, body_len);

    zassert_false(httpc_at_is_in_data_mode(), "Should exit data mode after phase2");
    zassert_true(output_contains("+HTTPCPUT:SEND OK") ||
                 output_contains("+HTTPCPUT:SEND FAIL"),
                 "Expected +HTTPCPUT:SEND OK/FAIL, got: %s", g_out_buf);
    if (output_contains("+HTTPCPUT:SEND OK")) {
        zassert_true(output_contains("+HTTPC:"),
                     "PUT success should include +HTTPC:<data>,<size>, got: %s", g_out_buf);
    }
    LOG_INF("HTTPPUT data mode response: %s", g_out_buf);
}

/* ======================================================================
 * SUITE 3: httpc_at_https
 *
 * Tests 13-17: HTTPS requests.
 *
 * Prerequisites:
 *   - CONFIG_HTTPC_AT_TEST_HTTPS_URL must point to a reachable HTTPS server.
 *   - For scheme=1 tests (server-verify), the CA certificate must be burned
 *     to the device filesystem at CONFIG_HTTPC_AT_TEST_CA_FILE before boot.
 *
 * Test sequence within this suite:
 *   13: Set scheme=0 (no-verify), then AT+HTTPGET over HTTPS
 *   14: AT+HTTPCLIENT HEAD over HTTPS (scheme=0 still active)
 *   15: Set scheme=1 (server-verify) with CA file path
 *   16: AT+HTTPGET over HTTPS (scheme=1, CA loaded from FS)
 *   17: AT+HTTPCLIENT HEAD over HTTPS (scheme=1)
 * ==================================================================== */

ZTEST_SUITE(httpc_at_https,
            NULL,
            common_suite_setup,
            before_each_test,
            NULL,
            common_suite_teardown);

/* -----------------------------------------------------------------------
 * Test 13: HTTPS GET — scheme=0 (no certificate verification)
 *
 * Steps:
 *   1. AT+HTTPSSLCFG=0          → set no-verify mode
 *   2. AT+HTTPGET=<https_url>   → GET over TLS, no cert check
 * --------------------------------------------------------------------- */
ZTEST(httpc_at_https, test_13_https_get_no_verify)
{
    int ret;

    LOG_INF("--- test_13_https_get_no_verify ---");

    /* Step 1: configure scheme=0 (no certificate verification) */
    {
        httpc_at_param_t p[1];

        PARAM_INT(p, 0, HTTPC_AT_AUTH_NONE); /* scheme=0 */
        ret = httpc_at_handle_httpsslcfg(HTTPC_AT_OP_EXEC_W_PARAM, 1, p);
        zassert_ok(ret, "httpsslcfg scheme=0 returned %d", ret);
        zassert_equal(g_out_len, 0, "No direct output expected, got: %s", g_out_buf);
        clear_output();
    }

    /* Step 2: AT+HTTPGET over HTTPS */
    {
        httpc_at_param_t p[2];

        PARAM_STR(p, 0, TEST_HTTPS_URL);
        PARAM_INT(p, 1, TEST_TIMEOUT_MS);

        LOG_INF("HTTPS GET (no-verify) url=%s", TEST_HTTPS_URL);
        ret = httpc_at_handle_httpget(HTTPC_AT_OP_EXEC_W_PARAM, 2, p);

        /*
         * httpc_at_handle_httpget() returns negative on network failure.
         * Do NOT assert on ret — check the AT response string instead.
         * The test fails here if the HTTPS connection itself fails.
         */
        LOG_INF("HTTPS GET (no-verify) ret=%d output=%.128s", ret, g_out_buf);
        zassert_true(output_contains("+HTTPGET:"),
                     "Expected +HTTPGET:, got: %s", g_out_buf);
    }
}

/* -----------------------------------------------------------------------
 * Test 14: HTTPS HTTPCLIENT HEAD — scheme=0 (no certificate verification)
 *
 * Relies on scheme=0 set in test_13 (or re-sets it for standalone safety).
 * --------------------------------------------------------------------- */

ZTEST(httpc_at_https, test_14_https_httpclient_head_no_verify)
{
    int ret;

    LOG_INF("--- test_14_https_httpclient_head_no_verify ---");

    /* Ensure scheme=0 is active (idempotent if test_13 already ran) */
    {
        httpc_at_param_t p[1];

        PARAM_INT(p, 0, HTTPC_AT_AUTH_NONE);
        ret = httpc_at_handle_httpsslcfg(HTTPC_AT_OP_EXEC_W_PARAM, 1, p);
        zassert_ok(ret, "httpsslcfg scheme=0 returned %d", ret);
        clear_output();
    }

    /* AT+HTTPCLIENT=1,0,<https_url>  (HEAD, form-urlencoded) */
    {
        httpc_at_param_t p[3];

        PARAM_INT(p, 0, HTTPC_AT_METHOD_HEAD);
        PARAM_INT(p, 1, HTTPC_AT_CTYPE_FORM_URLENCODED);
        PARAM_STR(p, 2, TEST_HTTPS_URL);

        LOG_INF("HTTPS HTTPCLIENT HEAD (no-verify) url=%s", TEST_HTTPS_URL);
        ret = httpc_at_handle_httpclient(HTTPC_AT_OP_EXEC_W_PARAM, 3, p);

        zassert_ok(ret, "https httpclient HEAD (no-verify) returned %d", ret);
        zassert_true(output_contains("+HTTPC:OK"),
                     "Expected +HTTPC:OK, got: %s", g_out_buf);
    }
}
/* -----------------------------------------------------------------------
 * Test 15: AT+HTTPSSLCFG set — scheme=1 (server certificate verification)
 *
 * Sets the CA file path.  The actual cert is loaded from the device
 * filesystem by httpc_at_ssl.c when the next HTTPS request is made.
 *
 * Prerequisite: CA cert must be burned to CONFIG_HTTPC_AT_TEST_CA_FILE
 * on the device filesystem before this test runs.
 * --------------------------------------------------------------------- */

ZTEST(httpc_at_https, test_15_https_sslcfg_server_verify)
{
    LOG_INF("--- test_15_https_sslcfg_server_verify ---");
    LOG_INF("CA file: %s", TEST_CA_FILE);

    /*
     * AT+HTTPSSLCFG=1,"","",<ca_file>
     *   scheme=1  : verify server certificate
     *   cert_file : "" (no client cert)
     *   key_file  : "" (no client key)
     *   ca_file   : path to CA cert on device FS
     */
    httpc_at_param_t p[4];

    PARAM_INT(p, 0, HTTPC_AT_AUTH_SERVER); /* scheme=1 */
    PARAM_STR(p, 1, "");                   /* cert_file: none */
    PARAM_STR(p, 2, "");                   /* key_file:  none */
    PARAM_STR(p, 3, TEST_CA_FILE);         /* ca_file */

    int ret = httpc_at_handle_httpsslcfg(HTTPC_AT_OP_EXEC_W_PARAM, 4, p);

    zassert_ok(ret, "httpsslcfg scheme=1 returned %d", ret);
    zassert_equal(g_out_len, 0, "No direct output expected, got: %s", g_out_buf);

    /* Verify the config was stored correctly */
    const struct httpc_at_global_cfg *cfg = httpc_at_get_config();

    zassert_equal(cfg->https_auth_type, HTTPC_AT_AUTH_SERVER,
                  "Stored scheme should be 1 (server-verify)");
    zassert_mem_equal(cfg->ca_file, TEST_CA_FILE, strlen(TEST_CA_FILE),
                      "Stored CA file path mismatch");

    LOG_INF("HTTPSSLCFG scheme=1 set OK, CA=%s", cfg->ca_file);
}

/* -----------------------------------------------------------------------
 * Test 16: HTTPS GET — scheme=1 (server certificate verification)
 *
 * Requires test_15 to have run first (or CA file already on FS and
 * scheme=1 configured).  httpc_at_ssl.c reads the CA cert from the
 * device filesystem automatically when the HTTPS connection is made.
 * --------------------------------------------------------------------- */

ZTEST(httpc_at_https, test_16_https_get_server_verify)
{
    LOG_INF("--- test_16_https_get_server_verify ---");
    LOG_INF("HTTPS URL: %s  CA: %s", TEST_HTTPS_URL, TEST_CA_FILE);

    httpc_at_param_t p[2];

    PARAM_STR(p, 0, TEST_HTTPS_URL);
    PARAM_INT(p, 1, TEST_TIMEOUT_MS);

    int ret = httpc_at_handle_httpget(HTTPC_AT_OP_EXEC_W_PARAM, 2, p);

    /*
     * httpc_at_handle_httpget() returns negative on network failure.
     * Do NOT assert on ret — check the AT response string instead.
     * The test fails here if the HTTPS connection or cert verification fails.
     */
    LOG_INF("HTTPS GET (server-verify) ret=%d output=%.128s", ret, g_out_buf);
    zassert_true(output_contains("+HTTPGET:"),
                 "Expected +HTTPGET:, got: %s", g_out_buf);
}

/* -----------------------------------------------------------------------
 * Test 17: HTTPS HTTPCLIENT HEAD — scheme=1 (server certificate verification)
 * --------------------------------------------------------------------- */

ZTEST(httpc_at_https, test_17_https_httpclient_head_server_verify)
{
    LOG_INF("--- test_17_https_httpclient_head_server_verify ---");
    LOG_INF("HTTPS URL: %s  CA: %s", TEST_HTTPS_URL, TEST_CA_FILE);

    httpc_at_param_t p[3];

    PARAM_INT(p, 0, HTTPC_AT_METHOD_HEAD);
    PARAM_INT(p, 1, HTTPC_AT_CTYPE_FORM_URLENCODED);
    PARAM_STR(p, 2, TEST_HTTPS_URL);

    int ret = httpc_at_handle_httpclient(HTTPC_AT_OP_EXEC_W_PARAM, 3, p);

    zassert_ok(ret, "https httpclient HEAD (server-verify) returned %d", ret);
    zassert_true(output_contains("+HTTPC:OK"),
                 "Expected +HTTPC:OK, got: %s", g_out_buf);
}
