#!/usr/bin/env bash
# waydroid-images.sh — fetch, verify and deploy the pinned Waydroid
# container images for hybris_generic (plan: android_app_compat_plan.md).
#
# The pin is device/soc/oniro/hybris_generic/waydroid/data/images.manifest:
#   <zip-filename> <sha256>
# lines; system zips match *-system.zip, vendor zips *-vendor.zip.  The same
# file is installed on the device as /system/etc/waydroid/images.manifest.
# Zips come from the Waydroid OTA channel (sourceforge mirrors).
#
# The two images are ALL the device needs under /data/waydroid: they are pushed
# unmodified.  The binary patches are applied by waydroidd on every start
# (patches.conf) and the prop / hosthals / graft files ship in system.img.
#
# Usage:
#   waydroid-images.sh fetch          # download + verify + extract locally
#   waydroid-images.sh push           # hdc-push the two images
#   waydroid-images.sh all            # fetch && push
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TREE_ROOT="$(cd "$SCRIPT_DIR/../../../../../.." && pwd)"
BLOB_DIR="$TREE_ROOT/halium-blobs/waydroid"   # download cache (untracked)
MANIFEST="$TREE_ROOT/device/soc/oniro/hybris_generic/waydroid/data/images.manifest"
DEVICE_DIR="/data/waydroid"
HDC="${HDC:-hdc}"

SF_SYSTEM="https://sourceforge.net/projects/waydroid/files/images/system/lineage/waydroid_arm64"
SF_VENDOR="https://sourceforge.net/projects/waydroid/files/images/vendor/waydroid_arm64"

die() { echo "ERROR: $*" >&2; exit 1; }

manifest_entries() { awk '!/^#/ && NF==2 {print $1, $2}' "$MANIFEST"; }

fetch() {
    [ -f "$MANIFEST" ] || die "no manifest at $MANIFEST"
    mkdir -p "$BLOB_DIR"
    cd "$BLOB_DIR"
    while read -r zip sha; do
        if [ ! -f "$zip" ]; then
            case "$zip" in
                *-system.zip) url="$SF_SYSTEM/$zip/download" ;;
                *-vendor.zip) url="$SF_VENDOR/$zip/download" ;;
                *) die "unrecognized zip name $zip" ;;
            esac
            echo "downloading $zip ..."
            curl -sL -o "$zip" "$url"
        fi
        echo "$sha  $zip" | sha256sum -c - || die "sha256 mismatch for $zip"
        unzip -o -q "$zip"
    done < <(manifest_entries)
    ls -la "$BLOB_DIR"/*.img
}

push() {
    [ -f "$BLOB_DIR/system.img" ] || die "run fetch first"
    $HDC shell "mkdir -p $DEVICE_DIR/images"
    $HDC file send "$BLOB_DIR/system.img" "$DEVICE_DIR/images/system.img"
    $HDC file send "$BLOB_DIR/vendor.img" "$DEVICE_DIR/images/vendor.img"
    $HDC shell "ls -la $DEVICE_DIR/images"
}

case "${1:-all}" in
    fetch) fetch ;;
    push)  push ;;
    all)   fetch; push ;;
    *)     die "usage: $0 {fetch|push|all}" ;;
esac
