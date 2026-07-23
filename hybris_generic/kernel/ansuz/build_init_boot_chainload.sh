#!/bin/bash
#
# Copyright (C) 2026 Oniro / Hybris Generic.
# Licensed under the Apache License, Version 2.0 (the "License").
#
# Build the two OHOS boot artifacts for the Plinius (ansuz):
#
#   out/hybris_generic/init_boot-chainload.img
#       The Halium generic ramdisk with /init replaced by our chainload
#       (launcher/init-chainload.sh, @HYBRIS_DEVICE@ -> ansuz) and the
#       original Halium init preserved as /init.halium for the
#       recovery/charger passthrough (plan D5).
#
#   out/hybris_generic/vendor_boot-ohos.img
#       The Halium vendor_boot (DTB + stock+built modules + ABM recovery
#       fragment) repacked with the OHOS cmdline additions:
#         ohos.boot.hardware=ansuz   (OHOS init derives fstab/init cfg
#                                     names from it; ansuz's LK puts
#                                     androidboot.* into bootconfig,
#                                     which OHOS init never reads)
#         lsm=selinux                (SELinux instead of AppArmor — same
#                                     rationale as the X23; vndservicemanager
#                                     aborts without /sys/fs/selinux)
#       The UT-flashable vendor_boot from build_kernel.sh stays pristine
#       (UT needs AppArmor) — never flash this one under UT.
#
# Donors default to the build_kernel.sh outputs (device-proven at the
# P2 UT gate); the on-device dumps in halium-blobs/ansuz/backups/ are
# the fallback.
#
# Unlike the X23 (boot.img = kernel+ramdisk), the splice target here is
# init_boot (ramdisk only, 8 MiB partition). The donor ramdisk is two
# concatenated LZ4-legacy frames (generic ramdisk + port-repo overlay);
# cpio-extract both in order, splice, repack as a single frame.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OHOS_ROOT="$(cd "$HERE/../../../../../.." && pwd)"
KERNEL_TREE="$OHOS_ROOT/kernel/linux/volla-ansuz"
BLOBS="$OHOS_ROOT/device/board/oniro/hybris_generic/halium-blobs/ansuz"
CHAINLOAD_INIT="${CHAINLOAD_INIT:-$OHOS_ROOT/device/board/oniro/hybris_generic/launcher/init-chainload.sh}"
MKBOOT_DIR="$KERNEL_TREE/workdir/downloads/android_system_tools_mkbootimg"
MKBOOTIMG="$MKBOOT_DIR/mkbootimg.py"
UNPACK_BOOTIMG="$MKBOOT_DIR/unpack_bootimg.py"
AVBTOOL="$KERNEL_TREE/workdir/downloads/avb/avbtool"
OUT="$OHOS_ROOT/out/hybris_generic"
INIT_BOOT_PART_SIZE=8388608

DONOR_INIT_BOOT="${DONOR_INIT_BOOT:-$KERNEL_TREE/out/init_boot.img}"
[ -f "$DONOR_INIT_BOOT" ] || DONOR_INIT_BOOT="$BLOBS/backups/init_boot_a.img"
DONOR_VENDOR_BOOT="${DONOR_VENDOR_BOOT:-$KERNEL_TREE/out/vendor_boot.img}"
[ -f "$DONOR_VENDOR_BOOT" ] || DONOR_VENDOR_BOOT="$BLOBS/backups/vendor_boot_a.img"

OHOS_CMDLINE_EXTRA="ohos.boot.hardware=ansuz lsm=selinux"

for f in "$DONOR_INIT_BOOT" "$DONOR_VENDOR_BOOT" "$CHAINLOAD_INIT" "$MKBOOTIMG"; do
    [ -e "$f" ] || { echo "Error: $f missing" >&2; exit 1; }
done
mkdir -p "$OUT"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# ---------------------------------------------------------------------------
# init_boot-chainload.img
# ---------------------------------------------------------------------------
echo "Unpacking donor init_boot ($DONOR_INIT_BOOT)..."
"$UNPACK_BOOTIMG" --boot_img "$DONOR_INIT_BOOT" --out "$WORK/ib" > /dev/null

mkdir -p "$WORK/rd"
python3 - "$WORK/ib/ramdisk" "$WORK/rd" "$WORK" <<'PYEOF'
import os, subprocess, sys
src, stage, work = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(src, 'rb').read()
LZ4_LEG = b'\x02\x21\x4c\x18'
positions = [i for i in range(len(data) - 4) if data[i:i+4] == LZ4_LEG]
positions.append(len(data))
for k in range(len(positions) - 1):
    frame = os.path.join(work, f'f{k}.lz4')
    open(frame, 'wb').write(data[positions[k]:positions[k+1]])
    cpio = os.path.join(work, f'f{k}.cpio')
    subprocess.run(['lz4', '-dc', frame], stdout=open(cpio, 'wb'), check=True)
    with open(cpio, 'rb') as fh:
        subprocess.run(['cpio', '-idmu', '--quiet'], stdin=fh, cwd=stage, check=False)
