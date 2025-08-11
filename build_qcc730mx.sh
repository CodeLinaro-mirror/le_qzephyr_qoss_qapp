#!/bin/bash

#========================================================================
#Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#SPDX-License-Identifier: BSD-3-Clause
#integration entry
#========================================================================

west build -b qcc730mx -d build/qcc730mx | tee build/build_qcc730mx.log
