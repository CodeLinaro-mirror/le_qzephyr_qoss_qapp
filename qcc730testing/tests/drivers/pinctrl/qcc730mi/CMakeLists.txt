# Copyright (c) 2025 Qualcomm Technologies, Inc. and/or its subsidiaries
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(pinctrl_qcc730mi)

target_sources(app PRIVATE src/main.c)
target_sources(app PRIVATE ${ZEPHYR_BASE}/tests/drivers/pinctrl/common/test_device.c)
target_include_directories(app PRIVATE ${ZEPHYR_BASE}/tests/drivers/pinctrl/common)
