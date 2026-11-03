#!/bin/bash
#
# Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
# SPDX-License-Identifier: Apache-2.0
# Licensed under the Apache License, Version 2.0 (the "License").
#
# Fetch + extract the Halium blobs the hybris_generic super image is
# built from, for either supported Volla phone.  Everything comes from
# public, SHA256-pinned upstream downloads: **no Ubuntu Touch install
# and no on-device dumping is required.**
#
#   -d x23    Volla Phone X23 (vidofnir)      Halium 12  -> halium-blobs/
#   -d ansuz  Volla Phone Plinius (ansuz)     Halium 14  -> halium-blobs/ansuz/
#
# One-time, host-side.  The blobs are not checked in (Volla-licensed,
# 1.5–2.5 GB per device) but the SHA256 pins tie them to specific
# upstream releases.
#
# ---------------------------------------------------------------------
# Where the blobs come from (both devices use the same two sources)
# ---------------------------------------------------------------------
#   1. The **Volla UBports-installer bootstrap zip** (volla.tech/filedump)
#      — the stock firmware set the ubports-installer flashes before it
#      installs UT.  Its `super.img` carries the MediaTek **vendor**
#      partition (Mali EGL, Android HALs, vendor init.rc).  Used for the
#      X23's vendor; on the Plinius only its dtbo.img is still used (as a
#      build-time check, see build_kernel.sh).
#
#   2. The **UBports system-image channel** — `device-<sha>.tar.xz`
#      carries `system/var/lib/lxc/android/android-rootfs.img`, the
#      Halium Android *system* root (init, hwservicemanager, bionic,
#      the libhybris compat layers).  We rename it to
#      halium_system_a.img.  The sibling `boot-<sha>.tar.xz` carries the
#      pristine Halium boot chain, kept as a donor/restore kit.  On the
#      Plinius the channel's `firmware-<sha>.tar.xz` supplies the vendor,
#      `vendor_dlkm` + `system_dlkm` (the stock kernel-module trees) and
#      the flashable firmware set (dtbo, lk, preloader, co-processors):
#      UT OTAs update the firmware, so it is newer than the bootstrap's.
#
# Why two sources?  Because the bootstrap zip's `system_a` inside
# super.img is allocated but zero-filled: UBports' recovery flow
# populates it later by writing android-rootfs.img into it.  Verified on
# both devices (`xxd halium_system_a.img | head` — all zeros).
#
# Updating a pin: fetch the channel index, take images[-1], and update
# the DEVICE_TAR_* / BOOT_TAR_* (/ FIRMWARE_TAR_*) constants below:
#   curl -s <channel>/index.json | jq '.images[-1]'
# The bootstrap zip pin follows the ubports-installer config:
#   curl -s https://ubports.github.io/installer-configs/v2/devices/<codename>.json | jq .
#
# Dependencies: curl, unzip, tar, xz, sha256sum, python3.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LPUNPACK="$HERE/lpunpack.py"
BLOBS_ROOT="$(cd "$HERE/../.." && pwd)/halium-blobs"

DEVICE="${HYBRIS_DEVICE:-x23}"
KEEP_DOWNLOADS=1

usage() {
    cat <<'EOF'
Usage: pull-halium-blobs.sh [-d x23|ansuz] [--clean-downloads]

Fetch the Halium blobs for a hybris_generic device from public,
SHA256-pinned upstream downloads.  No Ubuntu Touch install needed.

Options:
  -d, --device NAME     x23 | vidofnir  (default)   -> halium-blobs/
                        ansuz | plinius             -> halium-blobs/ansuz/
                                                       (env HYBRIS_DEVICE)
      --clean-downloads delete the fetched archives when done (they are
                        kept by default — re-fetching takes minutes)
  -h, --help            this text
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -d|--device)        DEVICE="${2:?-d requires a value}"; shift 2 ;;
        --device=*)         DEVICE="${1#*=}"; shift ;;
        --clean-downloads)  KEEP_DOWNLOADS=0; shift ;;
        -h|--help)          usage; exit 0 ;;
        *) echo "Error: unknown argument '$1'" >&2; usage >&2; exit 1 ;;
    esac
