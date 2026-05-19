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
DEVICE_NAME=x23

# Paths
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_TREE="$ROOT_DIR/kernel/linux/volla-vidofnir"
KERNEL_SRC="$KERNEL_TREE/build-dir/downloads/kernel-volla-mt6789"
OUT_DIR="$KERNEL_TREE/out"

# Temporary workspace for clean builds
KERNEL_SRC_TMP_PATH="$ROOT_DIR/out/kernel/src_tmp/volla-x23"
mkdir -p "$ROOT_DIR/out/kernel/src_tmp/"

# 1. Ensure kernel tree is initialized
if [ ! -d "$KERNEL_TREE" ]; then
    echo "Cloning volla-vidofnir..."
    git clone https://gitlab.com/ubports/porting/reference-device-ports/halium12/volla-x23/volla-vidofnir.git "$KERNEL_TREE"
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

# 3. Patch the kernel-port repo and the kernel source.
#
# All kernel modifications live under kernel/x23/ — this script is their
# single owner (system_patch/ deliberately does not touch the kernel; the
# kernel-port repo and the MT6789 source are not OHOS-checkout repos).
# There are two distinct target trees, hence two patch sets — see
# patches/README.md:
#   patches/port-repo/     -> the volla-vidofnir clone ($KERNEL_TREE)
#   patches/kernel-source/ -> the downloaded MT6789 source (temp copy)
#   config/                -> kernel defconfig fragment
PATCHES="$HERE/patches"
CONFIG_DIR="$HERE/config"

# 3a. Port-repo patches — deviceinfo, Halium build tools, libufdt.  These
# live in $KERNEL_TREE (which persists between builds), so `patch -N`
# no-ops cleanly when they are already applied.
echo "Applying port-repo patches..."
patch -N -p1 -d "$KERNEL_TREE" \
      < "$PATCHES/port-repo/deviceinfo.patch"  || echo "  deviceinfo: already applied"
patch -N -p1 -d "$KERNEL_TREE/build" \
      < "$PATCHES/port-repo/build-tools.patch" || echo "  build-tools: already applied"
patch -N -p1 -d "$KERNEL_TREE/build-dir/downloads/libufdt" \
      < "$PATCHES/port-repo/libufdt.patch"     || echo "  libufdt: already applied"

# 3b. Kernel-source patches — applied to the fresh temp copy, which is
# recreated every build, so a clean `patch -p1` always applies (a failure
# is a real error and aborts via `set -e`).
#
# HDF is helper-script driven (the script does symlink/copy fixups beyond
# the plain patch); the rest are plain patches applied in glob order.
# sharefs.patch adds fs/sharefs/ and wires it into fs/Kconfig + fs/Makefile.
echo "Applying HDF patch..."
bash "$PATCHES/kernel-source/hdf_patch.sh" \
     "$ROOT_DIR" "$KERNEL_SRC_TMP_PATH" "$PATCHES/kernel-source/hdf.patch"

for p in "$PATCHES"/kernel-source/*.patch; do
    [ "$(basename "$p")" = "hdf.patch" ] && continue   # applied via hdf_patch.sh above
    echo "Applying $(basename "$p")..."
    patch -p1 -d "$KERNEL_SRC_TMP_PATH" < "$p"
done

# QoS Auth (helper-script driven, like HDF).
echo "Applying QoS Auth..."
bash "$ROOT_DIR/kernel/linux/common_modules/qos_auth/apply_qos_auth.sh" "$ROOT_DIR" "$KERNEL_SRC_TMP_PATH"

# Kernel defconfig fragment.
cp "$CONFIG_DIR/openharmony.config" "$KERNEL_SRC_TMP_PATH/arch/arm64/configs/openharmony.config"

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

# ---------------------------------------------------------------------------
# Stage the extra GPU + touch kernel modules into the vendor_boot overlay.
#
# These modules (Mali GPU stack, MTK coprocessors, chipone-tddi touch) are NOT
# in Halium's `modules.load`, so `make-bootimage.sh`'s dep-resolution
# never copies them into vendor_boot.  We need them bundled anyway —
# OHOS init.x23.cfg `insmod`s them from /mnt/kmodules at pre-init (the
# chainload stashes vendor_boot's /lib/modules there).  See
# native_boot_plan/phase_n8_graphics_native.md §N8.11 / §N8.13.
#
# The module *list* (extra-modules.list) is tracked in git; the .ko
# binaries are NOT — they are build artifacts, regenerated here every
# build so their vermagic always matches the freshly-built kernel.
# Two-pass: the first `./build.sh -k` above compiled the kernel + a
# vendor_boot WITHOUT these .ko; we copy them into the overlay now and
# re-run so the second-pass vendor_boot includes them (kernel is cached,
# so the second pass only re-assembles the boot images — fast).
OVERLAY_MODS="$KERNEL_TREE/vendor-ramdisk-overlay/lib/modules"
EXTRA_LIST="$HERE/extra-modules.list"
BUILT_MODS="$(find "$KERNEL_TREE/build-dir/tmp/system/lib/modules" \
                   -maxdepth 1 -type d -name '5.*' | head -1)"
if [ -f "$EXTRA_LIST" ] && [ -n "$BUILT_MODS" ]; then
    echo "Staging extra GPU/touch modules into vendor_boot overlay..."
    staged=0
    while read -r mod; do
        case "$mod" in ''|'#'*) continue ;; esac
        src="$(find "$BUILT_MODS" -name "$mod" -type f | head -1)"
        if [ -n "$src" ]; then
            cp -f "$src" "$OVERLAY_MODS/$mod"
            staged=$((staged + 1))
        else
            echo "  WARNING: extra module $mod not found in build output"
        fi
    done < "$EXTRA_LIST"
    echo "  staged $staged extra modules; re-assembling vendor_boot..."
    ./build.sh -b build-dir -k
fi

echo "Copying artifacts to $OUT_DIR..."
mkdir -p "$OUT_DIR"
cp "$KERNEL_TREE/build-dir/tmp/partitions/boot.img" "$OUT_DIR/"
cp "$KERNEL_TREE/build-dir/tmp/partitions/dtbo.img" "$OUT_DIR/"
cp "$KERNEL_TREE/build-dir/tmp/partitions/vendor_boot.img" "$OUT_DIR/"

echo "Packaging modules..."
tar -czf "$OUT_DIR/modules.tar.gz" -C "$KERNEL_TREE/build-dir/tmp/system/lib" modules

echo "Build complete. Artifacts are in $OUT_DIR"
ls -l "$OUT_DIR"
