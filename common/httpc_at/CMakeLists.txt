# Copyright (c) 2026 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear

if(CONFIG_HTTPC_AT)

zephyr_library_named(httpc_at)

zephyr_library_sources(
    src/httpc_at_utils.c
    src/httpc_at_ssl.c
    src/httpc_at_core.c
    src/httpc_at_handler.c
)

zephyr_library_include_directories(
    inc
)

zephyr_library_link_libraries(kernel)

endif() # CONFIG_HTTPC_AT
