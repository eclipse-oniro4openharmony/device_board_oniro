#!/bin/bash

export OHOS_DIR=$(dirname "$(realpath $0)")/../../../../../

ROOTFS_TARBALL_PATH="${OHOS_DIR}/out/hybris_generic/packages/phone/images/ohos-rootfs.tar.gz"
PREBUILT_ROOTFS_TARBALL=""
DEVICE_PASSWORD="1234"
DISABLE_SERVICE=false

usage() {
    echo "Usage: $0 [-p <path_to_prebuilt_rootfs_tarball>] [-d]"
    echo "  -p: Specify the path to a prebuilt rootfs tarball. If provided, skips the creation of the rootfs tarball and uses the specified file instead."
    echo "  -d: Disable the ohos systemd service and reboot the device. Exits the script without deploying."
    exit 1
}

while getopts "p:d" opt; do
  case "$opt" in
    p) PREBUILT_ROOTFS_TARBALL="$OPTARG";;
    d) DISABLE_SERVICE=true;;
    *) usage;;
  esac
done

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1" 
}

log "Starting OHOS deployment script for hybris_generic."

if [ "$DISABLE_SERVICE" = true ]; then
    log "Disabling ohos systemd service..."
    adb shell "echo $DEVICE_PASSWORD | sudo -S systemctl disable ohos" && log "Disabled ohos systemd service."
    log "Rebooting device..."
    adb shell "echo $DEVICE_PASSWORD | sudo -S reboot" && log "Device reboot initiated."
    exit 0
fi

