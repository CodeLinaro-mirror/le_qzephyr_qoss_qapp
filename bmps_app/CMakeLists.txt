# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(bmps_app)

target_sources(app PRIVATE src/main.c)

# ===== Post-build: run Python script for hex/strip/bin/hashed =====
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(POST_PY "${ZEPHYR_HAL_QCOM_MODULE_DIR}/tools/post_process.py")
set(HASH_SCRIPT "${ZEPHYR_HAL_QCOM_MODULE_DIR}/tools/qhash/sechash/createxbl.py")
set(SBL_TOOL_PREFIX "${ZEPHYR_SDK_INSTALL_DIR}/arm-zephyr-eabi/bin/arm-zephyr-eabi")

set(APP_OUT_DIR "${APPLICATION_BINARY_DIR}/zephyr")
set(HASHED_ELF_BASENAME "zephyr_HASHED.elf")
set(HASHED_ELF "${APP_OUT_DIR}/${HASHED_ELF_BASENAME}")

# Use add_custom_command for incremental build support
add_custom_command(
  OUTPUT "${HASHED_ELF}"
  COMMAND "${Python3_EXECUTABLE}" "${POST_PY}"
          --elf "${APPLICATION_BINARY_DIR}/zephyr/zephyr.elf"
          --tool-prefix "${SBL_TOOL_PREFIX}"
          --hash-script "${HASH_SCRIPT}"
          --hashed-out "${HASHED_ELF}"
          --check-noinit
  DEPENDS zephyr_final "${APPLICATION_BINARY_DIR}/zephyr/zephyr.elf"
  COMMENT "Post-process ELF and generate hashed ELF"
  VERBATIM
)

# Use zephyr_final target which is created by Zephyr build system
add_custom_target(post_process_elf ALL DEPENDS "${HASHED_ELF}")


# ===== Signing support: Manual target "sign" =====
# Usage: west build -b qcc730mi -t sign
# This allows signing without rebuilding/reconfiguring the project

# Paths to sectools
set(SECTOOLS_DIR "${ZEPHYR_HAL_QCOM_MODULE_DIR}/tools/sectools")
set(SECTOOLS_SCRIPT "${SECTOOLS_DIR}/sectools.py")
set(SECTOOLS_CONFIG "${ZEPHYR_HAL_QCOM_MODULE_DIR}/tools/sectools/config/qcc730/qcc730_secimage.xml")

# Input ELF files
set(SBL_ELF "${APPLICATION_BINARY_DIR}/modules/hal_qcom/qboot/${BOARD}_sbl.elf")
set(APP_ELF "${APPLICATION_BINARY_DIR}/zephyr/zephyr.elf")

# Output directories
set(SBL_SIGNED_DIR "${APPLICATION_BINARY_DIR}/modules/hal_qcom/qboot/zephyr_sec_sbl")
set(APP_SIGNED_DIR "${APPLICATION_BINARY_DIR}/zephyr/zephyr_sec_app")

# Output files (based on sectools output structure)
set(SBL_SIGNED_BIN "${SBL_SIGNED_DIR}/qcc730/sbl/${BOARD}_sbl.elf")
set(APP_SIGNED_BIN "${APP_SIGNED_DIR}/qcc730/app/zephyr.elf")

# Sign SBL image custom command
add_custom_command(
  OUTPUT "${SBL_SIGNED_BIN}"
  COMMAND "${Python3_EXECUTABLE}" "${SECTOOLS_SCRIPT}"
          secimage
          -i "${SBL_ELF}"
          -c "${SECTOOLS_CONFIG}"
          -sa
          -g sbl
          -o "${SBL_SIGNED_DIR}"
  WORKING_DIRECTORY "${SECTOOLS_DIR}"
  DEPENDS "${SBL_ELF}"
  COMMENT "Signing SBL image with sectools"
  VERBATIM
)

# Sign application image custom command
add_custom_command(
  OUTPUT "${APP_SIGNED_BIN}"
  COMMAND "${Python3_EXECUTABLE}" "${SECTOOLS_SCRIPT}"
          secimage
          -i "${APP_ELF}"
          -c "${SECTOOLS_CONFIG}"
          -sa
          -g app
          -o "${APP_SIGNED_DIR}"
  WORKING_DIRECTORY "${SECTOOLS_DIR}"
  DEPENDS "${APP_ELF}"
  COMMENT "Signing application image with sectools"
  VERBATIM
)

# Define "sign" target that triggers signing
# NOT added to ALL, so it won't run by default
add_custom_target(sign
  DEPENDS "${SBL_SIGNED_BIN}" "${APP_SIGNED_BIN}"
  COMMAND ${CMAKE_COMMAND} -E echo "========================================================================"
  COMMAND ${CMAKE_COMMAND} -E echo "✓ Signing completed successfully!"
  COMMAND ${CMAKE_COMMAND} -E echo "========================================================================"
  COMMAND ${CMAKE_COMMAND} -E echo "Signed SBL:         ${SBL_SIGNED_DIR}"
  COMMAND ${CMAKE_COMMAND} -E echo "Signed Application: ${APP_SIGNED_DIR}"
  COMMAND ${CMAKE_COMMAND} -E echo "========================================================================"
  VERBATIM
)

# Ensure signing depends on ELF generation (if running in same build)
add_dependencies(sign post_process_elf)
