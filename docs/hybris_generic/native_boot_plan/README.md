# Native Boot Plan — OpenHarmony on Volla X23 / Volla Tablet

Status, reproduction, and per-phase pointers for booting OHOS natively
(no Ubuntu Touch host, no LXC) via a Halium chainload.

## Current state (2026-05-12)

✅ **Native boot + USB hdc work end-to-end on Volla X23.**  The device
boots OHOS natively, enumerates as `12d1:5000 Phone X23`, and
`hdc shell` returns a live shell.

🚧 **Graphics revival (N4 + N5 + N8) source-side complete, on-device
blocked.**  Halium HAL launcher (`androidd`), super-image Halium blobs,
and composer-ready gate cfg are all in tree.  Halium init `SIGSEGV`s at
startup before producing output — see `phase_n4_androidd.md` "Current
blocker" for the next-session debugging path.

## Phase index

| Phase | Doc | Status |
|---|---|---|
| N0  | [phase_n0_preflight_smoke_test.md](phase_n0_preflight_smoke_test.md) | ✅ historical — risk retired by chainload approach |
| N1  | [phase_n1_boot_image.md](phase_n1_boot_image.md) | ❌ Superseded by N11 — direct `boot-ohos.img` flash rejected by LK |
| N2  | [phase_n2_init_native.md](phase_n2_init_native.md) | ✅ DoMkSandbox skip under `OHOS_NATIVE_BOOT=1` |
| N3  | [phase_n3_fstab.md](phase_n3_fstab.md) | ✅ `fstab.x23` + `init.x23.cfg` deployed via vendor.img |
| N4  | [phase_n4_androidd.md](phase_n4_androidd.md) | ✅ Source complete; 🚧 Halium init SIGSEGVs at startup — current blocker |
| N5  | [phase_n5_android_image.md](phase_n5_android_image.md) | ✅ Halium system_a (UBports system-image) + vendor_a (bootstrap) baked into super.img |
| N6  | [phase_n6_binder.md](phase_n6_binder.md) | ✅ Default `/dev/binder` for OHOS; `android-binder` for guest via `BINDER_CTL_ADD` |
| N7  | [phase_n7_hdc_usb.md](phase_n7_hdc_usb.md) | ✅ **DONE.**  `cmode=3` + `developermode=true` setparam + aarch64 hdc cross-build |
| N8  | [phase_n8_graphics_native.md](phase_n8_graphics_native.md) | ✅ Source complete (composer-ready gate cfg + libhybris path-map inherited from LXC) |
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

- **Halium init SIGSEGV at startup.**  Current blocker for the
  graphics revival.  Next-session strace/LD_DEBUG plan in
  [phase_n4_androidd.md](phase_n4_androidd.md) "Current blocker".
- **Phase N9 peripherals beyond WiFi/audio.**  Bluetooth + sensors
  await `androidd`-resolved Android HALs.  See
  [phase_n9_firmware_peripherals.md](phase_n9_firmware_peripherals.md).
- **Persistent OHOS app state.**  `/data` is RO on native (fstab only
  mounts misc + persist); a userdata partition entry is needed before
  any OHOS app can persist data.  See lesson in
  [phase_n4_androidd.md](phase_n4_androidd.md) "Hard-won lessons".

## Pointers

- **Original design rationale** (pre-execution plan, frozen):
  [native_boot_plan.md](native_boot_plan.md).
- **aarch64 host setup** (Pi as USB rig):
  [HDC_AARCH64_HOST.md](HDC_AARCH64_HOST.md).
- **Per-phase implementation depth + hard-won lessons**: each phase
  doc linked in the phase index above.
