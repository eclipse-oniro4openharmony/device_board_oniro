# Phase 8: System Stability

> **Legacy (LXC-era) document.** Describes the original OHOS-as-LXC-container
> path, which is **no longer maintained** — the project now boots OHOS
> natively (no Ubuntu Touch host, no LXC). Kept as a reference for the HAL /
> driver bring-up detail (libhybris, graphics, audio, WiFi, …) that still
> applies under native boot. For current status start at [README.md](README.md).

## Status: 🔄 In Progress

---

## Overview

After Phase 7, the device boots fully: boot animation plays, launcher renders the lockscreen, and touch input is functional. However, swiping up to unlock causes the launcher to appear briefly before the lockscreen reappears. This phase investigates and fixes that regression and any related stability issues.

---

## 8.1 — Unlock Re-locks Immediately (FIXED)

### Symptom

Swipe-up gesture on the lockscreen triggers a visible unlock transition (launcher desktop shown for ~4 seconds), then the lockscreen reappears. The cycle repeats indefinitely on every swipe.

### Root Cause Analysis

**Log evidence** (`hilog` captured live across two swipe-unlock attempts):

Timeline of first unlock attempt (timestamps from 2026-03-27 session):

| Time | Event |
|------|-------|
| 22:25:51.583 | `Touch Event slidingLength: 150` — swipe threshold met |
| 22:25:51.584 | `ScreenLock-ScreenLockService --> unlockScreen` |
| 22:25:51.839 | `unlock the screen` / `unlocking` |
| 22:25:51.840 | `ScreenLock-ScreenLockModel --> window hide` |
| 22:25:51.853 | `ScreenLock-Entry --> onPageHide` — lockscreen UI hidden |
| 22:25:51.859 | `notifyUnlockScreenResult: error:null data:true` — unlock confirmed |
| 22:25:51.881 | `onStatusChange 4` — unlocked state reached |
| **22:25:52.721** | **`NavigationBar_ServiceExtAbility --> onCreate` with `"ohos.aafwk.app.restart":true`** |
| 22:25:53.169 | `ScreenLock-ServiceExtAbility --> onCreate` — fresh lockscreen process |
| 22:25:56.311 | `ScreenLock-Entry --> onPageShow` — lockscreen visible again |
| 22:25:56.702 | `EVENT_SYSTEM_READY` → `showLockScreen` → lockscreen locked |

The unlock itself succeeds: `notifyUnlockScreenResult` returns `data:true`. The re-lock is caused by `com.ohos.systemui` **crashing** immediately after the unlock, which triggers the OHOS framework to restart the entire `systemui` process (PID 1178 → 1683) with `ohos.aafwk.app.restart:true`. The restarted process performs the standard boot-time `showLockScreen` sequence, bringing the lockscreen back.

**Faultlog entry** matching the crash timestamp:

```
File: cppcrash-com.ohos.systemui-10008-20251206222551866.log
Timestamp: 2025-12-06 22:25:51.866
Pid: 1178
Reason: Signal:SIGSEGV(SEGV_MAPERR)@0xfe88369f9b9fb900
Fault thread: Tid:1390, Name:RSRenderThread
```

**Crash call stack:**

```
#00  get_meta+92                           ld-musl-aarch64.so.1
#01  __libc_free+24                        ld-musl-aarch64.so.1
#02  OhosNativeWindow::~OhosNativeWindow()+416   libeglplatform_ohos.z.so
#03  OhosNativeWindow::~OhosNativeWindow()+16    libeglplatform_ohos.z.so  (vtable thunk)
#04  ohosws_DestroyWindow+500              libeglplatform_ohos.z.so
#05  eglDestroySurface+112                 libEGL.z.so
#06  EglWrapperDisplay::DestroyEglSurface  libEGL.so (platformsdk)
#07  eglDestroySurface+140                 libEGL.so (platformsdk)
#08  RSSurfaceOhosGl::ClearBuffer()+316    librender_service_base.z.so
#09  RSUIDirector::GoBackground(bool)      librender_service_client.z.so
#10  EventHandler::DistributeEvent         libeventhandler.z.so
#11–14  EventRunner / RSRenderThread::Start
```

**Root cause — Mali permanent incRef / double-free:**

When the lockscreen window hides (`onPageHide`), `RSUIDirector::GoBackground` → `eglDestroySurface` → `ohosws_DestroyWindow` → `OhosNativeWindow::~OhosNativeWindow()` → `freeBuffers()`.

The original `freeBuffers()` used raw `delete` on each cached `OhosNativeWindowBuffer*`. However, **Mali EGL holds a permanent `incRef` on every buffer it has ever dequeued**, keeping it alive from first dequeue until `eglDestroySurface`. The lifetime contract is:

1. First `dequeueBuffer`: Mali calls `incRef` (refcount 1 → 2). Mali keeps this reference permanently.
2. Each subsequent dequeue/queue cycle: Mali's working reference is separate; it adds an `incRef` at dequeue and a `decRef` at `queueBuffer`/`cancelBuffer`.
3. `eglDestroySurface`: Mali releases the permanent reference, calling `decRef` (2 → 1) on every buffer it has ever dequeued.

The original code's `freeBuffers()` called `delete wrapper` directly, bypassing the refcount entirely. This freed the memory while Mali's `decRef` (from step 3 above, running in the same `~OhosNativeWindow()` call chain) then tried to access the now-freed object's fields to check whether to `delete` again — resulting in the SIGSEGV with a corrupted address in `__libc_free`.

### Fix Applied

**`ohos_window.cpp` — `dequeueBuffer()`:** Added a map-ownership `incRef` when inserting a new wrapper into `m_bufferMap`. This ensures the wrapper's refcount always has at least one map-held reference in addition to Mali's permanent reference:

```cpp
wrapper = new OhosNativeWindowBuffer(ohBuffer);
// Map-ownership reference: prevents Mali's permanent decRef from ever
// reaching zero while the wrapper is still in the map.
wrapper->common.incRef(&wrapper->common);
m_bufferMap[ohBuffer] = wrapper;
```

**`ohos_window.cpp` — `freeBuffers()`:** Changed from raw `delete` to `decRef`. This releases the map-ownership reference; if Mali's permanent reference has already been released by `eglDestroySurface` (refcount = 1), this `decRef` drives it to 0 and triggers the real `delete` via `_decRef`. If Mali's permanent reference has not yet been released (refcount = 2), this `decRef` drives it to 1, and Mali's subsequent `decRef` will then correctly trigger `delete`.

**`nativewindowbase.cpp` — `_incRef` / `_decRef`:** Added HiLog instrumentation to trace every refcount change and flag when `_decRef` triggers deletion.

**`ohos_window.cpp` — `~OhosNativeWindowBuffer()`:** Added magic canary (`kMagicLive` / `kMagicDead`) to detect any remaining double-destroy path. Added HiLog for full lifecycle tracing.

**Result:** `com.ohos.systemui` teardown after unlock is now clean. No SIGSEGV. The process stays alive.

---

## 8.2 — UI Freeze After Unlock

### Symptom

After fixing 8.1, `com.ohos.systemui` no longer crashes, but the launcher does not appear after swipe-to-unlock. The screen stays on the last lockscreen frame indefinitely.

### Log Evidence

