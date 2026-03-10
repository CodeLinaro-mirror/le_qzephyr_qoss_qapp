
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause


zephyr_sources_ifdef(CONFIG_PM src/pm_timer.c)

zephyr_include_directories(${ZEPHYR_HAL_QCOM_MODULE_DIR}/qwifi/inc/libwifi)
zephyr_include_directories(include)
