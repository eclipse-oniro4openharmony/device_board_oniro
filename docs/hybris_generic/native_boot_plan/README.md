# Native Boot Plan — OpenHarmony on Volla X23 / Volla Tablet

Status, reproduction, and per-phase pointers for booting OHOS natively
(no Ubuntu Touch host, no LXC) via a Halium chainload.

> **Keep this index streamlined.**  Per-phase technical depth, root
> causes, debug recipes, and hard-won lessons belong in the relevant
> `phase_nX_*.md` doc.  This README is the entry point — link to the
> phase docs instead of restating their content here.

## Current state (2026-05-14 PM)

✅ **Native boot + USB hdc work end-to-end on Volla X23.**  The device
boots OHOS natively, enumerates as `12d1:5000 Phone X23`, and
`hdc shell` returns a live shell.

✅ **All Halium HAL services come up; composer registers; the
watchdog flips `android.composer.ready=1`.**  See N4 doc for the
five-layer caps/securebits/perms cascade landed earlier today.

✅ **SetSelfTokenID wiring complete (2026-05-14, N3.5).**  Real
TokenIDs now propagate to all native services; the N8.7 marker-file
`CanRequest` bypass has been removed (revert committed).  Root cause
was a missing `userdata` entry in `fstab.x23` — see N3.5 below.

🔄 **N8.7 + N8.8 + N8.10 landed; `composer_host` alive but service
not yet registered (N8.9).**

N8.10 — OHOS-patched kernel under the chainload (2026-05-14):
- `build_kernel.sh` rebuilds the volla-vidofnir kernel with
  `ohos_adaptation.patch` (adds access_tokenid, hilog, hievent,
  blackbox, binder token-id staging drivers) and `openharmony.config`.
- Patch updated: drop `-Wundef`/`-Werror=strict-prototypes` from
  `KBUILD_CFLAGS` (HDF USB headers don't include `<stdbool.h>`,
  fail with `-Wundef -Werror`).
- `build_boot_img_chainload.sh` substitutes the patched kernel from
  `kernel/linux/volla-vidofnir/out/boot.img` into the chainload boot
  (env override `OHOS_KERNEL_BOOT_IMG`).
- `flash-native.sh` also flashes `vendor_boot_a` with the matched
  modules.tar.gz from the same build, so the 181 vendor modules carry
  the same `vermagic=5.10.209` (no scmversion) as the kernel.
- Confirmed on device: `/dev/access_token_id` present
  (`crw-rw-rw- access_token:access_token 10:126`), kernel reports
  `5.10.209`, 161 modules loaded (same count as Halium baseline).

N3.5 — Userdata mount → SetSelfTokenID end-to-end (2026-05-14):
- After N8.10 the `access_tokenid` driver was present but
  `SetSelfTokenID` still no-op'd — every native service kept
  TokenID=0 and the marker bypass had to stay in place.
- Root cause was *not* the driver: `fstab.x23` had no entry for the
  `userdata` partition, so `/data` was the read-only system rootfs at
  the time init's pre-init `load_access_token_id` ran.
  `GetAccessTokenId()` failed silently on the unwritable
  `nativetoken.json` path and returned 0 for every service.  Init then
  called `SetSelfTokenID(0)`, samgr's `CanRequest` saw
  `TOKEN_INVALID`, and only uid==0/1000 services passed the fallback.
- Fix: add a `userdata` entry to `fstab.x23` using the Halium
  device-path convention (`/dev/block/platform/soc/11270000.ufshci/by-name/userdata`,
  *not* `/dev/disk/by-partlabel/*` — those symlinks aren't populated
  by stock Halium ueventd on the X23).  Same convention applied to
  the existing `/misc` and `/persist` entries.
- Reverted in the same change: marker-file `CanRequest` bypass in
  `system_ability_manager_stub.cpp::CanRequest()` and the
  `write /dev/.ohos_native_boot 1` line in `init.x23.cfg`.
- Verified on device (2026-05-14): `/data` mounts as
  `/dev/block/sdc58 on /data type ext4`; `nativetoken.json`
  populated; `atm dump -t -n composer_host` returns
  `tokenID=671648039`; no `CanRequest` denials in 30+ s of runtime
  with the bypass removed; all 6 key services
  (samgr, hdf_devmgr, composer_host, allocator_host, foundation,
  render_service) running cleanly.