```
12-07 01:54:50.150  1350  1551 E C01401/Bufferqueue: FlushBuffer failed, ret:41210000
12-07 01:54:50.151  1350  1551 E C01406/OHOS::RS: SwapBuffers: Failed to SwapBuffers on surface, error is 300b
12-07 01:54:50.152  1350  1551 I C01400/HybrisOhosWin: ~OhosNativeWindow: this=... mapSize=3
12-07 01:54:50.152  1350  1551 I C01400/HybrisOhosWin: freeBuffers[dtor]: win=... mapSize=3   ← clean teardown
...
12-07 01:54:50.171   305   305 I C01400/HybrisOhosWin: OhosNativeWindow constructor: nativeWindow=...
12-07 01:54:50.171   305   305 I C01400/HybrisOhosWin: OhosNativeWindow initial geometry: 0x0 format=...
12-07 01:54:50.172   305   305 E C01406/OHOS::RS: SwapBuffers: Failed to SwapBuffers on surface, error is 300d
12-07 01:54:51.185   305   305 I C01400/HybrisOhosWin: ~OhosNativeWindow: ... mapSize=0   ← no buffers allocated
12-07 01:54:51.185   305   305 I C01400/HybrisOhosWin: OhosNativeWindow constructor: ...   ← retry
12-07 01:54:51.187   305   305 E C01406/OHOS::RS: SwapBuffers: Failed to SwapBuffers on surface, error is 300d
... (repeats every ~1 second indefinitely)
```

Error codes:
- **`41210000` (`GSERROR_BUFFER_NOT_INCACHE`):** The lockscreen surface's consumer is already gone at GoBackground time; the pending buffer can't be flushed. Expected during teardown, not the root cause.
- **`300b` (`EGL_BAD_NATIVE_WINDOW`):** OHOS EGL wrapper rejects the native window after the flush failure. Triggers systemui's EGL surface destruction (the clean teardown seen above).
- **`300d` (`EGL_BAD_SURFACE`):** render_service (PID 305) creates a new `OhosNativeWindow`, but `eglSwapBuffers` immediately fails. The window has `mapSize=0` on destruction — no buffer was ever dequeued, meaning `eglCreateWindowSurface` itself silently failed.

### Root Cause Analysis — Zero-Dimension Surface (FIXED)

The `render_service` was failing to create EGL surfaces for the launcher because `OhosNativeWindow::width()` and `OhosNativeWindow::height()` (called by Mali EGL via `BaseNativeWindow::_query`) were returning `0` (zero-dimension surface). This was because the `OhosNativeWindow` constructor cached `0x0` geometry, and the "late geometry resolution" logic was incorrectly placed in an unused `OhosNativeWindow::query()` method, rather than in the virtual `width()` and `height()` methods that are actually invoked.

### Fix Applied (Geometry Resolution)

The late geometry resolution logic was moved from the unused `OhosNativeNativeWindow::query()` method to the virtual `OhosNativeWindow::width()` and `OhosNativeWindow::height()` methods. This ensures that when Mali EGL queries for dimensions, the `NativeWindowHandleOpt` is correctly invoked to retrieve the current geometry from the underlying OHOS `NativeWindow`.

**Result:** `OhosNativeWindow::width()` and `::height()` now correctly return `1560x720` after geometry is set on the underlying `NativeWindow`. The `EGL_BAD_SURFACE` (300d) loop due to zero-dimension surfaces is resolved.

---

## 8.3 — EGLImage Creation Failure

### Symptom

Even after fixing the geometry, the UI remains frozen. Logs show `skia` attempting to create EGL Images with `eglCreateImageKHR` but failing with `EGL_BAD_PARAMETER` (error 300c). This occurs for the launcher process (PID 225 in recent logs).

```
12-07 02:25:37.745   225   225 E C01406/skia: EglImageResource::Create: eglCreateImageKHR failed, error EGL_BAD_PARAMETER.
```

### Root Cause Analysis

OHOS's `render_service` (and other components using `skia`) calls `eglCreateImageKHR` with `EGL_NATIVE_BUFFER_OHOS` (value `0x34E1`) as the target and an OHOS `NativeWindowBuffer*` (or similar native buffer object) as the `buffer` argument. The `libhybris` EGL platform (`eglplatformcommon.cpp`) did not initially recognize `EGL_NATIVE_BUFFER_OHOS` as a valid target. Even after adding a fix to convert `EGL_NATIVE_BUFFER_OHOS` to `EGL_NATIVE_BUFFER_ANDROID` (value `0x3140`), the underlying `buffer` pointer passed by OHOS is still a raw OHOS `NativeWindowBuffer*`. Mali EGL, however, expects an `ANativeWindowBuffer*` (our `OhosNativeWindowBuffer` wrapper) when the target is `EGL_NATIVE_BUFFER_ANDROID`. Passing an incompatible buffer type results in `EGL_BAD_PARAMETER`.

### Fix Applied (Buffer Pointer Translation)

Two-part fix in `eglplatformcommon.cpp` and `ohos_window.cpp`:

**`eglplatformcommon_passthroughImageKHR`** — when `target == EGL_NATIVE_BUFFER_OHOS`:
1. Converts `target` to `EGL_NATIVE_BUFFER_ANDROID` (as before).
2. Calls `ohosws_find_anwb_for_ohbuffer(*buffer)` to translate the raw `OHNativeWindowBuffer*` to the `OhosNativeWindowBuffer*` (`ANativeWindowBuffer*`) wrapper that Mali expects.
3. Replaces `*buffer` with the wrapper if found/created.

**`ohos_window.cpp` — `ohosws_find_anwb_for_ohbuffer(void* ohBuf)`** (new `extern "C"` function, declared `__attribute__((weak))` in `eglplatformcommon.cpp`):
- **Fast path (app process):** Buffers dequeued via `OhosNativeWindow::dequeueBuffer()` are registered in a global `std::map<void*, OhosNativeWindowBuffer*> g_bufferLookup` (protected by `g_bufferLookupMutex`). The lookup returns the cached wrapper directly. Entries are added on first dequeue and removed in `freeBuffers()` before the map-ownership `decRef`.
- **Slow path (render_service / consumer processes):** `render_service` creates its own `OHNativeWindowBuffer*` via `CreateNativeWindowBufferFromSurfaceBuffer()` in a different process; these are never in `g_bufferLookup`. For these, a fresh `OhosNativeWindowBuffer` wrapper is created on demand. `BaseNativeWindowBuffer` initializes `refcount = 0`; Mali's internal `incRef` inside `eglCreateImageKHR` brings it to 1, and `eglDestroyImageKHR`'s `decRef` brings it to 0, triggering `delete`. No manual lifecycle management is needed.

**Result:** `eglCreateImageKHR` succeeds. `skia` can create textures from OHOS surface buffers. **Launcher renders and stays visible after swipe-to-unlock.** The lockscreen no longer reappears.

---

## 8.4 — Current Status (2026-03-28)

| Issue | Status |
|-------|--------|
| `com.ohos.systemui` SIGSEGV on unlock (8.1) | ✅ **FIXED** |
| render_service `EGL_BAD_SURFACE` retry loop (8.2) | ✅ **FIXED** (geometry resolution) |
| `eglCreateImageKHR` fails with `EGL_BAD_PARAMETER` (8.3) | ✅ **FIXED** (buffer pointer translation) |
| Launcher visible after unlock | ✅ **WORKING** |
| `com.ohos.systemui` GLES library loading failure | ⚠️ **WARNING** (symlinks correct, but GLES library load error in hilog — appears benign) |
| `EGL_BAD_NATIVE_WINDOW` (300b) on background surface `SwapBuffers` | ⚠️ **KNOWN** (surfaces transitioning to background; benign, no visible impact) |

