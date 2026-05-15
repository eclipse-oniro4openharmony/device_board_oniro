# Phase 6: Graphics Stack & RenderService Adaptation

> **Legacy (LXC-era) document.** Describes the original OHOS-as-LXC-container
> path, which is **no longer maintained** — the project now boots OHOS
> natively (no Ubuntu Touch host, no LXC). Kept as a reference for the HAL /
> driver bring-up detail (libhybris, graphics, audio, WiFi, …) that still
> applies under native boot. For current status start at [README.md](README.md).

> **Architecture Overview:**
>
> The OHOS graphics stack has three layers that must be wired up:
>
> 1. **Display Composer VDI** (`libdisplay_composer_vdi_impl.z.so`) — loaded by `drivers/peripheral/display/composer/hdi_service` at startup via `dlopen`/`dlsym`. Implements `IDisplayComposerVdi` interface. The HDF `display_composer` driver hosts this as an HDI service that RenderService's `HdiBackend` connects to via `IDisplayComposerInterface::Get()`.
>
> 2. **Display Buffer VDI** (`libdisplay_buffer_vdi_impl.z.so`) — loaded by `drivers/peripheral/display/buffer/hdi_service`. Implements `IDisplayBufferVdi` (AllocMem, FreeMem, Mmap, etc.). Used for GPU buffer allocation.
>
> 3. **EGL/GLES impl libraries** (`libEGL_impl.so`, etc.) — loaded by the OHOS `opengl_wrapper` (`libEGL.so`) via `dlopen`. On other devices these are symlinks to Mesa/vendor blobs.
>
> **Our approach:** VDI libs wrap libhybris's `libhwc2` and `libgralloc` (which talk to Android HALs via HIDL/binder). For EGL/GLES, `libEGL_impl.so` etc. are symlinks to libhybris's EGL/GLES libraries, which load the Android vendor GPU driver (`libGLES_mali.so`) via the hybris linker.
>
> **Reference implementations studied:**
> - `device/soc/oniro/x86_general/hardware/display/` — DRM/KMS-based VDI (closest template)
> - `device/soc/rockchip/rk3568/hardware/display/` — DRM/KMS + GBM-based VDI
> - `device/soc/oniro/mt6789/hardware/gpu/` — prebuilt Mesa GPU libs with `libEGL_impl.so` symlinks
> - `drivers/peripheral/display/composer/vdi_base/` — default DRM-based VDI

### 6.1 Implement Display Composer VDI (libhybris-hwc2 wrapper)

