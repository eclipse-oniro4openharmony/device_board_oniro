# Phase N7 — HDC over USB

**Status:** ✅ **DONE (2026-05-11)** — `hdc shell` over USB works
end-to-end.  Reproduction recipe: [`README.md`](README.md).
aarch64 host setup (Pi as USB rig): [`HDC_AARCH64_HOST.md`](HDC_AARCH64_HOST.md).

---

## Final design

Three things had to be right for USB hdc to enumerate and respond:

1. **MUSB peripheral mode** — `cmode=3` (DEVICE) in
   `/sys/devices/platform/soc/mt_usb/musb-hdrc/cmode`.
2. **Developer mode true** before `start hdcd` — otherwise `hdcd`'s
   main() rejects with `developerMode != "true"` and exits in a
   critical-restart loop.
3. **USB gadget configfs structure** + `ffs.hdc` function + UDC write to
   bind, all wired in `vendor/oniro/hybris_generic/etc/init/init.x23.usb.cfg`.

## Source artifacts (final)

### `vendor/oniro/hybris_generic/etc/init/init.x23.usb.cfg`

Imported by `/system/etc/init.usb.cfg` (line 3) via
`/vendor/etc/init.${ohos.boot.hardware}.usb.cfg` →
`/vendor/etc/init.x23.usb.cfg`.

```json
{
    "jobs" : [{
        "name" : "init",
        "cmds" : [
            "write /sys/devices/platform/soc/mt_usb/musb-hdrc/cmode 3",
            "mkdir /dev/usb-ffs ...",
            "mkdir /config/usb_gadget/g1 ...",
            "write /config/usb_gadget/g1/idVendor 0x12D1",
            "write /config/usb_gadget/g1/idProduct 0x5000",
            ...
            "mount functionfs hdc /dev/usb-ffs/hdc uid=2000,gid=2000",
            "setparam sys.usb.configfs 1",
            "setparam sys.usb.controller musb-hdrc"
        ]
    }, {
        "name" : "boot",
        "cmds" : [
            "sleep 5",
            "symlink /config/usb_gadget/g1/functions/ffs.hdc /config/usb_gadget/g1/configs/b.1/f1",
            "write /config/usb_gadget/g1/UDC musb-hdrc"
        ]
    }]
}
```

The `init` trigger sets up the gadget structure; the 5-second sleep in
the `boot` trigger gives hdcd time to write its FunctionFS descriptors
and set `sys.usb.ffs.ready=1` before we symlink the function into the
config and bind the UDC.

### `device/board/oniro/hybris_generic/cfg/z_hdcd_autostart.cfg`

```json
{
    "jobs" : [{
        "name" : "init",
        "cmds" : [
            "setparam const.security.developermode.state true",
            "setparam persist.hdc.mode.usb enable",
            "setparam persist.hdc.mode.tcp enable",
            "start hdcd"
        ]
    }]
}
```

Filename prefix `z_` makes this sort last among the `/system/etc/init/*.cfg`
load order, so by the time `start hdcd` fires, the gadget-setup commands
in `init.x23.usb.cfg` have already run.

### `vendor/oniro/hybris_generic/etc/param/hybris_native.para`

```
persist.hdc.mode.usb = "enable"
persist.hdc.mode.tcp = "enable"
const.security.developermode.state = "true"
```

Same values as the autostart `setparam`s above.  Build deploys this to
`/sys_prod/etc/param/hybris_native.para` for builds that mount `sys_prod`
at boot.  Our chainload doesn't mount `sys_prod`, so the autostart cfg is
what actually takes effect; this file is kept for completeness and for
any future builds that take the standard sys_prod path.

---

## Hard-won lessons

### MUSB `cmode` enum

From the kernel `drivers/misc/mediatek/usb20/musb.h`:

```c
enum {
    MUSB_DR_OPERATION_NONE = 0,
    MUSB_DR_OPERATION_NORMAL,    // OTG auto-switch
    MUSB_DR_OPERATION_HOST,      // 2
    MUSB_DR_OPERATION_DEVICE,    // 3 ← what we want
};
```