done

case "$DEVICE" in
    x23|vidofnir)  DEVICE=x23 ;;
    ansuz|plinius) DEVICE=ansuz ;;
    *) echo "Error: unknown device '$DEVICE' — known: x23 (vidofnir), ansuz (plinius)" >&2; exit 1 ;;
esac

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------
need() { command -v "$1" >/dev/null || { echo "ERROR: $1 not found on \$PATH" >&2; exit 1; }; }
for t in curl unzip tar xz sha256sum python3; do need "$t"; done

# fetch URL DEST SHA256 [HUMAN_SIZE] — idempotent, verifies every time.
fetch() {
    local url="$1" dest="$2" sha="$3" size="${4:-}"
    if [[ ! -f "$dest" ]]; then
        echo "==> Downloading $(basename "$dest")${size:+ (~$size)}…"
        curl --location --fail --progress-bar --output "$dest.tmp" "$url"
        mv "$dest.tmp" "$dest"
    fi
    echo "==> Verifying $(basename "$dest") SHA256…"
    if ! echo "$sha  $dest" | sha256sum -c -; then
        echo "ERROR: $dest does not match its pin.  Delete it and re-run to" >&2
        echo "       re-fetch, or update the pin if upstream moved." >&2
        exit 1
    fi
}

# inner_super ZIP — echo the path of super.img inside the bootstrap zip
# (it has lived at the root and one level down across Volla releases).
inner_super() {
    local zip="$1" inner
    inner="$(unzip -l "$zip" | awk '$NF ~ /super\.img$/ { print $NF; exit }')"
    [[ -n "$inner" ]] || {
        echo "ERROR: super.img not found inside $zip" >&2
        unzip -l "$zip" >&2; exit 1; }
    echo "$inner"
}

# unpack_super SUPER_IMG OUTDIR NAME:DEST [NAME:DEST …]
# lpunpack.py reads Android sparse images directly, so no simg2img and
# no multi-GB raw expansion on disk.
unpack_super() {
    local super="$1" outdir="$2"; shift 2
    echo "==> Halium super partition table:"
    python3 "$LPUNPACK" "$super"
    local spec name dest
    for spec in "$@"; do
        name="${spec%%:*}"; dest="${spec#*:}"
        python3 "$LPUNPACK" --partition "$name" "$super" "$outdir"
        mv "$outdir/$name.img" "$dest"
    done
}

