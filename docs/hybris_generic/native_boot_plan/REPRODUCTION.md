# Native-Boot OHOS on Volla X23 — Reproduction Guide

Walks a fresh checkout through to a Volla X23 running OpenHarmony natively
(no Ubuntu Touch host, no LXC), with `hdc shell` working over USB.

This is the consolidated end-state of the native-boot effort (Phases N0–N11).
Status of the journey is in [`STATUS.md`](STATUS.md); per-phase rationale is
in `phase_n*.md`.

---

## What this gets you

- `boot_a` and `super` partitions on the device replaced with our artifacts.
- LK boots a Halium kernel (unchanged) into our chain-load `/init`, which
  mounts the OHOS system_a from `super` and `exec chroot`s into OHOS init.
- OHOS init runs PID 1 through `SystemRun()`; ~40+ services come up
  (samgr, foundation, hdcd, hilogd, hdf_devmgr, appspawn, render_service,
  audio_server, …).
- A USB-host (Linux PC, Pi) plugged into the X23 sees it as
  `12d1:5000 Huawei "Phone X23"` with an "HDC Interface" configuration and
  can run `hdc shell`.

## What this does NOT get you (yet)

- Display: composer_host / render_service can't render — Phase N8 is open.
  Phase 6's display VDIs were tuned for LXC and assume `/android/vendor/...`
  HALs, which don't exist in native mode.
- Audio/Wi-Fi/sensors past basic enumeration: HAL bridges are still
  libhybris-shaped (Phases 10+ in the LXC plan).
- The on-device system is functional from `hdc` but not user-visible yet.
  Use this build to iterate on Phases N8/N9 (display + peripherals in
  native mode).

## Hardware

- Volla X23 (Helio G99 / MT6789, aarch64). Unlocked bootloader.
- USB-A cable to a Linux host running the OHOS source tree.

## One-time host setup

1. Clone the OHOS source tree per the standard OHOS `repo` instructions —
   ensure `device/board/oniro`, `vendor/oniro`, and the Volla kernel trees
   (`kernel/linux/volla-vidofnir`, `kernel/linux/volla-mimir`) are present.
2. Have a working OHOS build container (the project's CLAUDE.md captures
   the docker invocation: `./build.sh --product-name hybris_generic`).
3. Pull a pristine Halium boot.img and stash it as a backup — we use it
   to reach `fastbootd` for `super` flashing, since our chain-load boot.img
   does not contain fastbootd:
   ```
   adb pull /dev/disk/by-partlabel/boot_a out/hybris_generic/backups/boot_a.bak
   ```

## Build

```
./build.sh --product-name hybris_generic --ccache
bash device/board/oniro/hybris_generic/kernel/x23/build_super_img.sh
bash device/board/oniro/hybris_generic/kernel/x23/build_boot_img_chainload.sh
```

This produces:
- `out/hybris_generic/super.img` (system_a + vendor_a in LP-formatted
  super, built with `lpmake`)
- `out/hybris_generic/boot-chainload.img` (Halium kernel + Halium ramdisk
  with `/init` replaced by `device/board/oniro/hybris_generic/launcher/init-chainload.sh`)

## Flash

Reboot the phone into **LK fastboot** (hold Volume-Down + Power), then:

```
bash device/board/oniro/hybris_generic/utils/host/flash-native.sh
```

The script:
1. Flashes `boot_a.bak` (Halium boot.img) to `boot_a` so we can reach
   fastbootd.  Our chain-load boot.img has no fastbootd inside.
2. `fastboot reboot fastboot` → enters fastbootd.
3. `fastboot flash super out/hybris_generic/super.img`.
4. `fastboot reboot bootloader` → back to LK.
5. `fastboot flash boot_a out/hybris_generic/boot-chainload.img`.
6. `fastboot reboot` → device boots into native OHOS.

## Verify on device

Wait ~60–70 s after reboot for OHOS init to settle.  Then on the host:

```
sudo hdc list targets
# 0a20230726

sudo hdc shell "uname -a"
# Linux localhost 5.10.209-ga4ec076d798b ... aarch64 Toybox

sudo hdc shell "ps -A | head"
# 40+ OHOS services
```

If `hdc list targets` is empty:
- `lsusb | grep 12d1` should show `12d1:5000 Huawei "Phone X23"`.  If not,
  the gadget didn't bind.  Most common cause is the system_a copy of
  `init.x23.usb.cfg` having an old `cmode=2` value instead of `cmode=3` —
  rebuild + reflash super.
- If `lsusb` shows the device but `hdc list targets` is empty: check the
  `/data/hdc/hdc_debug/` directory exists with write perms on the **host**
  (where hdc client/server runs).  Server bind()s its UDS socket there.

## Host-side hdc client (when host is aarch64 Linux)