> **Context:** The display composer VDI wraps `libhybris-hwc2` (which calls Android's HWC2 HIDL service) instead of using DRM/KMS directly.

- [x] **Action:** Create `device/soc/oniro/hybris_generic/hardware/display/` with `BUILD.gn`, `hybris_composer_vdi_impl.h/.cpp`, `hybris_display.h/.cpp`, `hybris_layer.h/.cpp`, `display_common.h`.

- [x] **Action:** Implement `HybrisComposerVdiImpl` inheriting from `IDisplayComposerVdi`. Key mappings: `RegHotPlugCallback` → `hwc2_compat_device_register_callback(HWC2_CALLBACK_HOTPLUG)`, `GetDisplaySupportedModes` → `hwc2_compat_display_get_configs` + `get_config_attribute`, `SetLayerBuffer` → `hwc2_compat_layer_set_buffer`, `PrepareDisplayLayers` → `hwc2_compat_display_validate`, `Commit` → `hwc2_compat_display_accept_changes` + `hwc2_compat_display_present`. Methods that don't apply return `HDF_ERR_NOT_SUPPORT`. `SetLayerBuffer`/`SetDisplayClientBuffer` reconstruct `native_handle_t` from `BufferHandle` and wrap in `ANativeWindowBuffer` for HWC2.

- [x] **Action:** Export `CreateComposerVdi`/`DestroyComposerVdi` factory functions plus ~30 individual `extern "C"` wrappers (same pattern as x86_general).

- [x] **Action:** `BUILD.gn` with `ohos_shared_library("libdisplay_composer_vdi_impl")`, deps: `libhwc2`, `libhardware`; external deps: `display_composer_idl_headers`, `hdf_core:libhdf_utils`, `hilog:libhilog`, `graphic_surface:buffer_handle`, `c_utils:utils`, `ipc:ipc_single`. `install_images = [ chipset_base_dir ]`, `innerapi_tags = [ "passthrough" ]`.

- **Deliverable:** ✓ `libdisplay_composer_vdi_impl.z.so` built and installed to `oniro_soc_products/hybris_generic_soc/`.
- **Status & Notes:** **COMPLETE (2026-03-19).** Build fixes: `bundle.json` deps added; wrong `BlendType` names fixed (`BLEND_SRCOVER`/`BLEND_NONE`); `HWC_TRANSFORM_ROT_0` → integer `0`; added `display_common.h` for callback types; added `using namespace OHOS::HDI::Display::Composer` for `extern "C"` shims; added `ipc:ipc_single`.

### 6.2 Implement Display Buffer VDI (libhybris-gralloc wrapper)

> **Context:** Wraps `libhybris-gralloc` instead of GBM/DRM. Android gralloc returns `buffer_handle_t`; we wrap into OHOS `BufferHandle`.

- [x] **Action:** Create `src/display_gralloc/hybris_buffer_vdi_impl.h/.cpp` implementing `IDisplayBufferVdi`. Key mappings: `AllocMem` → `hybris_gralloc_allocate` → wrap into `BufferHandle`; `FreeMem` → `hybris_gralloc_release`; `Mmap` → `hybris_gralloc_lock`; `Unmap` → `hybris_gralloc_unlock`; `IsSupportedAlloc` → `NOT_SUPPORT` (RenderService falls back).

- [x] **Action:** `BufferHandle` ↔ `buffer_handle_t` conversion: `fd` = `data[0]`, remaining fds in `reserve[0..reserveFds-1]`, ints in `reserve[reserveFds..+numInts-1]`, 64-bit pointer stored in last 2 int slots (`kPtrSlots`) for recovery via `LoadNativeHandle()`. OHOS→Android format/usage mapping tables in `OhosFormatToAndroid()`/`OhosUsageToAndroid()`.

- [x] **Action:** Constructor pre-loads gralloc mapper via `android_dlopen` then calls `hybris_gralloc_initialize(0)`. Exports `CreateDisplayBufferVdi`/`DestroyDisplayBufferVdi`. `BUILD.gn` target with deps: `libgralloc`, `libhardware`, `libhybris-common`; external: `display_buffer_idl_headers`, `buffer_handle`, `libhdf_utils`, `libhilog`, `ipc_single`.

- **Deliverable:** ✓ `libdisplay_buffer_vdi_impl.z.so` built and installed.
- **Status & Notes:** **COMPLETE (2026-03-20).** Fix: added `#include <dlfcn.h>` (musl's, not hybris's) for `RTLD_GLOBAL`.

### 6.3 Register Display VDI Libraries as SOC Component

- [x] **Action:** Update `device/soc/oniro/hybris_generic/BUILD.gn` to include `display_composer_model` and `display_buffer_model` in `hybris_generic_soc_group`.
- [x] **Action:** `bundle.json` already had all required deps. `vendor/oniro/hybris_generic/config.json` confirmed `"drivers_peripheral_display_vdi_default = true"` (our VDI takes priority; default is fallback).

- **Deliverable:** ✓ Both VDI libraries build and install: `libdisplay_composer_vdi_impl.z.so` (57 KB), `libdisplay_buffer_vdi_impl.z.so` (12 KB).
- **Status & Notes:** **COMPLETE (2026-03-20).**

### 6.4 Provide EGL/GLES Impl Libraries (GPU Backend)

> **Context:** `libEGL_impl.so` must resolve to libhybris's EGL, which loads Android `libGLES_mali.so` underneath.

- [x] **Action:** Added `symlink_target_name` to existing libhybris build targets: `libEGL` → `libEGL_impl.so`; `libGLESv1_CM` → `libGLESv1_impl.so`; `libGLESv2` → `libGLESv2_impl.so` + `libGLESv3_impl.so`. Created `device/soc/oniro/hybris_generic/hardware/gpu/BUILD.gn` group pulling in the three libs; added to `hybris_generic_soc_group`.

- [x] **Action:** Created `device/board/oniro/hybris_generic/cfg/hybris_graphic_env.cfg` — init service env override for `render_service` setting `LIBEGL`, `LIBGLESV2`, `HYBRIS_LD_LIBRARY_PATH`, `LD_LIBRARY_PATH`. Installed via `ohos_prebuilt_etc` to `/system/etc/init/`. Init loads all `*.cfg` from `etc/init/` at boot; service env arrays in later files update (not replace) existing definitions.

- **Deliverable:** ✓ EGL/GLES symlinks registered; `hybris_graphic_env.cfg` installs and sets Mali GPU path env vars on `render_service`.
- **Status & Notes:** **COMPLETE (2026-03-20).** `hybris_default_egl_platform` stays `"hwcomposer"` so `test_hwcomposer` is unaffected; `render_service` uses `HYBRIS_EGLPLATFORM=ohos` (set in 6.5).

### 6.5 EGL Platform Adaptation for RenderService

> **Context:** RenderService uses `eglCreateWindowSurface` with an OHOS `OHNativeWindow`, not a `HWComposerNativeWindow`. The `ohos` EGL platform bridges this.

- [x] **Action:** Chose **Option B** — `HYBRIS_EGLPLATFORM=ohos` for `render_service`. Added to `hybris_graphic_env.cfg`. Compiled default stays `"hwcomposer"` for `test_hwcomposer`.

- [x] **Action:** Fixed critical buffer type mismatch in `ohos_window.cpp/h`: the original `dequeueBuffer()` cast `OHNativeWindowBuffer*` directly to `BaseNativeWindowBuffer*` (undefined behavior — completely different class layouts). **Fix:** Added `OhosNativeWindowBuffer : public BaseNativeWindowBuffer` that calls `GetBufferHandleFromNative(ohBuffer)` → `LoadNativeHandleFromBH(bh)` (recovering the `native_handle_t*` from `BufferHandle.reserve[-2..-1]`) and sets `ANativeWindowBuffer::{handle, width, height, stride, format, usage}`. Maintains a `std::map<OHNativeWindowBuffer*, OhosNativeWindowBuffer*> m_bufferMap` cache; wrappers are created once per buffer slot and reused.

- **Deliverable:** ✓ `libeglplatform_ohos.z.so` builds with all key symbols verified. `render_service` loads ohos EGL platform at boot.
- **Status & Notes:** **COMPLETE (2026-03-20).** Buffers must be allocated via our buffer VDI (6.2) for `LoadNativeHandleFromBH` to find the stored pointer — guaranteed by standard OHOS flow.

### 6.6 Ensure Android Composer HIDL Service is Running

- [x] **Action:** `device/board/oniro/hybris_generic/utils/start-ohos.sh` checks if `android.hardware.graphics.composer@2.1-service` is running in the android LXC container (via `lxc-attach pgrep`), starts it if not, waits 2s for hwservicemanager registration, then starts the openharmony container. Guard makes restarts idempotent.

- **Deliverable:** ✓ Android HWC2 HIDL service reliably available when OHOS display composer starts.
- **Status & Notes:** **COMPLETE (2026-03-20).**

### 6.7 Pre-load Gralloc Mapper in Composer Host

- [x] **Action:** Hooks `_hybris_hook_android_load_sphal_library` / `_hybris_hook_android_dlopen_ext` (in `hooks.c`, registered via `HOOK_INDIRECT`) are active for all libhybris users including `composer_host`. `PreloadGrallocMapper()` (called from VDI constructor) tries `android.hardware.graphics.mapper@4.0-impl-mediatek.so` then `-impl.so`. `/vendor/lib64/hw` bind mount confirmed in `device/board/oniro/hybris_generic/utils/lxc/config`.

- **Deliverable:** Gralloc mapper loads without `FATAL EXCEPTION`.
- **Status & Notes:** **COMPLETE (2026-03-20).** All items were already implemented in Phase 5.10 / 6.1.

### 6.8 Build and Smoke-Test the VDI Libraries

- [x] **Action:** Full product build succeeded (2026-03-20, ~5m18s). Deploy rootfs via `deploy-lxc-container.sh`.
- [x] **Action:** Verify `composer_host` loads `libdisplay_composer_vdi_impl.z.so` and `allocator_host` loads `libdisplay_buffer_vdi_impl.z.so` — confirmed via `/proc/<pid>/maps`.
- [x] **Action:** Confirm `RegHotPlugCallback` fires and reports display 0 (720×1560). Confirmed indirectly: Android HWC2 shows `Register hotplug callback` with no `DEAD_OBJECT`; `render_service` fully initializes (28 threads, in `do_epoll_wait`).

- **Deliverable:** Both VDI libraries load; basic display enumeration works.
- **Status & Notes:** **COMPLETE (2026-03-21).** A deadlock was found and fixed (6.8.3).

---

### 6.8.1 Fix: Binder Context Collision (Android servicemanager vs OHOS samgr)

> **Root cause:** `/dev/binder` is a symlink to `/dev/binderfs/binder`. Android's `servicemanager` (PID ~2117) registers as context manager (handle 0) first. OHOS `samgr` gets `EBUSY` and Android servicemanager remains at handle 0 — it returns `UNKNOWN_TRANSACTION = -EBADMSG = -74` for all OHOS IPC.

**Fix (2026-03-20):** Create dedicated binder devices for the OHOS container via `BINDER_CTL_ADD` ioctl (`BINDER_CTL_ADD = 0xC1086201`, struct size 264 bytes — use `__u32` not `__u8` for major/minor). Creates `ohos-binder`, `ohos-hwbinder`, `ohos-vndbinder`. Script at `/home/phablet/openharmony/create_ohos_binder.py`.

Update LXC config to bind-mount the new devices:
```
lxc.mount.entry = /dev/binderfs/ohos-binder dev/binder bind bind,create=file,optional 0 0
lxc.mount.entry = /dev/binderfs/ohos-hwbinder dev/hwbinder bind bind,create=file,optional 0 0
lxc.mount.entry = /dev/binderfs/ohos-vndbinder dev/vndbinder bind bind,create=file,optional 0 0
```
Updated in both `/var/lib/lxc/openharmony/config` and `device/board/oniro/hybris_generic/utils/lxc/config`.

**⚠️ Persistence:** Devices are lost on reboot. A systemd service `ohos-binder-setup.service` (`Before=ohos.service`, `After=dev-binderfs.mount`) runs the script at boot. Script stored at `/home/phablet/openharmony/create_ohos_binder.py`; pushed by `deploy-lxc-container.sh`.

- **Status & Notes:** **COMPLETE (2026-03-20).** After fix, error changes from `-74` to `ERR_PERMISSION_DENIED` from OHOS's own samgr.

---

### 6.8.2 Fix: samgr CanRequest() & SELinux — PERMISSION DENIED for all callers

**Problem 1 — SELinux:** All `Check*Permission` functions call `selinux_check_access`. OHOS process labels not in Ubuntu Touch SELinux policy → all checks fail.
- **Fix:** Set `"build_selinux": false` in `vendor/oniro/hybris_generic/config.json`. All permission check functions compile to `return true` via `#else` branch. The `InContainerMode()` bypass added earlier was reverted — superseded by this flag.

**Problem 2 — AccessToken / CanRequest():** `GetCallingTokenID()` returns the caller's UID (not a proper OHOS token) when called via hybris binder. `GetTokenTypeFlag` returns `TOKEN_HAP` instead of `TOKEN_NATIVE`.
- **Fix:** Patched `CanRequest()` in `system_ability_manager_stub.cpp`: (1) if `OHOS_RUNTIME_CONFIG=1`, return `true` immediately; (2) if `tokenType != TOKEN_NATIVE` AND `tid == (uint32_t)callerUid`, allow UIDs 0 and 1000.

- **Status & Notes:** **COMPLETE (2026-03-20).** Services `foundation`, `accountmgr`, `resource_schedule_service` now reach registration phase.

### 6.9 Full Product Build and RenderService Bring-up

- [x] **Action:** Full product build (2026-03-20). Deploy rootfs. Boot container, audit hilog — three blockers identified and fixed.
- [x] **Action:** Rebuild with all 2026-03-20 fixes, deploy, verify `composer_host` + `render_service` startup.
- **Status & Notes:** **COMPLETE (2026-03-21). All three blockers fixed. Both services running stably.**

  #### Blocker A — Android EGL bind mounts poisoning OHOS library namespace (FIXED)

  **Root cause:** Phase 5.7a placed Android's `libEGL.so`, `libGLESv1_CM.so`, `libGLESv2.so` in `/system/lib64/` inside the container. OHOS musl linker searches `/system/lib64/` first, so every OHOS service that links `libEGL.so` found Android's blob (which needs `libbacktrace.so`) instead of the OHOS opengl_wrapper at `platformsdk/libEGL.so`. **Note:** "OHOS uses .z.so suffix so these do not conflict" was wrong — OHOS services link `libEGL.so` (no `.z.`), not `libEGL.z.so`.

  **Fix:** Removed the three Android EGL bind mounts from `device/board/oniro/hybris_generic/utils/lxc/config` and the live config. Removed placeholder files from rootfs. Also removed the `libcutils.so` bind mount. Production EGL flow: `render_service` → `libEGL.so` (OHOS opengl_wrapper at `platformsdk/`) → `libEGL_impl.so` (symlink → hybris `libEGL.z.so`) → `libGLES_mali.so` (hybris Android linker remaps paths internally).

  #### Blocker B — HDF DevSvc Manager SELinux permission checks (FIXED via build flag)

  **Root cause:** `devsvc_manager_stub.c` compiles in `#ifdef WITH_SELINUX` checks; `GetServicePermCheck` → `selinux_check_access` fails in container. **Fix:** `"build_selinux": false` in `vendor/oniro/hybris_generic/config.json` — same root cause as 6.8.2, fully resolved by the same flag.

  #### Blocker C — `composer_host` SIGABRT: recursive static initialization (FIXED)

  **Root cause:** `RegHotPlugCallback` → `GetVdiInstance()` → static `HybrisComposerVdiImpl` construction → `InitHwc2Device()` → `hwc2_compat_device_register_callback()` → Android HIDL fires `onHotplug` **synchronously** → `OnHotplug` calls `GetVdiInstance()` again → C++ static-local guard still locked → abort.

  **Fix (2026-03-21):** Defer `hwc2_compat_device_register_callback()` and `hwc2_compat_device_on_hotplug()` out of the constructor path (`InitHwc2Device`) and into `RegHotPlugCallback()`, which is called only after the singleton is fully constructed. Added `bool hwc2CallbackRegistered_{false}` guard for one-time execution.

---

### 6.8.3 Fix: RegHotPlugCallback Mutex Deadlock

- [x] **Action:** Fix deadlock in `HybrisComposerVdiImpl::RegHotPlugCallback`.

> **Root cause:** `RegHotPlugCallback` held `mutex_` while calling `hwc2_compat_device_register_callback()`. Android HWC2 fires `onHotplug` **synchronously** on the same thread inside `registerCallback`. `OnHotplug` → `HandleHotplug` both try to acquire `mutex_` → deadlock (`std::mutex` is non-recursive). Result: `composer_host` stuck in `futex_wait_queue_me`; `render_service` initialization stalls; all clients get "RenderService connect fail".
>
> **Fix (2026-03-21):** In `RegHotPlugCallback`, set `hotplugCb_`/`hotplugData_`/`hwc2CallbackRegistered_` inside the lock, then **release the lock** before calling `hwc2_compat_device_register_callback()` and `hwc2_compat_device_on_hotplug()`. Re-acquire only for the "re-fire" loop at the end. File: `hybris_composer_vdi_impl.cpp`.
>
> **Result (2026-03-21):** `render_service` fully initializes (28 threads, in `do_epoll_wait`). Clients connect successfully. Hotplug callback delivered without `DEAD_OBJECT`.

### 6.10 System Service Audit, IPC Security Fix, and Cross-Process Gralloc Fix

> **Context:** For a graphical UI, several services must be running in the correct order.

#### Boot chain status (as of 2026-03-23)

| # | Service | Role | Status |
|---|---------|------|--------|
| 1 | `samgr` | System Ability Manager | ✅ Running — SELinux disabled (6.8.2) |
| 2 | `param_watcher` | System parameter service | ✅ Running |
| 3 | `hilogd` | Logging daemon | ✅ Running (fixed Phase 3.1) |
| 4 | `hdf_devmgr` | HDF device manager | ✅ Running — loads `composer_host` + `allocator_host` |
| 5 | `allocator_host` | Display buffer HDI service | ✅ Running — our buffer VDI loaded |
| 6 | `composer_host` | Display composer HDI service | ✅ Running — recursive-init + mutex deadlock fixed (6.8.3) |
| 7 | `render_service` | Graphics rendering | ✅ Running — stable, `bootevent.renderservice.ready=true` (confirmed 2026-03-23) |
| 8 | `foundation` | Ability Manager Service (AMS) | 🔲 Not yet verified |
| 9 | `appspawn` / `nwebspawn` | Application spawning | 🔲 Not yet verified |
| 10 | `bootanimation` | Boot animation | ✅ Plays 150 frames at 30fps on physical display — all three root causes fixed (6.11) |
| 11 | `launcher` | Home screen | ✅ Starts, reaches `onPageShow`, lockscreen visible (2026-03-27) |

#### Fix D — IPC Security: `GetTokenType()` UID fallback for hybris binder callers (FIXED 2026-03-23)

**Root cause:** `IPCSkeleton::GetCallingTokenID()` returns the caller's UID (not an OHOS token ID) when calls cross Android/OHOS binder via hybris. `AccessTokenKit::GetTokenTypeFlag()` returns `TOKEN_INVALID` for small UID values, denying all IPC security checks.

**Fix:** Added UID-based fallback in `GetTokenType()` (`rs_ipc_interface_code_access_verifier_base.cpp`): if `tokenId < 10000` (system UID range), return `TOKEN_NATIVE` directly. Real OHOS token IDs have type bits at bits 27–28, giving values ≥ `0x08000000` — unambiguously distinguishable from raw UIDs.

#### Fix E — Cross-process gralloc `Mmap`: `hybris_gralloc_import_buffer` (FIXED 2026-03-23)

**Root cause:** `AllocMem()` runs in `composer_host`. The raw `buffer_handle_t` pointer stored in `BufferHandle::reserve[-2..-1]` is invalid in `render_service`'s address space. `Mmap()` passes this stale pointer to `hybris_gralloc_lock()` → `EINVAL (2)`. Log: `Mmap: hybris_gralloc_lock failed ret=2` → `SwapBuffers: Failed ... error is 300d`.

**Fix:** Added `ReconstructNativeHandle()` in `hybris_buffer_vdi_impl.cpp` that rebuilds `native_handle_t` from portable fds and ints (excluding the stale pointer slots). Modified `Mmap()` to: (1) try stored pointer first (same-process path); (2) on failure, reconstruct the handle and call `hybris_gralloc_import_buffer(rawNh, &importedHandle)` to register it in the current process; (3) store the imported handle back. Modified `Unmap()` to call `hybris_gralloc_release(nativeHandle, 0)` (imported, not allocated) and clear the pointer.

**Build:** `--fast-rebuild --build-target libdisplay_buffer_vdi_impl`. Deploy `oniro_soc_products/hybris_generic_soc/libdisplay_buffer_vdi_impl.z.so` to `/vendor/lib64/passthrough/` on device.

**Verification:** No more `hybris_gralloc_lock failed`; no more `SwapBuffers Failed`; `bootevent.bootanimation.ready=true`; all 150 bootanimation frames rendered at 30 fps; Mali GPU threads present. Display stays physically black — see Phase 6.11.

- **Status & Notes:** **Partially complete (2026-03-23).** Services 1–7 and bootanimation rendering confirmed working. `GetDefaultScreenId()` fixed in 6.11 — bootanimation now attaches to screen 0. Physical display output unconfirmed.

---

### 6.11: BootAnimation

**Actions:**
- Diagnose and fix `GetDefaultScreenId()` returning `INVALID_SCREEN_ID` preventing bootanimation from attaching to display
- Deploy picture-mode boot config to avoid audio HAL failure in LXC container
- Fix hardware vsync delivery from Android HWC2
- Fix CLIENT composition deadlock preventing frame advancement
- Verify all 150 frames play at 30 fps on physical Volla X23 display

**Deliverable:** ✓ Boot animation plays all 150 frames on physical display at 30 fps. Launcher and lockscreen visible. `bootevent.bootanimation.started=true` and `bootevent.bootanimation.ready=true` confirmed.
**Status:** **COMPLETE (2026-03-27).**

---

#### Fix A: GetDefaultScreenId() async hotplug race

**Symptoms:**
- `BootCompatibleDisplayStrategy::Display()` receives `screenId = 18446744073709551615` (`UINT64_MAX` = `INVALID_SCREEN_ID`)
- Bootanimation never attaches to any display; no visual output

**Root cause:**
Race between `RSScreenManager::Init()` and the async HIDL hotplug callback:
1. `Init()` calls `ProcessScreenHotPlugEvents()` before the HIDL hotplug (delivered on a binder thread from `composer_host`) arrives
2. `ProcessScreenHotPlugEvents()` finds an empty queue and unconditionally sets `mipiCheckInFirstHotPlugEvent_ = true`
3. When hotplug later fires, `ProcessScreenConnected()` runs but `mipiCheckInFirstHotPlugEvent_` is already `true`, so the MIPI check condition `if (!mipiCheckInFirstHotPlugEvent_.exchange(true))` is false and `defaultScreenId_` is never updated from `INVALID_SCREEN_ID`
4. Screen 0 IS in `screens_` (query works) but `defaultScreenId_` stays `INVALID`

**Fix:**
- **RSScreenManager fallback** (`rs_screen_manager.cpp` — `ProcessScreenConnected()`): added fallback after MIPI if-else block — if `defaultScreenId == INVALID_SCREEN_ID` after the MIPI check, claim the incoming screen unconditionally
- **Bootanimation polling guard** (`boot_compatible_display_strategy.cpp`): added `WaitDefaultScreenId()` helper that polls `GetDefaultScreenId()` up to 100×30ms = 3 seconds before attaching the surface

**Verification:**
```
BootAnimation: WaitDefaultScreenId: got screenId=0 after 0 retries
BootAnimation: Init enter, width: 720, height: 1560, screenId : 0
OHOS::RS: RSSurfaceNode:attach to display, node:[...], screen id: 0
```

**Files changed:**
- `foundation/graphic/graphic_2d/rosen/modules/render_service/core/screen_manager/rs_screen_manager.cpp`
- `foundation/graphic/graphic_2d/frameworks/bootanimation/src/boot_compatible_display_strategy.cpp`

---

#### Fix B: Boot animation exits immediately — video player audio HAL failure

**Symptoms:**
- Display shows one frame then freezes, or stays black
- `PlayerListenerProxy: player callback onError, errorCode: 331350552, errorMsg: AUD_OUTPUT_ERR-null-unsupport interface, audio render failed`
- `StopBootAnimation` called ~125 ms after start

**Root cause:**
No custom config file (`etc/bootanimation/bootanimation_custom_config.json`) causes `CreateDefaultBootConfig()` to leave both `videoDefaultPath` and `picZipPath` empty. `IsBootVideoEnabled()` returns `true` when both are empty, so the video player path is taken. The video player sets up audio rendering (`player.audiosink` → `AudioPolicyService`); the LXC container has no audio HAL. Audio sink `Start()` fails, triggering `VideoPlayerCallback::OnError` → `boot->StopVideo()` → `StopBootAnimation()`. The entire animation exits, leaving whatever frame was last presented frozen on-screen.

**Fix:**
Deploy `bootanimation_custom_config.json` that sets `picZipPath` to `/system/etc/graphic/bootpic.zip` and leaves `videoDefaultPath` empty. `IsBootVideoEnabled()` then returns `false`, taking the picture-frame path. Both `bootpic.zip` (1.3 MB) and `bootvideo.mp4` (144 KB) already exist in the rootfs.

**File:** `vendor/oniro/hybris_generic/custom_conf/bootanimation/bootanimation_custom_config.json` (installed via `ohos_prebuilt_etc` → `/vendor/etc/bootanimation/bootanimation_custom_config.json`, found by `GetOneCfgFile`)

**Verification:**
```
I BootAnimation: video path is empty and picture path is not empty
I BootAnimation: boot animation play sequence frames
I BootAnimation: read freq: 30, pic num: 150
I BootAnimation: SetVSyncRate success: 30, 2
bootevent.bootanimation.started = true
```

---

#### Fix C: Hardware vsync never delivered — OnVsync pointer comparison bug

**Symptoms:**
- Display stays on first frame even after Fix B; frame rendering appears correct in logs but display never updates
- Memory leak of one `hwc2_compat_display_t` per vsync tick (~60 leaks/sec)

**Root cause:**
In `HybrisComposerVdiImpl::OnVsync`, the display match was:
```cpp
if (kv.second->GetHwc2Display() ==
    hwc2_compat_device_get_display_by_id(inst.device_, display))
```
`hwc2_compat_device_get_display_by_id` does a `malloc` and returns a **new** `hwc2_compat_display_t*` on every call. The stored `GetHwc2Display()` pointer is from the original `malloc` in `HandleHotplug` — the two addresses never match. All vsync signals are silently discarded. The system fell back to OHOS `RSVSyncGenerator`'s software vsync, which fires at the configured period but is not synchronized to the display refresh.

**Fix:**
Store `hwc2DisplayId_` (type `hwc2_display_t`) in `HybrisDisplay` at construction time. Compare IDs directly in `OnVsync` — no allocation, no leak:
```cpp
if (kv.second->GetHwc2DisplayId() == display)
```
Disconnect path in `HandleHotplug` also updated to compare by ID.

**Files changed:**
- `device/soc/oniro/hybris_generic/hardware/display/src/display_composer/hybris_display.h` — add `hwc2DisplayId_` member + `GetHwc2DisplayId()` accessor
- `device/soc/oniro/hybris_generic/hardware/display/src/display_composer/hybris_display.cpp` — init `hwc2DisplayId_` from `hwc2Id` in constructor; update disconnect path
- `device/soc/oniro/hybris_generic/hardware/display/src/display_composer/hybris_composer_vdi_impl.cpp` — fix `OnVsync` comparison

---

#### Fix D: needsClientComposition_ sticky deadlock — display frozen on first frame

**Symptoms:**
- Display shows only the first frame of the boot animation and then freezes
- `E C01400/Composer: GetFramebuffer: GetFramebuffer, no availableBuffers` at exactly 1-second intervals

**Root cause:**
`needsClientComposition_` is initialized to `true` in `HybrisDisplay` and is sticky:
```cpp
needFlushFb = (numTypes > 0) || needsClientComposition_;  // always true
needsClientComposition_ = needFlushFb;                    // sticky: once true, stays true
```
This forces all layers to `COMPOSITION_CLIENT` every frame, creating a circular deadlock:
1. `HdiOutput::Repaint()` → `GetFramebuffer()` waits up to 1000ms for a GPU-composited buffer
2. RSRenderThread only produces a buffer when `eglSwapBuffers` succeeds
3. `eglSwapBuffers` blocks because render_service never consumed the previous buffer (stuck in `GetFramebuffer`)
4. `GetFramebuffer` times out, frame is skipped — cycle repeats

The first frame is visible because the buffer queue is empty at startup, so `eglSwapBuffers` succeeds exactly once. After that the deadlock locks in permanently.

**Fix:**
Initialize `needsClientComposition_` to `false`. Base `needFlushFb` solely on what HWC2 `validate` reports:
```cpp
// hybris_display.h:
bool needsClientComposition_{false};

// hybris_display.cpp PrepareDisplayLayers:
needFlushFb = (numTypes > 0);
needsClientComposition_ = needFlushFb;
```
When HWC2 can scan out the bootanimation layer as DEVICE (which it does), `numTypes = 0`, `needFlushFb = false`, CLIENT composition path is skipped entirely, and `Commit` is called directly.

**Verification:** After deployment, exactly ONE `GetFramebuffer: no availableBuffers` at startup (normal first-frame race), followed by `Commit` calls at ~32ms intervals (30fps) sustained for the full animation duration. Boot animation plays all 150 frames on physical display. Launcher and lockscreen then appear.

**Files changed:**
- `device/soc/oniro/hybris_generic/hardware/display/src/display_composer/hybris_display.h`
- `device/soc/oniro/hybris_generic/hardware/display/src/display_composer/hybris_display.cpp`

---

> **Note — vsync "not processed in time" warning:** After `play sequence frames end`, the VSyncReceiver connection stays open. render_service keeps sending vsync signals at 30 fps that bootanimation doesn't consume. Log: `PostEvent: vsync signal is not processed in time, ... ret:24`. This is benign — `ret:24` = 24 bytes written = 3 × int64_t = success (socket buffer full, one packet drained and retried). Once the launcher votes the boot event and `persist.window.boot.inited` is set, `CheckExitAnimation()` exits the loop and bootanimation terminates cleanly.

---

### 6.12: RenderService EGL Initialization

**Actions:**
- Diagnose persistent `EGL_BAD_SURFACE (0x300d)` reported by `render_context_gl.cpp::SwapBuffers`
- Fix `libEGL_impl.so` loading failure in `OpenGLWrapper`
- Fix SIGSEGV in `ohosws_DestroyWindow` after `eglSwapBuffers` returns `EGL_BAD_SURFACE`
- Add `EGL_PLATFORM_OHOS_KHR` support to libhybris

**Deliverable:** ✓ RenderService EGL initializes successfully. `EGL_BAD_SURFACE` loop identified as benign and does not affect hardware composition.
**Status:** **COMPLETE (2026-03-25).**

---

#### Fix E: libEGL_impl.so loading failure in OpenGLWrapper

**Symptoms:**
- `OpenGLWrapper` fails to find `libEGL_impl.so`; EGL initialization fails for render_service

**Root cause:**
`OpenGLWrapper` has hardcoded search paths looking in `/vendor/lib64/`. The hybris EGL libraries are installed to `/system/lib64/` and `/system/lib64/libhybris/`.

**Fix:**
Created direct symlinks in `/vendor/lib64/` pointing to `/system/lib64/libEGL_impl.so`, `libGLESv1_impl.so`, `libGLESv2_impl.so`, `libGLESv3_impl.so` to satisfy `OpenGLWrapper`'s hardcoded search paths.

---

#### Fix F: OhosNativeWindow vtable offset SIGSEGV in ohosws_DestroyWindow

**Symptoms:**
- render_service SIGSEGV at `pc=0x0` in `ohosws_DestroyWindow` after `eglSwapBuffers` returns `EGL_BAD_SURFACE`

**Root cause:**
Incorrect pointer cast from `EGLNativeWindowType` to `OhosNativeWindow*` using `reinterpret_cast`. `OhosNativeWindow` inherits from `ANativeWindow` which inherits from `BaseNativeWindowBuffer`; the vtable pointer occupies the first 8 bytes. `reinterpret_cast` produced a pointer offset by 8 bytes, causing a NULL pointer dereference when calling `decRef`.

**Fix:**
Changed pointer cast to `static_cast<OhosNativeWindow*>(static_cast<ANativeWindow*>(win))`. This correctly accounts for the vtable offset via the C++ type system.

---

#### Fix G: EGL_PLATFORM_OHOS_KHR not recognized in libhybris eglGetPlatformDisplay

**Symptoms:**
- `RenderContextGL::Init()` cannot create an EGL display using `EGL_PLATFORM_OHOS_KHR` (0x34E0)
- Falls back to `eglGetDisplay(EGL_DEFAULT_DISPLAY)`, causing initialization warnings and potential mismatches

**Root cause:**
libhybris `egl.c` did not recognize the `EGL_PLATFORM_OHOS_KHR` constant in `eglGetPlatformDisplay`, returning `EGL_NO_DISPLAY` for this platform type.

**Fix:**
Explicitly added `EGL_PLATFORM_OHOS_KHR` (0x34E0) to `eglGetPlatformDisplay` in `third_party/libhybris/hybris/egl/egl.c`.

---

> **Note on `EGL_BAD_SURFACE` loop:** The `EGL_BAD_SURFACE` error from render_service's `SwapBuffers` is **benign**. render_service re-creates `RSSurfaceOhosGl` each frame during `RSPhysicalScreenProcessor::Redraw`. If `DrawLayers` has no actual content (all layers handled by hardware composition), Skia emits no GL draw commands. Mali EGL lazily defers `dequeueBuffer` until the first GL draw call — so `eglSwapBuffers` on a surface with no dequeued buffer returns `EGL_BAD_SURFACE` (Mali is strict, unlike Mesa). render_service handles this gracefully and continues. Hardware composition is unaffected.

---

### 6.13: HWC2 Buffer Handle Marshalling

**Actions:**
- Fix malformed `native_handle_t` in `composer_host` causing HWC2 to reject all layers (physical screen black)

**Deliverable:** ✓ Physical screen is no longer black. First frame of the boot animation visible on the Volla X23 display.
**Status:** **COMPLETE (2026-03-25).**

---

#### Fix H: native_handle_t size mismatch — kPtrSlots extra ints rejected by Gralloc mapper

**Symptoms:**
- Physical display stays black despite `test_hwcomposer` working in Phase 5
- HWC2 HIDL service silently rejects all layer buffer handles

**Root cause:**
`BufferHandle.reserveInts` includes 2 extra ints (`kPtrSlots`) added by our buffer VDI to store a raw `native_handle_t*` pointer for same-process recovery. `BuildNativeBuffer` was passing these extra ints to the Android HWC2 HIDL service. The Gralloc mapper validates `numInts` strictly and rejects any handle with an unexpected size, silently discarding all layers.

**Fix:**
Excluded the 2 `kPtrSlots` ints when reconstructing the `native_handle_t` passed to HWC2:
```cpp
int numInts = bh.reserveInts - kPtrSlots;
```

**File:** `device/soc/oniro/hybris_generic/hardware/display/src/display_composer/hybris_composer_vdi_impl.cpp`

---

#### Fix I: Double FD close invalidates original BufferHandle FDs

**Symptoms:**
- After first frame renders, subsequent frames fail with FD-related errors in Commit calls

**Root cause:**
`native_handle_close(nh)` was called on the temporary `native_handle_t` reconstructed from `BufferHandle` FDs. `native_handle_close` executes `close()` on all FDs in the handle, immediately invalidating the original FDs still owned by the OHOS `BufferHandle` in `composer_host`.

**Fix:**
Replaced `native_handle_close(nh)` + `native_handle_delete(nh)` with only `native_handle_delete(nh)`. The FDs are owned by the `BufferHandle`; the temporary `native_handle_t` wrapper must not close them.

**File:** `device/soc/oniro/hybris_generic/hardware/display/src/display_composer/hybris_composer_vdi_impl.cpp`

---

### 6.14: AMS & Launcher Bring-up

**Actions:**
- Create BMS preinstall-config so all 47 HAPs install on first boot
- Fix ATM token initialization failures blocking HAP installation
- Fix launcher RSRenderThread EGL crash preventing UI initialization
- Unblock app spawning (seccomp workaround)
- Fix LIFECYCLE_TIMEOUT: launcher ServiceExtension fails to load
- Ensure launcher renders and votes the boot event to exit bootanimation

**Deliverable:** ✓ All 47 HAPs install on first boot. Launcher starts, renders UI, votes the boot event. Lockscreen visible on physical display. Boot animation exits cleanly.
**Status:** **COMPLETE (2026-03-27).** Remaining open items: seccomp workaround is not a permanent solution; `base/startup/appspawn/appspawn.cfg` needs source-tree update with hybris env vars.

---

#### Fix J: HAP pre-installation fails — BMS preinstall-config missing

**Symptoms:**
- No apps install on first boot; launcher never starts
- BMS `installList_` stays empty; no log entries for any HAP installation

**Root cause:**
`USE_PRE_BUNDLE_PROFILE` is defined (`foundation/bundlemanager/bundle_framework/services/bundlemgr/BUILD.gn:213`). In this mode, `OnBundleBootStart()` calls `LoadPreInstallProFile()` which scans `/system/etc/app/`. If `install_list.json` is absent, `installList_` stays empty — no apps install. The directory-scan fallback (`ProcessBootBundleInstallFromScan`) is inside the `#else` branch and is never reached.

**Fix:**
Created the full preinstall-config infrastructure under `vendor/oniro/hybris_generic/preinstall-config/`:
- `install_list.json` — lists all 47 HAPs in `/system/app/`; `ohos.global.systemres` placed first (required dependency for all other apps); includes `com.ohos.launcher`, `com.ohos.systemui`, `com.ohos.settingsdata`, etc.
- `install_list_capability.json` — per-bundle privilege settings (singleton, keepAlive, ACL signatures); adapted from `vendor/oniro/x23` with corrected bundle names
- `install_list_permissions.json` — per-bundle pre-granted permissions; copied from x23, bundle names updated
- `uninstall_list.json` — empty (`{"uninstall_list": [], "recover_list": []}`)
- `BUILD.gn` — four `ohos_prebuilt_etc` targets deploying JSON files to `relative_install_dir = "app"` → `/system/etc/app/`

Added `"//vendor/oniro/hybris_generic/preinstall-config:preinstall-config"` to `vendor/oniro/hybris_generic/bundle.json`.

> **Note:** BMS database must be cleared (`rm /data/service/el1/public/bms/bundle_manager_service/bmsdb*.db*`) before each test boot to force a fresh first-boot HAP installation sequence.

---

#### Fix K: InitHapToken errCode:201 — ATM token initialization failures

**Symptoms:**
- Every HAP installation fails at `BundlePermissionMgr::InitHapToken()` with `errCode:201` (`ERR_PERMISSION_DENIED`)
- All 47 apps fail to install despite Fix J

**Root cause:**
Three independent failures prevent ATM from recognizing the calling process as `foundation` with `MANAGE_HAP_TOKENID` permission:

1. **`/dev/access_token_id` missing from container:** `GetSelfTokenID()` reads from `/dev/access_token_id`. With `lxc.autodev = 1`, the container creates a fresh `/dev/` at boot and this OHOS-specific char device (`cr--r--r-- 10,126`) is not auto-created. Returns 0 instead of the real token ID.

2. **`InContainerMode()` guard skips `SetSelfTokenID()`:** `SetAccessToken()` in `base/startup/init/services/init/standard/init_service.c` was guarded by `InContainerMode()`. Since `lxc.environment = container=lxc` is set, `InContainerMode()` returns true for every service, silently skipping `SetSelfTokenID(service->tokenId)` for every process. The binder driver therefore returns the caller's UID as the token ID for all binder transactions.

3. **`/dev/access_token_id` not world-readable:** The host device was `crw------- root root`. After `SetPerms()` drops foundation to UID 5523, every binder transaction's `GetSelfTokenID()` open fails silently and the binder driver falls back to UID 5523. ATM has no entry for UID 5523 → `PERMISSION_DENIED`.

**Fix:**
- **LXC bind mount** added to `device/board/oniro/hybris_generic/utils/lxc/config` and live `/var/lib/lxc/openharmony/config`:
  ```
  lxc.mount.entry = /dev/access_token_id dev/access_token_id bind bind,create=file,optional 0 0
  ```
- **Removed `InContainerMode()` guard** from `SetAccessToken()` in `base/startup/init/services/init/standard/init_service.c`. The guard was intended for containers hosting other containers, not for OHOS running inside LXC. Built with `--build-target init`, deployed to `/system/bin/init`.
- **udev rule** for persistent world-readable permissions on host:
  ```bash
  echo 'SUBSYSTEM=="misc", KERNEL=="access_token_id", MODE="0666"' > /etc/udev/rules.d/60-access-token.rules
  udevadm control --reload-rules && udevadm trigger --subsystem-match=misc
  ```

**Result:** Foundation token correctly reads as `672037098` in all binder transactions. `InitHapToken` returns 0 for every HAP. All 47 apps install successfully. Launcher `LauncherMainAbility --> onCreate start` appears in logs.

---

#### Fix L: Launcher RSRenderThread SIGSEGV — eglGetError NULL dereference

**Symptoms:**
- After launcher starts, `RSRenderThread` crashes with SIGSEGV at `pc=0x0` in `libEGL.z.so`
- Call chain: `RenderContextGL::Init()` → `GetPlatformEglDisplay()` → `eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS)` → `eglGetError()` → `HYBRIS_DLSYSM(egl, &_eglGetError, "eglGetError")` → `_eglGetError` is NULL → crash

**Root cause:**
`HYBRIS_DLSYSM` lazily loads Android EGL symbols from `libGLES_mali.so`. It returns NULL if `_libhybris_androidegl` (the `dlopen` handle to Android EGL) is not yet initialized. App processes spawned by `appspawn` did not inherit hybris env vars (`LIBEGL`, `HYBRIS_LD_LIBRARY_PATH`), so `_init_androidegl()` failed silently and `_eglGetError` was never set.

**Fix (two parts):**

1. **Add hybris env vars to `appspawn.cfg`** so all spawned app processes inherit them. Applied device-side to `/home/phablet/openharmony/rootfs/system/etc/init/appspawn.cfg`:
   ```json
   "env": [
       {"name": "LIBEGL",                 "value": "/android/vendor/lib64/egl/libGLES_mali.so"},
       {"name": "LIBGLESV2",              "value": "/android/vendor/lib64/egl/libGLES_mali.so"},
       {"name": "HYBRIS_LD_LIBRARY_PATH", "value": "/android/vendor/lib64:/android/system/lib64"},
       {"name": "LD_LIBRARY_PATH",        "value": "/system/lib64/libhybris:/system/lib64"},
       {"name": "HYBRIS_EGLPLATFORM",     "value": "ohos"}
   ]
   ```
   Source tree file to update: `base/startup/appspawn/appspawn.cfg`.

2. **Defensive NULL check in libhybris `eglGetError`** (`third_party/libhybris/hybris/egl/egl.c`): if `_eglGetError` is NULL (Android EGL not yet initialized), return only the hybris internal error code without crashing. Also added diagnostic HiLog logging to `_init_androidegl()` including `android_dlerror()` on failure. Build with `--build-target libEGL`, deploy `libEGL.z.so` to both `/system/lib64/` and `/system/lib64/libhybris/`.

---

#### Fix M: App spawning fails — seccomp filter loading error (workaround)

**Symptoms:**
- App spawning fails with `result:-22` / `get filter name failed` from `seccomp_policy.c`
- No apps can be spawned; launcher never starts

**Root cause:**
An `/apex` sandbox experiment left stale state in `/mnt/sandbox/`. `GetCfgFiles` finds 0 paths for `libapp_filter.z.so` after `pivot_root` into the app sandbox — the seccomp policy file is inaccessible inside the sandbox.

**Fix (workaround):**
```bash
adb shell "echo 1234 | sudo -S lxc-attach -n openharmony -- param set persist.init.debug.seccomp.enable 0"
```
`IsEnableSeccomp()` in `seccomp_policy.c` returns false; `SetSeccompFilter` becomes a no-op. App spawning restored.

> **Note:** Not a permanent solution. Seccomp should be properly fixed before production use. The actual root cause (stale `/mnt/sandbox/` mounts and/or `seccomp/` inaccessible inside pivot-root'd sandbox) should be investigated when app sandboxing is addressed.

---

#### Fix N: /android libraries inaccessible in app sandbox

**Symptoms:**
- `android_dlopen` fails silently for all Android libraries in sandboxed app processes: `HybrisEGL: android_dlopen(/android/vendor/lib64/egl/libGLES_mali.so) FAILED: dlopen failed:` (empty error suffix)
- Same call succeeds in non-sandboxed `render_service`

**Root cause:**
`appdata-sandbox.json` did not include `/android` in its mount path list. After `pivot_root` into the app sandbox, `/android` is absent from the sandbox filesystem, so all hybris Android library lookups fail with `ENOENT`. The asymmetry with `render_service` is because `render_service` runs outside the app sandbox.

**Fix:**
Added `/android` bind-mount entry to the common mount-paths section of `base/startup/appspawn/appdata-sandbox.json`:
```json
}, {
    "src-path" : "/android",
    "sandbox-path" : "/android",
    "sandbox-flags" : [ "bind", "rec" ],
    "check-action-status": "false"
}
```
Device-side file was already updated in a previous session; this syncs the source tree.

**Verification:** `/proc/<launcher-PID>/root/android/vendor/lib64/egl/libGLES_mali.so` accessible in sandbox. Combined with Fix N (extensionability bind-mount), `android_dlopen` succeeds — launcher EGL initializes, RSRenderThread requests VSync, and the lockscreen renders on physical hardware.

---
