#!/bin/bash
#
# Copyright (C) 2026 Oniro / Hybris Generic.
# Licensed under the Apache License, Version 2.0 (the "License").
#
# Flash a native-boot OHOS build to a Volla X23 currently in fastboot (LK).
#
# Up to three artifacts are flashed:
#   1. boot-chainload.img → boot_a       (LK boot mode, our chain-load /init
#                                         that mounts system_a + exec-chroots
#                                         into OHOS init; kernel is the
#                                         OHOS-patched kernel when present, or
#                                         the live Halium kernel otherwise).
#   2. super.img          → super        (fastbootd dynamic-partition flash;
#                                         contains system_a + vendor_a +
#                                         halium_system_a + halium_vendor_a).
#   3. vendor_boot.img    → vendor_boot_a  (OPTIONAL — when the OHOS-built
#                                         kernel is in the chainload, this
#                                         replaces vendor_boot's kernel
#                                         modules so vermagic matches the
#                                         running kernel.  Without matching
#                                         modules, /dev/access_token_id and
#                                         many other drivers fail to load.)
#
# super flashing requires fastbootd (Android userspace fastboot), which
# we enter via `fastboot reboot fastboot` AFTER flashing a Halium boot.img
# (boot_a.bak — the pristine boot.img pulled before any modifications).
# Our chain-load boot.img has no fastbootd inside; if it's flashed at this
# point, `fastboot reboot fastboot` falls back to LK.
#
# Pre-requisites on the host:
#   - `fastboot` available (Android platform-tools).
#   - Device in LK fastboot mode.
#   - $OHOS_ROOT/out/hybris_generic/backups/boot_a.bak (Halium boot.img,
#     pulled before reflashing — `adb pull /dev/disk/by-partlabel/boot_a`).
#
# Usage:  ./flash-native.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OHOS_ROOT="$(cd "$HERE/../../../../../.." && pwd)"
OUT="$OHOS_ROOT/out/hybris_generic"

CHAINLOAD="$OUT/boot-chainload.img"
SUPER="$OUT/super.img"
HALIUM_BOOT="$OUT/backups/boot_a.bak"
# OHOS-built vendor_boot (with matching kernel modules).  Optional; only
# flashed when the OHOS-patched kernel is in the chainload.
OHOS_VENDOR_BOOT="${OHOS_VENDOR_BOOT:-$OHOS_ROOT/kernel/linux/volla-vidofnir/out/vendor_boot.img}"

for f in "$CHAINLOAD" "$SUPER" "$HALIUM_BOOT"; do
    [[ -f "$f" ]] || { echo "Error: $f missing" >&2; exit 1; }
done

if ! fastboot devices | grep -q .; then
    echo "Error: no fastboot device.  Reboot phone into fastboot:" >&2
    echo "  hold Volume-Down + Power" >&2
    exit 1
fi

# Flash Halium boot.img first so we can enter fastbootd to flash super.
echo "[1/3] Flashing Halium boot.img to boot_a (transient — needed for fastbootd)"
fastboot flash boot_a "$HALIUM_BOOT"

echo "[2/3] Rebooting into fastbootd and flashing super"
fastboot reboot fastboot
sleep 8
fastboot wait-for-device
fastboot flash super "$SUPER"

echo "[3/4] Back to LK fastboot, flashing chain-load boot.img"
fastboot reboot bootloader
sleep 5
fastboot wait-for-device
fastboot flash boot_a "$CHAINLOAD"

if [[ -f "$OHOS_VENDOR_BOOT" ]]; then
    echo "[4/4] Flashing OHOS-built vendor_boot.img (matched kernel modules)"
    fastboot flash vendor_boot_a "$OHOS_VENDOR_BOOT"
fi

echo
echo "Done.  Rebooting device into native OHOS."
fastboot reboot
