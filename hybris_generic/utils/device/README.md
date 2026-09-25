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
Keep a compiled copy on the USB relay box (push it into a debug ramdisk via
the rescue RNDIS: `python3 -m http.server` on the relay + busybox `wget` on
the device).  Note: the BCB `bootonce-bootloader` command in
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

## screencast/

Live screen mirroring for demos: `screencast_agent.cpp` is a uitest
extension that serves the display as MJPEG over HTTP (`GET /` viewer page,
`GET /stream[?scale=0.1..0.9]`).  It drives uitest's own screen-copy engine
(`test/testfwk/arkxtest/uitest/addon/screen_copy.cpp`: screenshot polling,
change detection, libjpeg), which is reachable only from a library that
`uitest start-daemon singleness --extension-name <so>` dlopen()s from
`/data/local/tmp`.  libc-only, built with the plain SDK NDK.

Everything (build, push, start, port forward) is `utils/host/screencast.sh`
(`--remote <relay>` when the hdc server is elsewhere, `--wifi` to serve on
the phone's WiFi address instead of loopback, `--stop`).  Open the printed
URL in a browser; double-click for fullscreen.  `screencast.sh --pack DIR` writes
`DIR/oniro-screencast.tar.gz` (script + prebuilt agent + README) for a
presentation laptop with only bash and hdc: no tree, no NDK.  `--no-push`
reuses an agent already on the device.  Measured on the Plinius:
28–31 fps during continuous animation at scale 0.3–0.7 (the device
screenshot is the ceiling, not the link — 23 Mbit/s at 0.7 through the Pi
relay was fine), first frame ~0.3 s after connecting.  Frames are only sent
when the screen changes.  The agent logs to `/data/local/tmp/screencast.log`
(uitest's printLog goes to the LOG_APP hilog type, which is not shown).