Local x86_64 hosts work with the stock SDK hdc.  For an aarch64 Linux host
(e.g. a Raspberry Pi acting as the test rig), see
[`HDC_AARCH64_HOST.md`](HDC_AARCH64_HOST.md).

---

## What's actually in each artifact

### `boot-chainload.img`

- Halium kernel (byte-identical to live boot_a).  Keeping the kernel
  unchanged preserves any signing the bootloader checks.
- Halium ramdisk with `/init` replaced.  Halium's ramdisk ships
  `parse-android-dynparts`, `dmsetup`, `mount`, `modprobe`, plus the
  vendor kernel-modules tree we need for the block-device subsystem.
  Reusing it skips reimplementing all of that.

The chain-load script
(`device/board/oniro/hybris_generic/launcher/init-chainload.sh`) does:
1. Mount `sysfs`, `proc`, `devtmpfs`.
2. `modprobe -a` every `.ko` in the vendor modules tree except the
   watchdog/AEE modules that mask diagnostics.
3. Find `super` via `/sys/class/block/*/uevent`, run
   `parse-android-dynparts $super | sh` to populate `/dev/mapper/system_a`
   and `/dev/mapper/vendor_a`.
4. Mount `system_a` at `/root`, `vendor_a` at `/root/vendor`.
5. Pre-populate `/root/dev/disk/by-partlabel/*` symlinks (saves OHOS init
   from waiting on a ueventd it can't usefully consume here).
6. `mount -o bind /proc /sys /dev` → `/root/{proc,sys,dev}`.
   **Bind, not move** — `mount -o move` silently leaves these
   inaccessible from the chrooted child on this kernel (empirically
   verified).
7. `exec env OHOS_NATIVE_BOOT=1 chroot /root /system/bin/init --second-stage`.

### `super.img`

Built by `build_super_img.sh` using `lpmake` (from the Halium kernel build
tools).  Contains `system_a` + `vendor_a` in standard LP format.
`parse-android-dynparts` reads the LP metadata at runtime and creates the
dmsetup tables.  No `system_b`/`vendor_b` — the chainload only ever uses
`_a`.

### OHOS sources touched

| File | Change |
|------|--------|
| `base/startup/init/services/init/standard/init_cmds.c` | `DoMkSandbox` skips when `OHOS_NATIVE_BOOT=1` (the chainload sets this).  Without the skip, `pivot_root` inside the unshared mount namespace leaves init's `fs_struct` dangling and every subsequent fork+exec from init silently fails. |
| `device/board/oniro/hybris_generic/cfg/z_hdcd_autostart.cfg` | `setparam const.security.developermode.state true` + `persist.hdc.mode.{usb,tcp}=enable` before `start hdcd`.  Default-deploy of `hybris_native.para` to `/sys_prod/etc/param/` doesn't fire in native (we don't mount sys_prod), so we set the params here. |
| `vendor/oniro/hybris_generic/etc/init/init.x23.usb.cfg` | `cmode=3` (MTK musb DEVICE/peripheral mode).  The MTK musb driver's `cmode` enum is `0=NONE 1=NORMAL/auto 2=HOST 3=DEVICE` (see `drivers/misc/mediatek/usb20/musb.h`).  Setting `cmode=2` forces HOST and the Pi sees nothing on the bus. |
| `vendor/oniro/hybris_generic/etc/param/hybris_native.para` | Same param values as the autostart cfg — kept for builds that mount `sys_prod`. |

Everything else is wiring (BUILD.gn `ohos_prebuilt_etc` entries, etc.).

---

## Troubleshooting tips

**Device boots Halium splash and stays there.**
The chain-load `/init` panicked or failed before exec-chroot.  Reflash
`boot_a.bak` → `boot_a` and the device returns to Halium normally.  To
diagnose, see the marker channel in older revs of `init-chainload.sh` —
it writes 128-byte slot records into vendor_boot_a which can be fetched
back via fastbootd; not enabled in the consolidated script to keep it
clean.

**`lsusb` shows `12d1:5000` but `hdc list targets` is empty.**
- Server UDS dir missing on the **host**: `mkdir -p /data/hdc/hdc_debug
  && chmod 777 /data/hdc/hdc_debug`.
- Stale server PID file: `rm -f /root/.HDCServer.pid ~/.HDCServer.pid`.

**`hdc shell` hangs after a recent device reboot.**
The hdc daemon's USB descriptor handshake races with the device's USB
re-enumeration.  Wait 5–10 s; on persistent hangs, `fastboot reboot` and
let it boot fresh.

**Compose/render/display doesn't work.**
Expected — Phase N8 is open.  Native-mode display HALs are not the same
as the libhybris bridge used in the LXC build.  This was outside the
scope of the chainload + USB-hdc bring-up.
