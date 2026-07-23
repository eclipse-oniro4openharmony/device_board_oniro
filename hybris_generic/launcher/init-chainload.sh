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

# Device this image was spliced for.  The splice/build scripts substitute
# the placeholder (kernel/ansuz/build_init_boot_chainload.sh -> "ansuz");
# an unsubstituted placeholder means an X23-era build, where every new
# per-device branch below must stay inert.
HYBRIS_DEVICE="@HYBRIS_DEVICE@"
case "$HYBRIS_DEVICE" in ansuz) ;; *) HYBRIS_DEVICE=x23 ;; esac

# ---------------------------------------------------------------------------
# Stage -1 (ansuz) — recovery/charger passthrough.  On the Plinius the
# chainload lives in init_boot's generic ramdisk, which is ALSO loaded
# for recovery boots (v4 unified recovery: the ABM recovery ramdisk is a
# vendor_boot fragment, cpio-concatenated over us).  A recovery boot is
# "/ramdisk-recovery.img present AND force_normal_boot != 1" — same
# detection as Halium's scripts/halium — and charger boots come in via
# androidboot.mode=charger.  In both cases the preserved Halium init
# (/init.halium, saved by the splice script) must run instead of us, so
# the on-device rescue path keeps working with the chainload installed.
# Detection needs /proc only; mount it briefly and restore virgin state
# before the exec.
# ---------------------------------------------------------------------------
if [ "$HYBRIS_DEVICE" = "ansuz" ] && [ -x /init.halium ]; then
    [ -d /proc ] || mkdir /proc
    mount -t proc proc /proc 2>/dev/null
    pass=""
    normal_boot=""
    grep -qw 'androidboot.force_normal_boot=1' /proc/cmdline 2>/dev/null && normal_boot=y
    grep -q 'androidboot.force_normal_boot = "1"' /proc/bootconfig 2>/dev/null && normal_boot=y
    [ -f /ramdisk-recovery.img ] && [ -z "$normal_boot" ] && pass=recovery
    grep -qw 'androidboot.mode=charger' /proc/cmdline 2>/dev/null && pass=charger
    grep -q 'androidboot.mode = "charger"' /proc/bootconfig 2>/dev/null && pass=charger
    if [ -n "$pass" ]; then
        umount /proc 2>/dev/null
        exec /init.halium
    fi
    umount /proc 2>/dev/null
fi

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

# Fatal-error hook.  With no display and no shell console, a bare
# `exec /bin/sh` is a black hole for remote debugging — if the Halium
# ramdisk ships the initramfs panic helper (it does on ansuz), bring up
# the USB RNDIS + telnet rescue instead (device enumerates 18d1:d001
# "Failed to boot", telnet 192.168.2.15:23 from the USB host, DHCP
# served to the host by the device).  Falls back to /bin/sh.
rescue() {
    echo "[init-chainload] FATAL: $1"
    if [ -f /scripts/panic/telnet ]; then
        echo "[init-chainload] starting USB telnet rescue"
        sh /scripts/panic/telnet
    fi
    exec /bin/sh
}

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
        # Skip the Mali GPU stack here — loading mali_kbase this early
        # (before OHOS userspace) panics the kernel.  The .ko files stay
        # bundled in /lib/modules; OHOS loads them post-boot via
        # androidd once the system is up (see phase_n8 §N8.11).
        case "$1" in
            mali_*|mali-*) continue ;;
        esac
        modprobe -a "$1" 2>/dev/null || true
    done < modules.load
fi

# Load syscon-reboot-mode so OHOS userspace can enter LK fastboot via
# `reboot bootloader` (or `param set ohos.startup.powerctrl reboot,
# bootloader`) without the Vol-Down + Power chord.  The driver's notifier
# matches the cmd string against `mode-*` properties under the watchdog DT
# node and writes the magic into 0x10007024[3:0]; LK reads that on next
# boot.  Halium's modules.load doesn't list these because Android uses a
# different bootloader-handoff path, so we modprobe them explicitly.
modprobe reboot-mode        2>/dev/null || true
modprobe syscon-reboot-mode 2>/dev/null || true

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
[ -n "$SUPER_DEV" ] || rescue "super not found"
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
[ -b /dev/mapper/system_a ]   || rescue "system_a never appeared"
[ -b /dev/mapper/vendor_a ]   || rescue "vendor_a never appeared"
[ -b /dev/mapper/sys_prod_a ] || rescue "sys_prod_a never appeared"

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
mount -t ext4 -o ro /dev/mapper/system_a /root || rescue "mount system_a failed"