if [ -z "$PREBUILT_ROOTFS_TARBALL" ]; then
    log "Removing existing root filesystem directories..."
    sudo rm -rf ${OHOS_DIR}/out/hybris_generic/packages/phone/images/ohos-rootfs* && log "Root filesystem removed."

    log "Preparing root filesystem directory..."
    mkdir -p ${OHOS_DIR}/out/hybris_generic/packages/phone/images/ohos-rootfs

    log "Copying root filesystem contents..."
    sudo cp -a ${OHOS_DIR}/out/hybris_generic/packages/phone/root/. ${OHOS_DIR}/out/hybris_generic/packages/phone/images/ohos-rootfs/ && log "Root directory contents copied."
    
    # Ensure system and vendor directories exist
    sudo mkdir -p ${OHOS_DIR}/out/hybris_generic/packages/phone/images/ohos-rootfs/system
    sudo mkdir -p ${OHOS_DIR}/out/hybris_generic/packages/phone/images/ohos-rootfs/vendor
    
    sudo cp -a ${OHOS_DIR}/out/hybris_generic/packages/phone/system/. ${OHOS_DIR}/out/hybris_generic/packages/phone/images/ohos-rootfs/system/ && log "System directory contents copied."
    sudo cp -a ${OHOS_DIR}/out/hybris_generic/packages/phone/vendor/. ${OHOS_DIR}/out/hybris_generic/packages/phone/images/ohos-rootfs/vendor/ && log "Vendor directory contents copied."
    
    # chip_prod might not exist in hybris_generic, but let's check and copy if it does
    if [ -d "${OHOS_DIR}/out/hybris_generic/packages/phone/chip_prod" ]; then
        sudo mkdir -p ${OHOS_DIR}/out/hybris_generic/packages/phone/images/ohos-rootfs/chip_prod
        sudo cp -a ${OHOS_DIR}/out/hybris_generic/packages/phone/chip_prod/. ${OHOS_DIR}/out/hybris_generic/packages/phone/images/ohos-rootfs/chip_prod/ && log "Chip production directory copied."
    fi

    # sys_prod holds product-level overrides (e.g. product_hybris_generic.para which
    # enables developer mode + hdcd TCP for the container). init scans /sys_prod/etc/param
    # as part of the default cfg-policy layer, so this must land in the rootfs.
    if [ -d "${OHOS_DIR}/out/hybris_generic/packages/phone/sys_prod" ]; then
        sudo mkdir -p ${OHOS_DIR}/out/hybris_generic/packages/phone/images/ohos-rootfs/sys_prod
        sudo cp -a ${OHOS_DIR}/out/hybris_generic/packages/phone/sys_prod/. ${OHOS_DIR}/out/hybris_generic/packages/phone/images/ohos-rootfs/sys_prod/ && log "System production directory copied."
    fi

    # Overlay libhybris test binaries from the thirdparty output directory.
    # The packages/phone/system/bin/ copies are from a full packaging run and may be
    # stale when only a targeted build was done. The thirdparty/libhybris/ binaries
    # are always up-to-date after any libhybris build.
    for test_bin in test_hwcomposer test_egl test_egl_configs test_dlopen; do
        src="${OHOS_DIR}/out/hybris_generic/thirdparty/libhybris/${test_bin}"
        dst="${OHOS_DIR}/out/hybris_generic/packages/phone/images/ohos-rootfs/system/bin/${test_bin}"
        if [ -f "$src" ]; then
            sudo cp "$src" "$dst" && log "Overlaid ${test_bin} from thirdparty output."
        fi
    done

    # Create common missing directories
    sudo mkdir -p ${OHOS_DIR}/out/hybris_generic/packages/phone/images/ohos-rootfs/data
    sudo mkdir -p ${OHOS_DIR}/out/hybris_generic/packages/phone/images/ohos-rootfs/proc
    sudo mkdir -p ${OHOS_DIR}/out/hybris_generic/packages/phone/images/ohos-rootfs/sys
    sudo mkdir -p ${OHOS_DIR}/out/hybris_generic/packages/phone/images/ohos-rootfs/dev

    # Create Android partition mount points (used by libhybris to access HAL blobs)
    sudo mkdir -p ${OHOS_DIR}/out/hybris_generic/packages/phone/images/ohos-rootfs/android/system
    sudo mkdir -p ${OHOS_DIR}/out/hybris_generic/packages/phone/images/ohos-rootfs/android/vendor
    sudo mkdir -p ${OHOS_DIR}/out/hybris_generic/packages/phone/images/ohos-rootfs/android/odm

    # NOTE: No EGL/GLES placeholder files created here. The Android libEGL.so bind mounts
    # (present in phase 5.7a) were removed (2026-03-20) because they shadowed the OHOS
    # opengl_wrapper at /system/lib64/platformsdk/libEGL.so, causing cascading load failures
    # in OHOS services (libwms, libbms, etc.) via the missing libbacktrace.so dependency.
    # Android libs resolve their EGL/GLES deps via the hybris linker's internal path remapping.

    # Create placeholder bind-mount target directories for Android vendor HAL libs
    sudo mkdir -p ${OHOS_DIR}/out/hybris_generic/packages/phone/images/ohos-rootfs/vendor/lib64/egl
    sudo mkdir -p ${OHOS_DIR}/out/hybris_generic/packages/phone/images/ohos-rootfs/vendor/lib64/hw

    # Create placeholder for DRM/KMS device directory
    sudo mkdir -p ${OHOS_DIR}/out/hybris_generic/packages/phone/images/ohos-rootfs/dev/dri

    # Add HYBRIS env vars to hdf_devhost.cfg if not already present.
    # The original cfg from the OHOS build does not have the env blocks needed for
    # libhybris to load Android HALs in composer_host / allocator_host.
    HDF_CFG="${OHOS_DIR}/out/hybris_generic/packages/phone/images/ohos-rootfs/vendor/etc/init/hdf_devhost.cfg"
    if [ -f "$HDF_CFG" ] && ! grep -q 'HYBRIS_EGLPLATFORM' "$HDF_CFG"; then
        log "Adding HYBRIS env vars to hdf_devhost.cfg..."
        sudo python3 -c "
import json, sys
with open('$HDF_CFG') as f:
    cfg = json.load(f)
hybris_env = [
    {'name': 'HYBRIS_LD_LIBRARY_PATH', 'value': '/android/vendor/lib64:/android/system/lib64'},
    {'name': 'LD_LIBRARY_PATH', 'value': '/system/lib64/libhybris:/system/lib64'},
    {'name': 'HYBRIS_EGLPLATFORM', 'value': 'ohos'}
]
for svc in cfg.get('services', []):
    if svc['name'] in ('composer_host', 'allocator_host'):
        svc['env'] = hybris_env
with open('$HDF_CFG', 'w') as f:
    json.dump(cfg, f, indent=4)
" && log "hdf_devhost.cfg updated with HYBRIS env."
    fi

    # Copy hdf_devhost.cfg to /system/etc/init/ as well, because some rootfs builds
    # do not scan /vendor/etc/init/ in container mode.
    if [ -f "$HDF_CFG" ]; then
        sudo cp "$HDF_CFG" "${OHOS_DIR}/out/hybris_generic/packages/phone/images/ohos-rootfs/system/etc/init/hdf_devhost.cfg" && log "hdf_devhost.cfg copied to /system/etc/init/."
    fi

    log "Creating root filesystem archive..."
    sudo tar -czf $ROOTFS_TARBALL_PATH -C ${OHOS_DIR}/out/hybris_generic/packages/phone/images/ohos-rootfs . && log "Root filesystem archive created."