N8.7 — samgr binder access + native-boot bypass:
- `/dev/binderfs/binder` defaults to mode `0600 root:root` from
  binderfs.  samgr runs as uid `samgr (5555)`, so its
  `open(/dev/binder)` failed → `BINDER_SET_CONTEXT_MGR` returned -1 →
  `JoinWorkThread error, samgr main exit!` crash-loop.  Fix: `chmod
  0666 /dev/binderfs/binder` in `init.x23.cfg` pre-init.
- `samgr CanRequest()` requires `tokenType == TOKEN_NATIVE` or `uid in
  {0, 1000}`.  Halium 5.10's kernel has no `/dev/access_token_id`, so
  hdf_devmgr (uid 3044), composer_host (3036), etc. were denied SA
  registration.  Env vars don't propagate from the chainload to OHOS
  init's children, so the LXC `OHOS_RUNTIME_CONFIG=1` trick doesn't
  apply.  Fix: marker file `/dev/.ohos_native_boot` written by
  `init.x23.cfg`, samgr `CanRequest()` short-circuits on `access(F_OK)`.

N8.8 — chainload mount layout for libhybris:
- `halium_system_a` is a dynamic-partition image; its actual Android
  `/system` content lives in the inner `system/` subdir.  Mounting the
  partition directly at `/android/system` left libhybris's hardcoded
  `/android/system/lib64` empty → composer_host SIGSEGV on first
  Android dlopen.  Fix: chainload now mounts `halium_system_a` at
  `/halium-system` AND bind-mounts `/halium-system/system` over
  `/android/system` (matching the LXC view).  `androidd.c` `ANDROID_ROOT`
  switched from `/android/system` to `/halium-system` so its
  `pivot_root` still lands somewhere with `/system/bin/init`.
- Bionic libc is shipped via APEX
  (`/apex/com.android.runtime/lib64/bionic/libc.so`), not
  `/system/lib64`.  Without `/apex` mounted, every Android dlopen
  failed with `library "libc.so" not found`.  Fix: chainload mkdirs
  `/root/apex` in the rw window and binds `/halium-system/system/apex`
  over `/apex`.

After N8.7 + N8.8: `composer_host` and `allocator_host` are alive
(pid stable, no SIGSEGV), all Android system libs map in
(`/proc/$pid/maps` shows libEGL.so, libhwc2_compat_layer.so, libbinder,
libgralloctypes, etc.), main thread idle and IPC threads parked in
`binder_wait_for_work`.

🚧 **Next blocker (N8.9):** `composer_host` is up but it has not
published `display_composer_service` to hdf_devmgr — render_service
still logs `StubGetService service display_composer_service not
found` at 100 Hz.  Candidates: SA-registration permission inside the
HDF driver's `Bind()`, `Bind()` hanging in libhybris HWC2 init, HCS
mismatch, or `g_module` dlsym mismatch.  See `phase_n8_graphics_native.md`
§ N8.9 for the four candidates and recommended first probe (add
`HDF_LOGE` to `hybris_composer_vdi_impl.cpp::Bind`).

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
| N8  | [phase_n8_graphics_native.md](phase_n8_graphics_native.md) | 🔄 N8.7+N8.8+N8.10 done (2026-05-14): samgr binder chmod; `/halium-system` bind + `/apex` bind for libhybris paths; OHOS-patched kernel + matched vendor_boot.img so `/dev/access_token_id` is now present.  N8.7 marker bypass *removed* — the N3.5 userdata mount makes SetSelfTokenID work properly.  N8.9 open: composer_host alive but display_composer_service not published to hdf_devmgr. |
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

- **N8.9: composer_host is alive but display_composer_service is not
  published.**  Now-current blocker after the N8.7 samgr + N8.8 mount
  fixes.  composer_host has all Android libs mapped in
  (libEGL/libGLES_mali/libhwc2_compat_layer/libbinder/libgralloctypes/
  libfmq/…), main thread idle, IPC threads parked in
  `binder_wait_for_work`.  But hdf_devmgr's `devsvc_manager_stub`
  reports `display_composer_service not found` at 100 Hz to every
  render_service poll.  Four candidates documented in
  `phase_n8_graphics_native.md` § N8.9.  Recommended next probe: add
  `HDF_LOGE` traces to
  `device/soc/oniro/hybris_generic/hardware/display/src/display_composer/hybris_composer_vdi_impl.cpp::Bind()`
  and the dispatcher in `libdisplay_composer_driver_1.0.z.so` to see
  whether the driver's bind path runs at all in native boot.
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
