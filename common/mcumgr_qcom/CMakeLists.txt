# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

zephyr_library()

# Core fw_upgrade engine — no MCUmgr dependency
zephyr_library_sources_ifdef(CONFIG_QCOM_FW_UPGRADE
    src/fw_upgrade.c
    src/fw_upgrade_mem.c
    src/flash_adapter.c
)

# MCUmgr protocol handler — requires full MCUmgr stack
zephyr_library_sources_ifdef(CONFIG_QCOM_IMG_MGMT
    src/qcom_img_mgmt.c
)

zephyr_library_include_directories(
	include
	${CMAKE_CURRENT_SOURCE_DIR}/../mcumgr_qcom/include
	${ZEPHYR_BASE}/subsys/mgmt/mcumgr/util/include
	${ZEPHYR_BASE}/../modules/crypto/mbedtls/include
)

# Link mbedtls library for SHA256 support
zephyr_library_link_libraries(mbedTLS)
