#!/bin/bash

# Copyright (C) 2025 Huawei Inc.
# Copyright (c) 2024 zhanglin <849679859@qq.com>
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -e
ROOT_DIR=$(cd $(dirname $0);cd ../../../../../; pwd)

KERNEL_VERSION=linux-6.6
DEFCONFIG_FILE=std_emulator_defconfig

pushd ${ROOT_DIR}/kernel/linux/${KERNEL_VERSION}
OUT_PKG_DIR=${ROOT_DIR}/out/std_emulator/packages/phone/images
export PRODUCT_PATH=vendor/oniro/std_emulator
export DEVICE_COMPANY=oniro
export DEVICE_NAME=std_emulator
export PRODUCT_COMPANY=oniro

BUILD_SCRIPT_PATH=${ROOT_DIR}/device/board/oniro/std_emulator

NEWIP_PATCH_FILE=${ROOT_DIR}/kernel/linux/common_modules/newip/apply_newip.sh
TZDRIVER_PATCH_FILE=${ROOT_DIR}/kernel/linux/common_modules/tzdriver/apply_tzdriver.sh
XPM_PATCH_FILE=${ROOT_DIR}/kernel/linux/common_modules/xpm/apply_xpm.sh
CED_PATCH_FILE=${ROOT_DIR}/kernel/linux/common_modules/container_escape_detection/apply_ced.sh
HIDEADDR_PATCH_FILE=${ROOT_DIR}/kernel/linux/common_modules/memory_security/apply_hideaddr.sh
QOS_AUTH_PATCH_FILE=${ROOT_DIR}/kernel/linux/common_modules/qos_auth/apply_qos_auth.sh
UNIFIED_COLLECTION_PATCH_FILE=${ROOT_DIR}/kernel/linux/common_modules/ucollection/apply_ucollection.sh
CODE_SIGN_PATCH_FILE=${ROOT_DIR}/kernel/linux/common_modules/code_sign/apply_code_sign.sh

KERNEL_SRC_TMP_PATH=${ROOT_DIR}/out/kernel/src_tmp/${KERNEL_VERSION}
KERNEL_OBJ_TMP_PATH=${ROOT_DIR}/out/kernel/OBJ/${KERNEL_VERSION}
KERNEL_SOURCE=${ROOT_DIR}/kernel/linux/${KERNEL_VERSION}
KERNEL_PATCH_PATH=${ROOT_DIR}/device/board/oniro/std_emulator/kernel/patch/${KERNEL_VERSION}
KERNEL_PATCH=${ROOT_DIR}/device/board/oniro/std_emulator/kernel/patch/${KERNEL_VERSION}/kernel.patch
KERNEL_HDF_PATCH=${ROOT_DIR}/device/board/oniro/std_emulator/kernel/patch/drivers.patch
HDF_PATCH=${ROOT_DIR}/device/board/oniro/std_emulator/kernel/kernel_patch${KERNEL_VERSION}/std_emulator_patch/hdf.patch
KERNEL_CONFIG_FILE=${ROOT_DIR}/device/board/oniro/std_emulator/kernel/std_emulator_linux_defconfig

rm -rf ${KERNEL_SRC_TMP_PATH}
mkdir -p ${KERNEL_SRC_TMP_PATH}

# rm -rf ${KERNEL_OBJ_TMP_PATH}
# mkdir -p ${KERNEL_OBJ_TMP_PATH}

export KBUILD_OUTPUT=${KERNEL_OBJ_TMP_PATH}

