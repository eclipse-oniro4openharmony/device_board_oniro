#!/bin/bash
#
# Copyright (C) 2026 Oniro / Hybris Generic.
# Licensed under the Apache License, Version 2.0 (the "License").
#
# Build boot-chainload.img — a Halium-compatible boot.img whose ramdisk
# is the live Halium ramdisk with /init replaced by our chain-load script.
#
# Output:  $OHOS_ROOT/out/hybris_generic/boot-chainload.img
#
# Inputs:
#   - $OHOS_ROOT/out/hybris_generic/backups/boot_a.bak — pulled from device
#     with `adb pull /dev/disk/by-partlabel/boot_a` before reflashing.
#   - $OHOS_ROOT/device/board/oniro/hybris_generic/launcher/init-chainload.sh
#
# Why reuse the Halium ramdisk: it already has parse-android-dynparts,
# dmsetup, switch_root, modprobe, plus a kernel-module set that matches
# the kernel.  Building our own ramdisk would mean reimplementing all of
# that.  Reusing it lets us focus on the bring-up logic, not infrastructure.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OHOS_ROOT="$(cd "$HERE/../../../../../.." && pwd)"
KERNEL_TREE="$OHOS_ROOT/kernel/linux/volla-vidofnir"
LIVE_BOOT="$OHOS_ROOT/out/hybris_generic/backups/boot_a.bak"
CHAINLOAD_INIT="${CHAINLOAD_INIT:-$OHOS_ROOT/device/board/oniro/hybris_generic/launcher/init-chainload.sh}"
MKBOOT_DIR="$KERNEL_TREE/build-dir/downloads/android_system_tools_mkbootimg"
MKBOOTIMG="$MKBOOT_DIR/mkbootimg.py"
UNPACK_BOOTIMG="$MKBOOT_DIR/unpack_bootimg.py"
OUTPUT="$OHOS_ROOT/out/hybris_generic/boot-chainload.img"
# OHOS-patched kernel built by build_kernel.sh (Image.gz inside boot.img).
# When present, we substitute it for the live boot_a kernel so the running
# kernel carries our OHOS staging drivers (access_tokenid, hilog, hievent,
# binder token-id, etc.) — required for proper OHOS security model.  The
# matching modules MUST also be flashed (vendor_boot.img) or driver loads
# fail with vermagic mismatch.  Override with OHOS_KERNEL_BOOT_IMG=... or
# unset to fall back to the live Halium kernel.
OHOS_KERNEL_BOOT_IMG="${OHOS_KERNEL_BOOT_IMG:-$KERNEL_TREE/out/boot.img}"

if [[ ! -f "$LIVE_BOOT" ]]; then
    echo "Error: $LIVE_BOOT missing — run 'adb pull /dev/disk/by-partlabel/boot_a' first" >&2
    exit 1
fi
if [[ ! -f "$CHAINLOAD_INIT" ]]; then
    echo "Error: $CHAINLOAD_INIT missing" >&2
    exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "Unpacking live Halium boot.img..."
"$UNPACK_BOOTIMG" --boot_img "$LIVE_BOOT" --out "$WORK/unpack" > /dev/null
echo "  kernel:  $(stat -c %s "$WORK/unpack/kernel") bytes"
echo "  ramdisk: $(stat -c %s "$WORK/unpack/ramdisk") bytes"

# Substitute the OHOS-patched kernel if available.  vendor_boot.img must
# also be reflashed with a matching modules.tar.gz — the modules built
# from this same tree carry vermagic=5.10.209 (no scmversion) and load
# only on this same-vermagic kernel.
if [[ -n "$OHOS_KERNEL_BOOT_IMG" && -f "$OHOS_KERNEL_BOOT_IMG" ]]; then
    echo "Substituting OHOS-patched kernel from $OHOS_KERNEL_BOOT_IMG"
    mkdir -p "$WORK/ohos_unpack"
    "$UNPACK_BOOTIMG" --boot_img "$OHOS_KERNEL_BOOT_IMG" \
                      --out "$WORK/ohos_unpack" > /dev/null
    cp "$WORK/ohos_unpack/kernel" "$WORK/unpack/kernel"
    echo "  new kernel: $(stat -c %s "$WORK/unpack/kernel") bytes"
fi

RAMDISK_TYPE=$(file -b "$WORK/unpack/ramdisk" | head -1)
echo "  ramdisk type: $RAMDISK_TYPE"

mkdir -p "$WORK/ramdisk_unpack"

# Decompress ramdisk.
#
# Halium boot.img ramdisks usually contain TWO concatenated LZ4 legacy
# frames: frame 1 is the main rootfs (init, busybox, libs); frame 2 holds
# /scripts/halium and the recovery ramdisk.  Both `lz4 -dc` and `gunzip -dc`
# decode multi-frame streams, but `cpio -idm` reads only ONE archive (stops
# at the first TRAILER!!!).  So we split the LZ4 stream at frame magics
# (02 21 4c 18) and run cpio extraction per frame into the same staging dir.
case "$RAMDISK_TYPE" in
    *LZ4*)
        python3 - "$WORK/unpack/ramdisk" "$WORK/ramdisk_unpack" "$WORK" <<'PYEOF'
