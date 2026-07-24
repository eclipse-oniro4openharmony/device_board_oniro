# Device-side utilities

## reboot-bl.c

Static no-libc aarch64 binary issuing `reboot(RESTART2, "bootloader")` —
enters LK fastboot from environments whose `reboot` tool can't pass the
RESTART2 argument (busybox debug/rescue ramdisks).  Requires the
`syscon-reboot-mode` driver (in the Halium vendor_boot module set on
ansuz).  From a running OHOS use
`param set ohos.startup.powerctrl reboot,bootloader` instead — both paths
verified on the Plinius (7–10 s to fastboot).

Build:
```
prebuilts/clang/ohos/linux-x86_64/llvm/bin/clang \
    --target=aarch64-linux-gnu -nostdlib -static -O2 -fuse-ld=lld \
    -o reboot-bl reboot-bl.c
```
A compiled copy lives on the Pi at `~frankpi/reboot-bl` (push into a debug
ramdisk via the rescue RNDIS: `python3 -m http.server` on the Pi + busybox
`wget` on the device).  Note: the BCB `bootonce-bootloader` command in
`misc` is ignored by this LK — this binary is the working replacement.
