#!/bin/bash
#
# Copyright (C) 2026 Oniro / Hybris Generic.
# Licensed under the Apache License, Version 2.0 (the "License").
#
# Build out/hybris_generic/super.img for the Volla Phone Plinius (ansuz)
# from the OHOS-built images + the Halium blobs dumped from the device.
#
# Geometry facts (P0 recon): ansuz `super` is 9663676416 bytes — the
# SAME as the X23 — so the lpmake constants carry over. The Halium side
# gains vendor_dlkm + system_dlkm (both EROFS, mounted by the chainload;
# the stock vendor's module tree lives there).
#
# Inputs:
#   - $OHOS_ROOT/out/hybris_generic/packages/phone/images/{system,vendor,sys_prod,chip_prod}.img
#   - halium-blobs/ansuz/halium_{system,vendor,vendor_dlkm,system_dlkm}_a.img
#     (dumped from the UT-running device — see plinius_recon.md; re-dump
#     with the adb recipes there if missing)
#
# Flash from LK fastboot (raw physical-partition write, no fastbootd):
#   fastboot flash super out/hybris_generic/super.img

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OHOS_ROOT="$(cd "$HERE/../../../../../.." && pwd)"
KERNEL_TREE="$OHOS_ROOT/kernel/linux/volla-ansuz"
LPMAKE="$KERNEL_TREE/workdir/downloads/kernel-build-tools/linux-x86/bin/lpmake"
IMAGES="$OHOS_ROOT/out/hybris_generic/packages/phone/images"
BLOBS="$OHOS_ROOT/device/board/oniro/hybris_generic/halium-blobs/ansuz"
OUTPUT="$OHOS_ROOT/out/hybris_generic/super.img"

# Volla Plinius super partition geometry (== X23's).
SUPER_SIZE=9663676416
METADATA_SIZE=65536
METADATA_SLOTS=2
BLOCK_SIZE=4096

SYSTEM_IMG="$IMAGES/system.img"
VENDOR_IMG="$IMAGES/vendor.img"
SYS_PROD_IMG="$IMAGES/sys_prod.img"
CHIP_PROD_IMG="$IMAGES/chip_prod.img"

for f in "$LPMAKE" "$SYSTEM_IMG" "$VENDOR_IMG" "$SYS_PROD_IMG" "$CHIP_PROD_IMG"; do
    [[ -f "$f" ]] || { echo "Error: $f missing" >&2; exit 1; }
done

declare -a LPMAKE_ARGS=()
TOTAL=0
add_part() {  # name image
    local sz
    sz=$(stat -c %s "$2")
    LPMAKE_ARGS+=( --partition "$1":readonly:"$sz":main_a --image "$1"="$2" )
    TOTAL=$(( TOTAL + sz ))
    printf '%-24s %12d bytes\n' "$1" "$sz"
}

add_part system_a    "$SYSTEM_IMG"
add_part vendor_a    "$VENDOR_IMG"
add_part sys_prod_a  "$SYS_PROD_IMG"
add_part chip_prod_a "$CHIP_PROD_IMG"

# Halium blobs — optional as a set (graphics-disabled builds boot
# without them), but if the core pair is present the dlkm images should
# be too on this device.
if [[ -f "$BLOBS/halium_system_a.img" && -f "$BLOBS/halium_vendor_a.img" ]]; then
    add_part halium_system_a "$BLOBS/halium_system_a.img"
    add_part halium_vendor_a "$BLOBS/halium_vendor_a.img"
    [[ -f "$BLOBS/halium_vendor_dlkm_a.img" ]] && add_part halium_vendor_dlkm_a "$BLOBS/halium_vendor_dlkm_a.img"
    [[ -f "$BLOBS/halium_system_dlkm_a.img" ]] && add_part halium_system_dlkm_a "$BLOBS/halium_system_dlkm_a.img"

    # halium_apex_a — flattened APEX payloads for OHOS-side libhybris.
    # The Halium-14 GSI ships PACKED /system/apex/*.apex (unlike Halium 12's
    # flattened dirs), so the X23 trick of bind-mounting system/apex at
    # /apex gives libhybris nothing to resolve /apex/com.android.runtime/…
    # against and composer_host dies on a NULL hwc2_compat_* symbol.
    # Extract the two payloads libhybris needs (bionic runtime + VNDK 34
    # for the vendor mapper/gralloc deps) into an ext4 image the chainload
    # mounts at /apex.  Rootless: debugfs dump/rdump + mke2fs -d.
    APEX_IMG="$OHOS_ROOT/out/hybris_generic/halium_apex.img"
    if [[ ! -f "$APEX_IMG" || "$BLOBS/halium_system_a.img" -nt "$APEX_IMG" ]]; then
        echo "building halium_apex.img (flattened runtime + vndk.v34 payloads)"
        APEX_WORK="$(mktemp -d)"
        trap 'rm -rf "$APEX_WORK"' EXIT
        declare -A APEX_SRC=(
            [com.android.runtime]="/system/apex/com.android.runtime.apex"
            [com.android.vndk.v34]="/system/system_ext/apex/com.android.vndk.v34.apex"
        )
        for name in "${!APEX_SRC[@]}"; do
            debugfs -R "dump ${APEX_SRC[$name]} $APEX_WORK/$name.apex" \
                "$BLOBS/halium_system_a.img" 2>/dev/null
            [[ -s "$APEX_WORK/$name.apex" ]] || { echo "ERROR: ${APEX_SRC[$name]} not found in halium_system_a.img" >&2; exit 1; }
            unzip -o -q "$APEX_WORK/$name.apex" apex_payload.img -d "$APEX_WORK/$name-x"
            mkdir -p "$APEX_WORK/staging/$name"
            # chown warnings are expected rootless noise; suppress them but
            # keep other stderr.
            debugfs -R "rdump / $APEX_WORK/staging/$name" \
                "$APEX_WORK/$name-x/apex_payload.img" 2> >(grep -v 'changing ownership' >&2)
            rmdir "$APEX_WORK/staging/$name/lost+found" 2>/dev/null || true
        done
        # 59M of payloads today; 96M leaves room for adding i18n/tzdata later.
        mke2fs -q -t ext4 -d "$APEX_WORK/staging" -L halium_apex "$APEX_IMG.tmp" 96m
        mv "$APEX_IMG.tmp" "$APEX_IMG"
    fi
    add_part halium_apex_a "$APEX_IMG"
else
    echo "WARN: halium-blobs/ansuz not populated — OHOS-only super (no libhybris graphics)"
fi

GROUP_SIZE=$(( SUPER_SIZE / 2 - 1024 * 1024 ))
echo "group budget: $GROUP_SIZE bytes (need $TOTAL)"
if (( TOTAL > GROUP_SIZE )); then
    echo "ERROR: partition total $TOTAL > group budget $GROUP_SIZE" >&2
    exit 1
fi

"$LPMAKE" \
    --metadata-size "$METADATA_SIZE" \
    --metadata-slots "$METADATA_SLOTS" \
    --block-size "$BLOCK_SIZE" \
    --device super:"$SUPER_SIZE" \
    --group main_a:"$GROUP_SIZE" \
    "${LPMAKE_ARGS[@]}" \
    --sparse \
    --output "$OUTPUT"

echo
echo "Built $OUTPUT ($(stat -c %s "$OUTPUT") bytes, sparse)"