**Current status (as of 2026-03-28):** Swipe-to-unlock works end-to-end. The lockscreen hides, the launcher takes the foreground, and `com.ohos.systemui` stays alive (no crash, no restart). Phase 8 primary milestone complete.

### Remaining Items

*   **8.4.B — `Failed to load GLES library` warning:** Persists in hilog despite correct symlinks. Appears benign (rendering works). No investigation needed unless regressions appear.
*   **8.4.C — `EGL_BAD_NATIVE_WINDOW` (300b) on `SwapBuffers`:** Occurs when a surface transitions to background (e.g., lockscreen hiding). Expected; no user-visible impact.

---

## 8.5 — Screen Goes Black on Tap After Unlock

### Symptom

After Phase 8.1–8.3 fixes, swiping to unlock works but tapping the screen immediately afterward causes it to go black. Investigation revealed two separate crashes triggered by the first user interaction post-unlock.

---

### 8.5.1 — `allocator_host` SIGSEGV in `_hybris_hook_readdir` (FIXED)

**Crash log:** `cppcrash-allocator_host-3041-*.log`
**Signal:** `SIGSEGV(SEGV_ACCERR)` at a page-boundary address (e.g., `0x7f80aaa000`)

**Call stack:**
```
#00  memcpy (in libc)
#01  _hybris_hook_readdir+N        third_party/libhybris/hybris/common/hooks.c
#02  PassthroughServiceManager::get  libhidlbase.so
#03  Gralloc4Mapper (lazy init)    libhidlmemory.so
```

**Root cause:**

`_hybris_hook_readdir` in `third_party/libhybris/hybris/common/hooks.c` calls:
```c
memcpy(result.d_name, real_result->d_name, sizeof(result.d_name));  // copies 256 bytes
```

MUSL's `readdir()` returns a pointer into the internal `getdents64` result buffer. When the last directory entry in the buffer has its `d_name` field sitting near a page boundary, blindly copying `sizeof(result.d_name) == 256` bytes crosses into the next unmapped/protected page and triggers `SEGV_ACCERR`.

This crash was triggered during `Gralloc4Mapper` lazy initialization (on first `hybris_gralloc_allocate` or `hybris_gralloc_import_buffer` call), when `PassthroughServiceManager::get` scanned HAL library directories via `readdir`.

**Fix (`third_party/libhybris/hybris/common/hooks.c`):**
```c
/* Before (broken): */
memcpy(result.d_name, real_result->d_name, sizeof(result.d_name));
result.d_name[sizeof(result.d_name)-1] = '\0';

/* After (fixed): */
size_t name_len = strnlen(real_result->d_name, sizeof(result.d_name) - 1);
memcpy(result.d_name, real_result->d_name, name_len);
result.d_name[name_len] = '\0';
```

Using `strnlen` ensures the copy never reads past the NUL terminator of the actual filename, avoiding the page-boundary read.

---

### 8.5.2 — `composer_host` SIGABRT in `writeNativeHandleNoDup` (FIXED)

**Crash log:** `cppcrash-composer_host-3036-20251207083312160.log`
**Signal:** `SIGABRT` in `android::hardware::Parcel::writeNativeHandleNoDup+304`

**Call stack:**
```
#00  abort
#01  LOG_ALWAYS_FATAL_IF (handle version/numFds/numInts validation)
#02  android::hardware::Parcel::writeNativeHandleNoDup+304   libhidlbase.so
#03  HybrisDisplay::PrepareDisplayLayers                     libdisplay_composer_vdi_impl.z.so
```

**Root cause — use-after-free in `HybrisLayer::SetLayerBuffer`:**

`HybrisLayer::SetLayerBuffer` (`display_composer/hybris_layer.cpp`) built a temporary `HybrisNativeBuffer` (containing an `ANativeWindowBuffer` and its `native_handle_t`), passed it to `hwc2_compat_layer_set_buffer`, then immediately `delete`-d it:

```cpp
hwc2_error_t err = hwc2_compat_layer_set_buffer(layer_, 0, &nb->buf, fence);
delete nb;  // ← BUG: frees handle before Composer::execute() reads it
```

`hwc2_compat_layer_set_buffer` stores a raw pointer to `nb->buf.handle` inside the Android HWC2 HAL's layer state. During the subsequent `Composer::execute()` call (triggered by `validateDisplay` / `presentDisplay` in `PrepareDisplayLayers`), the HAL serializes the layer buffer via `writeNativeHandleNoDup`, which dereferences the now-freed `native_handle_t*`. The freed memory has garbage in the `version` field, causing `LOG_ALWAYS_FATAL` to abort.

**Fix (`hybris_layer.h` / `hybris_layer.cpp`):**

Added `HybrisNativeBuffer* currentLayerBuffer_{nullptr}` member to `HybrisLayer`. In `SetLayerBuffer`, the old buffer is deleted and the new one is stored (not immediately freed):

```cpp
hwc2_error_t err = hwc2_compat_layer_set_buffer(layer_, 0, &nb->buf, fence);
// Keep nb alive until the next SetLayerBuffer call — the HWC2 HAL holds a raw
// pointer to nb->buf.handle and reads it during Composer::execute().
delete currentLayerBuffer_;
currentLayerBuffer_ = nb;
```

The `HybrisLayer` destructor deletes `currentLayerBuffer_` as final cleanup.

**Result:** `composer_host` no longer aborts. `Composer::execute()` reads a valid `native_handle_t*` from the live `HybrisNativeBuffer`.

---

## 8.6 — Current Status (2026-03-28)

| Issue | Status |
|-------|--------|
| `com.ohos.systemui` SIGSEGV on unlock (8.1) | ✅ **FIXED** |
| render_service `EGL_BAD_SURFACE` retry loop (8.2) | ✅ **FIXED** |
| `eglCreateImageKHR` fails with `EGL_BAD_PARAMETER` (8.3) | ✅ **FIXED** |
| `allocator_host` SIGSEGV in `_hybris_hook_readdir` (8.5.1) | ✅ **FIXED** |
| `composer_host` SIGABRT use-after-free in `SetLayerBuffer` (8.5.2) | ✅ **FIXED** |
| Launcher visible after unlock; tap interaction works | ✅ **WORKING** |
| `com.ohos.systemui` GLES library loading failure | ⚠️ **WARNING** (benign) |
| `EGL_BAD_NATIVE_WINDOW` (300b) on background `SwapBuffers` | ⚠️ **KNOWN** (benign) |

**Current status (as of 2026-03-28):** Swipe-to-unlock works end-to-end with no post-unlock crashes. No `allocator_host` or `composer_host` crashes observed after the fixes. Tap interaction functional.

---

## 8.7 — Wrong Display Interface Type (FIXED)

### Symptom

Status bar and navigation bar intermittently flicker between transparent, visible, and black-background states. Occasional `com.ohos.systemui` crash.

### Root Cause

