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
#      partition (Mali EGL, Android HALs, vendor init.rc) and, on the
#      Plinius, `vendor_dlkm` + `system_dlkm` (the stock kernel-module
#      trees).  Vendor content is firmware-derived, not OTA-updated.
#
#   2. The **UBports system-image channel** — `device-<sha>.tar.xz`
#      carries `system/var/lib/lxc/android/android-rootfs.img`, the
#      Halium Android *system* root (init, hwservicemanager, bionic,
#      the libhybris compat layers).  We rename it to
#      halium_system_a.img.  The sibling `boot-<sha>.tar.xz` carries the
#      pristine Halium boot chain, kept as a donor/restore kit.
#
# Why two sources?  Because the bootstrap zip's `system_a` inside
# super.img is allocated but zero-filled: UBports' recovery flow
# populates it later by writing android-rootfs.img into it.  Verified on
# both devices (`xxd halium_system_a.img | head` — all zeros).
#
# Updating a pin: fetch the channel index, take images[-1], and update
# the DEVICE_TAR_* / BOOT_TAR_* constants below:
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

    # Source 1 — bootstrap zip (VollaOS 14 stock firmware set).  Pin follows
    # ubports-installer-configs v2 devices/ansuz.json.
    local BOOTSTRAP_URL="https://volla.tech/filedump/volla-ansuz-14.0-ubports-installer-bootstrap-v2.zip"
    local BOOTSTRAP_SHA256="0b6af6ce3fe535c73f95d6a52a548ed3cbe057fa3b68d0969167723be5fed1af"
    local BOOTSTRAP_ZIP="$DL/bootstrap.zip"

    # Source 2 — system-image 24.04-1.x/arm64/android9plus/stable/ansuz,
    # pinned to version 3 (tag 24.04-1.4, latest as of 2026-08-23).
    local DEVICE_TAR_URL="https://system-image.ubports.com/pool/device-1fd22a63f9390b084a9b4212cdb0bb345fff9b1dd249fea74afc8ff0c477adf6.tar.xz"
    local DEVICE_TAR_SHA256="0f5b370303770494cdec43b74651ec7f72020e2828601250423451ada43e90b5"
    local DEVICE_TAR="$DL/device.tar.xz"
    local BOOT_TAR_URL="https://system-image.ubports.com/pool/boot-aa9bc6bf7ed80df227324b7b6fef076ca26d9d01e83a4efd0098c7c87e11215c.tar.xz"
    local BOOT_TAR_SHA256="836c9ed146cbb795f085ac20675ab7fcfa091993901e5e5a4b7ac60d73c521c5"
    local BOOT_TAR="$DL/boot.tar.xz"

    # --- halium_{vendor,vendor_dlkm,system_dlkm}_a.img ----------------------
    if [[ ! -f "$BLOBS/halium_vendor_a.img" \
       || ! -f "$BLOBS/halium_vendor_dlkm_a.img" \
       || ! -f "$BLOBS/halium_system_dlkm_a.img" ]]; then
        fetch "$BOOTSTRAP_URL" "$BOOTSTRAP_ZIP" "$BOOTSTRAP_SHA256" "816 MB"

        local INNER_SUPER TMP_SUPER="$DL/stock-super.img"
        INNER_SUPER="$(inner_super "$BOOTSTRAP_ZIP")"
        echo "==> Extracting $INNER_SUPER from the bootstrap zip…"
        unzip -p "$BOOTSTRAP_ZIP" "$INNER_SUPER" > "$TMP_SUPER"
        unpack_super "$TMP_SUPER" "$DL" \
            "vendor_a:$BLOBS/halium_vendor_a.img" \
            "vendor_dlkm_a:$BLOBS/halium_vendor_dlkm_a.img" \
            "system_dlkm_a:$BLOBS/halium_system_dlkm_a.img"
        rm -f "$TMP_SUPER"
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
    if [[ ! -f "$BLOBS/ut-boot/init_boot.img" ]]; then
        fetch "$BOOT_TAR_URL" "$BOOT_TAR" "$BOOT_TAR_SHA256" "46 MB"
        echo "==> Extracting the pristine Halium boot chain…"
        mkdir -p "$BLOBS/ut-boot"
        tar xJf "$BOOT_TAR" -C "$BLOBS/ut-boot" --strip-components=1 partitions/
    fi

    (( KEEP_DOWNLOADS )) || rm -rf "$DL"

    echo
    echo "Halium blobs ready:"
    ls -lh "$BLOBS"/halium_*.img "$BLOBS"/ut-boot/*.img
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