LK fastboot leaves the controller in `NORMAL` (auto-switch).  The
chainload `init.x23.usb.cfg` originally wrote `cmode=2`, which forces
HOST.  The Pi (host PC) saw nothing on the bus because both sides were
hosts.  Fixing to `cmode=3` made the gadget visible immediately as
`12d1:5000 "Phone X23"`.

Cost: many weeks before realising what the enum values were.

### `hdcd` exits if `developerMode != "true"`

`developtools/hdc/src/daemon/main.cpp` checks
`const.security.developermode.state` very early; any value other than
literal `"true"` causes `return -1`.  init's `critical=10,10` restart
policy then restarts hdcd up to 10 times in 10 s, so the symptom is a
PID that flickers in `ps` but never gets to write FunctionFS descriptors.

`hybris_native.para` does set the param to `"true"` but lands in
`/sys_prod/etc/param/` — which our chainload doesn't mount.  Fix:
`setparam` in `z_hdcd_autostart.cfg` before `start hdcd`.  `setparam`
works for `const.*` params on first set; subsequent writes are rejected.

### USB descriptor signature must match HDC client expectations

`hdc list targets` only picks up devices whose USB interface descriptor
has:
- `bInterfaceClass = 0xFF`
- `bInterfaceSubClass = 0x50`
- `bInterfaceProtocol = 0x01`
- `bNumEndpoints = 2`

The functionfs descriptors hdcd writes already satisfy this — the
verification is in `developtools/hdc/src/host/host_usb.cpp::IsDebuggableDev`.

### `lsusb -v` is your friend

`lsusb -v -d 12d1:5000` shows the device's actual descriptors as the
kernel sees them.  If the device shows up at all but `hdc list targets`
is empty, check the interface descriptor fields against the four values
above.

### USB device file permissions on the host

On Debian, `/dev/bus/usb/001/<n>` is `root:plugdev 0666`.  Either run
hdc as root or via sudo (the consolidated recipe uses sudo).

### Stale `.HDCServer.pid` and UDS dir

The hdc server bind()s its UDS socket inside `/data/hdc/hdc_debug/` on
the host (the path is hardcoded in OHOS as
`/data/hdc/hdc_debug/hdc_server`).  The dir must exist with write perms
(`mkdir -p /data/hdc/hdc_debug && chmod 777 /data/hdc/hdc_debug`).

If the server gets killed without cleaning `~/.HDCServer.pid` or
`/root/.HDCServer.pid`, the next client invocation reads the stale pid,
sees `[ -d /proc/$pid ]` is false (process is gone), but the client
doesn't auto-clean.  The wrapper script in
[`HDC_AARCH64_HOST.md`](HDC_AARCH64_HOST.md) does the validation.

### `hdc shell` hangs after a recent device reboot

The hdc daemon's USB descriptor handshake races with the device's USB
re-enumeration.  Wait 5–10 s; on persistent hangs, `fastboot reboot`
and let it boot fresh.

---

## What's not in the consolidated tree

A lot of marker-based diagnostics from the bring-up are stripped from
the consolidated source.  Notable ones, preserved here for the next
debugger:

- `daemon_usb.cpp::HdcDiagMarker` wrote single-line records to
  `/dev/hdcd_diag` (created world-writable by the chainload) tracing
  every step of `HdcDaemonUSB::Initial()` and `ConnectEPPoint()`.
  The same approach is the right starting point for any USB hdc
  regression — hdcd runs as `shell` after `DropRootPrivileges()` so the
  marker file must be world-writable, or pre-created with `chmod 0666`.

- `main.cpp::HdcMainDiagMarker` traced the pre-`InitMod` decision tree
  (developer-mode probe, fork/foreground path, TCP/USB enable flags).
  Useful when hdcd is silently exiting before reaching the USB code.

- `init-chainload.sh` had a 60-second USB poll loop that snapshotted
  `cmode`, `/sys/class/udc/musb-hdrc/state`, `/config/usb_gadget/g1/UDC`,
  hdcd PID, and the `hdcd_diag` content into vendor_boot_a slot records.
  Read back via `fastboot reboot fastboot` → `fastboot fetch
  vendor_boot_a` → `strings | grep USBPOLL`.  Reintroduce if any USB
  hdc regression hits.
