#!/bin/bash

#========================================================================
#Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#SPDX-License-Identifier: BSD-3-Clause
#integration entry
#========================================================================
ws1="$(pwd)"
echo $ws1
cd ../
if [ ! -f SRC-IOE-SDK.tar.gz ]; then
    tar --exclude=.git --exclude=.gitignore -czpf SRC-IOE-SDK.tar.gz modules/hal/cmsis modules/hal/qcom modules/lib/hostap \
    zephyr modules/fs/littlefs modules/crypto/mbedtls qapp qcc730 modules/debug/segger modules/lib/zcbor
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
cd ../../
ws2="$(pwd)"
echo $ws2
if [ ! -d prebuilt_HY11 ]; then
    mkdir -p prebuilt_HY11
    mkdir -p prebuilt_HY11_ART
    cp ./modules/hal/qcom/zephyr/blobs/libsensorqcc730.a ./prebuilt_HY11/
    cp ./modules/hal/qcom/zephyr/blobs/libcryptoqcc730.a ./prebuilt_HY11/
    cp ./modules/hal/qcom/zephyr/blobs/libpowerqcc730.a ./prebuilt_HY11/
    cp ./modules/hal/qcom/zephyr/blobs/libwifiqcc730.a ./prebuilt_HY11/
    cp -r ./prebuilt_HY11/* ./prebuilt_HY11_ART/
fi
