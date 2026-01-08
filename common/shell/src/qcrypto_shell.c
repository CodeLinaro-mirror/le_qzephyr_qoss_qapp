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
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(qcrypto, &sub_pm_cmds, "crypto related commands", NULL);