else
    log "Using prebuilt root filesystem archive: $PREBUILT_ROOTFS_TARBALL"
    ROOTFS_TARBALL_PATH="$PREBUILT_ROOTFS_TARBALL"
fi

log "Pushing root filesystem archive to device..."
adb shell mkdir -p /home/phablet/openharmony && log "Created openharmony directory on device."
adb push $ROOTFS_TARBALL_PATH /home/phablet/openharmony/ && log "Root filesystem archive pushed to device."

log "Pushing additional files to device..."
adb push ${OHOS_DIR}/device/board/oniro/hybris_generic/utils/lxc/config /home/phablet/openharmony/ && log "Config file pushed."
adb push ${OHOS_DIR}/device/board/oniro/hybris_generic/utils/start-ohos.sh /home/phablet/openharmony/ && log "Start script pushed."
adb push ${OHOS_DIR}/device/board/oniro/hybris_generic/utils/systemd/ohos.service /home/phablet/openharmony/ && log "Service file pushed."
adb push ${OHOS_DIR}/device/board/oniro/hybris_generic/utils/systemd/ohos-binder-setup.service /home/phablet/openharmony/ && log "Binder setup service file pushed."
adb push ${OHOS_DIR}/device/board/oniro/hybris_generic/utils/create-ohos-binder-devices.py /home/phablet/openharmony/ && log "Binder device creation script pushed."

log "Configuring device directories and permissions..."
adb shell "echo $DEVICE_PASSWORD | sudo -S mount -o remount,rw /" && log "Remounted root filesystem as read-write."
adb shell "echo $DEVICE_PASSWORD | sudo -S mkdir -p /var/lib/lxc/openharmony" && log "Created LXC directory on device."
adb shell "echo $DEVICE_PASSWORD | sudo -S mv /home/phablet/openharmony/config /var/lib/lxc/openharmony" && log "Moved config file to LXC directory."

log "Extracting root filesystem on device..."
adb shell "echo $DEVICE_PASSWORD | sudo -S rm -rf /home/phablet/openharmony/rootfs" && log "Removed existing rootfs directory on device."
adb shell "mkdir -p /home/phablet/openharmony/rootfs" && log "Created rootfs directory on device."
log "Extracting tarball (this may take a while)..."
adb shell "echo $DEVICE_PASSWORD | sudo -S tar -xzf /home/phablet/openharmony/$(basename $ROOTFS_TARBALL_PATH) -C /home/phablet/openharmony/rootfs" && log "Extracted root filesystem archive on device."

log "Setting permissions and moving service files..."
adb shell chmod +x /home/phablet/openharmony/start-ohos.sh && log "Made start script executable."
# Make sandbox configs world-readable so nwebspawn (uid 3081, not root) can
# load them at preload time. Upstream appdata_sandbox_fixer.py installs these
# with mode 0660 which umask trims to 0640; on a real OHOS image fs_config
# rewrites them, but our LXC rootfs keeps the literal mode. Without this,
# LoadAppSandboxConfigCJson silently fails inside nwebspawn, the render
# sandbox mount-paths are skipped, and every webview renderer exits in
# SetFileDescriptors trying to open /dev/null. See README.md Phase 8.18.
adb shell "echo $DEVICE_PASSWORD | sudo -S chmod 0644 /home/phablet/openharmony/rootfs/system/etc/sandbox/appdata-sandbox.json /home/phablet/openharmony/rootfs/system/etc/sandbox/appdata-sandbox-isolated.json" && log "Made sandbox configs world-readable for nwebspawn."
adb shell "echo $DEVICE_PASSWORD | sudo -S mv /home/phablet/openharmony/ohos.service /lib/systemd/system" && log "Moved ohos service file to systemd directory."
adb shell "echo $DEVICE_PASSWORD | sudo -S mv /home/phablet/openharmony/ohos-binder-setup.service /lib/systemd/system" && log "Moved binder setup service file to systemd directory."
adb shell "echo $DEVICE_PASSWORD | sudo -S systemctl enable ohos" && log "Enabled ohos systemd service."
adb shell "echo $DEVICE_PASSWORD | sudo -S systemctl enable ohos-binder-setup" && log "Enabled ohos-binder-setup systemd service."

log "Deployment complete. To start the container, run 'sudo start-ohos.sh' on the device or 'sudo systemctl start ohos'."
