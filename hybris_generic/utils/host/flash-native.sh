#!/bin/bash
#
# Copyright (C) 2026 Oniro / Hybris Generic.
# Licensed under the Apache License, Version 2.0 (the "License").
#
# Flash a native-boot OHOS build to a Volla X23 currently in fastboot (LK).
#
# Up to three artifacts are flashed, all in a single LK-fastboot pass:
#   1. super.img          → super         (system_a + vendor_a +
#                                          sys_prod_a + chip_prod_a +
#                                          halium_system_a + halium_vendor_a).
#   2. boot-chainload.img → boot_a        (LK boot mode, our chain-load /init
#                                          that mounts system_a + exec-chroots
#                                          into OHOS init).
#   3. vendor_boot.img    → vendor_boot_a  (OPTIONAL — when the OHOS-built
#                                          kernel is in the chainload, this
#                                          replaces vendor_boot's kernel
#                                          modules so vermagic matches the
#                                          running kernel.  Without matching
#                                          modules, /dev/access_token_id and
#                                          many other drivers fail to load.)
#
# No fastbootd switch.  `super` is flashed as a whole — it is an ordinary
# *physical* partition in the GPT, and build_super_img.sh produces a
# complete lpmake image (LP metadata + every sub-partition baked in), so
# LK fastboot can write it raw.  fastbootd is only needed to flash an
# *individual logical* partition (`fastboot flash system_a …`), which this
# script never does.  Skipping fastbootd also avoids the Halium-boot
# splash hang that can leave the device unable to reach userspace fastboot.
#
# Pre-requisites on the host:
#   - `fastboot` available (Android platform-tools).
#   - Device in LK fastboot mode (`hold Volume-Down + Power`, or
#     `reboot bootloader` from a device shell).
#
# Usage:  ./flash-native.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OHOS_ROOT="$(cd "$HERE/../../../../../.." && pwd)"
OUT="$OHOS_ROOT/out/hybris_generic"

CHAINLOAD="$OUT/boot-chainload.img"
SUPER="$OUT/super.img"
# OHOS-built vendor_boot (with matching kernel modules).  Optional; only
# flashed when the OHOS-patched kernel is in the chainload.
OHOS_VENDOR_BOOT="${OHOS_VENDOR_BOOT:-$OHOS_ROOT/kernel/linux/volla-vidofnir/out/vendor_boot.img}"

for f in "$CHAINLOAD" "$SUPER"; do
    [[ -f "$f" ]] || { echo "Error: $f missing" >&2; exit 1; }
done

if ! fastboot devices | grep -q .; then
    echo "Error: no fastboot device.  Reboot phone into LK fastboot:" >&2
    echo "  hold Volume-Down + Power" >&2
    exit 1
fi

echo "[1/3] Flashing super"
fastboot flash super "$SUPER"

echo "[2/3] Flashing chain-load boot.img to boot_a"
fastboot flash boot_a "$CHAINLOAD"

if [[ -f "$OHOS_VENDOR_BOOT" ]]; then
    echo "[3/3] Flashing OHOS-built vendor_boot.img (matched kernel modules)"
    fastboot flash vendor_boot_a "$OHOS_VENDOR_BOOT"
else
    echo "[3/3] No OHOS vendor_boot.img — skipping (set OHOS_VENDOR_BOOT to override)"
fi

echo
echo "Done.  Rebooting device into native OHOS."
fastboot reboot
