#!/bin/bash
#
# Copyright (C) 2026 Oniro / Hybris Generic.
# Licensed under the Apache License, Version 2.0 (the "License").
#
# Fetch + extract Halium 12 system_a and vendor_a images for Volla X23
# (vidofnir).  Outputs are stashed under
# device/board/oniro/hybris_generic/halium-blobs/ for the super-image
# builder (kernel/x23/build_super_img.sh) to consume.
#
# One-time, host-side.  The blobs are not checked in (Volla-licensed,
# ~1.5 GB combined) but SHA256 pins tie them to specific upstream
# releases.
#
# Sources:
#   1. The UBports installer bootstrap zip
#      (https://volla.tech/filedump/...): provides the MTK 6789 *vendor*
#      partition (vendor_a inside super.img — Mali EGL, Android HALs,
#      vendor init.rc).  This is bit-identical across UT releases since
#      vendor is firmware-derived, not OTA-updated.
#
#   2. The UBports system-image stable channel
#      (https://system-image.ubports.com/.../vidofnir_esim/) provides
#      `device-<sha>.tar.xz`, which carries
#      `system/var/lib/lxc/android/android-rootfs.img` — the Halium
#      Android *system* root (init, hwservicemanager, /system/lib64/
#      bionic, etc.).  We rename it to halium_system_a.img.
#
# Why two sources?  Because the bootstrap zip's super.img allocates
# system_a slot at 8.7 GB but leaves it filled with zeros: UBports'
# recovery flow normally populates system_a by writing
# android-rootfs.img *as a file* into the rootfs.  Empirical check:
# `xxd halium_system_a.img | head` shows all zeros after extracting it
# from the bootstrap super.img.  We fetch android-rootfs.img directly
# from the system-image flow instead — it's a self-contained 506 MB
# ext4 image with the full Halium Android /system tree.
#
# Dependencies: curl, unzip, tar, xz, sha256sum, simg2img, python3.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LPUNPACK="$HERE/lpunpack.py"
BLOBS="$HERE/../../halium-blobs"

# ---------------------------------------------------------------------------
# Source 1 — bootstrap zip (vendor)
# ---------------------------------------------------------------------------
BOOTSTRAP_URL="https://volla.tech/filedump/volla-vidofnir-12.0-ubports-installer-bootstrap-v3.zip"
BOOTSTRAP_SHA256="da18b5498ebae0267be894fff73bfd629be73967cf33f071d571ffc3ef46ce97"
BOOTSTRAP_ZIP="$BLOBS/bootstrap.zip"

# ---------------------------------------------------------------------------
# Source 2 — UBports system-image stable channel (Android-rootfs / system)
# Pinned to vidofnir_esim stable v12 (latest as of 2026-05-12).  To
# update: fetch the channel index, find images[*].version max, and
# update DEVICE_TAR_PATH + DEVICE_TAR_SHA256 below.
#
#   curl -fsSL https://system-image.ubports.com/20.04/arm64/android9plus/stable/vidofnir_esim/index.json | jq '.images[-1]'
# ---------------------------------------------------------------------------
DEVICE_TAR_PATH="/pool/device-37ea68e425f921a10982a5cbd36345dde820b239d0c3962e2ec75adea6759e17.tar.xz"
DEVICE_TAR_SHA256="62306fcc600d7062a9d0e65e60c381c4605fdace17945565f9b0365c4a89b788"
DEVICE_TAR_URL="https://system-image.ubports.com${DEVICE_TAR_PATH}"
DEVICE_TAR="$BLOBS/device.tar.xz"

mkdir -p "$BLOBS"

