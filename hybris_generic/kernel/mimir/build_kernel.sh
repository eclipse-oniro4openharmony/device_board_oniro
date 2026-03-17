#!/bin/bash

# Copyright (C) 2025-2026 Huawei Device Co., Ltd.
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
ROOT_DIR=$(cd $(dirname $0);cd ../../../../../../; pwd)

KERNEL_VERSION=linux-5.10
PRODUCT_NAME=hybris_generic
DEVICE_NAME=mimir

# Paths
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_TREE="$ROOT_DIR/kernel/linux/volla-mimir"
KERNEL_SRC="$KERNEL_TREE/build-dir/downloads/android_kernel_volla_mt8781"
OUT_DIR="$KERNEL_TREE/out"

# Temporary workspace for clean builds
KERNEL_SRC_TMP_PATH="$ROOT_DIR/out/kernel/src_tmp/volla-mimir"
mkdir -p "$ROOT_DIR/out/kernel/src_tmp/"

# 1. Ensure kernel tree is initialized
if [ ! -d "$KERNEL_TREE" ]; then
    echo "Cloning volla-mimir..."
    git clone https://gitlab.com/ubports/porting/reference-device-ports/halium13/volla-tablet/volla-mimir.git "$KERNEL_TREE"
fi

# Initialize build tools and sub-repos if needed
if [ ! -d "$KERNEL_SRC" ]; then
    cd "$KERNEL_TREE"
    ./build.sh -b build-dir -c
fi

# 2. Copy to temporary workspace for a clean build
echo "Copying kernel source to temporary workspace..."
rm -rf "$KERNEL_SRC_TMP_PATH"
mkdir -p "$KERNEL_SRC_TMP_PATH"
if [ -d "${KERNEL_SRC}.orig" ]; then
    cp -arf "${KERNEL_SRC}.orig"/* "$KERNEL_SRC_TMP_PATH/"
else
    cp -arf "$KERNEL_SRC"/* "$KERNEL_SRC_TMP_PATH/"
fi

# 3. Patching
cd "$KERNEL_SRC_TMP_PATH"
PATCH_PATH="$HERE/patch/linux-5.10"

# Apply volla-mimir, halium-build-tools, and libufdt patches ONLY once to the source tree if needed
echo "Applying Volla and Halium build tools patches..."
pushd "$KERNEL_TREE"
patch -N -p1 < "$PATCH_PATH/volla-mimir.patch" || echo "Volla patch already applied or failed"
pushd build
patch -N -p1 < "$PATCH_PATH/halium-generic-adaptation-build-tools.patch" || echo "Halium build tools patch already applied or failed"
popd
pushd build-dir/downloads/libufdt
patch -N -p1 < "$PATCH_PATH/libufdt.patch" || echo "libufdt patch already applied or failed"
popd
popd

# HDF patch using hdf_patch.sh
echo "Applying HDF patch..."
HDF_PATCH="$PATCH_PATH/common_patch/hdf.patch"
bash "$PATCH_PATH/common_patch/hdf_patch.sh" "$ROOT_DIR" "$KERNEL_SRC_TMP_PATH" "$HDF_PATCH"

# Apply OpenHarmony adaptation patch
echo "Applying OpenHarmony adaptation patch..."
patch -N -p1 < "$PATCH_PATH/kernel_patch/ohos_adaptation.patch" || echo "OHOS adaptation patch already applied or failed"

# Apply QoS Auth
echo "Applying QoS Auth..."
bash "$ROOT_DIR/kernel/linux/common_modules/qos_auth/apply_qos_auth.sh" "$ROOT_DIR" "$KERNEL_SRC_TMP_PATH"

# Copy OpenHarmony config
cp "$PATCH_PATH/kernel_patch/openharmony.config" "$KERNEL_SRC_TMP_PATH/arch/arm64/configs/openharmony.config"

# Build hc-gen
echo "Building hc-gen..."
make -C "$ROOT_DIR/drivers/hdf_core/framework/tools/hc-gen" BUILD_DIR="$ROOT_DIR/drivers/hdf_core/framework/tools/hc-gen/build/"

# 4. Building using the Halium build system
echo "Building kernel..."
# Symlink the temporary source back into the Halium tree to trick build.sh
if [ ! -L "$KERNEL_SRC" ]; then
    mv "$KERNEL_SRC" "${KERNEL_SRC}.orig"
    ln -sf "$KERNEL_SRC_TMP_PATH" "$KERNEL_SRC"
fi

# Set PRODUCT_PATH and build
cd "$KERNEL_TREE"
export PRODUCT_PATH=vendor/oniro/hybris_generic
./build.sh -b build-dir -k

echo "Copying artifacts to $OUT_DIR..."
mkdir -p "$OUT_DIR"
cp "$KERNEL_TREE/build-dir/tmp/partitions/boot.img" "$OUT_DIR/"
cp "$KERNEL_TREE/build-dir/tmp/partitions/dtbo.img" "$OUT_DIR/"
cp "$KERNEL_TREE/build-dir/tmp/partitions/vendor_boot.img" "$OUT_DIR/"

echo "Packaging modules..."
tar -czf "$OUT_DIR/modules.tar.gz" -C "$KERNEL_TREE/build-dir/tmp/system/lib" modules

echo "Build complete. Artifacts are in $OUT_DIR"
ls -l "$OUT_DIR"
