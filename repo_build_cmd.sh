#!/bin/bash

#========================================================================
#Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#SPDX-License-Identifier: BSD-3-Clause
#integration entry
#========================================================================
pushd ${WS}/zephyr
git update-ref refs/heads/manifest-rev $(git rev-parse HEAD)
popd
pushd ${WS}
west init -l qcc730
west build -b qcc730evbx qapp/power_app/ -d output/powerapp
west build -b qcc730evbx qapp/wifi_app/ -d output/wifiapp