# ===========================================================================
# Volla Phone X23 (vidofnir) — Halium 12
# ===========================================================================
pull_x23() {
    local BLOBS="$BLOBS_ROOT"
    mkdir -p "$BLOBS"

    # Source 1 — bootstrap zip (vendor + the pristine Halium boot.img)
    local BOOTSTRAP_URL="https://volla.tech/filedump/volla-vidofnir-12.0-ubports-installer-bootstrap-v3.zip"
    local BOOTSTRAP_SHA256="da18b5498ebae0267be894fff73bfd629be73967cf33f071d571ffc3ef46ce97"
    local BOOTSTRAP_ZIP="$BLOBS/bootstrap.zip"

    # Source 2 — system-image 20.04/arm64/android9plus/stable/vidofnir_esim,
    # pinned to stable v12 (latest as of 2026-05-12).
    local DEVICE_TAR_URL="https://system-image.ubports.com/pool/device-37ea68e425f921a10982a5cbd36345dde820b239d0c3962e2ec75adea6759e17.tar.xz"
    local DEVICE_TAR_SHA256="62306fcc600d7062a9d0e65e60c381c4605fdace17945565f9b0365c4a89b788"
    local DEVICE_TAR="$BLOBS/device.tar.xz"

    # --- halium_vendor_a.img ------------------------------------------------
    if [[ ! -f "$BLOBS/halium_vendor_a.img" ]]; then
        fetch "$BOOTSTRAP_URL" "$BOOTSTRAP_ZIP" "$BOOTSTRAP_SHA256" "478 MB"

        local INNER_SUPER
        INNER_SUPER="$(inner_super "$BOOTSTRAP_ZIP")"
        echo "    inner super: $INNER_SUPER"

        local TMP_SUPER="$BLOBS/halium-super.img"
        echo "==> Extracting $INNER_SUPER…"
        unzip -p "$BOOTSTRAP_ZIP" "$INNER_SUPER" > "$TMP_SUPER"
        unpack_super "$TMP_SUPER" "$BLOBS" "vendor_a:$BLOBS/halium_vendor_a.img"
        rm -f "$TMP_SUPER"
    fi

    # --- halium_system_a.img ------------------------------------------------
    # The bootstrap zip's system_a is zero-filled; the real Halium Android
    # /system tree is the device tarball's android-rootfs.img (~506 MB ext4).
    if [[ ! -f "$BLOBS/halium_system_a.img" ]]; then
        fetch "$DEVICE_TAR_URL" "$DEVICE_TAR" "$DEVICE_TAR_SHA256" "140 MB"
        echo "==> Extracting android-rootfs.img from device tarball…"
        ( cd "$BLOBS" && tar xJf "$DEVICE_TAR" \
            system/var/lib/lxc/android/android-rootfs.img )
        mv "$BLOBS/system/var/lib/lxc/android/android-rootfs.img" \
           "$BLOBS/halium_system_a.img"
        # tar preserves the upstream build date, which can be older than a
        # derived artifact already in out/ — that defeats -nt staleness checks.
        touch "$BLOBS/halium_system_a.img"
        rm -rf "$BLOBS/system"
    fi

    # --- halium_boot_a.img --------------------------------------------------
    # build_boot_img_chainload.sh reuses the Halium ramdisk from a pristine
    # boot.img (parse-android-dynparts, dmsetup, modprobe + a kernel-matched
    # module set).  The bootstrap zip carries it next to super.img.
    if [[ ! -f "$BLOBS/halium_boot_a.img" ]]; then
        fetch "$BOOTSTRAP_URL" "$BOOTSTRAP_ZIP" "$BOOTSTRAP_SHA256" "478 MB"
        echo "==> Extracting boot.img → halium_boot_a.img…"
        unzip -p "$BOOTSTRAP_ZIP" boot.img > "$BLOBS/halium_boot_a.img.tmp"
        mv "$BLOBS/halium_boot_a.img.tmp" "$BLOBS/halium_boot_a.img"
    fi

    (( KEEP_DOWNLOADS )) || rm -f "$BOOTSTRAP_ZIP" "$DEVICE_TAR"

    echo
    echo "Halium blobs ready:"
    ls -lh "$BLOBS/halium_system_a.img" "$BLOBS/halium_vendor_a.img" "$BLOBS/halium_boot_a.img"
    echo
    echo "Next: bash device/board/oniro/hybris_generic/kernel/x23/build_super_img.sh"
    echo "      (will detect halium-blobs/ and bake halium_system_a + halium_vendor_a"
    echo "       into the super partition alongside OHOS partitions)"
}

