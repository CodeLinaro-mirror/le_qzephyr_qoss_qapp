# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(power_app)

target_sources(app PRIVATE src/main.c)

# ===== Post-build: run Python script for hex/strip/bin/hashed =====
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(POST_PY "${ZEPHYR_HAL_QCOM_MODULE_DIR}/tools/post_process.py")
set(HASH_SCRIPT "${ZEPHYR_HAL_QCOM_MODULE_DIR}/tools/qhash/sechash/createxbl.py")
set(SBL_TOOL_PREFIX "${ZEPHYR_SDK_INSTALL_DIR}/arm-zephyr-eabi/bin/arm-zephyr-eabi")

set(APP_OUT_DIR "${APPLICATION_BINARY_DIR}/zephyr")
set(HASHED_ELF_BASENAME "zephyr_HASHED.elf")

# Use zephyr_final target which is created by Zephyr build system
add_custom_target(post_process_elf ALL
  COMMAND "${Python3_EXECUTABLE}" "${POST_PY}"
          --elf "${APPLICATION_BINARY_DIR}/zephyr/zephyr.elf"
          --tool-prefix "${SBL_TOOL_PREFIX}"
          --hash-script "${HASH_SCRIPT}"
          --hashed-out "${APP_OUT_DIR}/${HASHED_ELF_BASENAME}"
          --check-noinit

  COMMENT "Post-process ELF and generate hashed ELF"
  VERBATIM
)
# make sure to execute after generate zephyr.elf
add_dependencies(post_process_elf zephyr_final)