`HybrisDisplay::GetDisplayCapability` reported `DISP_INTF_HDMI` (= 0) as the interface type. In `rs_screen.cpp`, `RSScreen::PhysicalScreenInit()` only classifies a screen as `BUILT_IN_TYPE_SCREEN` when the type is `GRAPHIC_DISP_INTF_MIPI` (= 9); every other type falls through to `EXTERNAL_TYPE_SCREEN`. The Volla X23 has an internal MIPI DSI panel — the wrong type was causing the display to be treated as a hotplug external monitor, which affects render pipeline decisions.

**`rs_screen.cpp` lines 192–196:**
```cpp
if (capability_.type == GraphicInterfaceType::GRAPHIC_DISP_INTF_MIPI) {
    property_.SetScreenType(RSScreenType::BUILT_IN_TYPE_SCREEN);
} else {
    property_.SetScreenType(RSScreenType::EXTERNAL_TYPE_SCREEN);  // ← was hit before fix
}
```

### Fix

`device/soc/oniro/hybris_generic/hardware/display/src/display_composer/hybris_display.cpp` —
`GetDisplayCapability`:

```cpp
// Before:
info.type = DISP_INTF_HDMI;  /* closest generic type */
// After:
info.type = DISP_INTF_MIPI;  /* Volla X23 has an internal MIPI DSI panel */
```

---

## 8.8 — Composition Oscillation: Flicker and Crash (FIXED)

### Root Cause

When `validateDisplay` returned `HWC2_ERROR_HAS_CHANGES` (`numTypes > 0`), `GetDisplayCompChange` forced **all** layers to `COMPOSITION_CLIENT`. This caused render_service to destroy each layer's individual EGL window surface and create a combined client-target surface. `RSRenderThread` calls `eglSwapBuffers` concurrently; destroying the surface on the main thread while the render thread is inside Mali's `eglSwapBuffers` is a data race — Mali's internal surface object is freed and zeroed, causing a SIGSEGV (`NULL+0x1d8`).

### Fix

When `numTypes > 0`: force all layers back to `COMPOSITION_DEVICE` and immediately re-validate. If the MTK HAL accepts all-DEVICE on the retry (`retryTypes == 0`), skip the CLIENT path entirely. Only fall through to CLIENT if HWC2 still insists after the retry.

> **Note:** This double-validate approach caused a 1fps regression when the MTK HAL persistently returns `numTypes > 0` every frame (see 8.9). It was replaced by the single-validate + all-CLIENT fallback in 8.9.

---

## 8.9 — 1fps Drop from Double-Validate (RESOLVED)

### Root Cause

The MTK HWC2 HAL persistently returns `HWC2_ERROR_HAS_CHANGES` (`numTypes > 0`) on certain layer configurations — not just transiently during unlock. The 8.8 double-validate called `hwc2_compat_display_validate` twice per frame (~2 × 9ms = ~18ms), exceeding the 16.7ms vsync budget and dropping the frame rate to ~1fps.

### Fix

Removed the double-validate. Single `validateDisplay` call: when `numTypes > 0`, fall back to full CLIENT composite (`needFlushFb=true`, `needsClientComposition_=true`); report all layer IDs as `COMPOSITION_CLIENT` from `GetDisplayCompChange`. Introduces the composition incoherence described in 8.10.

---

## 8.10 — Composition Incoherence: Flicker and Black Screen

### Root Cause

Two interacting architectural bugs:

**Bug A — HWC2 spec violation:** The OHOS VDI architecture calls `SetLayerCompositionType(CLIENT)` between `validateDisplay` and `acceptDisplayChanges`, which the HWC2 spec explicitly forbids. This invalidates the validated state; `acceptDisplayChanges` then operates on stale state, and `presentDisplay` may silently fail. The violation is structural: the OHOS framework splits the HWC2 state machine across two IPC calls (`PrepareDisplayLayers` → render_service → `Commit`), so there is no way to prevent render_service from calling `SetLayerCompositionType` between validate and accept.

**Bug B — `numTypes=0` ambiguity:** `numTypes=0` only means "HAL agrees with whatever types were requested" — not necessarily "all DEVICE." If layers were left CLIENT from the previous frame and the HAL accepts them (`numTypes=0`), the HAL presents in CLIENT mode. If `SetDisplayClientBuffer` is not called for that frame, the HAL scans out a stale client-target buffer.

The correct fix (`getChangedCompositionTypes`, called after `validateDisplay` and before `acceptChanges`) would reveal exactly which layers the HAL changed, letting the VDI call `acceptChanges` before render_service can call `SetLayerCompositionType`. However, `hwc2_compat_display_get_changed_composition_types` does not exist in the prebuilt `libhwc2_compat_layer.so` at `/android/system/lib64/`; rebuilding it requires an Android bionic toolchain (not currently available).

### Approaches Tried (All Failed)

| # | Approach | Outcome |
|---|---|---|
| 1 | **Always DEVICE** (`needFlushFb=false`) | FAILED — HAL blocks `presentDisplay` waiting for a client target that never arrives → 1fps freeze |
| 2 | **Double-validate / force DEVICE** (8.8) | FAILED — 1fps regression (8.9) |
| 3 | **All-CLIENT when `numTypes>0`** (8.9 fallback) | FAILED — EGL surface destruction race → RSRenderThread SIGSEGV |
| 4 | **Client target + suppress layer transitions** (`needFlushFb=(numTypes>0)`, `needsClientComposition_=false`) | FAILED — screen turns black after a period of use; HAL enters persistent CLIENT mode that the workaround cannot recover from |
| 5 | **Specific CLIENT layers via `getChangedCompositionTypes` dlsym** | FAILED — status bar/nav bar are among the HAL-requested CLIENT layers; narrowing the set doesn't eliminate the EGL surface destruction race for those specific layers |

### Current Deployed Code (2026-03-31)

`PrepareDisplayLayers`: call `validateDisplay`, then immediately call `acceptDisplayChanges` (before returning to render_service), set `needFlushFb = (numTypes > 0)`. Removes `acceptChanges` from `Commit`. Eliminates Bug A's spec-violation window; Bug B risk is low because the HAL's DEVICE state is cleanly committed per frame.

`GetDisplayCompChange`: when `needsClientComposition_` (i.e. `numTypes > 0`), returns all layer IDs as `COMPOSITION_CLIENT`.

The only correct general fix requires rebuilding `libhwc2_compat_layer.so` with the `hwc2_compat_display_get_changed_composition_types` C wrapper using an Android bionic toolchain for MT6789/Android 12 SDK 32. The C++ method `HWC2::Display::getChangedCompositionTypes` is present in the prebuilt (confirmed via `readelf`).

---

## 8.11 — `SetLayerAlpha` Use-After-Free in `composer_host` (IDENTIFIED, NOT YET FIXED)

### Root Cause

Race condition: `DestroyLayer` (called from one IPC thread) frees `hwc2_compat_layer_t* layer_` while a concurrent IPC thread is mid-execution of `SetLayerAlpha` on the same `HybrisLayer`. After `DestroyLayer` calls `hwc2_compat_display_destroy_layer`, the heap memory is reused; `SetLayerAlpha` then dereferences the dangling `layer_` pointer.

### Fix (Not Yet Applied)

Protect layer access with a per-display reader-writer lock, or use a handle-based layer lookup that validates the layer is still alive before dereferencing.

---

## 8.12 — Sticky CLIENT Layer Tracking (2026-03-31)