# ===========================================================================
# 1. halium_vendor_a.img — from bootstrap zip
# ===========================================================================
if [[ ! -f "$BLOBS/halium_vendor_a.img" ]]; then
    if [[ ! -f "$BOOTSTRAP_ZIP" ]]; then
        echo "==> Downloading UBports bootstrap zip (~478 MB)…"
        curl --location --fail --output "$BOOTSTRAP_ZIP.tmp" "$BOOTSTRAP_URL"
        mv "$BOOTSTRAP_ZIP.tmp" "$BOOTSTRAP_ZIP"
    fi

    echo "==> Verifying bootstrap zip SHA256…"
    echo "$BOOTSTRAP_SHA256  $BOOTSTRAP_ZIP" | sha256sum -c -

    INNER_SUPER="$(unzip -l "$BOOTSTRAP_ZIP" \
                    | awk '$NF ~ /super\.img$/ { print $NF; exit }')"
    [[ -n "${INNER_SUPER:-}" ]] || {
        echo "ERROR: super.img not found inside $BOOTSTRAP_ZIP" >&2
        unzip -l "$BOOTSTRAP_ZIP"; exit 1; }
    echo "    inner super: $INNER_SUPER"

    TMP_SUPER="$BLOBS/halium-super.img"
    echo "==> Extracting $INNER_SUPER…"
    unzip -p "$BOOTSTRAP_ZIP" "$INNER_SUPER" > "$TMP_SUPER"

    SIG=$(head -c 4 "$TMP_SUPER" | xxd -p)
    if [[ "$SIG" == "3aff26ed" ]]; then
        echo "==> Converting sparse → raw via simg2img…"
        command -v simg2img >/dev/null || {
            echo "ERROR: install android-sdk-libsparse-utils (provides simg2img)" >&2
            exit 1; }
        simg2img "$TMP_SUPER" "$TMP_SUPER.raw"
        mv "$TMP_SUPER.raw" "$TMP_SUPER"
    fi

    echo "==> Halium super partition table:"
    python3 "$LPUNPACK" "$TMP_SUPER"

    echo "==> Extracting vendor_a from halium super…"
    python3 "$LPUNPACK" --partition vendor_a "$TMP_SUPER" "$BLOBS"
    mv "$BLOBS/vendor_a.img" "$BLOBS/halium_vendor_a.img"
    rm -f "$TMP_SUPER"
fi

# ===========================================================================
# 2. halium_system_a.img — from UBports system-image device tarball
# ===========================================================================
# The bootstrap zip's system_a inside super.img is allocated but empty
# (zeroed).  The actual Halium Android /system tree is delivered via
# UBports' system-image flow as `system/var/lib/lxc/android/
# android-rootfs.img` inside the device tarball.  This is a self-
# contained ~506 MB ext4 with /init -> /system/bin/init, hwservice-
# manager, bionic libs, init.rc files, etc.
# ===========================================================================
if [[ ! -f "$BLOBS/halium_system_a.img" ]]; then
    if [[ ! -f "$DEVICE_TAR" ]]; then
        echo "==> Downloading UBports device tarball (~140 MB)…"
        curl --location --fail --output "$DEVICE_TAR.tmp" "$DEVICE_TAR_URL"
        mv "$DEVICE_TAR.tmp" "$DEVICE_TAR"
    fi

    echo "==> Verifying device tarball SHA256…"
    echo "$DEVICE_TAR_SHA256  $DEVICE_TAR" | sha256sum -c -

    echo "==> Extracting android-rootfs.img from device tarball…"
    (cd "$BLOBS" && tar xJf "$DEVICE_TAR" \
        system/var/lib/lxc/android/android-rootfs.img)
    mv "$BLOBS/system/var/lib/lxc/android/android-rootfs.img" \
       "$BLOBS/halium_system_a.img"
    rm -rf "$BLOBS/system"
fi

# ---------------------------------------------------------------------------
# Cleanup downloads (keep them — they're large; re-fetching takes minutes).
# Comment-out to free disk space.
# ---------------------------------------------------------------------------
# rm -f "$BOOTSTRAP_ZIP" "$DEVICE_TAR"

echo
echo "Halium blobs ready:"
ls -lh "$BLOBS/halium_system_a.img" "$BLOBS/halium_vendor_a.img"
echo
echo "Next: bash device/board/oniro/hybris_generic/kernel/x23/build_super_img.sh"
echo "      (will detect halium-blobs/ and bake halium_system_a + halium_vendor_a"
echo "       into the super partition alongside OHOS partitions)"
