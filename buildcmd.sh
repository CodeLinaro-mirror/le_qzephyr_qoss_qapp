#!/bin/bash

#========================================================================
#Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#SPDX-License-Identifier: BSD-3-Clause
#integration entry
#========================================================================
ws1="$(pwd)"
echo $ws1
cd ..
if [ ! -f SRC-IOE-SDK.tar.gz ]; then
    tar --exclude=.git --exclude=.gitignore -czpf SRC-IOE-SDK.tar.gz modules/hal/cmsis modules/hal/qcom modules/lib/hostap \
    zephyr prop/libcryptoqcc730 prop/libpowerqcc730 prop/libsensorqcc730 prop/libwifiqcc730 modules/fs/littlefs \
    modules/crypto/mbedtls qapp qcc730 modules/debug/segger modules/lib/zcbor
fi
cd zephyr
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