### Root Cause

`pendingClientLayers_` was cleared on every `numTypes==0` frame, causing `GetDisplayCompChange` to return empty. render_service transitioned all previously-CLIENT layers back to DEVICE (recreating DEVICE EGL window surfaces). On the next frame the HAL requested CLIENT for those layers again (`numTypes > 0`), and render_service transitioned them back to CLIENT — destroying the DEVICE surfaces while RSRenderThread was still using them. This DEVICE↔CLIENT cycle repeated at 60fps, causing constant flicker.

### Fix

Added `stickyClientLayers_` (`std::unordered_map<uint32_t, int32_t>`): once the HAL requests CLIENT for a layer, it stays in the sticky set until the layer is destroyed. `pendingClientLayers_` is always populated from the sticky set, so render_service never transitions sticky layers back to DEVICE — eliminating EGL surface destruction races.

Three `PrepareDisplayLayers` code paths:
1. `numTypes == 0` + no sticky layers → pure DEVICE (`needFlushFb=false`)
2. `numTypes == 0` + sticky layers → `pendingClientLayers_ = stickyClientLayers_`, `needFlushFb=true` — GPU composite continues, no surface transitions
3. `numTypes > 0` → dlsym `getChangedCompositionTypes`, add newly CLIENT layers to sticky, `needFlushFb=true` — one-time surface transition for newly sticky layers only

`DestroyLayer`: added `stickyClientLayers_.erase(layerId)` and `pendingClientLayers_.erase(layerId)`.

**Status:** Partially fixed — EGL surface destruction races eliminated, but 1Hz clock-tick flicker remained. Root cause identified in Bug 8.13.

---

## 8.13 — Pre-validate CLIENT Override (2026-03-31)

### Root Cause

render_service resets **all** layers to DEVICE at the start of every frame before calling `PrepareDisplayLayers`. With sticky CLIENT tracking, this produced two alternating frame types:

- **Clock-tick frame** (`numTypes > 0`): HAL has new content → wants CLIENT → `acceptDisplayChanges` commits CLIENT → client target presented correctly.
- **Maintaining frame** (`numTypes == 0`): HAL sees DEVICE (same as requested) → agrees → `acceptDisplayChanges` commits DEVICE → `presentDisplay` uses DEVICE mode → the GPU-composited client target is **ignored** → status bar / nav bar invisible.

These alternated at 1Hz (the clock update rate), producing visible flicker.

### Fix

Before calling `validateDisplay` in `PrepareDisplayLayers`, force all sticky CLIENT layers to CLIENT on the HAL:

```cpp
for (const auto& kv : stickyClientLayers_) {
    auto it = layers_.find(kv.first);
    if (it != layers_.end()) {
        it->second->SetLayerCompositionType(COMPOSITION_CLIENT);
    }
}
// validateDisplay follows immediately below
```

HAL sees CLIENT → agrees (`numTypes=0`) → `acceptDisplayChanges` commits CLIENT → `presentDisplay` always uses the client target. The "already sticky" `numTypes > 0` path is eliminated entirely for known layers. New layers still trigger `numTypes > 0` on first appearance and are added to sticky at that point.

**Status:** DEPLOYED (2026-03-31). 1Hz clock-tick flicker eliminated. Residual issue found and fixed in Bug 8.14.

---

## 8.14 — Status Bar/Nav Bar Invisible After App Transitions (2026-03-31)

### Symptom

After the Bug 8.13 fix, the 1Hz clock-tick flicker is eliminated. However, the status bar and navigation bar are not visible immediately after opening an app or returning to the launcher. They reappear after a few seconds of touch interaction.

### Root Cause Analysis

**Log evidence** (live capture during layer management events):

```
20:23:38  Layer 10 created → numTypes=0 (4 sticky) → DEVICE mode for multiple frames
...
20:23:38  Layer 9 destroyed → 3 sticky
20:23:38  validateDisplay → layer 10: NEW sticky CLIENT → 4 sticky  ← now visible
```

The MTK HAL does **not** request CLIENT composition for a newly created layer on the first `validateDisplay` call — it returns `numTypes=0`, accepting the default DEVICE composition type. The HAL only requests CLIENT for the new layer after a subsequent layer management event (another create/destroy) triggers re-evaluation.

During the window between layer creation and the first `numTypes > 0` event (which may take several seconds and requires touch interaction to trigger), the new layer exists in DEVICE mode. DEVICE mode without GPU blending does not correctly display layers that depend on client-side composition (status bar, nav bar), rendering them invisible.

**Why touch interaction helps:** Touch events drive UI updates, which trigger layer management events (other layers being created/destroyed), which cause the HAL to re-evaluate and finally request CLIENT for the previously-invisible layers.

**Confirmed layer pattern from logs:**
- Steady state: 4 sticky CLIENT layers (e.g., IDs 6, 79, 81, 82)
- App opens: some layers destroyed (e.g., 81, 82) — sticky count drops
- New layers created (e.g., 83) — start as DEVICE, NOT in sticky set
- After layer 83 stays DEVICE for many frames, some other event triggers `numTypes > 0` → layer 83 becomes sticky
- During the DEVICE window, if layer 83 is the new status bar or nav bar → invisible

**Status:** DEPLOYED (2026-03-31). Pre-register new layers as sticky CLIENT in `CreateLayer`. Fixed in README as Bug 8.14.

---

## 8.15 — Power Button Causes Device Shutdown (2026-04-01)

### Symptom

Short press of the physical power button causes the Volla X23 to power off completely. Expected: display off + screen lock; next press → display on.

### Root Cause Analysis

**`systemd-logind` on the Ubuntu Touch host** has `HandlePowerKey=poweroff` set in its default config. The host `logind` receives the power button event (via `event0`/mtk-pmic-keys) and initiates a host shutdown via `systemd`. This fires regardless of OHOS behavior.

### Fix

Created `/etc/systemd/system/ohos-logind-powerkey.service` (persistent on the writable `sdc58` data partition via `/etc/systemd/system/` bind mount):

```ini
[Unit]
Description=Ignore power key in systemd-logind (OHOS container handles it)
Before=systemd-logind.service
DefaultDependencies=no

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/home/phablet/ohos-logind-powerkey.sh

[Install]
WantedBy=sysinit.target
```

Script at `/home/phablet/ohos-logind-powerkey.sh`:

```sh
#!/bin/sh
mkdir -p /run/systemd/logind.conf.d
printf "[Login]\nHandlePowerKey=ignore\nHandlePowerKeyLongPress=ignore\n" \
    > /run/systemd/logind.conf.d/ohos-container.conf
```

The service runs `Before=systemd-logind.service` so the drop-in is in place before logind starts. `HandlePowerKey=ignore` and `HandlePowerKeyLongPress=ignore` prevent logind from acting on the power button. The OHOS container's `multimodalinput` + power manager handle the event instead.

Enabled with: `systemctl enable ohos-logind-powerkey.service`

**Status:** FIXED (2026-04-01). Logind override service enabled and persistent.

---

## 8.16 — Status Bar / Nav Bar Invisible After App Transitions (2026-04-09)

### Symptom

After opening an app or returning to the launcher, the status bar and navigation bar are often not visible. They reappear after a few seconds or following a touch interaction that triggers a display refresh.

### Root Cause Analysis

**Log evidence** (live capture during unlock and app transitions):

