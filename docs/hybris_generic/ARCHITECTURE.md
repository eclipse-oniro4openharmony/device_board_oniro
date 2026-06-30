# Native Boot — Architecture Overview

How OpenHarmony boots **natively** on the Volla X23 (and Volla Tablet)
— no Ubuntu Touch host, no LXC container. OHOS runs as PID 1 on bare
metal; Android's HAL services run beside it in a child namespace.

This document explains *how the running system is structured*. It does
not retrace the bring-up journey — for per-phase root causes, debug
recipes, and hard-won lessons see the `phase_nX_*.md` docs.

---

## 1. The problem

The Volla X23 is a MediaTek (MT6789) Android phone. Its display, GPU,
WiFi, audio, sensors, etc. are only driven by **Android HAL binaries**
compiled against bionic libc and Android's HIDL/binder ABI. OHOS can't
talk to that hardware directly.

The earlier `hybris_generic` work solved this by running OHOS as an
**LXC container** on top of an Ubuntu Touch / Halium host: the host
owned the kernel and the Android HALs, OHOS was a guest. Native boot
removes that host entirely. OHOS becomes PID 1. But the Android HALs
still need to run somewhere — so we keep a *minimal* slice of Halium
(its Android `system` + `vendor` partitions and HAL services) and run
it as a **guest namespace of OHOS**, the inverse of the LXC topology.

Two pieces make this work:

- **The chainload** — gets OHOS init running as PID 1 with the right
  partitions mounted, reusing Halium's boot.img so the bootloader's
  chain-of-trust stays intact.
- **`androidd`** — a small launcher that starts Halium's HAL services
  inside a child PID/mount namespace, so libhybris-based OHOS services
  can reach them over a shared binder.

---

## 2. Boot flow at a glance

```
  MTK LK bootloader
        │  (loads boot_a — a Halium boot.img we modified)
        ▼
  Linux kernel  +  Halium ramdisk
        │  (ramdisk /init replaced by init-chainload.sh)
        ▼
  init-chainload.sh                         ── runs as PID 1, in initramfs
    • mount /proc /sys /dev
    • modprobe vendor kernel modules
    • parse-android-dynparts → /dev/mapper/{system_a,vendor_a,…}
    • mount OHOS system_a at /root, vendor_a at /root/vendor
    • mount Halium system at /root/android, vendor at /root/android/vendor
    • bind /proc /sys /dev into /root
    • exec env OHOS_NATIVE_BOOT=1 chroot /root /system/bin/init …
        │
        ▼
  OHOS init  --second-stage                 ── still PID 1 (exec preserves it)
    • parses /system/etc/init/*.cfg, /vendor/etc/init.x23.cfg
    • mounts /data, sets up binderfs, loads Mali GPU modules
    • starts samgr, hdf services, render_service, …
    • starts androidd
        │
        ├──────────────► androidd  ── clone() child PID/mount namespace
        │                  • sets up an Android-shaped /dev, /apex, /mnt
        │                  • pivot_root into the Halium root
        │                  • exec Halium /system/bin/init
        │                       └─► hwservicemanager, composer@2.x,
        │                           gralloc@4.0, … (Android HAL services)
        ▼
  OHOS userspace fully up — launcher/lockscreen on the panel
```

---

## 3. The chainload

### 3.1 Why chainload instead of a plain OHOS boot.img

The X23's LK bootloader rejects an OHOS-only `boot.img` (it fails LK's
signature / header validation). Rather than fight the bootloader, the
chainload **reuses Halium's signed `boot.img` byte-for-byte** — same
kernel, same header — and only swaps the *contents* of the ramdisk's
`/init`. Because the kernel bytes and boot header are unchanged,
`vbmeta`'s chain-of-trust still verifies.

The Halium ramdisk is also useful in its own right: it already ships
the dm-mapper tooling (`parse-android-dynparts`) needed to bring up
Android-style dynamic partitions.

### 3.2 What `init-chainload.sh` does

`device/board/oniro/hybris_generic/launcher/init-chainload.sh` is the
replacement `/init` — a ~150-line shell script that runs as PID 1 in
the initramfs. In order:

1. Mount `/proc`, `/sys`, `/dev`, create `/tmp`, `/root`.
2. `modprobe` the vendor kernel modules **by name** (reading Halium's
   ordered `modules.load`) so dependency resolution works — the block
   subsystem only comes up after the UFS modules load.
3. Run `parse-android-dynparts /dev/disk/by-partlabel/super` — this
   parses the LP metadata in the `super` partition and emits
   `dmsetup create` commands that materialise `/dev/mapper/system_a`,
   `/dev/mapper/vendor_a`, etc.
4. Mount OHOS `system_a` at `/root` (read-only) and `vendor_a` at
   `/root/vendor`.
