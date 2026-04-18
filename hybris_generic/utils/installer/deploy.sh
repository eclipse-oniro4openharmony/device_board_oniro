#!/bin/bash
#
# OpenHarmony hybris_generic — installer for Volla X23 / Volla Tablet (mimir).
#
# Run this from a host PC with `adb` installed, with a Volla device connected
# via adb in a fresh Ubuntu Touch installation. The script can deploy the OHOS
# rootfs (LXC container + systemd units), the kernel (boot.img + modules), or
# both.
#
# Layout expected next to this script (produced by package-deployment.sh):
#
#   ./DEVICE                                — single line: "x23" or "mimir"
#   ./rootfs/
#       ohos-rootfs.tar.gz                  — OHOS rootfs tarball
#       lxc/config                          — LXC config for the container
#       start-ohos.sh                       — container start hook
#       ohos-post-stop.sh                   — container post-stop hook
#       create-ohos-binder-devices.py       — binderfs setup (one-shot)
#       systemd/ohos.service                — systemd unit (container start)
#       systemd/ohos-binder-setup.service   — systemd unit (binderfs setup)
#   ./kernel/                               boot.img [vendor_boot.img] modules.tar.gz
#
# Each archive is built for one device (x23 or mimir); the DEVICE marker is
# informational only — the kernel artifacts inside are already device-specific.

set -e

DEPLOY_ROOTFS=false
DEPLOY_KERNEL=false
DEVICE_PASSWORD="1234"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOTFS_DIR=""
KERNEL_DIR=""

usage() {
    cat <<EOF
Usage: $0 [--rootfs] [--kernel] [--rootfs-dir <path>] [--kernel-dir <path>]

  --rootfs               Deploy OHOS rootfs + LXC config + systemd units.
  --kernel               Deploy kernel (boot.img, vendor_boot.img, modules).
  --rootfs-dir <path>    Override the rootfs source dir
                         (default: \$HERE/rootfs). The dir must contain
                         ohos-rootfs.tar.gz, lxc/config, start-ohos.sh,
                         ohos-post-stop.sh, create-ohos-binder-devices.py,
                         and systemd/{ohos,ohos-binder-setup}.service.
  --kernel-dir <path>    Override the kernel source dir
                         (default: \$HERE/kernel). The dir must contain
                         boot.img and modules.tar.gz; vendor_boot.img is optional.
  -h, --help             Show this help.

If neither --rootfs nor --kernel is given, both are deployed.
EOF
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        --rootfs) DEPLOY_ROOTFS=true; shift ;;
        --kernel) DEPLOY_KERNEL=true; shift ;;
        --rootfs-dir) ROOTFS_DIR="$2"; shift 2 ;;
        --kernel-dir) KERNEL_DIR="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) echo "Unknown argument: $1" >&2; usage ;;
    esac
done

if ! $DEPLOY_ROOTFS && ! $DEPLOY_KERNEL; then
    DEPLOY_ROOTFS=true
    DEPLOY_KERNEL=true
fi

[ -z "$ROOTFS_DIR" ] && ROOTFS_DIR="$HERE/rootfs"
[ -z "$KERNEL_DIR" ] && KERNEL_DIR="$HERE/kernel"

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1"
}

if ! command -v adb >/dev/null 2>&1; then
    echo "Error: adb not found in PATH." >&2
    exit 1
fi

if ! adb devices | awk -F'\t' 'NR>1 && $2=="device"{found=1} END{exit !found}'; then
    echo "Error: no adb device connected." >&2
    exit 1
fi

if [ -f "$HERE/DEVICE" ]; then
    log "Target device (from archive): $(cat "$HERE/DEVICE")"
fi

# ── Kernel deployment ───────────────────────────────────────────────────────
if $DEPLOY_KERNEL; then
    KDIR="$KERNEL_DIR"
    if [ ! -f "$KDIR/boot.img" ] || [ ! -f "$KDIR/modules.tar.gz" ]; then
        echo "Error: $KDIR is missing boot.img and/or modules.tar.gz." >&2
        exit 1
    fi

    log "Pushing kernel artifacts..."
    adb push "$KDIR/boot.img" /tmp/
    adb push "$KDIR/modules.tar.gz" /tmp/
    if [ -f "$KDIR/vendor_boot.img" ]; then
        adb push "$KDIR/vendor_boot.img" /tmp/
    fi

    log "Flashing kernel and installing modules on device..."
    adb shell <<EOF