import os, subprocess, sys
src, stage, work = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(src, 'rb').read()
LZ4_LEG = b'\x02\x21\x4c\x18'
positions = [i for i in range(len(data) - 4) if data[i:i+4] == LZ4_LEG]
positions.append(len(data))
for k in range(len(positions) - 1):
    start, end = positions[k], positions[k+1]
    frame = os.path.join(work, f'frame_{k}.lz4')
    open(frame, 'wb').write(data[start:end])
    cpio_path = os.path.join(work, f'frame_{k}.cpio')
    subprocess.run(['lz4', '-dc', frame], stdout=open(cpio_path, 'wb'), check=True)
    with open(cpio_path, 'rb') as fh:
        subprocess.run(['cpio', '-idmu', '--quiet'], stdin=fh, cwd=stage, check=False)
PYEOF
        ;;
    *gzip*)
        gunzip -c "$WORK/unpack/ramdisk" > "$WORK/ramdisk.cpio"
        ( cd "$WORK/ramdisk_unpack" && cpio -idmu --quiet < "$WORK/ramdisk.cpio" )
        ;;
    *XZ*|*xz*)
        xz -dc "$WORK/unpack/ramdisk" > "$WORK/ramdisk.cpio"
        ( cd "$WORK/ramdisk_unpack" && cpio -idmu --quiet < "$WORK/ramdisk.cpio" )
        ;;
    *) echo "Unknown ramdisk compression: $RAMDISK_TYPE" >&2; exit 1 ;;
esac

# Replace /init with the chain-load script.
install -m 0755 "$CHAINLOAD_INIT" "$WORK/ramdisk_unpack/init"

# Repack the ramdisk in the same format we extracted.
#
# CRITICAL: force every cpio entry to root:root via --owner=+0:+0.  The kernel
# initramfs extractor preserves UID/GID from the archive; if /init isn't
# root-owned, suid helpers like /bin/mount lose privilege at runtime and
# init silently fails before reaching userspace.  Building from a non-root
# user (us) would otherwise leave files owned by the build user's UID.
SPLICED="$WORK/ramdisk-spliced.img"
( cd "$WORK/ramdisk_unpack" && find . -mindepth 1 -printf '%P\n' \
    | sort | cpio -o -H newc --owner=+0:+0 --quiet ) > "$WORK/ramdisk.repack.cpio"

case "$RAMDISK_TYPE" in
    *gzip*) gzip -9 < "$WORK/ramdisk.repack.cpio" > "$SPLICED" ;;
    # CRITICAL: the kernel decompressor only handles LZ4 LEGACY frames
    # (v0.1–0.9).  Modern lz4 writes v1.4+ frames which the kernel can't
    # decode — the boot silently fails (Volla LK splash forever).
    # -l forces legacy format.
    *LZ4*)  lz4 -9 -l -c < "$WORK/ramdisk.repack.cpio" > "$SPLICED" ;;
    *XZ*|*xz*) xz -c -9 < "$WORK/ramdisk.repack.cpio" > "$SPLICED" ;;
esac
echo "  spliced ramdisk: $(stat -c %s "$SPLICED") bytes"

# Header version 4 is what Volla X23 / mimir LK accepts.
# Kernel byte-identical to live boot_a so vbmeta_a's chain-of-trust stays
# valid; only the ramdisk content differs.
#
# Cmdline notes:
#   - init=/init      — force our chainload script (kernel default search
#                       would pick /sbin/init from the Halium ramdisk).
#   - panic=0         — no auto-reboot on init crash so we can debug.
#   - lsm=...selinux...
#                     — LK's base cmdline pins `security=apparmor`, which
#                       leaves SELinux uninitialized and `/sys/fs/selinux/`
#                       absent.  Halium's `vndservicemanager` then SIGABRTs
#                       on `Check failed: selinux_status_open(true) >= 0`,
#                       which cascades into a class-hal restart loop —
#                       composer@2.3-service ends up cycling every 4–6 s
#                       (see phase_n8_graphics_native.md §N8.9.2).  `lsm=`
#                       (Linux 5.10+) overrides `security=` and lets us
#                       enable SELinux instead of (not alongside —
#                       exclusive LSMs conflict) AppArmor.  Native boot
#                       has no Ubuntu Touch host so dropping AppArmor is
#                       free; OHOS itself was already built with
#                       `build_selinux=false` so it doesn't care which
#                       major LSM is active.
# CRITICAL — Volla X23 LK cmdline-truncation quirk: LK strips the FIRST
# 20 chars of the boot.img cmdline AND keeps only up to the first space
# after that — i.e. exactly ONE space-free token survives (observed
# empirically — see phase_n8_graphics_native.md §N8.9.2-fix).  We pad
# the front with a 20-char no-op token (`PAD=xxxxxxxxxxxxxxx`, a dummy
# kernel parameter the kernel ignores) so `lsm=selinux` — the one token
# we actually need — arrives intact.  The trailing
# `androidboot.selinux=permissive` is therefore DROPPED by LK and never
# reaches the kernel; it is kept in the string only as documentation of
# intent.  It is not needed: with `lsm=selinux` and no SELinux policy
# loaded, `/sys/fs/selinux/enforce` reads 0 (permissive) and every
# access check passes — Halium boots with `init second_stage` (not
# `selinux_setup`) so it never loads a policy anyway.
mkdir -p "$(dirname "$OUTPUT")"
"$MKBOOTIMG" \
    --kernel  "$WORK/unpack/kernel" \
    --ramdisk "$SPLICED" \
    --cmdline "PAD=xxxxxxxxxxxxxxx lsm=selinux androidboot.selinux=permissive" \
    --header_version 4 \
    --os_version 12.0.0 \
    --os_patch_level 2025-09 \
    --output  "$OUTPUT"

echo
echo "Built $OUTPUT"
ls -la "$OUTPUT"
echo
echo "Flash with:"
echo "  fastboot flash boot_a $OUTPUT"
