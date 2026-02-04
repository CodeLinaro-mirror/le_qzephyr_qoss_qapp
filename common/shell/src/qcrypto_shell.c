/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>
#include <zephyr/pm/pm.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <CeML.h>
#include <qapi_wlan_base.h>
#include "self_test.h"
#include "mbedtls/aes.h"
#include "mbedtls/ccm.h"
#include "mbedtls/dhm.h"
#include "mbedtls/sha1.h"
#include "mbedtls/sha256.h"
#include "mbedtls/entropy.h"
#include "entropy_poll.h"

static int hex2digit(int c)
{
    if ('0' <= c && c <= '9') {
        return c - '0';
    } else if ('a' <= c && c <= 'f') {
        return c - 'a' + 10;
    } else if ('A' <= c && c <= 'F') {
        return c - 'A' + 10;
    }

    return -EINVAL;
}

static int hexstr_to_digits(const char *data, uint8_t *buf, uint32_t n_buf)
{
    const uint32_t len = strlen(data);

    if (len != 2 * n_buf) {
        return -EINVAL;
    }

    for (int i = len - 1, k = 0; i >= 0; i--, k++) {
        int t = hex2digit(data[i]);
        if (t < 0) {
            return -EINVAL;
        }

        if (k % 2 == 0) {
            buf[k / 2] = t;
        } else {
            buf[k / 2] |= t << 4;
        }
    }

    return 0;
}

static void hexdump(const struct shell *ctx, uint8_t *buf, uint32_t n_buf)
{
    for (int i = n_buf - 1; i >= 0; i--) {
        shell_fprintf_normal(ctx, "%02x", buf[i]);
    }
    shell_fprintf_normal(ctx, "\n");
}

static int kdf_key_test_cmd(const struct shell *ctx, size_t argc, char **argv)
{
    uint8_t user_input[16] = {0};
    uint64_t password = 0;
    uint32_t result[4] = {0};
    uint32_t result_len = sizeof(result) / sizeof(result[0]);

    if (hexstr_to_digits(argv[1], user_input, 16)) {
        shell_print(ctx, "Invalid kdf_key format, must be exactly 32(%d) hex chars\n", strlen(argv[1]));
        return -EINVAL;
    }

    if (0 != CeML_hw_kdf(NULL, CEML_KDF_SECURE_STORAGE, user_input, 16, password, (uint32 *)result, result_len)) {
        shell_print(ctx, "KDF Test Fail\n");
        return -EINVAL;
    }

    shell_fprintf_normal(ctx, "KDF output: ");
    for (int i = result_len - 1; i >= 0; i--) {
        shell_fprintf_normal(ctx, "%08x", result[i]);
    }
    shell_fprintf_normal(ctx, "\n");

    return 0;
}

