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
west build -b qcc730evbx qapp/power_app/ -d build/powerapp
west build -b qcc730evbx qapp/wifi_app/ -d build/wifiapp

