# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

if(CONFIG_QAT)

zephyr_library()

zephyr_library_sources_ifdef(CONFIG_QAT_COMMON	src/qat_common.c)
zephyr_library_sources_ifdef(CONFIG_QAT_TCPIP	src/qat_tcpip.c)

zephyr_library_include_directories(
    ${ZEPHYR_HAL_QCOM_MODULE_DIR}/qat/inc
)

endif()
