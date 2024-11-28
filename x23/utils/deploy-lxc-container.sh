#!/bin/bash

export OHOS_DIR=$(dirname "$(realpath $0)")/../../../../../

ROOTFS_TARBALL_PATH="${OHOS_DIR}/out/x23/packages/phone/images/ohos-rootfs.tar.gz"
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
  if [ "$#" -eq 0 ]; then
    usage
  fi
  case "$opt" in
    p) PREBUILT_ROOTFS_TARBALL="$OPTARG";;
    d) DISABLE_SERVICE=true;;
    *) usage;;
  esac
done

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1" 
}

log "Starting OHOS deployment script."

if [ "$DISABLE_SERVICE" = true ]; then
    log "Disabling ohos systemd service..."
    adb shell "echo $DEVICE_PASSWORD | sudo -S systemctl disable ohos" && log "Disabled ohos systemd service."
    log "Rebooting device..."
    adb shell "echo $DEVICE_PASSWORD | sudo -S reboot" && log "Device reboot initiated."
    exit 0
fi

if [ -z "$PREBUILT_ROOTFS_TARBALL" ]; then
    log "Removing existing root filesystem directories..."
    sudo rm -rf ${OHOS_DIR}/out/x23/packages/phone/images/ohos-rootfs* && log "Root filesystem removed."

    log "Copying root filesystem..."
    sudo cp -r ${OHOS_DIR}/out/x23/packages/phone/root/ ${OHOS_DIR}/out/x23/packages/phone/images/ohos-rootfs/ && log "Root directory copied."
    sudo cp -r ${OHOS_DIR}/out/x23/packages/phone/system ${OHOS_DIR}/out/x23/packages/phone/images/ohos-rootfs/ && log "System directory copied."
    sudo cp -r ${OHOS_DIR}/out/x23/packages/phone/vendor/ ${OHOS_DIR}/out/x23/packages/phone/images/ohos-rootfs/ && log "Vendor directory copied."
    sudo cp -r ${OHOS_DIR}/out/x23/packages/phone/chip_prod/ ${OHOS_DIR}/out/x23/packages/phone/images/ohos-rootfs/ && log "Chip production directory copied."

    log "Creating root filesystem archive..."
    sudo tar -czvf $ROOTFS_TARBALL_PATH -C ${OHOS_DIR}/out/x23/packages/phone/images/ohos-rootfs . && log "Root filesystem archive created."
else
    log "Using prebuilt root filesystem archive: $PREBUILT_ROOTFS_TARBALL"
    ROOTFS_TARBALL_PATH="$PREBUILT_ROOTFS_TARBALL"
fi

log "Pushing root filesystem archive to device..."
adb shell mkdir -p /home/phablet/openharmony && log "Created openharmony directory on device."
adb push $ROOTFS_TARBALL_PATH /home/phablet/openharmony/ && log "Root filesystem archive pushed to device."

log "Pushing additional files to device..."
adb push ${OHOS_DIR}/device/board/oniro/x23/utils/lxc/config /home/phablet/openharmony/ && log "Config file pushed."
adb push ${OHOS_DIR}/device/board/oniro/x23/utils/start-ohos.sh /home/phablet/openharmony/ && log "Start script pushed."
adb push ${OHOS_DIR}/device/board/oniro/x23/utils/systemd/ohos.service /home/phablet/openharmony/ && log "Service file pushed."

log "Configuring device directories and permissions..."
adb shell "echo $DEVICE_PASSWORD | sudo -S mount -o remount,rw /" && log "Remounted root filesystem as read-write."
adb shell "echo $DEVICE_PASSWORD | sudo -S mkdir -p /var/lib/lxc/openharmony" && log "Created LXC directory on device."
adb shell "echo $DEVICE_PASSWORD | sudo -S mv /home/phablet/openharmony/config /var/lib/lxc/openharmony" && log "Moved config file to LXC directory."

log "Extracting root filesystem on device..."
adb shell "echo $DEVICE_PASSWORD | sudo -S rm -rf /home/phablet/openharmony/rootfs" && log "Removed existing rootfs directory on device."
adb shell mkdir -p /home/phablet/openharmony/rootfs && log "Created rootfs directory on device."
adb shell tar -xzvf /home/phablet/openharmony/$(basename $ROOTFS_TARBALL_PATH) -C /home/phablet/openharmony/rootfs && log "Extracted root filesystem archive on device."

log "Setting permissions and moving service file..."
adb shell chmod +x /home/phablet/openharmony/start-ohos.sh && log "Made start script executable."
adb shell "echo $DEVICE_PASSWORD | sudo -S mv /home/phablet/openharmony/ohos.service /lib/systemd/system" && log "Moved service file to systemd directory."
adb shell "echo $DEVICE_PASSWORD | sudo -S systemctl enable ohos" && log "Enabled ohos systemd service."

log "Rebooting device..."
adb shell "echo $DEVICE_PASSWORD | sudo -S reboot" && log "Device reboot initiated."

log "OHOS deployment script completed."

