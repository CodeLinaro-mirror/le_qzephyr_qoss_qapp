# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.20.0)

list(APPEND ZEPHYR_EXTRA_MODULES
  ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/debug/segger
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ftm_app)

target_sources(app PRIVATE 
    src/main.c
    src/ftm_uart_polling.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../prop/libwifiqcc730/halphy/ftm/diag/diag_pkt_handler.c
)
target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(app PRIVATE wifiqcc730)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../prop/libwifiqcc730/halphy/phyrf_svc/core/inc  
    ${CMAKE_CURRENT_SOURCE_DIR}/../../prop/libwifiqcc730/mlm/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../prop/libwifiqcc730/sme/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../../prop/libwifiqcc730/hal/fermion/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../../prop/libwifiqcc730/hal/fermion/inc/fermion
    ${CMAKE_CURRENT_SOURCE_DIR}/../../prop/libwifiqcc730/halphy/phyrf_svc/core/regulatory/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../../prop/libwifiqcc730/coex/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../../prop/libwifiqcc730/config_ini/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../../prop/libwifiqcc730/dpm/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../../prop/libwifiqcc730/halphy/phyrf_svc/core/ani/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../../prop/libwifiqcc730/halphy/rf_drv/trx/fermion/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../prop/libwifiqcc730/halphy/rf_drv/trx/fermion
    ${CMAKE_CURRENT_SOURCE_DIR}/../../prop/libwifiqcc730/halphy/hdl/product/fermion/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../../prop/libwifiqcc730/halphy/hdl/phyDevLib/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../prop/libwifiqcc730/halphy/hdl/phyDevLib/product/common
    ${CMAKE_CURRENT_SOURCE_DIR}/../../prop/libwifiqcc730/halphy/hdl/phyDevLib/product/qcp5321/reset
    ${CMAKE_CURRENT_SOURCE_DIR}/../../prop/libwifiqcc730/halphy/hdl/rfDevLib/product/fermion/cal
    ${CMAKE_CURRENT_SOURCE_DIR}/../../prop/libwifiqcc730/halphy/hdl/phyDevLib/product/qcp5321
    ${CMAKE_CURRENT_SOURCE_DIR}/../../prop/libwifiqcc730/halphy/hdl/phyDevLib/product/qcp5321/PhyDbg
    ${CMAKE_CURRENT_SOURCE_DIR}/../../prop/libwifiqcc730/halphy/hdl/phyDevLib/product/qcp5321/ftpgTx
    ${CMAKE_CURRENT_SOURCE_DIR}/../../prop/libwifiqcc730/halphy/hdl/phyDevLib/product/qcp5321/ftpgRx
    ${CMAKE_CURRENT_SOURCE_DIR}/../../prop/libwifiqcc730/halphy/hdl/autoseq
    ${CMAKE_CURRENT_SOURCE_DIR}/../../prop/libwifiqcc730/halphy/ftm/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../prop/libwifiqcc730/halphy/ftm/tlv20/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../../prop/libwifiqcc730/mlm/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../prop/libwifiqcc730/security/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../../prop/libwifiqcc730/tools/unit_test/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../../prop/libwifiqcc730/wmi/inc
)


target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/hal/qcom/qsystem/common_inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/hal/qcom/qsystem/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/hal/qcom/qsystem/common_inc/hwio/TAPEOUT_07
)

target_include_directories(app PRIVATE
   ${CMAKE_CURRENT_SOURCE_DIR}/../../zephyr/include/zephyr/drivers
   ${CMAKE_CURRENT_SOURCE_DIR}/../../zephyr/include/zephyr
   src/
)

target_compile_definitions(app PRIVATE
    PLATFORM_FERMION
    PLATFORM_TYPE_SILICON
    VARIANT_IMAGE_TYPE_FERMION
    HALMAC_TYPE_FERMION_HAL
    VARIANT_IMAGE_ID_MM
    FTM_OVER_UART
)
