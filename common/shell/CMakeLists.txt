
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause

zephyr_sources(src/platform_shell.c)
zephyr_sources_ifdef(CONFIG_QCRYPTO src/qcrypto_shell.c)
zephyr_sources_ifdef(CONFIG_WIFI src/qwifi_shell.c)
zephyr_sources_ifdef(CONFIG_PM src/qpower_shell.c)
zephyr_sources_ifdef(CONFIG_PM_DEVICE_SYSTEM_MANAGED src/qbmps_shell.c)
