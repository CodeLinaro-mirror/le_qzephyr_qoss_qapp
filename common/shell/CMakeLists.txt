
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

zephyr_sources(src/platform_shell.c)
zephyr_sources_ifdef(CONFIG_QCRYPTO src/qcrypto_shell.c)
zephyr_sources_ifdef(CONFIG_WIFI src/qwifi_shell.c)
zephyr_sources_ifdef(CONFIG_PM src/qpower_shell.c)
zephyr_sources_ifdef(CONFIG_PM_DEVICE_SYSTEM_MANAGED src/qbmps_shell.c)
zephyr_sources_ifdef(CONFIG_SIGMADUT_TRAFFIC src/sigmadut_traffic.c)

zephyr_include_directories_ifdef(CONFIG_QCRYPTO_SUPPORT ${ZEPHYR_LIBCRYPTOQCC730_MODULE_DIR}/inc)
zephyr_include_directories_ifdef(CONFIG_QCRYPTO
    ${ZEPHYR_MBEDTLS_MODULE_DIR}/include
    ${ZEPHYR_MBEDTLS_MODULE_DIR}/library
)

zephyr_include_directories(${ZEPHYR_HAL_QCOM_MODULE_DIR}/qwifi/inc/libwifi)