```
19:38:18.614  Created layer 4 on display 0           ← no sticky CLIENT
19:38:18.622  Created layer 5 on display 0           ← no sticky CLIENT
19:38:18.627  Created layer 6 on display 0           ← no sticky CLIENT
19:38:18.630  Created layer 7 on display 0           ← no sticky CLIENT
19:38:18.652  PrepareDisplayLayers: numTypes=0 (DEVICE)  ← HAL accepts DEVICE
```

**Two interacting issues:**

1. **All new layers started as DEVICE composition.** `CreateLayer` set the initial composition type to `COMPOSITION_DEVICE` for all non-cursor layers. This is the HWC2 default.

2. **The MTK HAL often accepts DEVICE for overlay layers** (status bar 720×72, nav bar 720×72, privacy indicator 720×32) even though they require alpha blending. In DEVICE mode, the HAL composites layers directly without GPU blending, which can render transparent/semi-transparent overlay layers invisible or incorrectly.

**Why the bug was intermittent:** The HAL occasionally re-evaluates and requests CLIENT (`numTypes > 0`) on subsequent frames, typically triggered by layer management events (create/destroy) or content changes. A touch interaction causes UI updates → layer events → HAL re-evaluation → CLIENT requested → status bar/nav bar become visible. Without interaction, the re-evaluation could take several seconds.

**Why Bug 8.14's approach was insufficient:** Bug 8.14 planned to pre-register layers as sticky CLIENT only when `stickyClientLayers_` was already non-empty (i.e., after the HAL had requested CLIENT for at least one layer). However, the HAL may NEVER request CLIENT during normal operation — accepting DEVICE for all layers indefinitely. In testing, after full boot, unlock, and app launches, `numTypes` was consistently 0 with no CLIENT requests from the HAL.

### Fix

Changed `CreateLayer` to start all layers as `COMPOSITION_CLIENT` (instead of `COMPOSITION_DEVICE`) and immediately add them to `stickyClientLayers_`:

```cpp
// In CreateLayer:
CompositionType compType = COMPOSITION_CLIENT;
if (info.type == LAYER_TYPE_CURSOR) {
    compType = COMPOSITION_CURSOR;
}
layer->SetLayerCompositionType(compType);

// ...

stickyClientLayers_[layerId] = static_cast<int32_t>(HWC2::Composition::Client);
```

**Effect:**
- Every layer starts as CLIENT from creation
- `PrepareDisplayLayers` forces CLIENT before `validateDisplay` on every frame
- HAL sees CLIENT, agrees (`numTypes=0`), no type changes
- GPU composite (client target) always includes all layers with correct alpha blending
- Status bar, nav bar, and all overlays are visible from the first frame

**Trade-off:** All composition is now GPU-based (no hardware overlay optimization). On this hardware, this is acceptable — the MTK HWC2 HAL via libhybris consistently works best with CLIENT composition, and the Mali GPU handles the blending load without frame drops.

**Status:** FIXED (2026-04-09). Deployed and verified: unlock creates 4 layers, all immediately sticky CLIENT, all visible on first frame. No crashes after extended operation.

---

## 8.17 — `com.ohos.systemui` SIGSEGV in Mali `eglSwapBuffers` During Dropdown Dismiss (PARTIALLY FIXED — cross-thread race closed, same crash signature still reproduces on same-thread dropdown open/close)

### Symptom

`com.ohos.systemui` crashes with `SIGSEGV(SEGV_MAPERR)@0x00000000000001d8` (NULL pointer dereference) on thread `RSRenderThread` approximately 101 seconds after process start. The crash occurs inside `libGLES_mali.so` during `eglSwapBuffers`, triggered when the user opens the dropdown/notification panel and then swipes up to dismiss it.

**Crash stack:**
```
#00 pc 76e920  libGLES_mali.so
#01 pc 76e83c  libGLES_mali.so
#02 pc 770b60  libGLES_mali.so
#03 pc 76805c  libGLES_mali.so
#04 pc 7a9408  libGLES_mali.so
#05 pc 7a8894  libGLES_mali.so
#06 _my_eglSwapBuffersWithDamageEXT+140  libEGL.z.so
#07 eglSwapBuffers+144                   libEGL.so (platformsdk)
#08 RenderContextGL::SwapBuffers+128     librender_service_base.z.so
#09 RSSurfaceOhosGl::FlushFrame+192      librender_service_base.z.so
#10 RSRenderThreadVisitor::ProcessRootRenderNode+4624  librender_service_client.z.so
```

### Root Cause

**Race between `eglDestroySurface` and `eglSwapBuffers` on different threads.**

When the dropdown panel is dismissed, the OHOS window system triggers:

| Time | Thread | Event |
|------|--------|-------|
| 20:14:58.792 | main | Swipe-up touch ends → `aboutToDisappear` on all components |
| 20:14:58.982 | RSRenderThread | `setBuffersDimensions(1560, 720)` — still rendering frames |
| ~20:14:59 | main | `RSUIDirector::GoBackground()` → `ClearBuffer()` → `eglDestroySurface()` — frees Mali's internal surface state |
| 20:14:59.212 | RSRenderThread | `eglSwapBuffers()` — Mali accesses freed surface → `NULL+0x1d8` SIGSEGV |

The libhybris `eglDestroySurface` (egl.c:445) called Mali's `_eglDestroySurface` which freed internal state, then the concurrent `_my_eglSwapBuffersWithDamageEXT` called Mali's `_eglSwapBuffers` on the same (now-freed) surface handle. Mali's internal surface object was NULL, causing the dereference at offset 0x1d8.

Additionally, the `_surface_window_map` in `helper.cpp` had no thread safety — concurrent reads (from `eglSwapBuffers`) and writes (from `eglDestroySurface`) were data races.

### Fix

**Two changes:**

**1. `third_party/libhybris/hybris/egl/helper.cpp` — Thread-safe surface mapping**

Added `std::mutex` to protect all accesses to `_surface_window_map`:

```cpp
static std::mutex _surface_map_mutex;

void egl_helper_push_mapping(EGLSurface surface, EGLNativeWindowType window) {
    std::lock_guard<std::mutex> lock(_surface_map_mutex);
    _surface_window_map[surface] = window;
}
// ... same for has_mapping, get_mapping, pop_mapping
```

**2. `third_party/libhybris/hybris/egl/egl.c` — Surface lifecycle rwlock**

Added a `pthread_rwlock_t` that serializes `eglSwapBuffers` (read lock) against `eglDestroySurface` (write lock):

```c
static pthread_rwlock_t _surface_lifecycle_lock = PTHREAD_RWLOCK_INITIALIZER;

EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface) {
    pthread_rwlock_wrlock(&_surface_lifecycle_lock);  // exclusive
    // Remove mapping first, then call Mali's destroy
    if (egl_helper_has_mapping(surface))
        win = egl_helper_pop_mapping(surface);
    result = (*_eglDestroySurface)(dpy, surface);
    pthread_rwlock_unlock(&_surface_lifecycle_lock);
    if (win) ws_DestroyWindow(win);  // cleanup outside lock
    return result;
}

EGLBoolean _my_eglSwapBuffersWithDamageEXT(...) {
    pthread_rwlock_rdlock(&_surface_lifecycle_lock);  // shared
    if (egl_helper_has_mapping(surface)) {
        // Normal swap path
        ret = (*_eglSwapBuffers)(dpy, surface);
    } else {
        // Surface destroyed — do NOT call Mali, return EGL_FALSE
        ret = EGL_FALSE;
    }
    pthread_rwlock_unlock(&_surface_lifecycle_lock);
    return ret;
}
```

