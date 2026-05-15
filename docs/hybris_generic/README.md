# Hybris Generic — OpenHarmony on the Volla X23 / Volla Tablet

Running **OpenHarmony 6.1** natively on the Volla X23 (codename *vidofnir*,
MT6789, aarch64) and the Volla Tablet (*mimir*, MT8781), using **libhybris**
to bridge the OpenHarmony graphics/HAL stack onto each device's Android
(Halium) vendor HALs.

OHOS boots **natively** — no Ubuntu Touch host, no LXC container. A Halium
boot image chain-loads into OHOS init (`OHOS_NATIVE_BOOT=1 chroot`); a
companion `androidd` process runs the Halium HAL services in their own
mount namespace. See [ARCHITECTURE.md](ARCHITECTURE.md) for the full
structure.

> **The earlier LXC path is retired.** OHOS previously ran as an LXC guest
> under Ubuntu Touch. That path is no longer maintained — its infrastructure
> (LXC config, container start/stop hooks, deployment + installer scripts)
> has been removed. The per-feature bring-up docs from that era are kept as
> `legacy_*.md` references (see
> [Legacy documentation](#legacy-lxc-era-documentation)) because their
> HAL / driver detail still applies under native boot.

> **Keep this README streamlined.** Per-phase technical depth, root causes,
> debug recipes, and hard-won lessons belong in the relevant phase doc. This
> README is the entry point — link to the phase docs instead of restating
> their content here.

## Current state (2026-05-16)

✅ **Native boot + USB hdc, display, and touch all work end-to-end on the
Volla X23.** The device boots OHOS natively, enumerates as
`12d1:5000 Phone X23`, `hdc shell` returns a live shell, the OHOS
lockscreen renders on the physical 720×1560 panel, and touch input works.

- **Boot + hdc** — Halium ramdisk chain-loads into OHOS init; USB
  enumerated; `hdc shell` live. See
  [phase_n11_chainload.md](phase_n11_chainload.md),
  [phase_n7_hdc_usb.md](phase_n7_hdc_usb.md).
- **Display (2026-05-15)** — the full libhybris + Halium graphics stack is
  up: Mali GPU loaded (`/dev/mali0`), `render_service` GPU-composites,
  `allocator_host` allocates buffers, `composer_host` drives
  `hybris-hwc2-display` at 720×1560@59 Hz. Five independent blockers were
  cleared in one session — SELinux never initialised (`lsm=selinux` on the
  chainload cmdline); `/mnt`+`/storage` missing on the RO root (tmpfs
  mounts); Mali GPU 21-module load at pre-init; `/vendor/lib64/{hw,egl}`
  bind for the gralloc mapper; and the earlier property-store share. Full
  detail in [phase_n8_graphics_native.md](phase_n8_graphics_native.md).
- **Touch (2026-05-16)** — the X23 touch is a `chipone-tddi` (ICNL9911C)
  SPI controller; the driver is bundled in the `vendor_boot` overlay and
  `insmod`'d at pre-init, giving `mtk-tpd` on `/dev/input/event2`. See
  § N8.13 of the graphics doc.

Recently resolved — full root-cause detail in the linked phase docs:

- **N3.5** — a missing `userdata` entry in `fstab.x23` was the real reason
  `SetSelfTokenID` no-op'd; adding it lets real TokenIDs propagate and the
  samgr `CanRequest` marker-file bypass was removed.
  [phase_n3_fstab.md](phase_n3_fstab.md).
- **N8.10** — the volla-vidofnir kernel is rebuilt under the chainload with
  the OHOS staging drivers (access_tokenid, hilog, hievent, blackbox,
  binder token-id). [phase_n10_flash_recovery.md](phase_n10_flash_recovery.md).
- **N4** — all Halium HAL services come up and HIDL `IComposer` registers
  with hwservicemanager. [phase_n4_androidd.md](phase_n4_androidd.md).

## Phase index

| Phase | Doc | Status |
|---|---|---|
| N0  | [phase_n0_preflight_smoke_test.md](phase_n0_preflight_smoke_test.md) | ✅ historical — risk retired by chainload approach |
| N1  | [phase_n1_boot_image.md](phase_n1_boot_image.md) | ❌ Superseded by N11 — direct `boot-ohos.img` flash rejected by LK |
| N2  | [phase_n2_init_native.md](phase_n2_init_native.md) | ✅ DoMkSandbox skip under `OHOS_NATIVE_BOOT=1` |
| N3  | [phase_n3_fstab.md](phase_n3_fstab.md) | ✅ `fstab.x23` + `init.x23.cfg` deployed via vendor.img; N3.5 (2026-05-14) added `userdata` entry that unblocks SetSelfTokenID wiring (real TokenIDs propagate; samgr bypass removed) |
| N4  | [phase_n4_androidd.md](phase_n4_androidd.md) | ✅ All Halium HAL services up + IComposer registers (2026-05-14, umask + securebits + caps + binder-chmod + chroot/regex fixes) |
| N5  | [phase_n5_android_image.md](phase_n5_android_image.md) | ✅ Halium system_a (UBports system-image) + vendor_a (bootstrap) baked into super.img |
| N6  | [phase_n6_binder.md](phase_n6_binder.md) | ✅ Default `/dev/binder` for OHOS; `android-binder` for guest via `BINDER_CTL_ADD` |
| N7  | [phase_n7_hdc_usb.md](phase_n7_hdc_usb.md) | ✅ **DONE.**  `cmode=3` + `developermode=true` setparam + aarch64 hdc cross-build |
| N8  | [phase_n8_graphics_native.md](phase_n8_graphics_native.md) | ✅ **Display working (2026-05-15) — OHOS lockscreen on the physical panel.**  Five blockers cleared: SELinux (`lsm=selinux`), `/mnt`+`/storage` tmpfs, Mali GPU 21-module load, `/vendor/lib64/{hw,egl}` bind for the gralloc mapper, + earlier N8.9.1 property-store share.  ✅ Touch input working (2026-05-16) — `chipone-tddi` (ICNL9911C) SPI touch driver bundled + loaded at pre-init; § N8.13, verified on device. |
| N9  | [phase_n9_firmware_peripherals.md](phase_n9_firmware_peripherals.md) | 🔄 Partial — WiFi/audio native; BT/sensors need androidd-resolved Android HALs |
| N10 | [phase_n10_flash_recovery.md](phase_n10_flash_recovery.md) | ✅ `flash-native.sh` follows chainload flow (boot_a.bak → fastbootd → super → boot_a chainload) |
| N11 | [phase_n11_chainload.md](phase_n11_chainload.md) | ✅ **DONE.**  Halium ramdisk + replaced `/init` chain-loads into OHOS init via `OHOS_NATIVE_BOOT=1 chroot` |

## Reproduction

Prerequisites: OHOS source checkout, build container, Halium `boot_a.bak`
backup stashed at `out/hybris_generic/backups/boot_a.bak` (pull via
`adb pull /dev/disk/by-partlabel/boot_a` from a live Halium device
before the first reflash).

```
# Build
./build.sh --product-name hybris_generic --ccache
bash device/board/oniro/hybris_generic/kernel/x23/build_super_img.sh
bash device/board/oniro/hybris_generic/kernel/x23/build_boot_img_chainload.sh

# Flash (device in LK fastboot: Vol-Down + Power)
bash device/board/oniro/hybris_generic/utils/host/flash-native.sh

# Verify (~60–70 s after reboot)
sudo hdc list targets             # 0a20230726
sudo hdc shell "uname -a"         # aarch64 Linux 5.10.209-...
```

For graphics, also populate `device/board/oniro/hybris_generic/halium-blobs/`
before `build_super_img.sh`:

```
bash device/board/oniro/hybris_generic/utils/host/pull-halium-blobs.sh
```

- **What's in each artifact**, what `flash-native.sh` does step-by-step,
  and how the chainload stages mount things — see
  [phase_n11_chainload.md](phase_n11_chainload.md) +
  [phase_n10_flash_recovery.md](phase_n10_flash_recovery.md).
- **OHOS source changes** (DoMkSandbox skip, `cmode=3`, hdcd
  developer-mode setparam) — see the source-side phase docs:
  [phase_n2_init_native.md](phase_n2_init_native.md),
  [phase_n7_hdc_usb.md](phase_n7_hdc_usb.md),
  [phase_n11_chainload.md](phase_n11_chainload.md).
- **Halium blob sourcing** (UBports system-image + bootstrap zip,
  SHA256 pins, `lpunpack.py`) — see
  [phase_n5_android_image.md](phase_n5_android_image.md).
- **aarch64 USB host setup** (Pi as USB rig) —
  [HDC_AARCH64_HOST.md](HDC_AARCH64_HOST.md).
- **Troubleshooting** (Halium splash hang, `hdc shell` hangs,
  `lsusb` shows device but no hdc) — see the relevant phase doc
  (n7 for hdc, n11 for chainload boot).

## Open work

- **`super.img` rebuild (housekeeping).**  The touch fix's only
  OHOS-side change is the `init.x23.cfg` `insmod` line.  It was
  hand-patched into the on-device `vendor_a` partition for fast
  verification; the in-tree source already carries the same change,
  so the next full `build.sh` + `build_super_img.sh` regenerates an
  identical image.  A clean super rebuild+flash is optional — the
  running device is already functionally equivalent.
- **Stability bugs carry over from the LXC era.**  The Mali
  NULL+0x1d8 dropdown crash (8.17), `SetLayerAlpha` UAF (8.11), etc.
  reproduce unchanged under native boot — native boot doesn't change
  the EGL teardown sequence.  See
  [phase_n8_graphics_native.md](phase_n8_graphics_native.md) § N8.6 and
  [legacy_system_stability.md](legacy_system_stability.md).
- **Other input/peripheral kernel modules.**  Same class as Mali +
  touch: any vendor `.ko` not in Halium's `vendor_boot` `modules.load`
  (sensors, fingerprint `focaltech_fp.ko`, camera, etc.) needs the
  same bundle-in-overlay + `insmod`-at-pre-init treatment.
- **Phase N9 peripherals beyond WiFi/audio.**  Bluetooth + sensors
  await `androidd`-resolved Android HALs.  See
  [phase_n9_firmware_peripherals.md](phase_n9_firmware_peripherals.md).
- **GPU module loading is via `init.x23.cfg` `insmod` from
  `/mnt/kmodules`.**  Works, but the proper home for vendor kernel
  modules is the OHOS `vendor` partition (`/vendor/lib/modules/`) with
  a dedicated load service — revisit if the `insmod` list grows.

## Legacy (LXC-era) documentation

Before the native-boot work, OHOS ran as an LXC container under Ubuntu
Touch. The container infrastructure has been removed, but the per-feature
HAL / driver bring-up docs are kept — most of their content (libhybris,
graphics, audio, WiFi, …) still applies under native boot. Each carries a
banner pointing back here.

| Doc | Covers |
|---|---|
| [legacy_kernel_adaptation.md](legacy_kernel_adaptation.md) | Porting OHOS kernel-driver requirements (hilog, accesstokenid, binder token-id, HDF patches) onto the Halium 5.10 kernel — the template for any new vendor-driver port. |
| [legacy_libhybris_integration.md](legacy_libhybris_integration.md) | Building the libhybris EGL / GLES / gralloc / hwc2 stack against Android headers; the HAL bridge fundamentals. |
| [legacy_graphics_stack.md](legacy_graphics_stack.md) | The OHOS display/buffer VDIs that bridge the graphics stack onto Android HALs via libhybris; RenderService bring-up. |
| [legacy_input_system.md](legacy_input_system.md) | `/dev/input` plumbing and `multimodalinput` capabilities for libinput. |
| [legacy_system_stability.md](legacy_system_stability.md) | Post-bring-up bug investigations — EGL teardown races, composition oscillation, the Mali NULL+0x1d8 dropdown crash (8.17), `SetLayerAlpha` UAF (8.11). Most reproduce under native boot. |
| [legacy_volla_tablet_mimir.md](legacy_volla_tablet_mimir.md) | Volla Tablet (mimir) adaptation; Android 13 compatibility entirely via libhybris hooks. |
| [legacy_wifi_support.md](legacy_wifi_support.md) | Native OHOS WiFi via the HDI WPA path; MediaTek chip bring-up. |
| [legacy_power_off_and_backlight.md](legacy_power_off_and_backlight.md) | Backlight sysfs control from `composer_host`; power-off / reboot propagation. |
| [legacy_sharefs_user_files.md](legacy_sharefs_user_files.md) | User-file access (`/storage/Users`) via `sharefs` — workaround plus the proper kernel-driver port. |
| [legacy_audio_support.md](legacy_audio_support.md) | Audio — native ALSA via `libasound` + `audio_host` (13B, working) and the libhybris → MTK HAL fallback (13A). |

## Pointers

- **Architecture overview** (how the running native-boot system is
  structured — chainload, `androidd`, binder, graphics):
  [ARCHITECTURE.md](ARCHITECTURE.md).
- **Original design rationale** (pre-execution native-boot plan, frozen):
  [native_boot_design.md](native_boot_design.md).
- **aarch64 host setup** (Pi as USB rig):
  [HDC_AARCH64_HOST.md](HDC_AARCH64_HOST.md).
- **Per-phase implementation depth + hard-won lessons**: each phase
  doc linked in the phase index above.