static int kdf_encrypt_test_cmd(const struct shell *ctx, size_t argc, char **argv)
{
    uint8_t kdf_key[16] = {0};
    uint8_t derive_key[16] = {0};
    uint8_t iv[16] = {0};
    uint32_t decrypt_len = 16;
    uint32_t encrypt_len = 32;
    uint8_t *encrypt_kdf = NULL;
    uint8_t *encrypt_sw = NULL;
    uint8_t *data_test = NULL;
    qapi_Status_t ret = QAPI_ERROR;

    if(hexstr_to_digits(argv[1], kdf_key, 16)) {
        shell_print(ctx, "Invalid kdf_key, must be exactly 32(%d) hex chars", strlen(argv[1]));
        return -EINVAL;
    }

    if(hexstr_to_digits(argv[2], derive_key, 16)) {
        shell_print(ctx, "Invalid derive_key, must be exactly 32(%d) hex chars", strlen(argv[2]));
        return -EINVAL;
    }

    data_test = (uint8_t*)malloc(decrypt_len + encrypt_len * 2);
    if(!data_test) {
        shell_print(ctx, "malloc for data of test fail\r\n");
        goto error_back;
    }

    if(hexstr_to_digits(argv[3], data_test, decrypt_len)) {
        shell_print(ctx, "Invalid data, must be exactly 32(%d) hex chars", strlen(argv[3]));
        goto error_back;
    }

    encrypt_kdf = data_test + decrypt_len;
    encrypt_sw = encrypt_kdf + encrypt_len;

    memset(iv, 0x5a, sizeof(iv));
    shell_fprintf_normal(ctx, "IV:\t\t");
    hexdump(ctx, iv, sizeof(iv));

    shell_fprintf_normal(ctx, "Input Data:\t");
    hexdump(ctx, data_test, decrypt_len);

	/* encrypt data with kdf key */
    ret = CeML_util_encrypt_with_key(0, kdf_key, 16, iv, 16, data_test, decrypt_len, encrypt_kdf, (uint32 *)&encrypt_len);
    if(ret) {
        shell_print(ctx, "encrypt data with kdf key fail %d", ret);
        goto error_back;
    }
    shell_fprintf_normal(ctx, "Encrypt Data(KDF Key):\t");
    hexdump(ctx, encrypt_kdf, encrypt_len);

    /* encrypt data with sw key */
    ret = CeML_util_encrypt_with_key(1, derive_key, 16, iv, 16, data_test, decrypt_len, encrypt_sw, (uint32 *)&encrypt_len);
    if(ret) {
        shell_print(ctx, "encrypt data with sw key fail %d", ret);
        goto error_back;
    }
    shell_fprintf_normal(ctx, "Encrypt Data(SW Key):\t");
    hexdump(ctx, encrypt_sw, encrypt_len);

    shell_fprintf_normal(ctx, "\"Encrypt Data(KDF Key)\" vs \"Encrypt Data(SW Key)\":\t");
    if(memcmp(encrypt_kdf, encrypt_sw, encrypt_len)) {
        shell_print(ctx, "not match");
    }
    else {
        shell_print(ctx, "match");
    }

    ret = QAPI_OK;

error_back:
    if(data_test) {
        free(data_test);
    }

    return ret;
}

static int kdf_decrypt_test_cmd(const struct shell *ctx, size_t argc, char **argv)
{
    uint8_t kdf_key[16] = {0};
    uint8_t derive_key[16] = {0};
    uint32_t encrypt_len = 32;
    uint32_t decrypt_len = 16;
    uint8_t *kdf_decrypted_data = NULL;
    uint8_t *sw_decrypted_data = NULL;
    uint8_t *encrypted_data = NULL;
    qapi_Status_t ret = QAPI_ERROR;

    if(hexstr_to_digits(argv[1], kdf_key, 16)) {
        shell_print(ctx, "Invalid kdf_key, must be exactly 32(%d) hex chars", strlen(argv[1]));
        return -EINVAL;
    }

    if(hexstr_to_digits(argv[2], derive_key, 16)) {
        shell_print(ctx, "Invalid derive_key, must be exactly 32(%d) hex chars", strlen(argv[2]));
        return -EINVAL;
    }

    encrypted_data = (uint8_t *) malloc(encrypt_len + decrypt_len * 2);
    if(!encrypted_data) {
        shell_print(ctx, "malloc for data of test fail\r\n");
        goto error_back;
    }
    kdf_decrypted_data = encrypted_data + encrypt_len;
    sw_decrypted_data = kdf_decrypted_data + decrypt_len;

    if(hexstr_to_digits(argv[3], encrypted_data, encrypt_len)) {
        shell_print(ctx, "Invalid encrypted_data, must be exactly 64(%d) hex chars", strlen(argv[3]));
        goto error_back;
    }

    shell_fprintf_normal(ctx, "Encrypted Data:\t");
    hexdump(ctx, encrypted_data, encrypt_len);

    /* decrypt data with kdf key */
    ret = CeML_util_decrypt_with_key(0, kdf_key, 16, encrypted_data, encrypt_len, kdf_decrypted_data, (uint32 *)&decrypt_len);
    if(ret) {
        shell_print(ctx, "decrypt data with kdf key fail %d", ret);
        goto error_back;
    }

    shell_fprintf_normal(ctx, "Decrypt Data(KDF Key):\t");
    hexdump(ctx, kdf_decrypted_data, decrypt_len);

    /* decrypt data with sw key */
    ret = CeML_util_decrypt_with_key(1, derive_key, 16, encrypted_data, encrypt_len, sw_decrypted_data, (uint32 *)&decrypt_len);
    if(ret) {
        shell_print(ctx, "decrypt data with sw key fail %d", ret);
        goto error_back;
    }

    shell_fprintf_normal(ctx, "Decrypt Data(SW Key):\t");
    hexdump(ctx, sw_decrypted_data, decrypt_len);

    shell_fprintf_normal(ctx, "\"Decrypt Data(KDF Key)\" vs \"Decrypt Data(SW Key)\":\t");
    if(memcmp(kdf_decrypted_data, sw_decrypted_data, decrypt_len)) {
        shell_print(ctx, "not match");
    }
    else {
        shell_print(ctx, "match");
    }

    ret = QAPI_OK;

error_back:
    if(encrypted_data) {
        free(encrypted_data);
    }

    return ret;
}