**Why this works:**
- Multiple `eglSwapBuffers` calls proceed in parallel (shared/read lock)
- `eglDestroySurface` takes an exclusive/write lock, waiting for all in-flight swaps to complete before freeing Mali state
- After destroy, the mapping is removed; any subsequent swap sees no mapping and returns `EGL_FALSE` without calling Mali
- `ws_DestroyWindow` (OhosNativeWindow cleanup) runs outside the lock to avoid blocking swaps on other surfaces

**Status:** PARTIALLY FIXED (2026-04-09). The rwlock closes the cross-thread destroy-vs-swap race described above and remains correct/deployed. However, the same SIGSEGV signature (`NULL+0x1d8` in Mali at `_my_eglSwapBuffersWithDamageEXT+152` on `RSRenderThread`) still reproduces on dropdown open/close — see "Regression — Re-opened 2026-04-17" below for what the follow-up investigation ruled out.

### Regression — Re-opened 2026-04-17

The exact same crash signature (`SIGSEGV(SEGV_MAPERR)@0x1d8`, `RSRenderThread`, `libGLES_mali.so +0x76e920` → `libGLES_mali.so +0x7a8894` → `_my_eglSwapBuffersWithDamageEXT+152`) reproduced deterministically on Volla X23 by opening and closing the control-center dropdown a few times. All four crashes inspected in this session had byte-identical Mali frames and crash register patterns (`x0=0, x1=0xf, x3=0xf, x19=0xf, x25=0xf`).

**Precise Mali crash instruction** (decoded from `/android/vendor/lib64/egl/mt6789/libGLES_mali.so` at pc `0x76e920`):

```
  76e8dc: ldr w9, [x0]        # active-flag, non-zero — iteration continues
  76e900: ldr x8, [x0, #8]    # x8 = manager_ref  (valid)
  76e904: ldr x0, [x8, #8]    # x0 = backing     (NULL  ←  corruption)
  76e910: b.ne 0x76e920       # x3=0xf, not the 0xff00000000 fallback path
  76e920: ldr w10, [x0, #472] # NULL + 0x1d8 → SIGSEGV
```

This is a classic "reference alive, backing freed" pattern inside Mali's per-swap FBO-attachment walker: the attachment slot is still marked active and its manager ref is still valid, but the backing it points at has been zeroed. For `x3 = 0xf` (the normal "all 4 channels" mask) Mali has no fallback and just dereferences.

**Crash is NOT a cross-thread race.** Investigation confirmed both the surface-destroy path and the surface-swap path run on the *same* `RSRenderThread`:

| Caller | Source | Thread |
|--------|--------|--------|
| `RSSurfaceOhosGl::ClearBuffer()` → `MakeCurrent(EGL_NO_SURFACE)` → `eglDestroySurface()` | `rosen/modules/render_service_base/src/platform/ohos/backend/rs_surface_ohos_gl.cpp:128-140` | `RSRenderThread` (posted from main via `RSUIDirector::GoBackground`, `rs_ui_director.cpp:310-322`) |
| `RSSurfaceOhosGl::FlushFrame()` → `SwapBuffers(mEglSurface)` | `rs_surface_ohos_gl.cpp:112-126` | `RSRenderThread` (VSync-driven from `RSRenderThreadVisitor::ProcessRootRenderNode`) |

Because both paths are event-handler callbacks on the same `EventRunner`, they cannot overlap — the existing `_surface_lifecycle_lock` is effectively a no-op for this workload. The corruption is purely *temporal*: Mali's internal shared-context state gets poisoned by *something* in the dropdown-close teardown, and the crash happens on the *next* vsync swap of a surviving surface (status bar or nav bar) — which can be anywhere from hundreds of ms to several seconds later.

**The surface-destroy call itself is not the trigger.** Tested by short-circuiting `_eglDestroySurface` in libhybris (leak the Mali EGLSurface metadata but still call `ws_DestroyWindow` to release the OhosNativeWindow wrapper and the 13 MB of backbuffer memory). Deployed, verified by MD5 that the skip-destroy code was active, confirmed via hilog that the `"eglDestroySurface SKIPPING Mali destroy (leak)"` log line printed for every destroy — yet the same `NULL+0x1d8` crash returned after ~8 minutes of normal use (the process survived the first few stress-test swipes but later crashed from organic dropdown use; see `cppcrash-com.ohos.systemui-10008-20260417041041152.log`, process lifetime 476 s, MD5 `93636179997549d6091ead894eecc668` matches the skip-destroy build). Reverted after the regression confirmed skip-destroy is not the fix.

**`eglWaitGL()` before the destroy is also not the fix.** Tested by adding `(*_eglWaitGL)()` inside the destroy wrlock (guarded by an `eglGetCurrentContext()` check). Same crash, no improvement — ruling out "pending GPU work on a surface being destroyed" as the corruption source.

**Mali environment context (explains what paths are available):**

- Mali advertises `EGL_KHR_surfaceless_context` (confirmed via `strings /android/vendor/lib64/egl/mt6789/libGLES_mali.so`), so `RenderContextGL::CreatePbufferSurface()` (`render_context_gl.cpp:230`) leaves `pbufferSurface_ = EGL_NO_SURFACE`.
- `RenderContextGL::MakeCurrent(EGL_NO_SURFACE)` at `render_context_gl.cpp:246-258` therefore becomes `eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, eglContext_)` — a *surfaceless* transition, not a full unbind.
- `ClearBuffer` at `rs_surface_ohos_gl.cpp:128-140` calls, in order: `DestoryNativeWindow(mWindow)` → `MakeCurrent(EGL_NO_SURFACE)` → `DestroyEGLSurface(mEglSurface)` → `producer_->GoBackground()`. Each of these is a candidate trigger for the corruption that surfaces on the *next* swap.

### Still Open — Candidates Worth Instrumenting Next

With both "Mali destroy" and "pending GPU work" ruled out, the corruption must come from one of:

1. **`DestoryNativeWindow(mWindow)` before the EGL teardown** — decrements the OHOS NativeWindow refcount while the Mali-side EGLSurface still holds internal pointers into its buffer queue. `OhosNativeWindow` keeps its own ref, but the *inner* OHOS NativeWindow's producer state may transition.
2. **`producer_->GoBackground()` after the destroy** — may actively disconnect the BufferQueue producer; if Mali had cached buffer handles via earlier `dequeueBuffer` calls, those are now dangling.
3. **`eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)` (surfaceless transition)** — forces Mali per-thread state into surfaceless mode while the context is shared across multiple surfaces. A Mali bug in the surfaceless→bound re-transition could leave one FBO attachment slot mis-wired.
4. **GL-side resource deletes in ArkUI's dropdown teardown** — `glDeleteFramebuffers` / `glDeleteRenderbuffers` / `glDeleteTextures` called on shared-context resources that other surviving surfaces' FBOs still reference. These calls go through OHOS's `libGLESv3.z.so`, not libhybris, and are currently invisible to our tracing.

**Concrete next-session plan:**

