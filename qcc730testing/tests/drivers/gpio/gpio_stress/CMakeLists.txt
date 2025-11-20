# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2025 Qualcomm Technologies, Inc.

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(gpio_qcc730_test)

FILE(GLOB app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})