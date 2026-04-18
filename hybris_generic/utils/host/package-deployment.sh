#!/bin/bash
#
# package-deployment.sh — bundle a self-contained installer archive for
# OpenHarmony hybris_generic on a single Volla device (X23 or Tablet/mimir).
#
# The resulting archive contains:
#   - The OHOS rootfs tarball (out/hybris_generic/packages/phone/images/ohos-rootfs.tar.gz)
#   - LXC config + start-ohos.sh + ohos-post-stop.sh + binder-devices script + systemd units
#   - boot.img + vendor_boot.img (if present) + modules.tar.gz for the chosen device
#   - DEVICE marker file ("x23" or "mimir")
#   - deploy.sh — selective installer (--rootfs / --kernel)
#
# The user runs `./deploy.sh` from the extracted archive on a host PC with
# `adb` connected to the target device.

set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UTILS_DIR="$(cd "$HERE/.." && pwd)"
OHOS_DIR="$(cd "$UTILS_DIR/../../../../.." && pwd)"

DEVICE_DIR="$UTILS_DIR/device"
INSTALLER_DIR="$UTILS_DIR/installer"

ROOTFS_TARBALL="$OHOS_DIR/out/hybris_generic/packages/phone/images/ohos-rootfs.tar.gz"

DEVICE=""
OUTPUT_DIR="$OHOS_DIR/out/hybris_generic/deployment"
ARCHIVE_PATH=""

usage() {
    cat <<EOF
Usage: $0 --device {x23|mimir} [-o <archive_path>]

  --device {x23|mimir}   Target device (required). Produces one archive per device.
  -o <archive_path>      Override the output archive path
                         (default: $OUTPUT_DIR/ohos-hybris-generic-deployment-<device>-<date>.tar.gz).
  -h, --help             Show this help.
EOF
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        --device) DEVICE="$2"; shift 2 ;;
        -o) ARCHIVE_PATH="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) echo "Unknown argument: $1" >&2; usage ;;
    esac
done

case "$DEVICE" in
    x23)   KERNEL_OUT="$OHOS_DIR/kernel/linux/volla-vidofnir/out" ;;
    mimir) KERNEL_OUT="$OHOS_DIR/kernel/linux/volla-mimir/out" ;;
    *)     echo "Error: --device must be 'x23' or 'mimir'." >&2; usage ;;
esac

[ -z "$ARCHIVE_PATH" ] && \
    ARCHIVE_PATH="$OUTPUT_DIR/ohos-hybris-generic-deployment-${DEVICE}-$(date +%Y%m%d).tar.gz"

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1"
}

require_file() {
    if [ ! -f "$1" ]; then
        echo "Error: required file not found: $1" >&2
        echo "       $2" >&2
        exit 1
    fi
}

# ── Verify required artifacts exist ─────────────────────────────────────────
require_file "$ROOTFS_TARBALL" \
    "Build the rootfs first via deploy-lxc-container.sh (which produces it) or run a full ./build.sh."
require_file "$DEVICE_DIR/lxc/config"          "Missing device-side LXC config."
require_file "$DEVICE_DIR/start-ohos.sh"       "Missing device-side start-ohos.sh."
require_file "$DEVICE_DIR/ohos-post-stop.sh"   "Missing device-side ohos-post-stop.sh."
require_file "$DEVICE_DIR/create-ohos-binder-devices.py" "Missing device-side binder script."
require_file "$DEVICE_DIR/systemd/ohos.service" "Missing systemd ohos.service."
require_file "$DEVICE_DIR/systemd/ohos-binder-setup.service" "Missing systemd ohos-binder-setup.service."
require_file "$INSTALLER_DIR/deploy.sh"        "Missing installer deploy.sh."

require_file "$KERNEL_OUT/boot.img" \
    "Build the $DEVICE kernel via device/board/oniro/hybris_generic/kernel/$DEVICE/build_kernel.sh."
require_file "$KERNEL_OUT/modules.tar.gz" \
    "Build the $DEVICE kernel via device/board/oniro/hybris_generic/kernel/$DEVICE/build_kernel.sh."

# ── Stage the archive contents ──────────────────────────────────────────────
STAGE_DIR=$(mktemp -d)
trap 'rm -rf "$STAGE_DIR"' EXIT

PKG_NAME="ohos-hybris-generic-deployment-${DEVICE}"
PKG_ROOT="$STAGE_DIR/$PKG_NAME"
mkdir -p "$PKG_ROOT/rootfs/lxc" "$PKG_ROOT/rootfs/systemd" "$PKG_ROOT/kernel"

log "Staging rootfs files..."
cp "$ROOTFS_TARBALL"                              "$PKG_ROOT/rootfs/ohos-rootfs.tar.gz"
cp "$DEVICE_DIR/lxc/config"                       "$PKG_ROOT/rootfs/lxc/config"
cp "$DEVICE_DIR/start-ohos.sh"                    "$PKG_ROOT/rootfs/"
cp "$DEVICE_DIR/ohos-post-stop.sh"                "$PKG_ROOT/rootfs/"
cp "$DEVICE_DIR/create-ohos-binder-devices.py"    "$PKG_ROOT/rootfs/"
cp "$DEVICE_DIR/systemd/ohos.service"             "$PKG_ROOT/rootfs/systemd/"
cp "$DEVICE_DIR/systemd/ohos-binder-setup.service" "$PKG_ROOT/rootfs/systemd/"

log "Staging $DEVICE kernel artifacts..."
cp "$KERNEL_OUT/boot.img"        "$PKG_ROOT/kernel/"
cp "$KERNEL_OUT/modules.tar.gz"  "$PKG_ROOT/kernel/"
[ -f "$KERNEL_OUT/vendor_boot.img" ] && \
    cp "$KERNEL_OUT/vendor_boot.img" "$PKG_ROOT/kernel/"

log "Staging deploy.sh and DEVICE marker..."
cp "$INSTALLER_DIR/deploy.sh" "$PKG_ROOT/"
echo "$DEVICE" > "$PKG_ROOT/DEVICE"
chmod +x "$PKG_ROOT/deploy.sh" "$PKG_ROOT/rootfs/start-ohos.sh" "$PKG_ROOT/rootfs/ohos-post-stop.sh"

# ── Create the archive ──────────────────────────────────────────────────────
mkdir -p "$(dirname "$ARCHIVE_PATH")"
log "Creating archive: $ARCHIVE_PATH"
tar -czf "$ARCHIVE_PATH" -C "$STAGE_DIR" "$PKG_NAME"

log "Done."
echo
echo "Archive: $ARCHIVE_PATH"
echo "Size:    $(du -h "$ARCHIVE_PATH" | cut -f1)"
echo "Device:  $DEVICE"
echo
echo "To install on a fresh Ubuntu Touch device:"
echo "  tar -xzf $(basename $ARCHIVE_PATH)"
echo "  cd $PKG_NAME"
echo "  ./deploy.sh                    # both rootfs + kernel"
echo "  ./deploy.sh --rootfs           # rootfs only"
echo "  ./deploy.sh --kernel           # kernel only"
