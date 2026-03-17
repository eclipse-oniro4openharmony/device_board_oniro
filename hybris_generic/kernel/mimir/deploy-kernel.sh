#!/bin/bash
set -e

# This script automates the deployment of kernel modules and boot images to the Volla Tablet (mimir).
# Sudo password on the device is expected to be '1234'.

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OHOS_SOURCE_ROOT="$(cd "$HERE/../../../../../.." && pwd)"
KERNEL_OUT="$OHOS_SOURCE_ROOT/kernel/linux/volla-mimir/out"
MODULES_SRC="$OHOS_SOURCE_ROOT/kernel/linux/volla-mimir/build-dir/tmp/system/lib/modules"

DEVICE_PASS="1234"

# 1. Check if artifacts exist
if [ ! -f "$KERNEL_OUT/modules.tar.gz" ]; then
    echo "Error: $KERNEL_OUT/modules.tar.gz not found!"
    exit 1
fi

if [ ! -f "$KERNEL_OUT/boot.img" ]; then
    echo "Error: $KERNEL_OUT/boot.img not found!"
    exit 1
fi

# Check for adb connection
if ! adb devices | grep -q "[0-9a-fA-F]\+[[:space:]]\+device$"; then
    if ! adb devices | grep -q "device$"; then
        echo "Error: No device connected via adb."
        exit 1
    fi
fi

# 2. Push artifacts to the device
echo "Pushing artifacts to device..."
adb push "$KERNEL_OUT/modules.tar.gz" /tmp
adb push "$KERNEL_OUT/boot.img" /tmp
if [ -f "$KERNEL_OUT/vendor_boot.img" ]; then
    adb push "$KERNEL_OUT/vendor_boot.img" /tmp
fi

# 3. Install and Flash on device
echo "Flashing kernel and installing modules on device..."
adb shell <<EOF
echo "$DEVICE_PASS" | sudo -S mount -o remount,rw /
# Extract modules without the 'modules/' prefix into /lib/modules/
echo "$DEVICE_PASS" | sudo -S tar -xvf /tmp/modules.tar.gz --strip-components=1 -C /lib/modules
slot=\$(getprop ro.boot.slot_suffix)
echo "Current boot slot: \$slot"
echo "$DEVICE_PASS" | sudo -S dd if=/tmp/boot.img of=/dev/disk/by-partlabel/boot\${slot}
if [ -f /tmp/vendor_boot.img ]; then
    echo "$DEVICE_PASS" | sudo -S dd if=/tmp/vendor_boot.img of=/dev/disk/by-partlabel/vendor_boot\${slot}
fi
echo "Rebooting device..."
echo "$DEVICE_PASS" | sudo -S reboot
EOF

echo "Deployment script finished."
