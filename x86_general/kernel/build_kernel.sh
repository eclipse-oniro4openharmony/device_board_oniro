#!/bin/bash
# Copyright (c) 2024 Diemit <598757652@qq.com>
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
DEFCONFIG_FILE=pocket2_oh_defconfig
export DEVICE_NAME=x86_general
export PRODUCT_COMPANY=oniro
export PRODUCT_PATH=vendor/${PRODUCT_COMPANY}/${DEVICE_NAME}
OUT_PKG_DIR=${ROOT_DIR}/out/${DEVICE_NAME}/packages/phone/images

KERNEL_SRC_TMP_PATH=${ROOT_DIR}/out/${DEVICE_NAME}/kernel/src_tmp/${KERNEL_VERSION}
KERNEL_OBJ_TMP_PATH=${ROOT_DIR}/out/${DEVICE_NAME}/kernel/OBJ/${KERNEL_VERSION}
KERNEL_SOURCE=${ROOT_DIR}/kernel/linux/${KERNEL_VERSION}
KERNEL_PATCH_PATH=${ROOT_DIR}/device/board/${PRODUCT_COMPANY}/${DEVICE_NAME}/kernel/kernel_patch/
HDF_PATCH=${KERNEL_PATCH_PATH}/hdf.patch
KERNEL_PATCH=${ROOT_DIR}/kernel/linux/patches/${KERNEL_VERSION}/rk3568_patch/kernel.patch

NEWIP_PATCH_FILE=${ROOT_DIR}/kernel/linux/common_modules/newip/apply_newip.sh
TZDRIVER_PATCH_FILE=${ROOT_DIR}/kernel/linux/common_modules/tzdriver/apply_tzdriver.sh
XPM_PATCH_FILE=${ROOT_DIR}/kernel/linux/common_modules/xpm/apply_xpm.sh
CED_PATCH_FILE=${ROOT_DIR}/kernel/linux/common_modules/container_escape_detection/apply_ced.sh
HIDEADDR_PATCH_FILE=${ROOT_DIR}/kernel/linux/common_modules/memory_security/apply_hideaddr.sh
QOS_AUTH_PATCH_FILE=${ROOT_DIR}/kernel/linux/common_modules/qos_auth/apply_qos_auth.sh
UNIFIED_COLLECTION_PATCH_FILE=${ROOT_DIR}/kernel/linux/common_modules/ucollection/apply_ucollection.sh
CODE_SIGN_PATCH_FILE=${ROOT_DIR}/kernel/linux/common_modules/code_sign/apply_code_sign.sh
DEC_PATCH_FILE=${ROOT_DIR}/kernel/linux/common_modules/dec/apply_dec.sh

KERNEL_CONFIG_FILE=${ROOT_DIR}/device/board/${PRODUCT_COMPANY}/${DEVICE_NAME}/kernel/configs/${DEFCONFIG_FILE}
#编译ko模式（未启用）
OH_CONFIG_FILE=${ROOT_DIR}/device/board/${PRODUCT_COMPANY}/${DEVICE_NAME}/kernel/configs/oh_defconfig
DEVICE_CONFIG_FILE=${ROOT_DIR}/device/board/${PRODUCT_COMPANY}/${DEVICE_NAME}/kernel/configs/pocket2_defconfig

export KBUILD_OUTPUT=${KERNEL_OBJ_TMP_PATH}
export INSTALL_MOD_PATH=${OUT_PKG_DIR}/../../../driver_modules

source ${ROOT_DIR}/device/board/${PRODUCT_COMPANY}/${DEVICE_NAME}/kernel/kernel_source_checker.sh

function copy_and_patch_kernel_source()
{
    rm -rf ${KERNEL_SRC_TMP_PATH}
    mkdir -p ${KERNEL_SRC_TMP_PATH}

    rm -rf ${KERNEL_OBJ_TMP_PATH}
    mkdir -p ${KERNEL_OBJ_TMP_PATH}

    cp -arf ${KERNEL_SOURCE}/* ${KERNEL_SRC_TMP_PATH}/

    cd ${KERNEL_SRC_TMP_PATH}

    #打入内核补丁
    if [ ${KERNEL_VERSION} == "linux-5.10" ]
    then
        patch -p1 < ${KERNEL_PATCH_PATH}/0001-remove-get_fs.patch
    fi

    if [ ${KERNEL_VERSION} == "linux-6.6" ]
    then
        patch -p1 < ${KERNEL_PATCH_PATH}/0001-fix-hungtask.patch
        patch -p1 < ${KERNEL_PATCH_PATH}/0001-fix-.h.patch
        patch -p1 < ${KERNEL_PATCH_PATH}/0001-fix-build.patch
        patch -p1 < ${KERNEL_PATCH_PATH}/0002-fix-watchdog.patch
        patch -p1 < ${KERNEL_PATCH_PATH}/0001-fix-scripts.patch
        patch -p1 < ${KERNEL_PATCH_PATH}/0001-ashmem.patch
    fi

    patch -p1 < ${KERNEL_PATCH_PATH}/0002-OHPC-sharefs.patch

    #HDF patch
    echo "HDF patch"
    bash ${KERNEL_PATCH_PATH}/hdf_patch.sh ${ROOT_DIR} ${KERNEL_SRC_TMP_PATH} ${HDF_PATCH}

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

}

set +e
is_kernel_change ${ROOT_DIR}
KERNEL_SOURCE_CHANGED=$?
set -e
if [ ${KERNEL_SOURCE_CHANGED}  -ne 0 ]; then
    echo "kernel or it's deps changed, start source update."
    copy_and_patch_kernel_source
else
    echo "no changes to kernel, skip source copy."
fi

#config
cp -rf ${KERNEL_CONFIG_FILE} ${KERNEL_SRC_TMP_PATH}/arch/x86/configs/${DEFCONFIG_FILE}
#编译ko模式（未启用）
# bash ${KERNEL_SRC_TMP_PATH}/scripts/kconfig/merge_config.sh -O ${KERNEL_OBJ_TMP_PATH}/ -m ${DEVICE_CONFIG_FILE} ${OH_CONFIG_FILE}

cd ${KERNEL_SRC_TMP_PATH}

export PATH=${ROOT_DIR}/prebuilts/clang/ohos/linux-x86_64/llvm/bin/:${ROOT_DIR}/prebuilts/develop_tools/pahole/bin/:$PATH
MAKE="make LLVM=1 LLVM_IAS=1 "

${MAKE} ${DEFCONFIG_FILE}
${MAKE} bzImage -j$(nproc)
#编译ko模式（未启用）
# ${MAKE} modules -j$(nproc)
# ${MAKE} modules_install

mkdir -p ${OUT_PKG_DIR}

cp -f ${KERNEL_OBJ_TMP_PATH}/arch/x86/boot/bzImage ${OUT_PKG_DIR}

if [ ${KERNEL_SOURCE_CHANGED} -ne 0 ]; then
    cp ${ROOT_DIR}/out/${DEVICE_NAME}/kernel/checkpoint/last_build.info ${ROOT_DIR}/out/${DEVICE_NAME}/kernel/checkpoint/last_build.backup
    cp ${ROOT_DIR}/out/${DEVICE_NAME}/kernel/checkpoint/current_build.info ${ROOT_DIR}/out/${DEVICE_NAME}/kernel/checkpoint/last_build.info
    echo "kernel compile finish, save build info."
else
    echo "kernel compile finish."
fi
