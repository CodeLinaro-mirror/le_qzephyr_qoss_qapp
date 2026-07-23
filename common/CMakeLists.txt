
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

if(NOT CONFIG_SOC_FAMILY_QCOM)
	return()
endif()

add_subdirectory_ifdef(CONFIG_SHELL shell)
add_subdirectory(pm_service)
add_subdirectory_ifdef(CONFIG_QCOM_FW_UPGRADE mcumgr_qcom)
add_subdirectory_ifdef(CONFIG_QAT at_commands)
add_subdirectory_ifdef(CONFIG_HTTPC_AT httpc_at)
add_subdirectory_ifdef(CONFIG_MQTTC_AT mqttc_at)

function(gen_crm_number)
    set(CRM_NUMBER 999)
    set(AU_BUILD_VERSION_FILE /local/mnt/workspace/au_build_version.txt)
    set(BUILD_VERSION_FILE ${CMAKE_CURRENT_SOURCE_DIR}/build_version.txt)
    if(EXISTS ${AU_BUILD_VERSION_FILE})
        set(CRM_NUMBER_SOURCE_FILE ${AU_BUILD_VERSION_FILE})
        file(COPY_FILE ${AU_BUILD_VERSION_FILE} ${BUILD_VERSION_FILE})
    elseif(EXISTS ${BUILD_VERSION_FILE})
        set(CRM_NUMBER_SOURCE_FILE ${BUILD_VERSION_FILE})
    endif()

    if(CRM_NUMBER_SOURCE_FILE)
        file(READ "${CRM_NUMBER_SOURCE_FILE}" FILE_CONTENT)
        string(REGEX MATCH "[^.]+$" LAST_NUMBER "${FILE_CONTENT}")
        string(STRIP "${LAST_NUMBER}" CRM_NUMBER)
    endif()

    zephyr_compile_definitions(CONFIG_QCC730_CRM_NUMBER="${CRM_NUMBER}")
endfunction()

gen_crm_number()