static int cmd_pka_test(const struct shell *ctx, size_t argc, char **argv)
{
    if (argc != 3) {
        shell_error(ctx, "parameters count not right (cnt %d), should be 3", argc);
        return -EINVAL;
    }

    int err = 0;
    int verbose = shell_strtoul(argv[2], 10, &err);
    if (err) {
        shell_error(ctx, "Unable to parse input verbose (err %d)", err);
        return err;
    }

    if (strcmp(argv[1], "all") == 0) {
        /* MPI Test */
        shell_print(ctx, "MPI Test");
        if (mbedtls_mpi_pka_self_test(verbose) != 0)
            shell_error(ctx, "Test Fail");
        else
            shell_print(ctx, "Test Pass");

        /* RSA Test */
        shell_print(ctx, "RSA Test");

        if (mbedtls_rsa_pka_self_test(verbose) != 0)
            shell_error(ctx, "Test Fail");
        else
            shell_print(ctx, "Test Pass");

        /* DH Test */
        shell_print(ctx, "DH Test");

        if (mbedtls_dhm_pka_self_test(verbose) != 0)
            shell_error(ctx, "Test Fail");
        else
            shell_print(ctx, "Test Pass");

        /* ECP Test */
        shell_print(ctx, "ECP Test");

        if (mbedtls_ecp_pka_self_test(verbose) != 0)
            shell_error(ctx, "Test Fail");
        else
            shell_print(ctx, "Test Pass");

        /* ECDSA Test */
        shell_print(ctx, "ECDSA Test");

        if (mbedtls_ecdsa_pka_self_test(verbose) != 0)
            shell_error(ctx, "Test Fail");
        else
            shell_print(ctx, "Test Pass");

        return 0;
    } else if (strcmp(argv[1], "mpi") == 0) {
        shell_print(ctx, "MPI Test");

        if (mbedtls_mpi_pka_self_test(verbose) != 0)
            shell_error(ctx, "Test Fail");
        else
            shell_print(ctx, "Test Pass");

        return 0;
    } else if (strcmp(argv[1], "rsa") == 0) {
        shell_print(ctx, "RSA Test");

        if (mbedtls_rsa_pka_self_test(verbose) != 0)
            shell_error(ctx, "Test Fail");
        else
            shell_print(ctx, "Test Pass");

        return 0;
    } else if (strcmp(argv[1], "dh") == 0) {
        shell_print(ctx, "DH Test");

        if (mbedtls_dhm_pka_self_test(verbose) != 0)
            shell_error(ctx, "Test Fail");
        else
            shell_print(ctx, "Test Pass");

        return 0;

    } else if (strcmp(argv[1], "ecp") == 0) {
        shell_print(ctx, "ECP Test");

        if (mbedtls_ecp_pka_self_test(verbose) != 0)
            shell_error(ctx, "Test Fail");
        else
            shell_print(ctx, "Test Pass");

        return 0;
    } else if (strcmp(argv[1], "ecdsa") == 0) {
        shell_print(ctx, "ECDSA Test");

        if (mbedtls_ecdsa_pka_self_test(verbose) != 0)
            shell_error(ctx, "Test Fail");
        else
            shell_print(ctx, "Test Pass");

        return 0;
    } else {
        shell_error(ctx, "Invalid test type '%s'. Valid types: mpi, rsa, dh, ecp, ecdsa", argv[1]);
        return -EINVAL;
    }
}