5. Mount the Halium `system` partition at `/root/android` and the
   Halium `vendor` partition at `/root/android/vendor` — see §5.
6. Pre-create `/root/dev/disk/by-partlabel/*` symlinks so OHOS init
   doesn't block waiting on ueventd.
7. Bind-mount `/proc`, `/sys`, `/dev` into `/root`.
8. `exec env OHOS_NATIVE_BOOT=1 chroot /root /system/bin/init --second-stage`.

The `exec` is important: the kernel keeps PID 1 across `exec`, so OHOS
init becomes PID 1 directly — no `switch_root`, no fork.

### 3.3 `OHOS_NATIVE_BOOT=1`

OHOS init's first boot action is `mksandbox`, which does
`unshare(CLONE_NEWNS)` + `pivot_root` + `setns` to build a sandboxed
mount tree. That sequence corrupts init's `fs_struct` when init is
running *chrooted* (as it is here) — children inherit a dangling
root/cwd and their `exec`s fail silently.

The chainload exports `OHOS_NATIVE_BOOT=1`; `DoMkSandbox` checks this
env var and returns early. Sandboxing is defence-in-depth, not a
correctness boundary, so skipping it for the chainload path is
acceptable. This is the **only** OHOS init source change native boot
requires — the other 29 `InContainerMode()` call sites already do the
right thing when no `container=` env var is set (i.e. on a true PID-1
boot).

---

## 4. Partition layout — `super.img`

Native boot ships a single custom `super` partition (LP-formatted,
built by `kernel/x23/build_super_img.sh` with `lpmake --sparse`)
containing six logical partitions:

| Logical partition | Contents |
|---|---|
| `system_a` | OHOS system (becomes `/` after chroot) |
| `vendor_a` | OHOS vendor |
| `sys_prod_a` | OHOS sys_prod |
| `chip_prod_a` | OHOS chip_prod |
| `halium_system_a` | Halium 12 Android `/system` (HAL runtime) |
| `halium_vendor_a` | Halium 12 MTK `/vendor` (Mali EGL, HAL binaries, `.rc`) |

The two `halium_*` partitions are sourced host-side from public
UBports / Volla URLs (SHA256-pinned) by
`utils/host/pull-halium-blobs.sh` and baked in only when present — an
OHOS-only `super.img` (graphics disabled) still builds without them.

Three on-device partitions are flashed directly: `boot_a` (the
chainload boot.img), `super`, and `vendor_boot_a` (the matched vendor
kernel modules). See `phase_n10_flash_recovery.md` /
`phase_n11_chainload.md` for the flash sequence.

---

## 5. The two roots — OHOS and Halium side by side

After the chainload, the running mount tree has **two** distinct
userspace roots:

```
/                       ← OHOS system_a (chroot target, PID 1's root)
├── system/  vendor/  sys_prod/  chip_prod/   OHOS partitions
├── data/                                     OHOS userdata (ext4)
├── dev/  proc/  sys/                          bound from initramfs
└── android/                                  Halium Android root (full)
    ├── system/   ← inner Halium /system content (libhybris lib path)
    └── vendor/   ← Halium vendor_a overmount    (MTK HAL binaries)
```

The Halium `android-rootfs.img` is a *full* Android root — `system/`,
`vendor/`, `init`, `data/`, etc. at its top level, with the real
`/system` content one level down at `system/`. The chainload mounts
that partition **once** at `/android`, which serves both consumers:

- libhybris hardcodes `/android/system/lib64`, `/android/vendor/lib64`
  as Android-library search paths. OHOS-side services (`composer_host`,
  `render_service`, …) that use libhybris find Android libs there —
  the partition's inner `system/` lands exactly at `/android/system`.
- `androidd` `pivot_root`s the Halium guest NS into `/android`, so the
  inner `system/` becomes `/system` and `/system/bin/init` resolves.

`halium_vendor_a` overmounts the partition's own `/android/vendor`
dir, so the MTK HALs are at `/android/vendor` from the OHOS side and
at `/vendor` after the pivot.

Additionally, `/vendor/lib64/{hw,egl}` on the OHOS side are bound from
`/android/vendor/lib64/{hw,egl}` — Android's `libui` does a literal
`access()` check on those hardcoded paths (which libhybris does *not*
remap) when loading the gralloc mapper.

---

## 6. OHOS init in native mode

OHOS init runs unchanged from the LXC build except for the
`OHOS_NATIVE_BOOT` mksandbox skip. The hardware-specific setup lives in
two vendor cfg overlays imported by the stock `init.cfg`:

- **`/vendor/etc/init.x23.cfg`** (`pre-init` job) — mounts `binderfs`,
  creates the `/dev/binder*` symlinks, `chmod 0666` the binder nodes,
  mounts the property tmpfs, `insmod`s the 21-module Mali GPU stack
  and the touch driver, `rfkill unblock all`. (The `/android` mount
  point itself is created by `init-chainload.sh` before the chroot —
  `/` is read-only by the time OHOS init runs.)
