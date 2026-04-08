# Copyright (c) 2025 Qualcomm Technologies, Inc. and/or its subsidiaries
# SPDX-License-Identifier: BSD-3-Clause-Clear

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(test_reset_qcc730)
FILE(GLOB app_sources src/*.c)

target_sources(app PRIVATE ${app_sources})