echo "Copy kernel source"
cp -arf ${KERNEL_SOURCE}/* ${KERNEL_SRC_TMP_PATH}/

cd ${KERNEL_SRC_TMP_PATH}

# hdf
echo "HDF patch"
bash ${ROOT_DIR}/drivers/hdf_core/adapter/khdf/linux/patch_hdf.sh ${ROOT_DIR} ${KERNEL_SRC_TMP_PATH} ${KERNEL_PATCH_PATH} ${DEVICE_NAME}

#kernel patch
# Iterate over all .diff files in the DIFF_DIR directory and its subdirectories
find "$KERNEL_PATCH_PATH/kernel_patch" -type f -name "*.patch" | sort | while read -r diff_file; do
              
  # Attempt to apply the patch
  # Use the -p option to strip a number of leading path components from file names in the patch
  # You may need to adjust the -p argument value based on your directory structure
  patch -p1 < "$diff_file"
  # Check the return value of the patch command
  if [ $? -eq 0 ]; then
    echo "Successfully applied patch: $diff_file"
  else
    echo "Failed to apply patch: $diff_file"
  fi
done

#update linux-6.6 kernel stdarg.h path to linux/stdarg.h
if [ ${KERNEL_VERSION} == "linux-6.6" ]
then
    sed -i 's/<stdarg.h>/<linux\/stdarg.h>/' ${KERNEL_SRC_TMP_PATH}/bounds_checking_function/include/securec.h
fi

#newip
if [ -f $NEWIP_PATCH_FILE ]; then
    bash $NEWIP_PATCH_FILE ${ROOT_DIR} ${KERNEL_SRC_TMP_PATH} ${DEVICE_NAME} ${KERNEL_VERSION}
fi

#tzdriver
if [ -f $TZDRIVER_PATCH_FILE ]; then
    bash $TZDRIVER_PATCH_FILE ${ROOT_DIR} ${KERNEL_SRC_TMP_PATH} ${DEVICE_NAME} ${KERNEL_VERSION}
fi

#xpm
if [ -f $XPM_PATCH_FILE ]; then
    bash $XPM_PATCH_FILE ${ROOT_DIR} ${KERNEL_SRC_TMP_PATH} ${DEVICE_NAME} ${KERNEL_VERSION}
fi

#ced
if [ -f $CED_PATCH_FILE ]; then
    bash $CED_PATCH_FILE ${ROOT_DIR} ${KERNEL_SRC_TMP_PATH} ${DEVICE_NAME} ${KERNEL_VERSION}
fi

#qos_auth
if [ -f $QOS_AUTH_PATCH_FILE ]; then
    bash $QOS_AUTH_PATCH_FILE ${ROOT_DIR} ${KERNEL_SRC_TMP_PATH} ${DEVICE_NAME} ${KERNEL_VERSION}
fi

#hideaddr
if [ -f $HIDEADDR_PATCH_FILE ]; then
    bash $HIDEADDR_PATCH_FILE ${ROOT_DIR} ${KERNEL_SRC_TMP_PATH} ${DEVICE_NAME} ${KERNEL_VERSION}
fi

#ucollection
if [ -f $UNIFIED_COLLECTION_PATCH_FILE ]; then
    bash $UNIFIED_COLLECTION_PATCH_FILE ${ROOT_DIR} ${KERNEL_SRC_TMP_PATH} ${DEVICE_NAME} ${KERNEL_VERSION}
fi

#code_sign
if [ -f $CODE_SIGN_PATCH_FILE ]; then
    bash $CODE_SIGN_PATCH_FILE ${ROOT_DIR} ${KERNEL_SRC_TMP_PATH} ${DEVICE_NAME} ${KERNEL_VERSION}
fi

#config
cp -rf ${KERNEL_CONFIG_FILE} ${KERNEL_SRC_TMP_PATH}/arch/x86/configs/${DEFCONFIG_FILE}

CLANG_HOST_TOOLCHAIN=${ROOT_DIR}/prebuilts/clang/ohos/linux-x86_64/llvm/bin
export PATH=${CLANG_HOST_TOOLCHAIN}/:$PATH
MAKE="make LLVM=1 LLVM_IAS=1 "

${MAKE} ${DEFCONFIG_FILE}
${MAKE} bzImage -j$(nproc)
#编译ko模式（未启用）
# ${MAKE} modules -j$(nproc)
# ${MAKE} modules_install

mkdir -p ${OUT_PKG_DIR}

cp -f ${KERNEL_OBJ_TMP_PATH}/arch/x86/boot/bzImage ${OUT_PKG_DIR}
cp -f ${BUILD_SCRIPT_PATH}/kernel/run.bat ${OUT_PKG_DIR}

popd

