#!/bin/bash

# Copyright (c) 2025 Eclipse Oniro for OpenHarmony contributors.
# Copyright (c) 2024 zhanglin <849679859@qq.com>
# SPDX-License-Identifier: Apache-2.0
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

# Called by kernel/BUILD.gn:
#   build_kernel.sh <script dir> <OHOS root> <images out dir> <kernel source>
#                   <script dir> <target_cpu> <product name>
# One script serves both emulator products: the architecture selects the
# defconfig and the kernel image, the product its own out/ directories.
ROOT_DIR=$(cd $(dirname $0);cd ../../../../../; pwd)
TARGET_CPU=${6:-x86_64}
PRODUCT_NAME=${7:-emulator_${TARGET_CPU}}

KERNEL_VERSION=linux-6.6

case "${TARGET_CPU}" in
  x86_64)
    KERNEL_ARCH=x86
    KERNEL_IMAGE=bzImage
    ;;
  arm64)
    KERNEL_ARCH=arm64
    KERNEL_IMAGE=Image
    ;;
  *)
    echo "build_kernel.sh: unsupported target_cpu ${TARGET_CPU}" >&2
    exit 1
    ;;
esac
DEFCONFIG_FILE=emulator_${TARGET_CPU}_defconfig

pushd ${ROOT_DIR}/kernel/linux/${KERNEL_VERSION}
OUT_PKG_DIR=${ROOT_DIR}/out/${PRODUCT_NAME}/packages/phone/images
# hdf_core's khdf Makefile reads the kernel HCS from $PRODUCT_PATH/hdf_config.
# Both emulator products share vendor/oniro/emulator.
export PRODUCT_PATH=vendor/oniro/emulator
export DEVICE_COMPANY=oniro
export DEVICE_NAME=${PRODUCT_NAME}
export PRODUCT_COMPANY=oniro

BUILD_SCRIPT_PATH=${ROOT_DIR}/device/board/oniro/emulator

NEWIP_PATCH_FILE=${ROOT_DIR}/kernel/linux/common_modules/newip/apply_newip.sh
TZDRIVER_PATCH_FILE=${ROOT_DIR}/kernel/linux/common_modules/tzdriver/apply_tzdriver.sh
XPM_PATCH_FILE=${ROOT_DIR}/kernel/linux/common_modules/xpm/apply_xpm.sh
CED_PATCH_FILE=${ROOT_DIR}/kernel/linux/common_modules/container_escape_detection/apply_ced.sh
HIDEADDR_PATCH_FILE=${ROOT_DIR}/kernel/linux/common_modules/memory_security/apply_hideaddr.sh
QOS_AUTH_PATCH_FILE=${ROOT_DIR}/kernel/linux/common_modules/qos_auth/apply_qos_auth.sh
UNIFIED_COLLECTION_PATCH_FILE=${ROOT_DIR}/kernel/linux/common_modules/ucollection/apply_ucollection.sh
CODE_SIGN_PATCH_FILE=${ROOT_DIR}/kernel/linux/common_modules/code_sign/apply_code_sign.sh
DEC_PATCH_FILE=${ROOT_DIR}/kernel/linux/common_modules/dec/apply_dec.sh

# Per product, so that the two emulator kernels never share a tree.
KERNEL_SRC_TMP_PATH=${ROOT_DIR}/out/${PRODUCT_NAME}/kernel/src_tmp/${KERNEL_VERSION}
KERNEL_OBJ_TMP_PATH=${ROOT_DIR}/out/${PRODUCT_NAME}/kernel/OBJ/${KERNEL_VERSION}
KERNEL_SOURCE=${ROOT_DIR}/kernel/linux/${KERNEL_VERSION}
KERNEL_PATCH_PATH=${BUILD_SCRIPT_PATH}/kernel/patch/${KERNEL_VERSION}
HDF_PATCH=${KERNEL_PATCH_PATH}/common_patch/hdf.patch
KERNEL_CONFIG_FILE=${BUILD_SCRIPT_PATH}/kernel/${DEFCONFIG_FILE}

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
bash ${KERNEL_PATCH_PATH}/common_patch/hdf_patch.sh ${ROOT_DIR} ${KERNEL_SRC_TMP_PATH} ${HDF_PATCH}

#kernel patch
# Iterate over all .diff files in the DIFF_DIR directory and its subdirectories
find "$KERNEL_PATCH_PATH/kernel_patch" -type f -name "*.patch" | sort | while read -r diff_file; do
              
  # Attempt to apply the patch
  # Use the -p option to strip a number of leading path components from file names in the patch
  # You may need to adjust the -p argument value based on your directory structure
  # Use 'if' to avoid set -e exiting on partial patch failures
  if patch --forward --batch --reject-file=/dev/null -p1 < "$diff_file"; then
    echo "Successfully applied patch: $diff_file"
  else
    echo "Failed to apply patch (some hunks may have been skipped): $diff_file"
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

#dec
if [ -f $DEC_PATCH_FILE ]; then
    bash $DEC_PATCH_FILE ${ROOT_DIR} ${KERNEL_SRC_TMP_PATH} ${DEVICE_NAME} ${KERNEL_VERSION}
fi

#config
cp -rf ${KERNEL_CONFIG_FILE} ${KERNEL_SRC_TMP_PATH}/arch/${KERNEL_ARCH}/configs/${DEFCONFIG_FILE}

CLANG_HOST_TOOLCHAIN=${ROOT_DIR}/prebuilts/clang/ohos/linux-x86_64/llvm/bin
export PATH=${CLANG_HOST_TOOLCHAIN}/:$PATH
MAKE="make ARCH=${KERNEL_ARCH} LLVM=1 LLVM_IAS=1 "

${MAKE} ${DEFCONFIG_FILE}
${MAKE} ${KERNEL_IMAGE} -j$(nproc)
#编译ko模式（未启用）
# ${MAKE} modules -j$(nproc)
# ${MAKE} modules_install

mkdir -p ${OUT_PKG_DIR}

cp -f ${KERNEL_OBJ_TMP_PATH}/arch/${KERNEL_ARCH}/boot/${KERNEL_IMAGE} ${OUT_PKG_DIR}
cp -f ${BUILD_SCRIPT_PATH}/kernel/run.bat ${OUT_PKG_DIR}
cp -f ${BUILD_SCRIPT_PATH}/kernel/run.sh ${OUT_PKG_DIR}

popd