#if defined(CONFIG_MBEDTLS_TEST)
static int cmd_qcc_test(const struct shell *ctx, size_t argc, char **argv)
{
    if (argc != 3) {
        shell_print(ctx, "usage: qcc_test <module> <verbose>");
        shell_print(ctx, "module: aes | ccm | sha1 | sha256 | all");
        return -EINVAL;
    }

    int err = 0;
    int verbose = shell_strtoul(argv[2], 10, &err);
    if (err) {
        shell_error(ctx, "Invalid verbose (err %d)", err);
        return -EINVAL;
    }

    const char *m = argv[1];
    int rc = 0;
    bool matched = false;
#if defined(MBEDTLS_AES_C)
    if (!strcmp(m, "all") || !strcmp(m, "aes")) {
        matched = true;
        shell_print(ctx, "AES Test");
        rc = mbedtls_aes_self_test(verbose);
        shell_print(ctx, rc ? "Test Fail" : "Test Pass");
        if (!strcmp(m, "aes"))
            return 0;
    }
#endif
#if defined(MBEDTLS_CCM_GCM_CAN_AES) && defined(MBEDTLS_CCM_C)
    if (!strcmp(m, "all") || !strcmp(m, "ccm")) {
        matched = true;
        shell_print(ctx, "AES-CCM Test");
        rc = mbedtls_ccm_self_test(verbose);
        shell_print(ctx, rc ? "Test Fail" : "Test Pass");
        if (!strcmp(m, "ccm"))
            return 0;
    }
#endif
#if defined(MBEDTLS_SHA1_C)
    if (!strcmp(m, "all") || !strcmp(m, "sha1")) {
        matched = true;
        shell_print(ctx, "SHA1 Test");
        rc = mbedtls_sha1_self_test(verbose);
        shell_print(ctx, rc ? "Test Fail" : "Test Pass");
        if (!strcmp(m, "sha1"))
            return 0;
    }
#endif
#if defined(MBEDTLS_SHA256_C)
    if (!strcmp(m, "all") || !strcmp(m, "sha256")) {
        matched = true;
        shell_print(ctx, "SHA256 Test");
        rc = mbedtls_sha256_self_test(verbose);
        shell_print(ctx, rc ? "Test Fail" : "Test Pass");
        if (!strcmp(m, "sha256"))
            return 0;
    }
#endif
    if (!matched) {
        shell_error(ctx, "Invalid test module '%s'", m);
        shell_print(ctx, "Available modules:");
#if defined(MBEDTLS_AES_C)
        shell_print(ctx, "  - aes");
#endif
#if defined(MBEDTLS_CCM_C) && defined(MBEDTLS_CCM_GCM_CAN_AES)
        shell_print(ctx, "  - ccm");
#endif
#if defined(MBEDTLS_SHA1_C)
        shell_print(ctx, "  - sha1");
#endif
#if defined(MBEDTLS_SHA256_C)
        shell_print(ctx, "  - sha256");
#endif
        return -EINVAL;
    }
    return 0;
}
#else /* !CONFIG_MBEDTLS_TEST */
static int cmd_qcc_test(const struct shell *ctx, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    shell_error(ctx, "qcc_test is unavailable: CONFIG_MBEDTLS_TEST is not enabled");
    shell_print(ctx, "Please enable CONFIG_MBEDTLS_TEST in your Mbed TLS config");
    return -EINVAL;
}
#endif /* CONFIG_MBEDTLS_TEST */


#if defined(MBEDTLS_SELF_TEST) && defined(MBEDTLS_ENTROPY_C)
static int cmd_entropy_test(const struct shell *ctx, size_t argc, char **argv)
{
    if (argc != 2) {
        shell_print(ctx, "usage: entropy_test <verbose>");
        return -EINVAL;
    }

    int err = 0;
    int verbose = shell_strtoul(argv[1], 10, &err);
    if (err) {
        shell_error(ctx, "Invalid verbose (err %d)", err);
        return -EINVAL;
    }

    shell_print(ctx, "Entropy Self-Test");
    int rc = mbedtls_entropy_self_test(verbose);
    shell_print(ctx, rc ? "Test Fail" : "Test Pass");
    return rc ? -EFAULT : 0;
}
#else
static int cmd_entropy_test(const struct shell *ctx, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    shell_error(ctx, "entropy_test unavailable: need MBEDTLS_SELF_TEST and MBEDTLS_ENTROPY_C");
    shell_print(ctx, "Enable MBEDTLS_SELF_TEST + MBEDTLS_ENTROPY_C (and SHA256/SHA512) to use this command.");
    return -ENOTSUP;
}
#endif /* MBEDTLS_SELF_TEST && MBEDTLS_ENTROPY_C */


