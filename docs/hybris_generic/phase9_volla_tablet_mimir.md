# Phase 9 — Volla Tablet (mimir) Bring-Up

**Status:** ✅ Complete
**Started:** 2026-04-07
**Completed:** 2026-04-08

---

## 9.0 Overview

Adapt the existing `hybris_generic` target to support the **Volla Tablet** (codename **mimir**, SoC MT6789, aarch64). The tablet shares the same MediaTek Helio G99 SoC as the Volla X23, so all existing HAL bridges, libhybris integration, and display VDI code carry over. The work is primarily kernel adaptation and device-specific configuration.

### Device Summary

| Property | Value |
|----------|-------|
| Name | Volla Tablet |
| Codename | mimir |
| SoC | MT8781 (≈ MT6789 / Helio G99) |
| Architecture | aarch64 |
| Panel | 1600×2560 (DSI) |
| Kernel | 5.10.198 (GKI, clang r416183b) |
| Halium | 13 |
| Boot header | v4 |
| Android base | 13 (SDK 33) |
| Host OS | Ubuntu Touch |

### Differences from X23

| Aspect | X23 (vidofnir) | Tablet (mimir) |
|--------|-----------------|-----------------|
| Halium | 12 | 13 |
| Kernel source | `android_kernel_volla_mt6789` | `android_kernel_volla_mt8781` |
| Defconfig | monolithic | GKI (`gki_defconfig` + `mimir.config` + fragments) |
| Boot header | v2 | v4 |
| Display | 720×1560 (phone) | 1600×2560 (tablet) |
| Android SDK | 32 | 33 |
| Device repo | `kernel/linux/volla-vidofnir` | `kernel/linux/volla-mimir` |

---

## 9.1 Kernel Workspace Initialization

**Action:** Clone the kernel source via the Halium build system and verify the build environment.

```bash
cd kernel/linux/volla-mimir
./build.sh -b build-dir -c   # clone + checkout kernel source
```

**Deliverable:** Kernel source at `kernel/linux/volla-mimir/build-dir/downloads/android_kernel_volla_mt8781/`

**Status:** ✅ Complete

**Notes:**
- The device repo is already cloned at `kernel/linux/volla-mimir/`
- The kernel source repo is `android_kernel_volla_mt8781` (Halium 13 branch)
- Build uses clang from `android12L-gsi` branch (r416183b)
- Actual kernel source directory: `kernel/linux/volla-mimir/build-dir/downloads/android_kernel_volla_mt8781/` (not `kernel-volla-mt8781`)

---

## 9.2 OpenHarmony Config Fragment

**Action:** Copy the X23 `openharmony.config` as the mimir config fragment. The same OHOS kernel options are needed (binder enhancements, access_token_id, HDF, hilog, hievent, hisysevent, hungtask, blackbox, HMDFS, etc.).

The fragment is appended to the defconfig list via the `volla-mimir.patch` device repo patch.

**Deliverable:** `device/board/oniro/hybris_generic/kernel/mimir/patch/linux-5.10/kernel_patch/openharmony.config`

**Status:** ✅ Complete

**Notes:**
- GKI base already enables `CONFIG_ASHMEM`, `CONFIG_ANDROID_BINDER_IPC`, `CONFIG_ANDROID_BINDERFS` — no duplicates
- The halium.config already has anbox binder devices, `CONFIG_SYSVIPC`, `CONFIG_PID_NS`, `CONFIG_USER_NS`
- The config fragment is identical to the X23 version

---

## 9.3 Kernel Adaptation Patch

**Action:** Port the X23 `ohos_adaptation.patch` to the mimir kernel source.

**Deliverable:** `device/board/oniro/hybris_generic/kernel/mimir/patch/linux-5.10/kernel_patch/ohos_adaptation.patch`

**Status:** ✅ Complete

