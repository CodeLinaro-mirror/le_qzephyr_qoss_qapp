# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# qapp/rcp_app: QCC730 board port of the upstream rcp_zephyr sample app.
# Proxy source files (proxy_wifi_mgmt.c, proxy_wifi_data.c) are referenced
# directly from qrcp/rcp_zephyr/samples/rcp_app/src/ to avoid duplication.

cmake_minimum_required(VERSION 3.20.0)

set(UPSTREAM_RCP_APP
    ${CMAKE_CURRENT_LIST_DIR}/../../qrcp/rcp_zephyr/samples/rcp_app/src)

list(APPEND EXTRA_ZEPHYR_MODULES
    ${CMAKE_CURRENT_LIST_DIR}/../../qrcp/rcp_zephyr
    ${CMAKE_CURRENT_LIST_DIR}/../../qrcp/rcp_protocol
    ${CMAKE_CURRENT_LIST_DIR}/../../modules/lib/nanopb
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rcp_app)

target_sources(app PRIVATE
    src/main.c
    ${UPSTREAM_RCP_APP}/proxy_wifi_data.c
    ${UPSTREAM_RCP_APP}/proxy_wifi_mgmt.c
)

# Proxy headers live in the upstream src dir
target_include_directories(app PRIVATE ${UPSTREAM_RCP_APP})

# Nanopb: generate wifi_mgmt.pb.h/.c from the protocol proto
list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)
zephyr_nanopb_sources(app ${ZEPHYR_RCP_PROTOCOL_MODULE_DIR}/proto/wifi_mgmt.proto)

# Linker fragment: place .rcp_shmem in the RCP_SHMEM memory region
zephyr_linker_sources(SECTIONS src/rcp_shmem.ld)
