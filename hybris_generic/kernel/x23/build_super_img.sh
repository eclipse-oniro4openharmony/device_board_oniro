#!/bin/bash
#
# Copyright (C) 2026 Oniro / Hybris Generic.
# Licensed under the Apache License, Version 2.0 (the "License").
#
# Build out/hybris_generic/super.img from the OHOS-built system.img,
# vendor.img, sys_prod.img, and chip_prod.img.  Uses `lpmake` (shipped
# with the Halium kernel build tools) to produce a flashable LP-formatted
# super partition.
#
# Output:  $OHOS_ROOT/out/hybris_generic/super.img
#
# Inputs:
#   - $OHOS_ROOT/out/hybris_generic/packages/phone/images/system.img
#   - $OHOS_ROOT/out/hybris_generic/packages/phone/images/vendor.img
#   - $OHOS_ROOT/out/hybris_generic/packages/phone/images/sys_prod.img
#   - $OHOS_ROOT/out/hybris_generic/packages/phone/images/chip_prod.img
#
# Flash with:
#   fastboot reboot fastboot                     # enter fastbootd
#   fastboot flash super out/hybris_generic/super.img
#
# parse-android-dynparts (in the chainload's Halium ramdisk) reads the LP
# metadata at runtime and materialises /dev/mapper/{system,vendor,sys_prod,chip_prod}_a.
# OHOS second-stage init then mounts sys_prod_a + chip_prod_a via fstab.x23.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OHOS_ROOT="$(cd "$HERE/../../../../../.." && pwd)"
KERNEL_TREE="$OHOS_ROOT/kernel/linux/volla-vidofnir"
LPMAKE="$KERNEL_TREE/build-dir/downloads/kernel-build-tools/linux-x86/bin/lpmake"
SYSTEM_IMG="$OHOS_ROOT/out/hybris_generic/packages/phone/images/system.img"
VENDOR_IMG="$OHOS_ROOT/out/hybris_generic/packages/phone/images/vendor.img"
SYS_PROD_IMG="$OHOS_ROOT/out/hybris_generic/packages/phone/images/sys_prod.img"
CHIP_PROD_IMG="$OHOS_ROOT/out/hybris_generic/packages/phone/images/chip_prod.img"
OUTPUT="$OHOS_ROOT/out/hybris_generic/super.img"

# Volla X23 super partition geometry (from x23-super.txt reference).
SUPER_SIZE=9663676416   # 9.66 GB
METADATA_SIZE=65536     # 64 KB
METADATA_SLOTS=2        # A/B slots
BLOCK_SIZE=4096

for f in "$LPMAKE" "$SYSTEM_IMG" "$VENDOR_IMG" "$SYS_PROD_IMG" "$CHIP_PROD_IMG"; do
    [[ -f "$f" ]] || { echo "Error: $f missing" >&2; exit 1; }
done

SYS_SZ=$(stat -c %s "$SYSTEM_IMG")
VEN_SZ=$(stat -c %s "$VENDOR_IMG")
SP_SZ=$(stat -c %s "$SYS_PROD_IMG")
CP_SZ=$(stat -c %s "$CHIP_PROD_IMG")
echo "system.img:    $SYS_SZ bytes"
echo "vendor.img:    $VEN_SZ bytes"
echo "sys_prod.img:  $SP_SZ bytes"
echo "chip_prod.img: $CP_SZ bytes"

# Group budget: room for all four _a partitions plus a bit of slack.  The
# total must fit inside SUPER_SIZE / METADATA_SLOTS (LP reserves half
# the super for each A/B slot's metadata).
GROUP_SIZE=$(( SUPER_SIZE / 2 - 1024 * 1024 ))
echo "group budget:  $GROUP_SIZE bytes"

"$LPMAKE" \
    --metadata-size "$METADATA_SIZE" \
    --metadata-slots "$METADATA_SLOTS" \
    --block-size "$BLOCK_SIZE" \
    --device super:"$SUPER_SIZE" \
    --group main_a:"$GROUP_SIZE" \
    --partition system_a:readonly:"$SYS_SZ":main_a --image system_a="$SYSTEM_IMG" \
    --partition vendor_a:readonly:"$VEN_SZ":main_a --image vendor_a="$VENDOR_IMG" \
    --partition sys_prod_a:readonly:"$SP_SZ":main_a --image sys_prod_a="$SYS_PROD_IMG" \
    --partition chip_prod_a:readonly:"$CP_SZ":main_a --image chip_prod_a="$CHIP_PROD_IMG" \
    --sparse \
    --output "$OUTPUT"

echo
echo "Built $OUTPUT ($(stat -c %s "$OUTPUT") bytes)"
echo
echo "Flash with:"
echo "  fastboot reboot fastboot                              # enter fastbootd"
echo "  fastboot flash super $OUTPUT"