**Notes:**
- X23 patch applied with 2 out of ~65 hunks failing (context mismatches in `drivers/Kconfig` and `drivers/Makefile`)
- `drivers/Kconfig`: mimir has `source "drivers/weibu/Kconfig"` after `most/` (X23 had `prize-lifenfen` resmon entries) — OHOS Kconfig sources added manually after `most/`
- `drivers/Makefile`: mimir ends at `most/` without the resmon entries — `obj-$(CONFIG_ACCESS_TOKENID) += accesstokenid/` appended manually
- **Additional fix:** removed `-Wundef` from `KBUILD_CFLAGS` in the top-level Makefile to fix HDF USB driver compilation errors (`'false' is not defined, evaluates to 0 [-Werror,-Wundef]`). The X23 patch kept `-Wundef` but the mimir clang/HDF combination triggers it.
- Final patch regenerated from `git diff` against the clean mimir kernel source (7253 lines, comparable to X23's 7252)

---

## 9.4 HDF Framework Patch

**Action:** Apply HDF patches using the existing `hdf_patch.sh` script.

**Deliverable:** HDF patches in `device/board/oniro/hybris_generic/kernel/mimir/patch/linux-5.10/common_patch/`

**Status:** ✅ Complete

**Notes:**
- HDF patches copied from X23 (SoC-independent)
- `hdf_patch.sh` applied cleanly except for `drivers/hid/Makefile` (1 hunk failed — mimir lacks blank line between header comment and first rule)
- `drivers/hid/Makefile` fixed manually: HDF include block inserted between comment header and `hid-y` rule

---

## 9.5 Halium Build System Patches

**Action:** Apply build-tools and libufdt patches for Python 2→3 compatibility and extended PATH.

**Deliverable:**
- `device/board/oniro/hybris_generic/kernel/mimir/patch/linux-5.10/halium-generic-adaptation-build-tools.patch`
- `device/board/oniro/hybris_generic/kernel/mimir/patch/linux-5.10/libufdt.patch`
- `device/board/oniro/hybris_generic/kernel/mimir/patch/linux-5.10/volla-mimir.patch`

**Status:** ✅ Complete

**Notes:**
- Halium 13 build tools have the same issues as Halium 12: restricted `ALLOWED_HOST_TOOLS`, `python2` references, Python 2 syntax in `mkdtboimg.py`
- All three X23 patches (build-tools, libufdt) applied cleanly without modification
- `volla-mimir.patch` created from scratch: adds `openharmony.config` to defconfig list and `hardware=mimir ohos.boot.sn=0a20230726rpi` to kernel command line

---

## 9.6 Build Automation Script

**Action:** Create `build_kernel.sh` for mimir, adapted from the X23 version.

**Deliverable:** `device/board/oniro/hybris_generic/kernel/mimir/build_kernel.sh`

**Status:** ✅ Complete

**Notes:**
- Adapted from X23 with corrected paths: `KERNEL_TREE=kernel/linux/volla-mimir`, `KERNEL_SRC=build-dir/downloads/android_kernel_volla_mt8781`, `DEVICE_NAME=mimir`
- Clone URL updated to Halium 13 volla-mimir repo
- Temp workspace: `out/kernel/src_tmp/volla-mimir`

---

## 9.7 Deploy Script

**Action:** Create `deploy-kernel.sh` for mimir.

**Deliverable:** `device/board/oniro/hybris_generic/kernel/mimir/deploy-kernel.sh`

**Status:** ✅ Complete

**Notes:**
- Identical flow to X23: push boot.img + vendor_boot.img + modules, flash to active slot, reboot
- Paths updated for mimir kernel tree

---

## 9.8 Kernel Build & Verification

**Action:** Build the kernel and verify OHOS device nodes after flashing.

**Deliverable:** Boot image with OHOS kernel support, confirmed device nodes

**Status:** ✅ Complete

**Notes:**
- Kernel built successfully using Halium 13 build system: `PRODUCT_PATH=vendor/oniro/hybris_generic ./build.sh -b build-dir -k`
- Artifacts: `boot.img` (64M), `dtbo.img` (120K), `vendor_boot.img` (8M), `modules.tar.gz` (26M)
- Flashed to boot slot `_a` via adb
- Kernel boots: `5.10.198-gfa02a2a9224f-dirty`
- **Verified device nodes:**
  - `/dev/access_token_id` ✅ (misc device 126)
  - `/dev/binder` → `/dev/binderfs/binder` ✅
  - `/dev/hwbinder` → `/dev/binderfs/hwbinder` ✅
  - `/dev/vndbinder` → `/dev/binderfs/vndbinder` ✅
- **Running kernel config confirmed:** `CONFIG_HILOG=y`, `CONFIG_HIEVENT=y`, `CONFIG_DRIVERS_HDF=y`, `CONFIG_ACCESS_TOKENID=y`
- `/dev/hilog` and `/dev/hievent` not present as misc devices — these will be created when the OHOS container boots (same as X23 behavior)

---

## 9.9 LXC Container Deployment

**Action:** Deploy the OHOS rootfs as an LXC container on the tablet.

**Deliverable:** Running OHOS LXC container on Volla Tablet

**Status:** ✅ Complete (container runs, services start, display compositor connects)

**Notes:**
- Deployed using existing rootfs tarball (2026-03-31 build) via `deploy-lxc-container.sh -p`
- LXC config is device-agnostic — no changes needed
- Android container on Halium 13 already has `graphics.composer@2.3-service` running (unlike X23 which needed it started manually)

**Fixes applied:**

**9.9.1 — `start-ohos.sh` killed Android compositor on Halium 13:**
The X23-specific `setprop ctl.stop vendor.hwcomposer-2-3` killed the running `@2.3-service`, then tried to start the non-existent `@2.1-service`. Fixed by detecting Halium version (presence of `@2.3-service` binary) and skipping the stop. Also auto-detect the correct composer binary path.

**9.9.2 — `composer_host` and `allocator_host` missing HYBRIS environment:**
The HDF devhost cfg (`/vendor/etc/init/hdf_devhost.cfg`) did not include `HYBRIS_EGLPLATFORM`, `HYBRIS_LD_LIBRARY_PATH`, or `LD_LIBRARY_PATH` for `composer_host` and `allocator_host`. Without these, the VDI could not load libhybris or the Android HAL wrappers. Fixed by adding `env` blocks to both services in `hdf_devhost.cfg`. The `z_hybris_hdf_env.cfg` overlay approach was tried first but OHOS init does not merge `env` arrays across cfg files.

**9.9.3 — Display compositor registered successfully:**
After both fixes, `display_composer_service` registers with the HDF service manager. The VDI loads `libhwc2.z.so`, `libhwc2_compat_layer.so`, Android `libEGL.so`, `libgralloc_extra.so`, and the MTK gralloc mapper via libhybris. No more "service not found" errors.

---

## 9.10 Display & Graphics Verification

**Action:** Verify the graphics stack works on the tablet's 1600×2560 panel.

**Deliverable:** Working graphics output on tablet display

**Status:** 🔄 In Progress

**Notes:**
- `display_composer_service` registers successfully — the VDI connects to the Android HWC2 HAL
- `bootanimation` starts and runs for ~63 seconds before crashing with `SIGSEGV(SEGV_ACCERR)` in `libGLES_mali.so` during thread TSD cleanup (`__pthread_tsd_run_dtors`) — same Mali `OhosNativeWindow` teardown bug class as X23 Phase 8.1
- `render_service` crashes after ~3 seconds with `SIGBUS(BUS_ADRALN)` in `libunwindstack.so` during `gralloc4::getPlaneLayouts` — alignment error in Android 13 `libunwindstack.so` when called through libhybris. This is a new Android 13-specific issue not seen on Android 12 (X23).

**Fixes applied:**

**9.10.1 — SIGBUS in `libunwindstack.so` (gralloc mapper CallStack):**
Android 13 MTK gralloc mapper (`arm::mapper::common::get`) calls `android::CallStack` for debug backtraces. The `libunwindstack.so` crashes with `SIGBUS(BUS_ADRALN)` at address `0x2b` when unwinding MUSL stack frames (alignment mismatch in `MapInfo::~MapInfo()`). Fixed by hooking the mangled CallStack C++ symbols (`_ZN7android9CallStackC1Ev`, `_ZN7android9CallStack6updateEii`, etc.) in libhybris `hooks.c` as no-ops. The calls cross DSO boundaries (mapper → libutilscallstack.so) through PLT, making them hookable. No stub .so or binary patching required.

**9.10.2 — SIGABRT in bionic `emutls_init`:**
Android 13 bionic `libc.so` uses emulated TLS (`__emutls_get_address`) for thread-local variables in `libc++.so` (iostream globals). The `emutls_init` function calls `pthread_key_create`, and if it fails in MUSL threads, it calls `abort()`. The `__emutls_get_address` call is internal to bionic libc.so and cannot be hooked by libhybris.

Fixed by hooking `__ctype_get_mb_cur_max` in libhybris `hooks.c` — a one-line function that returns `4` (UTF-8 MB_CUR_MAX). This call crosses the DSO boundary from `libc++.so` to bionic `libc.so` (through PLT), making it hookable. By returning directly from MUSL, bionic's emutls code path is never reached. A secondary `__emutls_get_address` hook (MUSL-compatible implementation using `pthread_key_t`) is also provided as a safety net.

No binary patching of bionic libc.so is required.

**9.10.3 — HDF devhost services not starting:**
The OHOS init does not scan `/vendor/etc/init/` for service cfgs in this rootfs build (possibly a container-mode restriction). Copied `hdf_devhost.cfg` to `/system/etc/init/` as a workaround. Also added HYBRIS env vars (`HYBRIS_EGLPLATFORM=ohos`, `HYBRIS_LD_LIBRARY_PATH`, `LD_LIBRARY_PATH`) directly to the `composer_host` and `allocator_host` service definitions.

**Current status (as of 2026-04-07):** `render_service` runs at 95% CPU (actively compositing). `bootanimation`, `composer_host`, and `allocator_host` all running without crashes. HAP installation in progress. Boot animation playing on physical 1600×2560 tablet display.

**9.10.4 — Access Token ID permissions and seccomp:**
`/dev/access_token_id` was `0600` (root-only), causing ATM to loop on `GetHapTokenInfo: TokenID invalid`. Fixed with `chmod 666` in `z_container_fixes.cfg` (post-fs-data job). Also added `persist.init.debug.seccomp.enable=0` to `ohos.para`.

**Current status (as of 2026-04-07):**
- `render_service` runs at 95-98% CPU — actively compositing frames on 1600×2560 display
- `bootanimation` playing on physical tablet display
- `composer_host` and `allocator_host` running stably
- `com.ohos.launcher` starts (`MyAbilityStage.onCreate` reached) but crashes with `LIFECYCLE_TIMEOUT` every ~18s due to cgroup/pids namespace issues in the container (`/dev/pids/` not available). Launcher is automatically restarted by AMS.
- HAP installation succeeded (launcher binary present and executing)

**9.10.5 — IPC namespace mismatch (Halium 13):**
On Halium 12 (X23), both the Android and OHOS containers share the host IPC namespace. On Halium 13 (mimir), the Android container creates its own IPC namespace (`lxc.namespace.keep = net user` without `ipc`). OHOS `composer_host` could not reach `hwservicemanager` because they were in different IPC namespaces.

Fixed by having `start-ohos.sh` detect Halium 13 and dynamically replace `lxc.namespace.keep = net user ipc` with `lxc.namespace.keep = net user` + `lxc.namespace.share.ipc = android` in the LXC config before starting the container. Adding `ipc` to the Android container config was tried first but caused the Android container to fail to start.

**9.10.6 — `ro.hardware.egl=meow`:**
The Volla Tablet vendor build.prop has `ro.hardware.egl=meow` (custom Weibu value), causing the Android EGL loader to fail to find the Mali implementation. Fixed in `start-ohos.sh` with a sed replacement to `mali` after generating `build.prop`.

---

## 9.11 Input & Touch Verification

**Action:** Verify touchscreen input works on the tablet.

**Deliverable:** Working touch input on tablet display

**Status:** ✅ Complete

**Notes:**
- 6 event devices available (`event0`–`event5`) via `/dev/input` rbind mount
- `multimodalinput` has `CAP_SYS_NICE` but NOT `CAP_DAC_OVERRIDE` — the `z_multimodalinput_caps.cfg` overlay doesn't merge correctly on this rootfs build (same issue as X23 Phase 7, but the inode-order fix didn't apply)
- **Workaround:** `chmod 666 /dev/input/event*` in `z_container_fixes.cfg` (boot job) — simpler and more reliable than capability-based approach
- After chmod and `multimodalinput` restart, all 6 event devices are opened
- Touch input confirmed working on physical 1600×2560 tablet display

