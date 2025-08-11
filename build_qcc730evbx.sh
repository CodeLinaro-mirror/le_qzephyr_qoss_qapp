#!/bin/bash

#========================================================================
#Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#SPDX-License-Identifier: BSD-3-Clause
#integration entry
#========================================================================

west build -b qcc730evbx -d build/qcc730evbx | tee build/build_qcc730evbx.log
