#!/bin/bash

export OHOS_DIR=$(dirname "$(realpath $0)")/../../../../../

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1" 
}

log "Starting OHOS deployment script."

log "Removing existing root filesystem directories..."
sudo rm -rf ${OHOS_DIR}/out/x23/packages/phone/images/ohos-rootfs* && log "Root filesystem removed."

log "Copying root filesystem..."
sudo cp -r ${OHOS_DIR}/out/x23/packages/phone/root/ ${OHOS_DIR}/out/x23/packages/phone/images/ohos-rootfs/ && log "Root directory copied."
sudo cp -r ${OHOS_DIR}/out/x23/packages/phone/system ${OHOS_DIR}/out/x23/packages/phone/images/ohos-rootfs/ && log "System directory copied."
sudo cp -r ${OHOS_DIR}/out/x23/packages/phone/vendor/ ${OHOS_DIR}/out/x23/packages/phone/images/ohos-rootfs/ && log "Vendor directory copied."
sudo cp -r ${OHOS_DIR}/out/x23/packages/phone/chip_prod/ ${OHOS_DIR}/out/x23/packages/phone/images/ohos-rootfs/ && log "Chip production directory copied."

log "Creating root filesystem archive..."
sudo tar -cvf ${OHOS_DIR}/out/x23/packages/phone/images/ohos-rootfs.tar -C ${OHOS_DIR}/out/x23/packages/phone/images/ohos-rootfs . && log "Root filesystem archive created."

log "Pushing root filesystem archive to device..."
adb shell mkdir -p /home/phablet/openharmony && log "Created openharmony directory on device."
adb push ${OHOS_DIR}/out/x23/packages/phone/images/ohos-rootfs.tar /home/phablet/openharmony/ && log "Root filesystem archive pushed to device."

log "Pushing additional files to device..."
adb push ${OHOS_DIR}/device/board/oniro/x23/utils/lxc/config /home/phablet/openharmony/ && log "Config file pushed."
adb push ${OHOS_DIR}/device/board/oniro/x23/utils/start-ohos.sh /home/phablet/openharmony/ && log "Start script pushed."
adb push ${OHOS_DIR}/device/board/oniro/x23/utils/systemd/ohos.service /home/phablet/openharmony/ && log "Service file pushed."

log "Configuring device directories and permissions..."
adb shell "echo 1234 | sudo -S mount -o remount,rw /" && log "Remounted root filesystem as read-write."
adb shell "echo 1234 | sudo -S mkdir -p /var/lib/lxc/openharmony" && log "Created LXC directory on device."
adb shell "echo 1234 | sudo -S mv /home/phablet/openharmony/config /var/lib/lxc/openharmony" && log "Moved config file to LXC directory."

log "Extracting root filesystem on device..."
adb shell "echo 1234 | sudo -S rm -rf /home/phablet/openharmony/rootfs" && log "Removed existing rootfs directory on device."
adb shell mkdir -p /home/phablet/openharmony/rootfs && log "Created rootfs directory on device."
adb shell tar -xvf /home/phablet/openharmony/ohos-rootfs.tar -C /home/phablet/openharmony/rootfs && log "Extracted root filesystem archive on device."

log "Setting permissions and moving service file..."
adb shell chmod +x /home/phablet/openharmony/start-ohos.sh && log "Made start script executable."
adb shell "echo 1234 | sudo -S mv /home/phablet/openharmony/ohos.service /lib/systemd/system" && log "Moved service file to systemd directory."

log "Rebooting device..."
adb shell "echo 1234 | sudo -S reboot" && log "Device reboot initiated."

log "OHOS deployment script completed."

