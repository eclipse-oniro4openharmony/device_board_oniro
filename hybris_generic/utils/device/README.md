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

## init-strace-wrap.c

Static no-libc aarch64 wrapper for tracing Halium init inside the
androidd NS.  Bind it over `/system/bin/init` via the halium-debug
overlay and it execs Halium's own `/system/bin/strace` on a copy of the
real init, forwarding envp; falls back to exec'ing init directly if
strace won't load.  Same build recipe as reboot-bl.  Usage (P5 drill,
all on-device paths):

```
mkdir -p /module_update/halium-debug
cp /android/system/bin/init /module_update/halium-debug/init.real
hdc file send init-strace-wrap /module_update/halium-debug/
chmod 755 /module_update/halium-debug/init-strace-wrap
printf '/data/halium-debug/init-strace-wrap /system/bin/init\n' \
    > /module_update/halium-debug/overlay.txt
begetctl stop_service androidd; begetctl start_service androidd
# trace lands in /module_update/halium-debug/strace.log
```

Caveat: under strace, init is not PID 1 of the NS — `CgroupSetup()`
("can be done only by init process") and everything downstream of it
fails, so service starts break.  Use it to debug init's own early
crashes only; remove the overlay to test service bring-up.