set -e
echo "$DEVICE_PASSWORD" | sudo -S mount -o remount,rw /
echo "$DEVICE_PASSWORD" | sudo -S tar -xvf /tmp/modules.tar.gz --strip-components=1 -C /lib/modules
slot=\$(getprop ro.boot.slot_suffix)
echo "Current boot slot: \$slot"
echo "$DEVICE_PASSWORD" | sudo -S dd if=/tmp/boot.img of=/dev/disk/by-partlabel/boot\${slot}
if [ -f /tmp/vendor_boot.img ]; then
    echo "$DEVICE_PASSWORD" | sudo -S dd if=/tmp/vendor_boot.img of=/dev/disk/by-partlabel/vendor_boot\${slot}
fi
EOF
    log "Kernel deployment complete."
fi

# ── Rootfs deployment ───────────────────────────────────────────────────────
if $DEPLOY_ROOTFS; then
    RDIR="$ROOTFS_DIR"
    ROOTFS_TARBALL="$RDIR/ohos-rootfs.tar.gz"
    if [ ! -f "$ROOTFS_TARBALL" ]; then
        echo "Error: $ROOTFS_TARBALL not found." >&2
        exit 1
    fi
    for f in lxc/config start-ohos.sh ohos-post-stop.sh \
             create-ohos-binder-devices.py \
             systemd/ohos.service systemd/ohos-binder-setup.service; do
        if [ ! -f "$RDIR/$f" ]; then
            echo "Error: $RDIR/$f not found." >&2
            exit 1
        fi
    done

    log "Pushing rootfs tarball to device (this may take a while)..."
    adb shell mkdir -p /home/phablet/openharmony
    adb push "$ROOTFS_TARBALL" /home/phablet/openharmony/

    log "Pushing LXC config and helper scripts..."
    adb push "$RDIR/lxc/config" /home/phablet/openharmony/
    adb push "$RDIR/start-ohos.sh" /home/phablet/openharmony/
    adb push "$RDIR/ohos-post-stop.sh" /home/phablet/openharmony/
    adb push "$RDIR/systemd/ohos.service" /home/phablet/openharmony/
    adb push "$RDIR/systemd/ohos-binder-setup.service" /home/phablet/openharmony/
    adb push "$RDIR/create-ohos-binder-devices.py" /home/phablet/openharmony/

    log "Configuring directories and permissions on device..."
    adb shell "echo $DEVICE_PASSWORD | sudo -S mount -o remount,rw /"
    adb shell "echo $DEVICE_PASSWORD | sudo -S mkdir -p /var/lib/lxc/openharmony"
    adb shell "echo $DEVICE_PASSWORD | sudo -S mv /home/phablet/openharmony/config /var/lib/lxc/openharmony"

    log "Extracting rootfs on device..."
    adb shell "echo $DEVICE_PASSWORD | sudo -S rm -rf /home/phablet/openharmony/rootfs"
    adb shell "mkdir -p /home/phablet/openharmony/rootfs"
    adb shell "echo $DEVICE_PASSWORD | sudo -S tar -xzf /home/phablet/openharmony/ohos-rootfs.tar.gz -C /home/phablet/openharmony/rootfs"

    log "Setting permissions and installing systemd units..."
    adb shell chmod +x /home/phablet/openharmony/start-ohos.sh
    adb shell chmod +x /home/phablet/openharmony/ohos-post-stop.sh
    # Sandbox configs must be world-readable for nwebspawn (uid 3081). See
    # README.md Phase 8.18 for the full rationale.
    adb shell "echo $DEVICE_PASSWORD | sudo -S chmod 0644 /home/phablet/openharmony/rootfs/system/etc/sandbox/appdata-sandbox.json /home/phablet/openharmony/rootfs/system/etc/sandbox/appdata-sandbox-isolated.json"
    adb shell "echo $DEVICE_PASSWORD | sudo -S mv /home/phablet/openharmony/ohos.service /lib/systemd/system"
    adb shell "echo $DEVICE_PASSWORD | sudo -S mv /home/phablet/openharmony/ohos-binder-setup.service /lib/systemd/system"
    adb shell "echo $DEVICE_PASSWORD | sudo -S systemctl enable ohos"
    adb shell "echo $DEVICE_PASSWORD | sudo -S systemctl enable ohos-binder-setup"

    log "Rootfs deployment complete."
fi

log "Rebooting device..."
adb shell "echo $DEVICE_PASSWORD | sudo -S reboot" || true
log "Device reboot initiated. After reboot, OHOS should start automatically."