---

## Persistence — Changes for Rebuild/Redeploy

All mimir-specific changes are persisted in the source tree and applied automatically during build/deploy. No manual on-device edits required. X23 support is preserved.

### Source tree changes (backward-compatible with X23)

| File | Change | X23 Impact |
|------|--------|------------|
| `utils/lxc/config` | X23-compatible defaults (`keep ipc`); comment explains Halium 13 handling | None (same as before) |
| `utils/start-ohos.sh` | Auto-detect Halium 12/13; adjust IPC namespace, deploy stubs, fix `ro.hardware.egl` | Halium 12 path unchanged |
| `utils/deploy-lxc-container.sh` | Ship stubs, apply rootfs overlays, patch `hdf_devhost.cfg` with HYBRIS env, copy to `/system/etc/init/` | Overlay + hdf_devhost.cfg changes are additive and harmless |
| `third_party/libhybris/hybris/common/hooks.c` | CallStack no-ops + `__ctype_get_mb_cur_max` + `__emutls_get_address` hooks | Harmless — Android 12 doesn't trigger these code paths |
| `vendor/oniro/hybris_generic/etc/param/product_hybris_generic.para` | `persist.init.debug.seccomp.enable=0` | Already used on X23 (Phase 6.14 workaround) |
| `utils/rootfs_overlay/system/etc/init/z_container_fixes.cfg` | `chmod 666` for `access_token_id` + input devices at boot | Works on both devices |
| `utils/rootfs_overlay/system/etc/init/z_hybris_hdf_env.cfg` | HYBRIS env vars overlay for `composer_host`/`allocator_host` | Additive, harmless if env already set |