- **`/vendor/etc/init.x23.usb.cfg`** — USB gadget / FunctionFS setup
  for hdc (see §10).

`fstab.x23` mounts `/misc` and `/persist`; the `userdata` partition is
mounted at `/data` (added in N3.5 — without a writable `/data`,
`nativetoken.json` couldn't be written and every service's TokenID
stayed 0, breaking samgr permission checks).

The Mali GPU and touch controller are not in Halium's `vendor_boot`
`modules.load`, so their `.ko` files are bundled into the `vendor_boot`
overlay and `insmod`'d at `pre-init` (before `render_service` /
`composer_host` start, so `/dev/mali0` and `/dev/input/event2` exist).

---

## 7. `androidd` — the Halium HAL guest

`device/board/oniro/hybris_generic/launcher/androidd.c` (~370 LOC,
libc only) is an OHOS init service that launches Halium's Android HAL
services in a contained child namespace. It is the native-boot
replacement for the LXC container machinery — one container, started
once, static config, so a small C launcher beats LXC.

### 7.1 What it does

1. **Pre-flight (in the OHOS root NS):** create the `android-binder`
   binderfs device via `BINDER_CTL_ADD`.
2. **`clone(2)`** a child with `CLONE_NEWPID | CLONE_NEWNS |
   CLONE_NEWUTS`. Deliberately **no** `CLONE_NEWIPC` (Halium HALs and
   OHOS samgr must share `/dev/hwbinder`), **no** `CLONE_NEWNET` (WiFi
   and future RIL share OHOS's net namespace), **no** `CLONE_NEWUSER`.
3. **In the child (the "Halium namespace"):**
   - Build an Android-shaped private `/dev` (tmpfs + `null`, `zero`,
     `random`, binder nodes, GPU/DMA-BUF passthrough binds).
   - Bind a per-namespace property store, mount `/apex` (tmpfs +
     bind every flattened APEX module), `/mnt`, `/linkerconfig`.
   - `pivot_root` into the Halium root.
   - `exec /system/bin/init` — Halium 12's Android init.
4. **Composer-readiness watchdog (parent, in OHOS NS):** polls the
   Halium namespace via `setns` until `IComposer/default` registers
   with hwservicemanager, then sets the OHOS parameter
   `android.composer.ready=1`.

Halium init then runs its `.rc` files and starts the HAL services:
`hwservicemanager`, `servicemanager`, `vndservicemanager`,
`composer@2.x`, `gralloc@4.0`, and the rest of Android's userspace
(zygote, system_server, … — those crash and quiesce harmlessly, we
don't need them).

### 7.2 The composer-readiness gate

`composer_host` and `allocator_host` (the OHOS display VDIs, see §9)
must not start before Halium's composer HAL is registered. The cfg
`cfg/z_composer_host_gate.cfg` overrides them with
`start-mode: condition` + `condition: param:android.composer.ready=1`.
The watchdog (§7.1 step 4) flips that param. The `z_` filename prefix
makes the override sort last in init's cfg merge.

### 7.3 Property store sharing

OHOS-side `composer_host` uses libhybris, which loads Android's bionic
`libc.so`. bionic's property API reads `/dev/__properties__/`. Halium
init *writes* the property store inside its own namespace, but
`composer_host` runs in the **OHOS** namespace — so init's pre-init
mounts a tmpfs at the OHOS-side `/dev/__properties__/`, and `androidd`
binds that same tmpfs into the Halium namespace. Both sides then see
one property store; `composer_host` finds `hwservicemanager.ready=true`
instead of spinning forever in `WaitForProperty`.

---

## 8. Binder layout

The MT6789 kernel has `CONFIG_ANDROID_BINDERFS=y`, so binder devices
are created on demand via `ioctl`, not from a static kernel list.

| Device | OHOS sees | Android (Halium NS) sees |
|---|---|---|
| `binder` | `/dev/binderfs/binder` (default) | `/dev/binderfs/android-binder`, bound as its `/dev/binder` |
| `hwbinder` | shared | shared |
| `vndbinder` | shared | shared |

OHOS is PID 1 and registers samgr as the context manager on the
default `binder`. Android's `servicemanager` registers on a separate
`android-binder` device (created by `androidd`), so the two context
managers never collide. `hwbinder` and `vndbinder` are deliberately
**shared** — that is the whole bridge: Halium's `hwservicemanager`
registers HIDL services on `hwbinder`, and libhybris-based OHOS
services call into them across the same kernel binder object.

---

## 9. Graphics stack

Display is the inheritance of the LXC-era Phase 5–8 work — libhybris,
the two display VDIs, the stability fixes — running unchanged under
native boot. The data path:

```
  OHOS render_service / launcher / SystemUI
        │  (OHOS HDI: IDisplayComposer / IDisplayBuffer)
        ▼
  composer_host  ┐   allocator_host  ┐    ── OHOS HDF service hosts
   libdisplay_   │    libdisplay_    │       (uid composer_host / allocator_host)
   composer_vdi  │    buffer_vdi     │
        │        │         │         │
        ▼        ┘         ▼         ┘
  libhybris (EGL / hwc2 / gralloc bridge)
        │  (bionic libc, HIDL over hwbinder)
        ▼
  Halium HAL services  (composer@2.3, gralloc@4.0)  ── in androidd's NS
        │
        ▼
  Mali GPU  (/dev/mali0, libGLES_mali.so)  +  MTK display kernel driver
```

- **`composer_host`** loads `libdisplay_composer_vdi_impl.z.so`, which
  wraps Halium's HWC2 HIDL service via libhybris to implement OHOS's
  `IDisplayComposerVdi` — it drives `hybris-hwc2-display` at
  720×1560@59 Hz.
- **`allocator_host`** loads `libdisplay_buffer_vdi_impl.z.so`, which
  wraps Halium's gralloc to implement `IDisplayBufferVdi`.
- **libhybris** is the glue: it loads Android `.so`s (EGL, HWC2 compat
  layer, gralloc mapper) with an embedded bionic linker and remaps
  `/vendor`/`/system` paths to `/android/...`.
- The **Mali GPU** is driven by the 21-module `mali_kbase` stack
  `insmod`'d at OHOS pre-init; `render_service` GPU-composites.

Env vars (`HYBRIS_EGLPLATFORM=ohos`, `LD_LIBRARY_PATH`, …) on the VDI
services are inherited unchanged from the LXC build's cfg files.

---

## 10. Input and USB hdc

**Touch.** The X23 touchscreen is a `chipone-tddi` (ICNL9911C) SPI
controller. Like the Mali stack, its driver isn't in Halium's
`vendor_boot`, so `chipone-tddi.ko` is bundled in the `vendor_boot`
overlay and `insmod`'d at pre-init. It enumerates as
`/dev/input/event2`, opened by OHOS `multimodalinput`.

**USB hdc.** The MTK MUSB controller is forced into peripheral mode
(`cmode=3`), the USB gadget / FunctionFS structure is built by
`init.x23.usb.cfg`, and `hdcd` is started after
`const.security.developermode.state=true` is set. The device
enumerates to a USB host as `12d1:5000 "Phone X23"` and answers
`hdc shell`. Because this dev machine isn't aarch64, all `hdc` traffic
is relayed through an aarch64 host (e.g. a Raspberry Pi) with the X23
plugged in — see `HDC_AARCH64_HOST.md`.

---

## 11. What is and isn't working

✅ Native boot, USB hdc, display (OHOS lockscreen on the physical
panel), touch input, WiFi, audio.

🔄 Bluetooth and sensors await `androidd`-resolved Android HALs. The
Phase 8 graphics stability bugs (the Mali `NULL+0x1d8` dropdown crash,
the `SetLayerAlpha` UAF) carry over from the LXC build unchanged.

For current status see `README.md`; for peripherals see
`phase_n9_firmware_peripherals.md`.

---

## Source map

| Component | Path |
|---|---|
| Chainload `/init` | `device/board/oniro/hybris_generic/launcher/init-chainload.sh` |
| Halium HAL launcher | `device/board/oniro/hybris_generic/launcher/androidd.c` |
| `androidd` service cfg | `device/board/oniro/hybris_generic/launcher/androidd.cfg` |
| OHOS init overlay | `vendor/oniro/hybris_generic/etc/init/init.x23.cfg` |
| USB hdc cfg | `vendor/oniro/hybris_generic/etc/init/init.x23.usb.cfg` |
| Composer-ready gate | `device/board/oniro/hybris_generic/cfg/z_composer_host_gate.cfg` |
| fstab | `vendor/oniro/hybris_generic/etc/fstab/fstab.x23` |
| Display VDIs | `device/soc/oniro/hybris_generic/hardware/display/` |
| libhybris | `third_party/libhybris/` |
| `super.img` builder | `device/board/oniro/hybris_generic/kernel/x23/build_super_img.sh` |
| Chainload boot.img builder | `device/board/oniro/hybris_generic/kernel/x23/build_boot_img_chainload.sh` |
| Halium blob fetcher | `device/board/oniro/hybris_generic/utils/host/pull-halium-blobs.sh` |
| Flash automation | `device/board/oniro/hybris_generic/utils/host/flash-native.sh` |
| `OHOS_NATIVE_BOOT` mksandbox skip | `base/startup/init/services/init/standard/init_cmds.c` |