# ---------------------------------------------------------------------------
# Stage 3a — Bug 8.18 sandbox-perm fix.
#
# OHOS upstream's `appdata_sandbox_fixer.py` install path lands
# /system/etc/sandbox/appdata-sandbox{,-isolated}.json at mode 0640
# root:root.  In production OHOS images, fs_config rewrites this to
# 0644 before the system image is packed.  Our OHOS build pipeline
# preserves the upstream 0640 — and that breaks every spawn that
# isn't running as root (nwebspawn / appspawn for nweb render).
#
# We can't fix this from `init.x23.cfg` because /system is RO once
# OHOS init runs.  Easiest one-shot fix is here: briefly remount
# system_a rw, chmod, remount ro before the chroot.  Remounting
# read-write is safe at this point because OHOS hasn't started yet.
#
# See native_boot_plan/phase_n8_graphics_native.md (Bug 8.18 port).
# ---------------------------------------------------------------------------
if mount -o remount,rw /root 2>/dev/null; then
    # /android must exist on disk before OHOS init takes over — it is
    # the mount point for halium_system_a (Stage 3b).  Its system/,
    # vendor/, data/ etc. sub-trees come from that partition once
    # mounted, so only the top-level dir is pre-created here.
    mkdir -p /root/android /root/apex /root/storage 2>/dev/null
    chmod 0755 /root/android /root/apex /root/storage 2>/dev/null

    # Bug 8.18 — sandbox configs ship at 0640 from upstream;
    # nwebspawn (uid 3081) needs 0644 to load them.
    chmod 0644 /root/system/etc/sandbox/appdata-sandbox.json          2>/dev/null
    chmod 0644 /root/system/etc/sandbox/appdata-sandbox-isolated.json 2>/dev/null

    mount -o remount,ro /root 2>/dev/null || \
        echo "[init-chainload] remount ro failed (non-fatal)"
else
    echo "[init-chainload] remount rw for /android mkdir + sandbox chmod failed (non-fatal)"
fi
[ -d /root/vendor ] || mkdir -p /root/vendor 2>/dev/null
mount -t ext4 -o ro /dev/mapper/vendor_a /root/vendor || rescue "mount vendor_a failed"
[ -d /root/sys_prod ] || mkdir -p /root/sys_prod 2>/dev/null
mount -t ext4 -o ro /dev/mapper/sys_prod_a /root/sys_prod || rescue "mount sys_prod_a failed"
[ -d /root/chip_prod ] || mkdir -p /root/chip_prod 2>/dev/null
mount -t ext4 -o ro /dev/mapper/chip_prod_a /root/chip_prod 2>/dev/null \
    || echo "[init-chainload] mount chip_prod_a failed (non-fatal)"

# ---------------------------------------------------------------------------
# Stage 3b — mount Halium system + vendor when their partitions are
# present in super.  Optional: a graphics-disabled native build skips
# the Halium blobs (utils/host/pull-halium-blobs.sh not run), so
# build_super_img.sh leaves them out and the /dev/mapper entries never
# appear.  Both mounts are non-fatal — OHOS still boots without them,
# you just don't get the libhybris HAL stack.  /root/android was
# created above (Stage 3a) during the brief remount-rw window.
# ---------------------------------------------------------------------------
# Filesystem probe for the Halium blob mounts: X23 blobs are ext4; the
# ansuz stock vendor/vendor_dlkm/system_dlkm are EROFS (Android 14 MTK
# default).  Try ext4 first (preserves X23 behavior exactly), then erofs.
mount_ro_probe() {
    mount -t ext4  -o ro "$1" "$2" 2>/dev/null && return 0
    mount -t erofs -o ro "$1" "$2" 2>/dev/null && return 0
    return 1
}

