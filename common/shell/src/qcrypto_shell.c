/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
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
#if CONFIG_MBEDTLS_PSA_CRYPTO_C
#include <psa/internal_trusted_storage.h>
#endif

#define USER_PASSWORD_SIZE 16
#define QCRYPTO_TEST_DATA_FLAGS PSA_STORAGE_FLAG_NONE

extern uint8_t g_securefs_password[16];

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


#if defined(MBEDTLS_SELF_TEST) && defined(MBEDTLS_ENTROPY_C) && defined(CONFIG_MBEDTLS_TEST)
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
    shell_error(ctx, "entropy_test unavailable: need CONFIG_MBEDTLS_TEST and MBEDTLS_ENTROPY_C");
    return -ENOTSUP;
}
#endif /* MBEDTLS_SELF_TEST && MBEDTLS_ENTROPY_C && CONFIG_MBEDTLS_TEST */


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

#if CONFIG_SECURE_STORAGE_ITS_IMPLEMENTATION_ZEPHYR
static int cmd_secure_write(const struct shell *ctx, size_t argc, char **argv)
{
	int err = 0;
    size_t data_length = 0;   /* length in bytes */
    psa_storage_uid_t uid = 0;
    uint8_t *data_write = NULL;   /* Data to be written to ITS. */
    uint8_t *dup_hex = NULL;

    if (argc != 3 && argc != 4) {
        shell_error(ctx, "Usage: secure_write <uid> <hex_data> [optional: <length>]");
        return -EINVAL;
    }

    uid = shell_strtoul(argv[1], 10, &err);
    if (err) {
        shell_error(ctx, "Invalid uid parameter (err %d)", err);
        return err;
    }

    if (argc == 3) {
        const char *hex_string = argv[2];
        data_length = strlen(hex_string)/2;
        if (data_length <= 0 || data_length > CONFIG_SECURE_STORAGE_ITS_MAX_DATA_SIZE) {
            shell_error(ctx, "Invalid data length %zu bytes (max: %d bytes)", 
                    data_length, CONFIG_SECURE_STORAGE_ITS_MAX_DATA_SIZE);
            return -EINVAL;
        }

        data_write = (uint8_t *)malloc(data_length);
        if (!data_write) {
            shell_error(ctx, "Failed to allocate memory for data");
            return -ENOMEM;
        }

        if(hexstr_to_digits(argv[2], data_write, data_length)) {
            shell_error(ctx, "Invalid hex data, must be even-length");
            return -EINVAL;
        }
    } else if (argc == 4) {
        size_t hex_data_length = shell_strtoul(argv[3], 10, &err);
        if (err) {
            shell_error(ctx, "Invalid length parameter (err %d)", err);
            return err;
        }

        if (hex_data_length % 2 != 0) {
            shell_error(ctx, "The length of the hex string is %d, make sure to input even number of hex char.", hex_data_length);
            return -EINVAL;
        } else {
            data_length = hex_data_length/2;
        }

        if (data_length <= 0 || data_length > CONFIG_SECURE_STORAGE_ITS_MAX_DATA_SIZE) {
            shell_error(ctx, "Invalid data length %zu bytes (max: %d bytes)", 
                    data_length, CONFIG_SECURE_STORAGE_ITS_MAX_DATA_SIZE);
            return -EINVAL;
        }

        dup_hex = (uint8_t *)argv[2];
        if (!dup_hex || !isxdigit((int)dup_hex[0]) || (strlen(dup_hex) != 1)) {
            shell_error(ctx, "hex data must contain only [0-9] or [A-F]");
            return -EINVAL;
        }

        data_write = (uint8_t *)malloc(data_length);
        if (!data_write) {
            shell_error(ctx, "Failed to allocate memory for data");
            return -ENOMEM;
        }

        uint8_t nibble = (uint8_t)hex2digit(dup_hex[0]);
        uint8_t byte_value = (nibble << 4) | nibble;   /* Single hex digit: "a" -> 0xaa */
        for (size_t i = 0; i < data_length; i++) {
            data_write[i] = byte_value;
        }
    }
    
	err = psa_its_set(uid, data_length, data_write, QCRYPTO_TEST_DATA_FLAGS);
	if (err != PSA_SUCCESS) {
		shell_error(ctx, "Writing the data to ITS failed. (%d)", err);
        free(data_write);
		return -1;
	}

    shell_print(ctx, "Successfully wrote %zu bytes to ITS", data_length);
    free(data_write);

	return 0;
}

