#!/bin/bash

#========================================================================
#Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#SPDX-License-Identifier: BSD-3-Clause
#integration entry
#========================================================================
ws1="$(pwd)"
echo $ws1
cd ../zephyr
newws="$(pwd)"
echo $newws
git update-ref refs/heads/manifest-rev $(git rev-parse HEAD)
cd ../
west init -l qcc730
cd qapp/power_app
source ../build_qcc730evbx.sh
source ../build_qcc730mx.sh
cd ../wifi_app
source ../build_qcc730evbx.sh
source ../build_qcc730mx.sh