#if defined(MBEDTLS_ENTROPY_HARDWARE_ALT)
static int cmd_entropy_hw_poll(const struct shell *ctx, size_t argc, char **argv)
{
    if (argc != 2) {
        shell_print(ctx, "usage: entropy_hw_poll <len>");
        return -EINVAL;
    }

    int err = 0;
    size_t req = shell_strtoul(argv[1], 10, &err);
    if (err || req == 0 || req > 1024) {
        shell_error(ctx, "Invalid len (1..1024)");
        return -EINVAL;
    }

    uint8_t *buf = (uint8_t *)malloc(req);
    if (!buf) {
        shell_error(ctx, "malloc failed");
        return -ENOMEM;
    }

    size_t olen = 0;
    int rc = mbedtls_hardware_poll(NULL, buf, req, &olen);
    if (rc != 0 || olen == 0) {
        shell_error(ctx, "hardware_poll failed rc=%d, olen=%zu", rc, olen);
        free(buf);
        return -EIO;
    }

    shell_print(ctx, "HW entropy bytes (len=%zu, olen=%zu):", req, olen);
    hexdump(ctx, buf, (uint32_t)olen);
    free(buf);
    return 0;
}
#else
static int cmd_entropy_hw_poll(const struct shell *ctx, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    shell_error(ctx, "entropy_hw_poll unavailable: MBEDTLS_ENTROPY_HARDWARE_ALT not enabled");
    shell_print(ctx, "Enable MBEDTLS_ENTROPY_HARDWARE_ALT and provide mbedtls_hardware_poll().");
    return -ENOTSUP;
}
#endif /* MBEDTLS_ENTROPY_HARDWARE_ALT */

SHELL_STATIC_SUBCMD_SET_CREATE(sub_pm_cmds,
                               SHELL_CMD_ARG(kdf_key, NULL,
                                             "Test KDF derive key\n"
                                             "Usage: kdf_key <kdf_key:str>\n"
                                             "kdf_key: The length must be 32.\n"
                                             "\tFor example: qcrypto kdftool \"27ef1314791dae3a7329f1433de0c303\"\n",
                                             kdf_key_test_cmd, 2, 0),
                               SHELL_CMD_ARG(kdf_encrypt, NULL,
                                             "Test KDF Encrypt \n"
                                             "Usage: kdf_encrypt <kdf_key:str> <derive_key:str> <data:str>\n"
                                             "kdf_key: The length must be 32.\n"
                                             "derive_key: The length must be 32.\n"
                                             "data: The length must be 32.\n",
                                             kdf_encrypt_test_cmd, 4, 0),
                               SHELL_CMD_ARG(kdf_decrypt, NULL,
                                             "Test KDF Decrypt \n"
                                             "Usage: kdf_decrypt <kdf_key:str> <derive_key:str> <encrypted_data:str>\n"
                                             "kdf_key: The length must be 32.\n"
                                             "derive_key: The length must be 32.\n"
                                             "encrypted_data: The length must be 64.\n",
                                             kdf_decrypt_test_cmd, 4, 0),
                               SHELL_CMD_ARG(pka_test, NULL,
                                             "pka_test\n"
                                             "Usage: pka_test\n",
                                             cmd_pka_test, 3, 0),
                                
                               SHELL_CMD_ARG(qcc_test, NULL,
                                             "Run qcc test\n"
                                             "Usage: qcc_test <module> <verbose>\n"
                                             "module: aes | ccm | sha1 | sha256 | all\n",
                                             cmd_qcc_test, 3, 0),

                               SHELL_CMD_ARG(entropy_test, NULL,
                                             "Run entropy self-test\n"
                                             "Usage: entropy_test <verbose>\n",
                                             cmd_entropy_test, 2, 0),
                               SHELL_CMD_ARG(entropy_hw_poll, NULL,
                                             "Poll HW entropy bytes\n"
                                             "Usage: entropy_hw_poll <len>\n",
                                             cmd_entropy_hw_poll, 2, 0),
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(qcrypto, &sub_pm_cmds, "crypto related commands", NULL);