static int cmd_secure_read(const struct shell *ctx, size_t argc, char **argv)
{
	int err = 0;
    size_t data_length = 0;
    size_t actual_length = 0;
    psa_storage_uid_t uid = 0;
    uint8_t *data_read = NULL;  /* Data to be read from ITS. */
    size_t data_offset = 0;  /* Read back the data starting from an offset. */

    if (argc != 3) {
        shell_error(ctx, "Usage: secure_read <uid> <length>");
        return -EINVAL;
    }
    
    uid = shell_strtoul(argv[1], 10, &err);
    if (err) {
        shell_error(ctx, "Invalid uid parameter (err %d)", err);
        return err;
    }

    size_t hex_data_length = shell_strtoul(argv[2], 10, &err);
    if (err) {
        shell_error(ctx, "Invalid length parameter (err %d)", err);
        return err;
    }

    data_length = hex_data_length/2;
    if (data_length <= 0 || data_length > CONFIG_SECURE_STORAGE_ITS_MAX_DATA_SIZE) {
        shell_error(ctx, "Invalid data length %zu (max: %d)", 
                    data_length, CONFIG_SECURE_STORAGE_ITS_MAX_DATA_SIZE);
        return -EINVAL;
    }

    data_read = (uint8_t *)malloc(data_length);
    if (!data_read) {
        shell_error(ctx, "Failed to allocate memory for data");
        return -ENOMEM;
    }

    err = psa_its_get(uid, data_offset, data_length, data_read, &actual_length);
	
	if (err != PSA_SUCCESS) {
		shell_error(ctx, "Reading back the data from ITS failed. (%d).", err);
        free(data_read);
		return -1;
	}

    shell_print(ctx, "Successfully read %zu bytes from ITS:", actual_length);
    shell_fprintf_normal(ctx, "Data: ");
    for (int i = actual_length - 1; i >= 0; i--) {
        shell_fprintf_normal(ctx, "%02x", data_read[i]);
    }
    shell_fprintf_normal(ctx, "\n");

    free(data_read);

	return 0;
}

static int cmd_secure_get_info(const struct shell *ctx, size_t argc, char **argv)
{
	int err = 0;
    struct psa_storage_info_t info;
    psa_storage_uid_t uid = 0;

    if (argc != 2) {
        shell_error(ctx, "Usage: secure_get_info <uid>");
        return -EINVAL;
    }

    uid = shell_strtoul(argv[1], 10, &err);
    if (err) {
        shell_error(ctx, "Invalid uid parameter (err %d)", err);
        return err;
    }

    err = psa_its_get_info(uid, &info);
	if (err != PSA_SUCCESS) {
		shell_error(ctx, "Failed to retrieve the entry's metadata. (%d)", err);
		return -1;
	}

    shell_print(ctx, "size: %d", info.size);

	return 0;
}

static int cmd_secure_remove(const struct shell *ctx, size_t argc, char **argv)
{
	int err = 0;
    psa_storage_uid_t uid = 0;

    if (argc != 2) {
        shell_error(ctx, "Usage: secure_remove <uid>");
        return -EINVAL;
    }

    uid = shell_strtoul(argv[1], 10, &err);
    if (err) {
        shell_error(ctx, "Invalid uid parameter (err %d)", err);
        return err;
    }

    shell_print(ctx, "Removing the entry from ITS...");

	err = psa_its_remove(uid);
	if (err != PSA_SUCCESS) {
		shell_error(ctx, "Failed to remove the entry. (%d)", err);
		return -1;
	}

	shell_print(ctx,"Entry removed from ITS.");

	return 0;
}