if [ -b /dev/mapper/halium_system_a ] && [ -b /dev/mapper/halium_vendor_a ]; then
    # halium_system_a is a dynamic-partition image with a Halium-style
    # FHS at its root (acct/, apex/, bin/, system/, vendor/, etc.).
    # The actual Android `/system` content (lib64/, bin/, …) lives in
    # the inner system/ subdir.  Mounting the partition at /android
    # satisfies both consumers from a single mount:
    #   - libhybris (in OHOS NS) hardcodes /android/system/lib64 etc.;
    #     the partition's inner system/ lands exactly at /android/system.
    #   - androidd (in its Halium NS) pivots into /android so Halium
    #     init finds itself at /system/bin/init (the inner system/
    #     becomes /system after pivot).
    mount_ro_probe /dev/mapper/halium_system_a /root/android \
        || echo "[init-chainload] mount halium_system_a failed (non-fatal)"
    # halium_vendor_a overmounts the partition's own /vendor dir, so the
    # Halium MTK HALs are visible at /android/vendor (OHOS PoV) and at
    # /vendor after androidd's pivot.
    mount_ro_probe /dev/mapper/halium_vendor_a /root/android/vendor \
        || echo "[init-chainload] mount halium_vendor_a failed (non-fatal)"
    # ansuz: the stock vendor keeps most driver modules (Mali GPU stack,
    # WiFi, camera…) in dedicated dlkm dynamic partitions; Android's
    # /vendor/lib/modules symlinks resolve into /vendor_dlkm.  Mount
    # them so androidd's Halium NS (and OHOS-side module loaders) see
    # the full module set.  Mount points exist in the android-rootfs.
    if [ -b /dev/mapper/halium_vendor_dlkm_a ]; then
        mount_ro_probe /dev/mapper/halium_vendor_dlkm_a /root/android/vendor_dlkm \
            || echo "[init-chainload] mount halium_vendor_dlkm_a failed (non-fatal)"
    fi
    if [ -b /dev/mapper/halium_system_dlkm_a ]; then
        mount_ro_probe /dev/mapper/halium_system_dlkm_a /root/android/system_dlkm \
            || echo "[init-chainload] mount halium_system_dlkm_a failed (non-fatal)"
    fi
    # libhybris's bionic loader pulls libc.so etc. from /apex/com.android.runtime/
    # (the Android APEX path).  Expose android/system/apex at /apex so
    # those lookups resolve — without this composer_host SIGSEGVs early in its
    # first Android-namespace dlopen (libc.so not found).
    if [ -d /root/android/system/apex ]; then
        mount --bind /root/android/system/apex /root/apex 2>/dev/null \
            || echo "[init-chainload] bind /android/system/apex→/apex failed"
    fi

    # Expose the Android gralloc/EGL HAL dirs at OHOS-side /vendor/lib64/{hw,egl}.
    # Android's libui GraphicBufferMapper (used by allocator_host's libhybris
    # gralloc bridge) loads the passthrough mapper from a HARDCODED
    # /vendor/lib64/hw/android.hardware.graphics.mapper@4.0-impl*.so and does
    # an access() existence check on that literal path *before* dlopen.
    # libhybris remaps the dlopen to /android/vendor/... but does NOT hook
    # access(), so without the path physically existing the check fails,
    # GraphicBufferMapper finds no Gralloc4/3/2 mapper and LOG_ALWAYS_FATAL
    # aborts allocator_host (SIGABRT in GraphicBufferMapper ctor) — every
    # render_service AllocMem then fails with "allocator_service not found"
    # and nothing reaches the panel.  The LXC build solved this with
    # lxc.mount.entry binds of /vendor/lib64/{hw,egl}; native boot needs the
    # equivalent.  Only these two SUBDIRS are bound (not all of
    # /vendor/lib64) so Android libs don't poison OHOS's own vendor linker
    # namespace — same containment the LXC config relies on.
    mount -o remount,rw /root/vendor 2>/dev/null && {
        mkdir -p /root/vendor/lib64/hw /root/vendor/lib64/egl 2>/dev/null
        mount -o remount,ro /root/vendor 2>/dev/null
    }
    if [ -d /root/vendor/lib64/hw ] && [ -d /root/android/vendor/lib64/hw ]; then
        mount --bind /root/android/vendor/lib64/hw /root/vendor/lib64/hw 2>/dev/null \
            || echo "[init-chainload] bind vendor/lib64/hw failed"
    fi
    if [ -d /root/vendor/lib64/egl ] && [ -d /root/android/vendor/lib64/egl ]; then
        mount --bind /root/android/vendor/lib64/egl /root/vendor/lib64/egl 2>/dev/null \
            || echo "[init-chainload] bind vendor/lib64/egl failed"
    fi
