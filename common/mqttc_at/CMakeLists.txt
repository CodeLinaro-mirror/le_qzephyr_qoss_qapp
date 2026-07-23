# Copyright (c) 2026 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear

if(CONFIG_MQTTC_AT)

zephyr_library_named(mqttc_at)

zephyr_library_sources(
    src/mqttc_at_handler.c
    src/mqttc_at_core.c
)

zephyr_library_include_directories(
    inc
)

zephyr_library_link_libraries(kernel)

endif() # CONFIG_MQTTC_AT
