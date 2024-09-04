#!/bin/bash
# Copyright (c) 2022 Diemit <598757652@qq.com>
# Copyright (c) 2024 Institute of Software, Chinese Academy of Sciences. 
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

export PROJECT_ROOT=$1
export PRODUCT_PATH=$2
export KERNEL_ARCH=$3
export KERNEL_VERSION=$4
export KERNEL_IMAGE=$5
export DEFCONFIG_FILE=$6
export OUT_PKG_DIR=$7
export KERNEL_SRC_TMP_PATH=${PROJECT_ROOT}/out/kernel/src_tmp/${KERNEL_VERSION}
export KERNEL_OBJ_TMP_PATH=${PROJECT_ROOT}/out/kernel/OBJ/${KERNEL_VERSION}
export KERNEL_IMAGE_FILE=${KERNEL_OBJ_TMP_PATH}/arch/${KERNEL_ARCH}/boot/${KERNEL_IMAGE}

if [ $KERNEL_ARCH == "arm" ];then
    LINUX_KERNEL_MAKEFILE=${PROJECT_ROOT}/device/board/${product_company}/common/kernel/make_kernel_32.mk
else
    LINUX_KERNEL_MAKEFILE=${PROJECT_ROOT}/device/board/${product_company}/common/kernel/make_kernel_64.mk
fi


LINUX_dtb_MAKEFILE=${PROJECT_ROOT}/device/board/${product_company}/common/kernel/make_dtb.mk

rm -f ${KERNEL_IMAGE_FILE}

export KBUILD_OUTPUT=${KERNEL_OBJ_TMP_PATH}

make -f ${LINUX_dtb_MAKEFILE}
make -f ${LINUX_KERNEL_MAKEFILE}

if [ -f ${KERNEL_IMAGE_FILE} ];then
    mkdir -p ${OUT_PKG_DIR}
    cp ${KERNEL_IMAGE_FILE} ${OUT_PKG_DIR}/${KERNEL_IMAGE}
    echo "Image: ${KERNEL_IMAGE_FILE} build success"
else
    echo "Image: ${KERNEL_IMAGE_FILE} build failed!!!"
    exit 1
fi