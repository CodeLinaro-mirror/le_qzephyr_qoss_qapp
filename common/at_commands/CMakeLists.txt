# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

if(CONFIG_QAT)

zephyr_library()

zephyr_library_sources_ifdef(CONFIG_QAT_COMMON	src/qat_common.c)
zephyr_library_sources_ifdef(CONFIG_QAT_WLAN	src/qat_wlan.c)
zephyr_library_sources_ifdef(CONFIG_QAT_TCPIP	src/qat_tcpip.c)
zephyr_library_sources_ifdef(CONFIG_QAT_HTTPC	src/qat_httpc.c)
zephyr_library_sources_ifdef(CONFIG_QAT_FSTORE	src/qat_fstore.c)
zephyr_library_sources_ifdef(CONFIG_QAT_HTTPSERVER	src/qat_httpserver.c)

zephyr_library_sources_ifdef(CONFIG_QAT_OTA  src/qat_ota.c)

zephyr_library_include_directories_ifdef(CONFIG_QAT_OTA
    ${CMAKE_CURRENT_SOURCE_DIR}/../mcumgr_qcom/include
    ${ZEPHYR_BASE}/../modules/crypto/mbedtls/include
)

zephyr_library_include_directories(
    ${ZEPHYR_HAL_QCOM_MODULE_DIR}/qat/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../httpc_at/inc
)

endif()