else
    echo "[init-chainload] halium_{system,vendor}_a absent — graphics disabled"
fi

[ -x /root/system/bin/init ] || rescue "/root/system/bin/init missing — wrong partition?"

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
# Stage 4b — pre-mount tmpfs + selinuxfs that OHOS init's FirstStageMain
# would normally set up via MountBasicFs() (base/startup/init/services/
# init/standard/device.c).  We exec'd into second_stage, so FirstStageMain
# never ran — without these, /mnt and /storage stay on the RO root FS,
# `mkdir /mnt/sandbox` (appspawn.cfg boot job) fails, and every app spawn
# bind-mount under /mnt/sandbox/<userId>/<bundleName>/ hits ENOENT.  The
# launcher then crash-loops in appspawn's DoAppSandboxMountOnce and OHOS
# never paints over the LK splash.
#
# selinuxfs at /sys/fs/selinux — kernel cmdline `lsm=selinux` (set in
# build_boot_img_chainload.sh) registers SELinux but does NOT auto-mount
# selinuxfs.  We mount it on chainload-side /sys (which then bind-binds
# into /root/sys below); libselinux's selinuxfs_exists() in OHOS-side
# processes can then find it.  The Halium NS gets its own selinuxfs
# mount via androidd.c::child_main (since androidd makes a fresh sysfs
# mount for the Halium NS, that path isn't shared with the OHOS view).
# ---------------------------------------------------------------------------
mount -t tmpfs -o nosuid,mode=0755 tmpfs /root/mnt       2>/dev/null \
    || echo "[init-chainload] mount tmpfs /mnt failed (non-fatal)"
mount --make-slave /root/mnt 2>/dev/null
mkdir /root/mnt/data 2>/dev/null
mount -t tmpfs -o nosuid,mode=0755 tmpfs /root/mnt/data  2>/dev/null \
    || echo "[init-chainload] mount tmpfs /mnt/data failed (non-fatal)"
mount --make-shared /root/mnt/data 2>/dev/null
mount -t tmpfs -o nosuid,noexec,nodev,mode=0755 tmpfs /root/storage 2>/dev/null \
    || echo "[init-chainload] mount tmpfs /storage failed (non-fatal)"

# Stash the vendor_boot kernel modules into an OHOS-visible tmpfs.  The
# chainload's /lib/modules lives in the initramfs and is unreachable
# after the chroot; OHOS-side code that needs to insmod modules
# post-boot (the Mali GPU stack — see Stage 1 skip) loads them from
# here.  /root/mnt is a tmpfs we just mounted, visible as /mnt in OHOS.
mkdir /root/mnt/kmodules 2>/dev/null
cp -a /lib/modules/. /root/mnt/kmodules/ 2>/dev/null \
    && echo "[init-chainload] stashed kernel modules in /mnt/kmodules" \
    || echo "[init-chainload] stash kmodules failed (non-fatal)"

mount -t selinuxfs none /sys/fs/selinux 2>/dev/null \
    || echo "[init-chainload] mount selinuxfs failed — was lsm=selinux in cmdline?"

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
rescue "exec of OHOS init failed"