print(f"extracted {len(positions)-1} lz4-legacy frame(s)")
PYEOF

[ -f "$WORK/rd/init" ] || { echo "Error: donor ramdisk has no /init" >&2; exit 1; }

# Preserve Halium's init for the recovery/charger passthrough, then
# install the chainload with the device baked in.
cp -a "$WORK/rd/init" "$WORK/rd/init.halium"
sed 's/@HYBRIS_DEVICE@/ansuz/' "$CHAINLOAD_INIT" > "$WORK/rd/init"
chmod 0755 "$WORK/rd/init" "$WORK/rd/init.halium"

# Repack: single LZ4 LEGACY frame (kernel decompressor can't do v1.4+
# frames), everything root-owned (suid helpers break otherwise).
( cd "$WORK/rd" && find . -mindepth 1 -printf '%P\n' | sort \
    | cpio -o -H newc --owner=+0:+0 --quiet ) > "$WORK/rd.cpio"
lz4 -9 -l -c < "$WORK/rd.cpio" > "$WORK/rd.lz4"

INIT_BOOT_OUT="$OUT/init_boot-chainload.img"
"$MKBOOTIMG" \
    --ramdisk "$WORK/rd.lz4" \
    --header_version 4 \
    --output "$INIT_BOOT_OUT"

SZ=$(stat -c %s "$INIT_BOOT_OUT")
if (( SZ > INIT_BOOT_PART_SIZE )); then
    echo "ERROR: $INIT_BOOT_OUT is $SZ bytes > init_boot partition ($INIT_BOOT_PART_SIZE)" >&2
    exit 1
fi

# AVB hash footer — the stock/baseline init_boot carries one, and this
# device's LK rejects a footerless init_boot at boot (BROM/preloader
# loop, never reaching our ramdisk) even though it's unlocked.  Add the
# same unsigned hash footer make-bootimage.sh does (no key: vbmeta
# device_state=unlocked accepts an unsigned image, orange state).
"$AVBTOOL" add_hash_footer --image "$INIT_BOOT_OUT" \
    --partition_name init_boot --partition_size "$INIT_BOOT_PART_SIZE"
echo "Built $INIT_BOOT_OUT ($(stat -c %s "$INIT_BOOT_OUT") bytes incl. AVB footer)"

# ---------------------------------------------------------------------------
# vendor_boot-ohos.img — same fragments, OHOS cmdline appended
# ---------------------------------------------------------------------------
echo "Repacking vendor_boot with OHOS cmdline..."
mkdir -p "$WORK/vb"
MKARGS="$("$UNPACK_BOOTIMG" --boot_img "$DONOR_VENDOR_BOOT" --out "$WORK/vb" --format mkbootimg)"

ORIG_CMDLINE="$(python3 - "$DONOR_VENDOR_BOOT" <<'PYEOF'
import struct, sys
d = open(sys.argv[1], 'rb').read(8192)
# vendor boot v4 header: magic(8) ver(4) page(4) kaddr(4) raddr(4)
# vendor_ramdisk_size(4) cmdline(2048 @ offset 28)
print(d[28:28+2048].split(b'\0')[0].decode())
PYEOF
)"
NEW_CMDLINE="$ORIG_CMDLINE $OHOS_CMDLINE_EXTRA"

VENDOR_BOOT_OUT="$OUT/vendor_boot-ohos.img"
# unpack_bootimg's --format mkbootimg emits shell-quoted args incl.
# --vendor_cmdline 'orig'; swap in ours, then append the output path.
python3 - "$MKBOOTIMG" "$NEW_CMDLINE" "$VENDOR_BOOT_OUT" <<PYEOF
import shlex, subprocess, sys
mkbootimg, cmdline, outpath = sys.argv[1], sys.argv[2], sys.argv[3]
args = shlex.split('''$MKARGS''')
if '--vendor_cmdline' in args:
    args[args.index('--vendor_cmdline') + 1] = cmdline
else:
    args += ['--vendor_cmdline', cmdline]
subprocess.run([sys.executable, mkbootimg] + args + ['--vendor_boot', outpath], check=True)
PYEOF

echo "Built $VENDOR_BOOT_OUT ($(stat -c %s "$VENDOR_BOOT_OUT") bytes)"
echo "  cmdline: $NEW_CMDLINE"
echo
echo "Flash (LK fastboot):"
echo "  fastboot flash init_boot_a $INIT_BOOT_OUT"
echo "  fastboot flash vendor_boot_a $VENDOR_BOOT_OUT"