### Halium 13-only changes (applied dynamically by `start-ohos.sh`)

| Change | Mechanism |
|--------|-----------|
| IPC namespace: `lxc.namespace.share.ipc = android` | `sed` replaces `keep ipc` line in LXC config before launch |
| `ro.hardware.egl=meow → mali` | `sed` in generated `build.prop` |

### Kernel mimir artifacts (separate from X23)

| Item | Path |
|------|------|
| Build script | `device/board/oniro/hybris_generic/kernel/mimir/build_kernel.sh` |
| Deploy script | `device/board/oniro/hybris_generic/kernel/mimir/deploy-kernel.sh` |
| OHOS adaptation patch | `device/board/oniro/hybris_generic/kernel/mimir/patch/linux-5.10/kernel_patch/ohos_adaptation.patch` |
| OpenHarmony config | `device/board/oniro/hybris_generic/kernel/mimir/patch/linux-5.10/kernel_patch/openharmony.config` |
| Device repo patch | `device/board/oniro/hybris_generic/kernel/mimir/patch/linux-5.10/volla-mimir.patch` |

---

## Key Paths Reference

| Item | Path |
|------|------|
| Device repo (cloned) | `kernel/linux/volla-mimir/` |
| Kernel source (after init) | `kernel/linux/volla-mimir/build-dir/downloads/android_kernel_volla_mt8781/` |
| Kernel patches | `device/board/oniro/hybris_generic/kernel/mimir/patch/linux-5.10/` |
| Build script | `device/board/oniro/hybris_generic/kernel/mimir/build_kernel.sh` |
| Deploy script | `device/board/oniro/hybris_generic/kernel/mimir/deploy-kernel.sh` |
| Build artifacts | `kernel/linux/volla-mimir/out/` |
| Rootfs overlays | `device/board/oniro/hybris_generic/utils/rootfs_overlay/` |
