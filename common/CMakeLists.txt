
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause

if(NOT CONFIG_SOC_FAMILY_QCOM)
	return()
endif()

add_subdirectory_ifdef(CONFIG_SHELL shell)