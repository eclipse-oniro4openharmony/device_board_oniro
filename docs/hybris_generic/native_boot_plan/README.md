# Native Boot Plan — OpenHarmony on Volla X23 / Volla Tablet

Status, reproduction, and per-phase pointers for booting OHOS natively
(no Ubuntu Touch host, no LXC) via a Halium chainload.

> **Keep this index streamlined.**  Per-phase technical depth, root
> causes, debug recipes, and hard-won lessons belong in the relevant
> `phase_nX_*.md` doc.  This README is the entry point — link to the
> phase docs instead of restating their content here.

## Current state (2026-05-14)

✅ **Native boot + USB hdc work end-to-end on Volla X23.**  The device
boots OHOS natively, enumerates as `12d1:5000 Phone X23`, and
`hdc shell` returns a live shell.

✅ **Halium init + HAL service SEGV root-caused and fixed (2026-05-14).**
`mknod_min(/dev/null, 0666, …)` in `androidd` was running with the
inherited umask `022`, so `/dev/null` was created mode `0644` instead of
`0666`.  Every Halium HAL service forks as a non-root uid (system=1000,
logd=1036, …); the bionic linker's `__libc_init_AT_SECURE` immediately
calls `open("/dev/null", O_RDWR)`, gets `EACCES`, retries 4× on EINTR,
then deliberately aborts with abort-code 160 (`pc=…+0xe6c, x8=0xa0` —
this is the unique signature we saw for every service).  Fix: `umask(0)`
in `child_main()` before the `mknod_min` calls.  Verified: 0 SIGSEGVs
in 25 s of crash-loop window post-fix, and a static probe binary forks
+ setresuid(1000)s + execve(sh) cleanly to `_exit(0)`.

🚧 **Graphics revival blocked on `logd` exit status 6 (next layer).**
Halium init runs through `early-init`/`init`/`late-init`/`fs`/`post-fs`/`late-fs`,
no SEGVs anywhere, and `init: starting service 'hwservicemanager' /
'logd' / 'servicemanager' / 'vndservicemanager'` fires on the 5 s
restart cadence — but `logd` exits with WEXITSTATUS=6 every cycle.
Without a working logd, `hwservicemanager` and `servicemanager` can't
publish their endpoints (they `LOG(FATAL)` on the missing
`/dev/socket/logd` rendezvous or the missing logd socket pair) and
composer never has anywhere to register.  Need to diagnose what logd
is asserting on — most likely candidates are bionic capability
inheritance, `/dev/kmsg` access (needs `CAP_SYSLOG`, granted to
`androidd`'s caps + bounding set in `androidd.cfg`), or a logd-specific
`/data/misc/logd/` path that doesn't exist in our tmpfs `/data`.  Same
debug-overlay + probe-fork mechanism that nailed the umask bug applies.
See `phase_n4_androidd.md` "2026-05-14" section for the diagnostic
workflow.

## Phase index

| Phase | Doc | Status |
|---|---|---|
| N0  | [phase_n0_preflight_smoke_test.md](phase_n0_preflight_smoke_test.md) | ✅ historical — risk retired by chainload approach |
| N1  | [phase_n1_boot_image.md](phase_n1_boot_image.md) | ❌ Superseded by N11 — direct `boot-ohos.img` flash rejected by LK |
| N2  | [phase_n2_init_native.md](phase_n2_init_native.md) | ✅ DoMkSandbox skip under `OHOS_NATIVE_BOOT=1` |
| N3  | [phase_n3_fstab.md](phase_n3_fstab.md) | ✅ `fstab.x23` + `init.x23.cfg` deployed via vendor.img |
| N4  | [phase_n4_androidd.md](phase_n4_androidd.md) | ✅ Halium init runs + HAL services start (2026-05-14, umask fix); 🚧 composer never registers |
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

- **`logd` exits with status 6 every restart.**  Now-current blocker
  after the 2026-05-14 umask fix.  Halium init reaches
  `class_start core` and forks logd, hwservicemanager, servicemanager,
  vndservicemanager every 5 s, but logd's `main()` returns 6 and the
  other three failures cascade.  Likely root cause categories: missing
  `/data/misc/logd/`, an LSM denial on a kmsg-related operation, or a
  bionic capability check.  Use `/module_update/halium-debug/probe` +
  `androidd`'s pre-init probe-fork (already wired) to fork logd under
  `PTRACE_TRACEME` and capture its dying syscall, similar to the
  technique that found the /dev/null EACCES.
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
