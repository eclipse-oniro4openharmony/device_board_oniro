#!/bin/sh
#
# Copyright (C) 2026 Oniro / Hybris Generic.
# Licensed under the Apache License, Version 2.0 (the "License").
#
# Phase N11 chain-load /init for OHOS native boot.
#
# Boots Halium's kernel + vendor_boot ramdisk, modprobes Halium's vendor
# kernel modules to bring up the block subsystem, mounts the OHOS system_a
# and vendor_a logical partitions from `super`, then `exec chroot`s into
# /system/bin/init.
#
# This script replaces /init inside the Halium boot.img ramdisk; see
# device/board/oniro/hybris_generic/kernel/x23/build_boot_img_chainload.sh
# for how the boot.img is repacked.

# No `set -u` and no `set -e`: busybox ash in the Halium initramfs is
# flaky with strict modes (silent exits on benign empty expansions), and
# we prefer to keep going through best-effort steps.

# ---------------------------------------------------------------------------
# Stage 0 — basic mounts.  The Halium initramfs only has /, /bin, /sbin,
# /etc, /scripts, /usr — every other dir we need we mkdir here.
# ---------------------------------------------------------------------------
[ -d /dev  ] || mkdir -m 0755 /dev
[ -d /root ] || mkdir -m 0700 /root
[ -d /sys  ] || mkdir /sys
[ -d /proc ] || mkdir /proc
[ -d /tmp  ] || mkdir /tmp

mount -t sysfs    -o nodev,noexec,nosuid sysfs /sys
mount -t proc     -o nodev,noexec,nosuid proc  /proc
mount -t devtmpfs -o nosuid,mode=0755    udev  /dev

# Redirect stdout/stderr to kmsg for any later diagnostics.
exec > /dev/kmsg 2>&1
echo "[init-chainload] starting"

# ---------------------------------------------------------------------------
# Stage 1 — load Halium's vendor kernel modules so the block subsystem
# comes up.  Modules live in vendor_boot's ramdisk at /lib/modules/*.ko;
# /lib/modules/modules.load is the ordered list that Halium itself uses
# (see scripts/halium:load_kernel_modules).
#
# Important — kmod's `modprobe` expects module *names* (not full paths)
# and resolves dependencies via modules.dep, which requires the
# /lib/modules/$(uname -r) self-symlink first.  Passing full .ko paths
# bypasses dep resolution and leaves UFS unloaded → no /dev/sdc*.
# ---------------------------------------------------------------------------
cd /lib/modules
ln -sf /lib/modules "/lib/modules/$(uname -r)" 2>/dev/null
if [ -f modules.load ]; then
    while read line; do
        set -- $line
        [ "$1" = "#" ] && continue
        [ -n "$1" ] || continue
        modprobe -a "$1" 2>/dev/null || true
    done < modules.load
fi
cd /

# Halt instead of silent reboot on any kernel panic — easier to diagnose.
echo 0 > /proc/sys/kernel/panic 2>/dev/null

