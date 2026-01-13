# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(qcc730_host)

# ============================================================================
# OSAL (Operating System Abstraction Layer)
# ============================================================================
# Add Zephyr OSAL implementation
target_sources(app PRIVATE
  ../../Port/osal/zephyr/qc_osal_zephyr.c
)

target_include_directories(app PRIVATE
  ../../Port/osal
)

# ============================================================================
# Platform Hardware Abstraction Layer
# ============================================================================
# Add STM32 platform implementation for Zephyr
target_sources(app PRIVATE
  ../../Port/platform/stm32/qc_platform_stm32_zephyr.c
)

target_include_directories(app PRIVATE
  ../../Port/
  ../../Port/platform
)

# ============================================================================
# QCSPI Driver
# ============================================================================
target_sources(app PRIVATE
  ../../Service/qcspi/qcspi_driver.c
)

target_include_directories(app PRIVATE
  ../../Service/qcspi
)

# ============================================================================
# Transport Layer
# ============================================================================
target_sources(app PRIVATE
  ../../Service/transport/qcspi_transport.c
)

target_include_directories(app PRIVATE
  ../../Service/transport
)

# ============================================================================
# Ring Service
# ============================================================================
target_sources(app PRIVATE
../../Service/ring/ring_service_host.c
)

target_include_directories(app PRIVATE
../../Service/ring
)

# ============================================================================
# Application Source
# ============================================================================
target_sources(app PRIVATE src/main.c)