#define hex_to_dec_nibble(hex_nibble) ( \
    ((hex_nibble >= '0' && hex_nibble <= '9') ? (hex_nibble - '0') : \
    (hex_nibble >= 'a' && hex_nibble <= 'f') ? (hex_nibble - 'a' + 10) : \
    (hex_nibble >= 'A' && hex_nibble <= 'F') ? (hex_nibble - 'A' + 10) : -1) )

int convert_data_in_hex_to_byte_array(const char * data_in_hex, uint8_t * data_as_byte_array, uint32_t data_as_byte_array_size)
{
    uint32_t i;
    const uint32_t data_in_hex_length = strlen(data_in_hex);

    if (data_as_byte_array_size > (UINT32_MAX / 2)) {
        return -1;
    }

    if ( (2*data_as_byte_array_size) != data_in_hex_length ) {
        return -1;
    }

    for ( i = 0; i < data_as_byte_array_size; i++ ) {
        int high_nibble = hex_to_dec_nibble(data_in_hex[2*i]);
        int low_nibble = hex_to_dec_nibble(data_in_hex[2*i+1]);

        if (high_nibble < 0 || low_nibble < 0) {
            return -1;  // Invalid hex character
        }

        data_as_byte_array[i] = (high_nibble << 4) | low_nibble;
    }

    return 0;
}

static int cmd_secure_set_password(const struct shell *ctx, size_t argc, char **argv)
{
	int err = 0;
    uint8_t user_password[USER_PASSWORD_SIZE];

    if (argc != 2) {
        shell_error(ctx, "Usage: secure_set_password <password_in_hex>");
        return -EINVAL;
    }

    char *password_in_hex = argv[1];

    int status_code = convert_data_in_hex_to_byte_array(password_in_hex, user_password, USER_PASSWORD_SIZE);
    if (0 != status_code) {
        shell_error(ctx, "Invalid password_in_hex, must be exactly 32 hex chars");
        return -EINVAL;
    } else {
        memcpy(g_securefs_password, user_password, USER_PASSWORD_SIZE);
        shell_info(ctx, "Set user password success, total 32 hex chars", password_in_hex);
    }

	return 0;
}
#endif /* CONFIG_SECURE_STORAGE_ITS_IMPLEMENTATION_ZEPHYR */

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
#if CONFIG_SECURE_STORAGE_ITS_IMPLEMENTATION_ZEPHYR                                        
                               SHELL_CMD_ARG(secure_write, NULL,
                                             "encrypts the hex data and writes it with the provided uid\n"
                                             "Usage: secure_write <uid> <hex_data> [optional: <length>]\n",
                                             cmd_secure_write, 3, 1),
                               SHELL_CMD_ARG(secure_read, NULL,
                                             "reads and decrypts length bytes of data for the provided uid\n"
                                             "Usage: secure_read <uid> <length>\n",
                                             cmd_secure_read, 3, 0),
                               SHELL_CMD_ARG(secure_get_info, NULL,
                                             "get stored info\n"
                                             "Usage: secure_get_info <uid>\n",
                                             cmd_secure_get_info, 2, 0),
                               SHELL_CMD_ARG(secure_remove, NULL,
                                             "remove stored data for the provided uid\n"
                                             "Usage: secure_remove <uid>\n",
                                             cmd_secure_remove, 2, 0),
                               SHELL_CMD_ARG(secure_set_password, NULL,
                                             "set passowrd for securefs\n"
                                             "Usage: secure_set_password <password_in_hex>\n",
                                             cmd_secure_set_password, 2, 0),
#endif /* CONFIG_SECURE_STORAGE_ITS_IMPLEMENTATION_ZEPHYR */
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(qcrypto, &sub_pm_cmds, "crypto related commands", NULL);
