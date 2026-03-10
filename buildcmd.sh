#!/bin/bash

#========================================================================
#Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#SPDX-License-Identifier: BSD-3-Clause
#integration entry
#========================================================================
ws1="$(pwd)"
echo $ws1
cd ../
basedirs="$(pwd)"
subdirs=(
    "bmps_app"
    "power_app"
    "qcli_app"
    "ftm_app"
)
hello_world_app=(
    "zephyr/samples/hello_world"
)
if [ ! -f SRC-IOE-SDK.tar.gz ]; then
    tar --exclude=.git --exclude=.gitignore -czpf SRC-IOE-SDK.tar.gz modules/hal/qcom modules/lib/hostap \
    zephyr modules/fs/littlefs modules/crypto/mbedtls qapp qcc730 modules/debug/segger modules/lib/zcbor modules/hal/cmsis_6
fi
if [ -d prebuilt_HY11 ]; then
    echo "It is HY11 build,copy lib to folder modules\hal\qcom\zephyr\blobs"
    cp ./prebuilt_HY11/*.a ./modules/hal/qcom/zephyr/blobs/
    cp ./prebuilt_HY11/*.bin ./modules/hal/qcom/zephyr/blobs/
    rm -rf ./prop
    echo "start compile with lib"
    cd qapp/qcli_app
    west build -b qcc730mi -d build/qcc730mi | tee build/build_qcc730mi.log
    west build -b qcc730mx -d build/qcc730mx | tee build/build_qcc730mx.log
    west build -b qcc730evbi -d build/qcc730evbi | tee build/build_qcc730evbi.log
    west build -b qcc730evbx -d build/qcc730evbx | tee build/build_qcc730evbx.log
    cd ../
    for d in "${subdirs[@]}"; do
        echo "=== enter $d and build ==="
        cd "$d"
        west build -b qcc730mi -d build/qcc730mi | tee build/build_qcc730mi.log
        west build -b qcc730mx -d build/qcc730mx | tee build/build_qcc730mx.log
        west build -b qcc730evbi -d build/qcc730evbi | tee build/build_qcc730evbi.log
        west build -b qcc730evbx -d build/qcc730evbx | tee build/build_qcc730evbx.log
        cd ..
    done

    cd $basedirs
    for app_path in "${hello_world_app[@]}"; do
        echo "=== enter $app_path and build ==="
        cd "$app_path"
        west build -b qcc730mi -d build/qcc730mi | tee build/build_qcc730mi.log
        west build -b qcc730mx -d build/qcc730mx | tee build/build_qcc730mx.log
        west build -b qcc730evbi -d build/qcc730evbi | tee build/build_qcc730evbi.log
        west build -b qcc730evbx -d build/qcc730evbx | tee build/build_qcc730evbx.log
        cd $basedirs
    done
    
    cd $basedirs
    exit
fi
cd zephyr
newws="$(pwd)"
echo $newws
git update-ref refs/heads/manifest-rev $(git rev-parse HEAD)
cd ../

west init -l qcc730
cd qapp

for d in "${subdirs[@]}"; do
    echo "=== enter $d and build ==="
    cd "$d"
    west build -b qcc730mi -d build/qcc730mi | tee build/build_qcc730mi.log
    west build -b qcc730mx -d build/qcc730mx | tee build/build_qcc730mx.log
    west build -b qcc730evbi -d build/qcc730evbi | tee build/build_qcc730evbi.log
    west build -b qcc730evbx -d build/qcc730evbx | tee build/build_qcc730evbx.log
    cd ..
done

cd $basedirs
for app_path in "${hello_world_app[@]}"; do
    echo "=== enter $app_path and build ==="
    cd "$app_path"
    west build -b qcc730mi -d build/qcc730mi | tee build/build_qcc730mi.log
    west build -b qcc730mx -d build/qcc730mx | tee build/build_qcc730mx.log
    west build -b qcc730evbi -d build/qcc730evbi | tee build/build_qcc730evbi.log
    west build -b qcc730evbx -d build/qcc730evbx | tee build/build_qcc730evbx.log
    cd $basedirs
done

cd $basedirs
ws2="$(pwd)"
echo $ws2
if [ ! -d prebuilt_HY11 ]; then
    mkdir -p prebuilt_HY11
    mkdir -p prebuilt_HY11_ART
    cp ./modules/hal/qcom/zephyr/blobs/libsensorqcc730.a ./prebuilt_HY11/
    cp ./modules/hal/qcom/zephyr/blobs/libcryptoqcc730.a ./prebuilt_HY11/
    cp ./modules/hal/qcom/zephyr/blobs/libpowerqcc730.a ./prebuilt_HY11/
    cp ./modules/hal/qcom/zephyr/blobs/libwifiqcc730.a ./prebuilt_HY11/
    cp ./prop/libwifiqcc730/bin/regdb.bin ./prebuilt_HY11/
    cp -r ./prebuilt_HY11/* ./prebuilt_HY11_ART/
fi