# ===========================================================================
# Volla Phone Plinius (ansuz) — Halium 14
# ===========================================================================
# Verified 2026-08-23 against the images dumped off a UT-running Plinius
# in P1: vendor_a / vendor_dlkm_a / system_dlkm_a extracted here are
# byte-identical to the on-device dumps (which only carry extra
# whole-partition zero padding).
pull_ansuz() {
    local BLOBS="$BLOBS_ROOT/ansuz"
    local DL="$BLOBS/downloads"
    mkdir -p "$BLOBS" "$DL"

    # Source 1 — bootstrap zip (VollaOS 14 stock firmware set; newest images
    # dated 2026-08-31).  Pin follows ubports installer-configs v2
    # devices/ansuz.yml.  Since the v4 firmware bump only its dtbo.img is
    # used here: build_kernel.sh checks that the DTB still accepts it, for
    # phones bootstrapped but not yet given the v4 firmware set.
    local BOOTSTRAP_URL="https://volla.tech/filedump/volla-ansuz-14.0-ubports-installer-bootstrap-v3.zip"
    local BOOTSTRAP_SHA256="b0f05235ea8e73cda5809f68f739ec59696e13c702298b2db1f1e2c795d7cdf2"
    local BOOTSTRAP_ZIP="$DL/bootstrap.zip"

    # Source 2 — system-image 24.04-1.x/arm64/android9plus/stable/ansuz,
    # pinned to version 4 (tag 24.04-1.5; version 5 ships the same firmware).
    local DEVICE_TAR_URL="https://system-image.ubports.com/pool/device-457cb574165c2dcd9f39b2e2cd1c9856adcda3e4b8f85c6789396ada8afb31e3.tar.xz"
    local DEVICE_TAR_SHA256="faf45a65822b5ff4926b9791d60e8ee390fcd007d47ecfae696ad64a7935c065"
    local DEVICE_TAR="$DL/device.tar.xz"
    local BOOT_TAR_URL="https://system-image.ubports.com/pool/boot-637bdb7343d083e07a991ece4669ca30c93b13e4e7e18f455b594530d08c6fa4.tar.xz"
    local BOOT_TAR_SHA256="5ed5d4af48f66a888d009ba9cc47cc18d0d4fa84c57358f62e760b3b01e4aeb6"
    local BOOT_TAR="$DL/boot.tar.xz"
    # The same version's firmware tarball: the VollaOS 20260828 generation
    # (built 2026-09-07) that UT OTAs flash.  Its vendor_dlkm/system_dlkm are
    # what the port repo's 29554f8 vendor_boot modules (mtk-pogo-usb for the
    # Plinius Plus, new tcpc/charger classes) were built against: the
    # bootstrap's older vendor_dlkm rejects them (touch, sensors, camera and
    # accdet fail on symbol CRCs).  Its dtbo/lk/preloader/co-processor images
    # go to halium-blobs/ansuz/firmware/ for `flash-native.sh --firmware`.
    local FIRMWARE_TAR_URL="https://system-image.ubports.com/pool/firmware-da7620ded0f1904afa9a4fb4feabc1df81603728c464b6b13506b9e86745bc03.tar.xz"
    local FIRMWARE_TAR_SHA256="4b8902d01033891ea007267c211b02048f81d04f54978457089d7cfbe2b42ad6"
    local FIRMWARE_TAR="$DL/firmware.tar.xz"

    # --- halium_{vendor,vendor_dlkm,system_dlkm}_a.img + firmware/ ----------
    # A stamp records which firmware pin the extracted images came from, so
    # a tree that pulled an older pin re-extracts after a pin bump.
    local STAMP="$BLOBS/.firmware-sha256"
    if [[ ! -f "$BLOBS/halium_vendor_a.img" \
       || ! -f "$BLOBS/halium_vendor_dlkm_a.img" \
       || ! -f "$BLOBS/halium_system_dlkm_a.img" \
       || ! -d "$BLOBS/firmware" \
       || "$(cat "$STAMP" 2>/dev/null)" != "$FIRMWARE_TAR_SHA256" ]]; then
        fetch "$FIRMWARE_TAR_URL" "$FIRMWARE_TAR" "$FIRMWARE_TAR_SHA256" "578 MB"
        echo "==> Extracting the firmware tarball…"
        rm -rf "$DL/partitions" "$BLOBS/firmware"
        tar xJf "$FIRMWARE_TAR" -C "$DL"
        local p
        for p in vendor vendor_dlkm system_dlkm; do
            mv "$DL/partitions/$p.img" "$BLOBS/halium_${p}_a.img"
            touch "$BLOBS/halium_${p}_a.img"   # see the halium_system_a note below
        done
        mv "$DL/partitions" "$BLOBS/firmware"
        echo "$FIRMWARE_TAR_SHA256" > "$STAMP"
    fi
    if [[ ! -f "$BOOTSTRAP_ZIP" ]] && (( KEEP_DOWNLOADS )); then
        fetch "$BOOTSTRAP_URL" "$BOOTSTRAP_ZIP" "$BOOTSTRAP_SHA256" "816 MB"
    fi

    # --- halium_system_a.img ------------------------------------------------
    if [[ ! -f "$BLOBS/halium_system_a.img" ]]; then
        fetch "$DEVICE_TAR_URL" "$DEVICE_TAR" "$DEVICE_TAR_SHA256" "144 MB"
        echo "==> Extracting android-rootfs.img from device tarball…"
        ( cd "$DL" && tar xJf "$DEVICE_TAR" \
            system/var/lib/lxc/android/android-rootfs.img )
        mv "$DL/system/var/lib/lxc/android/android-rootfs.img" \
           "$BLOBS/halium_system_a.img"
        # tar preserves the upstream build date, which can be older than
        # out/hybris_generic/halium_apex.img and so defeat build_super_img.sh's
        # -nt staleness check.  Stamp it now instead.
        touch "$BLOBS/halium_system_a.img"
        rm -rf "$DL/system"
    fi

    # --- ut-boot/{boot,init_boot,vendor_boot}.img ---------------------------
    # The pristine Halium boot chain for this channel version.  Two uses:
    #   * donor fallback for kernel/ansuz/build_init_boot_chainload.sh when
    #     the kernel has not been built locally (its ramdisk is the one the
    #     chainload splices /init into, and its vendor_boot carries the DTB +
    #     ABM recovery fragment);
    #   * the UT restore kit, if you ever want the phone back on Ubuntu Touch.
    # NOTE: these are the *Halium* images, not the bootstrap zip's stock
    # VollaOS ones.  Their kernel-module set matches this channel's kernel —
    # pair them with build_kernel.sh's boot.img only if you know what you
    # are doing (build_kernel.sh emits its own matched trio).
    if [[ ! -f "$BLOBS/ut-boot/init_boot.img" \
       || "$(cat "$BLOBS/ut-boot/.sha256" 2>/dev/null)" != "$BOOT_TAR_SHA256" ]]; then
        fetch "$BOOT_TAR_URL" "$BOOT_TAR" "$BOOT_TAR_SHA256" "47 MB"
        echo "==> Extracting the pristine Halium boot chain…"
        rm -rf "$BLOBS/ut-boot"
        mkdir -p "$BLOBS/ut-boot"
        tar xJf "$BOOT_TAR" -C "$BLOBS/ut-boot" --strip-components=1 partitions/
        echo "$BOOT_TAR_SHA256" > "$BLOBS/ut-boot/.sha256"
    fi

    (( KEEP_DOWNLOADS )) || rm -rf "$DL"

    echo
    echo "Halium blobs ready:"
    ls -lh "$BLOBS"/halium_*.img "$BLOBS"/ut-boot/*.img
    echo "Firmware set for flash-native.sh --firmware: $(ls "$BLOBS/firmware" | tr '\n' ' ')"
    echo
    echo "Next: bash device/board/oniro/hybris_generic/kernel/ansuz/build_super_img.sh"
    echo "      (bakes halium_{system,vendor,vendor_dlkm,system_dlkm}_a + a"
    echo "       flattened halium_apex_a into the super partition alongside"
    echo "       the OHOS partitions)"
}

case "$DEVICE" in
    x23)   pull_x23 ;;
    ansuz) pull_ansuz ;;
esac