# ---------------------------------------------------------------------------
# Stage 2 — find `super` and unpack the logical partitions inside it.
# ---------------------------------------------------------------------------
SUPER_DEV=""
i=0
while [ -z "$SUPER_DEV" ] && [ "$i" -lt 50 ]; do
    for ueventf in /sys/class/block/*/uevent; do
        partname="$(grep '^PARTNAME=' "$ueventf" 2>/dev/null | cut -d= -f2)"
        devname="$(grep  '^DEVNAME='  "$ueventf" 2>/dev/null | cut -d= -f2)"
        [ "$partname" = "super" ] && SUPER_DEV="/dev/$devname"
    done
    [ -n "$SUPER_DEV" ] && break
    sleep 0.1
    i=$((i + 1))
done
[ -n "$SUPER_DEV" ] || { echo "[init-chainload] super not found"; exec /bin/sh; }
echo "[init-chainload] super = $SUPER_DEV"

# parse-android-dynparts (in Halium's ramdisk) reads the LP metadata and
# emits dmsetup commands that materialise the logical partitions as
# /dev/mapper/<name> via /dev/dm-N.
parse-android-dynparts "$SUPER_DEV" > /tmp/dyn.sh
sh /tmp/dyn.sh

# `dmsetup mknodes` creates /dev/mapper/* symlinks — normally a udev job
# (which doesn't run here).
dmsetup mknodes 2>/dev/null

# Wait for system_a, vendor_a, and sys_prod_a nodes to appear.  chip_prod_a
# is soft-required (no load-bearing params on it today) so it's not in the
# wait loop, but we still try to mount it below.
i=0
while { [ ! -b /dev/mapper/system_a ] || [ ! -b /dev/mapper/vendor_a ] \
        || [ ! -b /dev/mapper/sys_prod_a ]; } && [ "$i" -lt 50 ]; do
    sleep 0.1
    i=$((i + 1))
done
[ -b /dev/mapper/system_a ]   || { echo "[init-chainload] system_a never appeared";   exec /bin/sh; }
[ -b /dev/mapper/vendor_a ]   || { echo "[init-chainload] vendor_a never appeared";   exec /bin/sh; }
[ -b /dev/mapper/sys_prod_a ] || { echo "[init-chainload] sys_prod_a never appeared"; exec /bin/sh; }

# ---------------------------------------------------------------------------
# Stage 3 — mount OHOS system_a + vendor_a + sys_prod_a + chip_prod_a
# read-only into /root.  Note we trust the mount return code rather than
# `mountpoint -q`: the latter is unreliable in this initramfs (it
# misclassifies bind/move mounts).
#
# sys_prod must be mounted here (NOT via OHOS fstab) because OHOS init's
# InitLoadParamFiles() scans /sys_prod/etc/param/ before pre-init's
# `mount_fstab_sp` runs.  Mounting here makes hybris_native.para visible
# to the initial param scan, so persist.hdc.mode.usb=enable and
# const.security.developermode.state=true are set without a separate
# `setparam` workaround in z_hdcd_autostart.cfg.
# ---------------------------------------------------------------------------
mount -t ext4 -o ro /dev/mapper/system_a /root || {
    echo "[init-chainload] mount system_a failed"; exec /bin/sh; }
[ -d /root/vendor ] || mkdir -p /root/vendor 2>/dev/null
mount -t ext4 -o ro /dev/mapper/vendor_a /root/vendor || {
    echo "[init-chainload] mount vendor_a failed"; exec /bin/sh; }
[ -d /root/sys_prod ] || mkdir -p /root/sys_prod 2>/dev/null
mount -t ext4 -o ro /dev/mapper/sys_prod_a /root/sys_prod || {
    echo "[init-chainload] mount sys_prod_a failed"; exec /bin/sh; }
[ -d /root/chip_prod ] || mkdir -p /root/chip_prod 2>/dev/null
mount -t ext4 -o ro /dev/mapper/chip_prod_a /root/chip_prod 2>/dev/null \
    || echo "[init-chainload] mount chip_prod_a failed (non-fatal)"

[ -x /root/system/bin/init ] || {
    echo "[init-chainload] /root/system/bin/init missing — wrong partition?"
    exec /bin/sh
}

# ---------------------------------------------------------------------------
# Stage 4 — pre-populate /root/dev/disk/by-partlabel/ so OHOS init's
# fstab entries that reference partitions by name (userdata, misc,
# persist, …) resolve without waiting on a ueventd it can't usefully
# consume here.
# ---------------------------------------------------------------------------
mkdir -p /root/dev/disk/by-partlabel
for ueventf in /sys/class/block/*/uevent; do
    partname="$(grep '^PARTNAME=' "$ueventf" 2>/dev/null | cut -d= -f2)"
    devname="$(grep  '^DEVNAME='  "$ueventf" 2>/dev/null | cut -d= -f2)"
    [ -n "$partname" ] && [ -n "$devname" ] && \
        ln -sf "/dev/$devname" "/root/dev/disk/by-partlabel/$partname"
done

# ---------------------------------------------------------------------------
# Stage 5 — bind-mount /proc /sys /dev into the chroot.
# CRITICAL: use `-o bind`, NOT `-o move`.  On this kernel `mount -o move`
# returns success but leaves the destination inaccessible from the
# chrooted child (verified empirically with a static-linked test init).
# ---------------------------------------------------------------------------
mount -o bind /proc /root/proc
mount -o bind /sys  /root/sys
mount -o bind /dev  /root/dev

# ---------------------------------------------------------------------------
# Stage 6 — exec OHOS init.  PID 1 is preserved by exec.
#
# Use `chroot`, not busybox `switch_root`.  switch_root deletes the old
# root and gets fussy about what's still mounted; chroot just changes
# the path anchor for the exec'd process.
#
# OHOS_NATIVE_BOOT=1 tells OHOS init's DoMkSandbox to skip pivot_root.
# Without that env var, the unshare()+pivot_root+umount2 sequence inside
# a chrooted PID 1 leaves init's fs_struct dangling and every subsequent
# fork+exec from init fails silently — see
# base/startup/init/services/init/standard/init_cmds.c:DoMkSandbox.
# ---------------------------------------------------------------------------
echo "[init-chainload] handing off to OHOS init"
exec env OHOS_NATIVE_BOOT=1 chroot /root /system/bin/init --second-stage

# Fallthrough — should never reach.
echo "[init-chainload] exec failed; dropping to shell"
exec /bin/sh
