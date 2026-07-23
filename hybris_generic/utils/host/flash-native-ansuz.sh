#!/bin/bash
#
# Copyright (C) 2026 Oniro / Hybris Generic.
# Licensed under the Apache License, Version 2.0 (the "License").
#
# Flash a native-boot OHOS build to the Volla Phone Plinius (ansuz)
# through the frankpi USB relay.
#
# Flashed (LK fastboot, slot _a):
#   super          <- out/hybris_generic/super.img            (build_super_img.sh)
#   boot_a         <- kernel/linux/volla-ansuz/out/boot.img   (build_kernel.sh — OHOS-patched GKI kernel)
#   init_boot_a    <- out/hybris_generic/init_boot-chainload.img (build_init_boot_chainload.sh)
#   vendor_boot_a  <- out/hybris_generic/vendor_boot-ohos.img    (same script; carries
#                     "ohos.boot.hardware=ansuz lsm=selinux" — NEVER flash this under UT)
#
# Device entry into fastboot:
#   from UT:   adb shell "echo 1234 | sudo -S systemctl reboot --reboot-argument=bootloader"
#   from OHOS: hdc shell "param set ohos.startup.powerctrl reboot,bootloader"
#   by hand:   power off, hold Vol-Down + Power, select `fastboot`
#
# UT restore: flash halium-blobs/ansuz/backups/{boot,init_boot,vendor_boot}_a.img
# back (super's UT rootfs lives in system_a which we overwrite — full UT
# restore additionally needs the channel rootfs push, see plan P9).

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OHOS_ROOT="$(cd "$HERE/../../../../../.." && pwd)"
OUT="$OHOS_ROOT/out/hybris_generic"
PI="${PI:-frankpi}"

SUPER="$OUT/super.img"
BOOT="$OHOS_ROOT/kernel/linux/volla-ansuz/out/boot.img"
INIT_BOOT="$OUT/init_boot-chainload.img"
VENDOR_BOOT="$OUT/vendor_boot-ohos.img"

for f in "$SUPER" "$BOOT" "$INIT_BOOT" "$VENDOR_BOOT"; do
    [[ -f "$f" ]] || { echo "Error: $f missing" >&2; exit 1; }
done

if ! ssh "$PI" 'fastboot devices' 2>/dev/null | grep -q fastboot; then
    echo "Error: no fastboot device behind $PI — reboot the phone to LK fastboot first" >&2
    exit 1
fi

echo "[1/5] Staging artifacts on $PI:/tmp (clearing old ones)"
ssh "$PI" 'rm -f /tmp/super.img /tmp/boot.img /tmp/init_boot-chainload.img /tmp/vendor_boot-ohos.img'
scp -q "$SUPER" "$BOOT" "$INIT_BOOT" "$VENDOR_BOOT" "$PI":/tmp/

echo "[2/5] Flashing super"
ssh "$PI" 'fastboot flash super /tmp/super.img'
echo "[3/5] Flashing boot_a"
ssh "$PI" 'fastboot flash boot_a /tmp/boot.img'
echo "[4/5] Flashing init_boot_a (chainload)"
ssh "$PI" 'fastboot flash init_boot_a /tmp/init_boot-chainload.img'
echo "[5/5] Flashing vendor_boot_a (OHOS cmdline)"
ssh "$PI" 'fastboot flash vendor_boot_a /tmp/vendor_boot-ohos.img'

echo "Rebooting into native OHOS."
ssh "$PI" 'fastboot reboot'