- Add entry/exit hilog to `_my_eglSwapBuffersWithDamageEXT` with surface pointer (60 Hz but tractable for a few seconds of repro) and correlate the last swap surface against the `hideSelf, hide anim finish` / `ClearBuffer` events, so we know *which* surface crashes and whether it's the dropdown itself or a sibling like the status bar.
- Hook `glDeleteFramebuffers` / `glDeleteRenderbuffers` / `glDeleteTextures` via `eglGetProcAddress` from inside libhybris and log each call, to see if the main thread is deleting shared-context resources mid-render.
- If (1) or (2) is implicated, try moving `DestoryNativeWindow` *after* `DestroyEGLSurface` in a local `rs_surface_ohos_gl.cpp` patch, or skip `producer_->GoBackground()`. Both are one-line changes; if either prevents the crash, we've localized the Mali bug to a specific buffer-queue disconnect.

**Working state as of 2026-04-17:** All experimental changes reverted. `libEGL.z.so` is back to the 2026-04-09 Bug 8.17 rwlock-only version (MD5 `515dcb427474afd69183b3730c905e5c`, size 32648). The cross-thread race fix is still in place and still correct for any future path that does actually destroy a surface from a non-`RSRenderThread` context.

---

## 8.18 — Webview (ArkWeb / nweb render) Fails to Load in Any Webview App (FIXED)

### Symptom

Any app embedding an `@ohos/arkweb` webview (e.g. `io.ionic.starter`) launches but the webview area stays blank. hilog shows a tight loop of:

```
C04500/chromium: NWebId: 1 render process exit, reason = 4 reason info = process exit unknown
C02c11/APPSPAWN: [appspawn_common.c:350]open dev_null error: 2
C02c11/APPSPAWN: [appspawn_modulemgr.c:214]Execute hook [31] result -2
```

The main app process (`io.ionic.starter`, chromium browser-side) runs fine and even logs `[arkweb_child_process_launcher_helper_utils.cc:73] Initiate a request to AMS to create a child process, child type: renderer`. But the spawned `web_render` child exits with `result:-2` (`-ENOENT`) before running a single line of user code. Every subsequent relaunch attempt fails identically.

### Root Cause

`nwebspawn` runs as uid 3081, not root. Upstream `base/startup/appspawn/etc/sandbox/appdata_sandbox_fixer.py:276` generates the sandbox config file with `mode = S_IWUSR | S_IRUSR | S_IWGRP | S_IRGRP` (0660), and the build/install umask trims it to **0640, owner root:root**. On a real OHOS device the production image's fs_config rewrites these to world-readable, but our LXC rootfs copy path doesn't go through fs_config — it just preserves whatever mode the `packages/phone/` staging tree has.

As a result, in `nwebspawn`:
1. `LoadAppSandboxConfigCJson` → `GetJsonObjFromFile("/system/etc/sandbox/appdata-sandbox.json")` returns NULL (EACCES).
2. The `APPSPAWN_CHECK` on the NULL result uses `continue`, not `return`, and the error log it emits is easy to miss.
3. `appSandboxCJsonConfig_[SANDBOX_APP_JSON_CONFIG]` stays empty for the life of the nwebspawn process.
4. Every render fork reaches `SetRenderSandboxPropertyNweb`, finds `GetCJsonConfig(type).size() == 0`, iterates zero configs, and returns 0 **without ever mounting anything**.
5. `SetAppSandboxPropertyNweb` still proceeds to `pivot_root` into the freshly-created `/mnt/sandbox/com.ohos.render/<PackageName>/` directory, which is **empty** — no `/dev`, no `/proc`, no `/sys`.
6. The next hook in the chain, `SpawnSetProperties` → `SetFileDescriptors` (`appspawn_common.c:344`), tries `open("/dev/null", O_RDWR)` to redirect stdin/stdout/stderr. It gets `ENOENT` and returns `-errno`, killing the render child.

strace on nwebspawn made the diagnosis unambiguous: the render fork child runs `unshare(CLONE_NEWNS) → mount(/, MS_REC|MS_SLAVE) → self-bind sandbox root → chdir → pivot_root → umount2(., MNT_DETACH)` and immediately fails at `openat("/dev/null")`, with **zero** `/dev`/`/proc`/`/sys` bind mount syscalls in between. A debug printf in `SetRenderSandboxPropertyNweb` confirmed `cfgSize=0` in the nwebspawn fork child (while the normal appspawn fork child had `cfgSize=1` and mounted everything fine).

The main `appspawn` (root) is unaffected and loads the config successfully, which is why regular apps sandbox correctly and only webview renders break.

### Fix

Added to `device/board/oniro/hybris_generic/utils/deploy-lxc-container.sh`, immediately after the rootfs tarball is extracted on-device:

```bash
# Make sandbox configs world-readable so nwebspawn (uid 3081, not root) can
# load them at preload time. Upstream appdata_sandbox_fixer.py installs these
# with mode 0660 which umask trims to 0640; on a real OHOS image fs_config
# rewrites them, but our LXC rootfs keeps the literal mode.
adb shell "echo $DEVICE_PASSWORD | sudo -S chmod 0644 \
    /home/phablet/openharmony/rootfs/system/etc/sandbox/appdata-sandbox.json \
    /home/phablet/openharmony/rootfs/system/etc/sandbox/appdata-sandbox-isolated.json"
```

No source patch is needed — the config file content is fine, only its mode was wrong.

### Red Herrings Ruled Out During Debugging

- **`OpenGLWrapper: Failed to load GLES library using dl open`** — upstream OHOS bug in `foundation/graphic/graphic_2d/frameworks/opengl_wrapper/src/EGL/egl_wrapper_entry.cpp:304`: hardcoded `/system/lib64/libGLESv3.so` (real path is `platformsdk/libGLESv3.so`). The `FindBuiltinWrapper` fall-through uses `EglWrapperLoader::GetProcAddrFromDriver` which works regardless. Benign; unrelated to the webview failure.
- **`MUSL-LDSO: load libGLESv1_CM.so.1 / libGLESv2.so.2 failed`** — chromium's GL binding loader in ArkWebCore tries standard Linux `.so.N` sonames as one of several fallbacks. The `libEGL.z.so → libEGL_impl.so → libhybris` dispatcher path succeeds, so these failed probes are cosmetic. Benign.
- **Phase 3 `DoMkSandbox` / `IsEnableSandbox` / `SetServiceEnterSandbox` disable in container mode** — these are init-level switches and don't reach the appspawn fork-time sandbox code path that was actually broken. Initially suspected, ruled out.

### Verification

After `chmod 0644` + container restart:
- `HYBRIS_DBG SetRenderSandboxPropertyNweb enter pid=... cfgSize=1` (was 0)
- 19 `IsValidMountConfig PASS` entries covering `/dev`, `/proc`, `/sys`, `/system/fonts`, `/system/etc`, `/system/bin`, `/system/lib`, `/system/lib64`, `/vendor/lib`, `/vendor/lib64`, `/system/app/NWeb`, `/data/app/el1/bundle/public/<arkWebPackageName>`, etc.
- No more `open dev_null error: 2`
- `io.ionic.starter` webview renders its HTML content.

**Status:** FIXED (2026-04-10). Deploy-script chmod committed; clean `libappspawn_sandbox.z.so` rebuilt and deployed (no residual debug prints). Webview confirmed working on Volla X23.

