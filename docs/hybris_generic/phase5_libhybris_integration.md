# Phase 5: Libhybris Integration & HAL Bridge

> **Context (pre-read before starting):**
> - Device is Volla X23 (Halium 12 / Android 12, aarch64).
> - Android HAL ABI version: `android.hardware.graphics.mapper@4.0` passthrough, legacy `gralloc.common.so` loadable via `hw_get_module()`.
> - HWC: `hwcomposer.mtk_common.so` (HWC2 API).
> - **No `/dev/ion`**: the kernel 5.10 device uses `/dev/dma_heap/*` (DMA-BUF heaps). Plan around this.
> - Much of the build infrastructure is already written (see `third_party/libhybris/BUILD.gn`, `libhybris_args.gni`, `bundle.json`, and individual module `BUILD.gn` files). The items below complete and wire it together.

### 5.1 Clone `android-headers`
- [x] **Action:** Clone `Halium/android-headers` using the **`halium-11.0`** branch into `third_party/android-headers`.
  ```bash
  git clone -b halium-11.0 https://github.com/Halium/android-headers.git third_party/android-headers
  ```
- [x] **Action:** Verify the clone contains `hardware/hardware.h`, `hardware/gralloc.h`, `hardware/hwcomposer2.h`, and `EGL/egl.h`.
- **Deliverable:** `//third_party/android-headers` resolves all includes in `third_party/libhybris/BUILD.gn`.
- **Status & Notes:** Completed. Cloned `halium-11.0` into `third_party/android-headers`. Confirmed `hardware/hardware.h`, `hardware/gralloc.h`, `hardware/hwcomposer2.h` are present. Note: `EGL/egl.h` is not included in this repo — EGL headers come from the system/mesa headers already in the OHOS tree, which is sufficient. The `halium-11.0` hardware headers are stable for Android 11–12 HAL interfaces.

### 5.3 Enable HAL Bridge Modules in `BUILD.gn`
- [x] **Action:** In `third_party/libhybris/BUILD.gn`, uncomment and enable the following deps in the `libhybris` group:
  - `hybris/hardware:libhardware` — needed by gralloc and hwc2
  - `hybris/gralloc:libgralloc` — GPU buffer allocation via Android HAL
  - `hybris/hwc2:libhwc2` — display output via Android HWC2 HAL
- [x] **Action:** In `third_party/libhybris/hybris/egl/platforms/BUILD.gn`, add `hwcomposer` as a dep in the `platforms` group (currently only `null` and `ohos` are included).
- **Deliverable:** `libhardware.so`, `libgralloc.so`, `libhwc2.so`, and `libeglplatform_hwcomposer.so` appear in the build output.
- **Status & Notes:** Completed. Also created `third_party/libhybris/hybris/egl/platforms/hwcomposer/BUILD.gn` — the hwcomposer directory had source files and a `Makefile.am` but no GN build file. The new `BUILD.gn` builds `libhybris-hwcomposerwindow` and `eglplatform_hwcomposer` with the same deps as the upstream `Makefile.am`.

### 5.4 Select HWComposer as the EGL Platform
- [x] **Action:** In `third_party/libhybris/libhybris_args.gni`, change `hybris_default_egl_platform` from `"null"` to `"hwcomposer"`.
  - **Why:** The `"null"` platform initializes successfully but renders to nothing. `"hwcomposer"` is the correct backend for devices using Android HWC2.
- **Deliverable:** `libEGL.so` loads `libeglplatform_hwcomposer.so` at runtime.
- **Status & Notes:** Completed.

### 5.5 Register `libhybris` as a Product Component
- [x] **Action:** Add a `libhybris` entry to the `thirdparty` subsystem in `vendor/oniro/hybris_generic/config.json`:
  ```json
  { "component": "libhybris", "features": [] }
  ```
- **Deliverable:** The build system includes `libhybris` artifacts in the `hybris_generic` rootfs image.
- **Status & Notes:** Completed.

### 5.6 Build `libhybris` and Fix Compile Errors
- [x] **Action:** Attempt an isolated build of the libhybris target:
  ```bash
  sudo docker exec -u root -w /home/openharmony/workdir 8f7084d45c89 \
    ./build.sh --product-name hybris_generic --build-target "third_party/libhybris:libhybris" --ccache
  ```
  - **Note:** Use `third_party/libhybris:libhybris` (ninja label format, no leading `/`) as the build target.
- **Deliverable:** Zero build errors; `libhybris-common.so`, `libEGL.so`, `libhardware.so`, `libgralloc.so`, `libhwc2.so`, and test binaries are present in `out/hybris_generic/`.
- **Status & Notes:** Completed on first attempt — no compile errors. All artifacts confirmed in `out/hybris_generic/thirdparty/libhybris/`:
    - `libhybris-common.z.so`, `libhybris-platformcommon.z.so`, `libhybris-hwcomposerwindow.z.so`
    - `libEGL.z.so`, `libGLESv1_CM.z.so`, `libGLESv2.z.so`
    - `libhardware.z.so`, `libgralloc.z.so`, `libhwc2.z.so`
    - `libeglplatform_hwcomposer.z.so`, `libeglplatform_null.z.so`, `libeglplatform_ohos.z.so`
    - `libq.z.so` (Android Q linker module)
    - `test_egl`, `test_egl_configs`, `test_dlopen` binaries

### 5.7 Update LXC Config: Android Filesystem Bind Mounts
- [x] **Action:** Add the following to `device/board/oniro/hybris_generic/utils/lxc/config`:
  ```
  lxc.mount.entry = /system  android/system  none bind,ro,create=dir,optional 0 0
  lxc.mount.entry = /vendor  android/vendor  none bind,ro,create=dir,optional 0 0
  lxc.mount.entry = /odm    android/odm     none bind,ro,create=dir,optional 0 0
  lxc.mount.entry = /apex   apex            none rbind,ro,create=dir,optional 0 0
  ```
  - **Why `rbind` for `/apex`:** On Android 12, `libc.so`, `libm.so`, and `libdl.so` in `/system/lib64/` are dangling symlinks pointing into `/apex/com.android.runtime/lib64/bionic/`. Plain `bind` only mounts the top-level `/apex` directory, not the inner loop-device mount points for individual APEXes. `rbind` (recursive bind) propagates all sub-mounts so the symlinks resolve correctly inside the container.
  - **Why `/android/` prefix:** OpenHarmony ships its own `/system` and `/vendor`. The `/android/` prefix keeps the two stacks fully separated. The libhybris Q linker's default library search paths are `/android/vendor/lib64:/android/system/lib64`.
- [x] **Action:** Ensure mount point directories exist in the rootfs; added `mkdir -p /android/{system,vendor,odm}` to `deploy-lxc-container.sh`.
- **Deliverable:** Inside the running container, `ls /android/vendor/lib64/hw/gralloc.common.so` and `ls /apex/com.android.runtime/lib64/bionic/libc.so` both succeed.
- **Status & Notes:** Completed. Added all bind mounts and `mkdir -p` calls.

### 5.7a Resolve Hardcoded Android Library Paths
- [x] **Action (bind mount approach — no linker patching needed):** Android's `libEGL.so` (from `/android/system/lib64/`) uses hardcoded absolute paths to find EGL implementations and system wrapper libraries:
  1. It calls `access("/vendor/lib64/egl/libGLES_meow.so")` and `dlopen("/vendor/lib64/egl/libGLES_meow.so")` — paths without the `/android/` prefix.
  2. After loading the vendor EGL, it loads system EGL dispatch wrapper libs by soname `/system/lib64/libEGL.so`, `/system/lib64/libGLESv1_CM.so`, `/system/lib64/libGLESv2.so`.
  In both cases the bare paths fail because `/vendor/` and `/system/` in the container belong to OHOS.
- [x] **Fix:** Add targeted bind mounts exposing the Android blobs at exactly the paths Android code expects:
  ```
  # Android EGL implementation (libEGL.so uses hardcoded /vendor/lib64/egl/ paths)
  lxc.mount.entry = /vendor/lib64/egl  vendor/lib64/egl  none bind,ro,create=dir,optional 0 0

  # Android system EGL dispatch wrappers (loaded by libGLES_meow.so via /system/lib64/ path)
  # Safe: OHOS uses .z.so suffix, so these filenames do not conflict
  lxc.mount.entry = /system/lib64/libEGL.so       system/lib64/libEGL.so       none bind,ro,create=file,optional 0 0
  lxc.mount.entry = /system/lib64/libGLESv1_CM.so system/lib64/libGLESv1_CM.so none bind,ro,create=file,optional 0 0
  lxc.mount.entry = /system/lib64/libGLESv2.so    system/lib64/libGLESv2.so    none bind,ro,create=file,optional 0 0
  ```
- [x] **Fix:** Create the bind mount target files as placeholders in the rootfs:
  ```bash
  touch /home/phablet/openharmony/rootfs/system/lib64/libEGL.so
  touch /home/phablet/openharmony/rootfs/system/lib64/libGLESv1_CM.so
  touch /home/phablet/openharmony/rootfs/system/lib64/libGLESv2.so
  mkdir -p /home/phablet/openharmony/rootfs/vendor/lib64/egl
  ```
- [x] **Fix:** Create `/system/build.prop` in the container rootfs containing the combined Android vendor and system build props:
  ```bash
  cat /android/vendor/build.prop /android/system/build.prop > /rootfs/system/build.prop
  ```
  This is required because the libhybris property cache (`hybris/common/legacy_properties/cache.c`) reads from the hardcoded path `/system/build.prop` to resolve `ro.hardware.egl=meow` and `ro.board.platform=mt6789`.
- **Deliverable:** Android EGL/GLES libraries resolve correctly; `libGLES_meow.so` and its dependencies load without `ENOENT`.
- **Status & Notes:** Completed. Diagnosed with `HYBRIS_LD_DEBUG=4`. The linker patching approach was not needed — targeted bind mounts cleanly solve all observed path failures.

### 5.8 Handle `/dev/dma_heap` and GPU Device Nodes
- [x] **Action:** Add DMA-BUF heap and GPU device bind mounts to the LXC config:
  ```
  lxc.mount.entry = /dev/dma_heap  dev/dma_heap  none bind,create=dir,optional 0 0
  lxc.mount.entry = /dev/mali0     dev/mali0     none bind,create=file,optional 0 0
  lxc.mount.entry = /dev/dri       dev/dri       none rbind,create=dir,optional 0 0
  ```
  - `/dev/mali0` (character device `10:105`) — Mali GPU device node, required by `libGLES_mali.so`.
  - `/dev/dri/card0` — DRM/KMS device for display, required for HWC.
  - `rbind` for `/dev/dri` to include the `by-path/` sub-directory.
- [x] **Action:** Create mount point directories in the rootfs:
  ```bash
  mkdir -p /home/phablet/openharmony/rootfs/dev/dri
  ```
  (`/dev/mali0` as a file mount point: container's `autodev=1` creates the file automatically on device node creation, or create with `touch`.)
- [ ] **Action:** Investigate whether any loaded Android HAL calls `open("/dev/ion")` directly. MTK 5.10 kernel may expose a compat `/dev/ion` shim. If not, a `hooks.c` redirect to `/dev/dma_heap/system` may be needed.
- **Deliverable:** GPU buffer allocations succeed; `libGLES_mali.so` can open `/dev/mali0` inside the container.
- **Status & Notes:** `/dev/dma_heap`, `/dev/mali0`, and `/dev/dri` bind mounts added. Confirmed `/dev/mali0` is opened successfully by `libGLES_mali.so` during EGL initialization (visible in `/proc/<pid>/fd`). The `/dev/ion` question is deferred — no `ENOENT` on ion observed yet.

### 5.9 Deploy and Run `test_egl` (Smoke Test)
- [x] **Action:** Deploy fresh 64-bit rootfs via `deploy-lxc-container.sh` (resolved earlier arm/arm64 mismatch from stale on-device rootfs).
- [x] **Action:** Run `test_egl` inside the container.
- **Status & Notes:** **Substantially complete — EGL stack initializes, GPU opens, Mali loaded. Blocked on `eglCreateWindowSurface(NULL)` with hwcomposer platform.**

  **What works:**
  - All Android EGL/GLES libraries load cleanly (`libGLES_meow.so` → `libGLES_mali.so`).
  - EGL initializes: 12 threads are spawned (GPU context, worker threads, event threads).
  - `/dev/mali0` is opened and memory-mapped; multiple Mali GPU memory regions are allocated.
  - `/proc/ged` (MTK Graphics Engine Driver) is accessed successfully.
  - No crashes, no `dlopen` failures, no missing symbol errors.

  **What hangs:**
  - The `test_egl.cpp` test passes `(EGLNativeWindowType)NULL` to `eglCreateWindowSurface`. For the hwcomposer EGL platform, a valid `HWComposerNativeWindow*` is required — NULL is not a valid native window type.
  - The Mali driver enters a tight `clock_gettime(CLOCK_MONOTONIC)` busy-spin (user-space spin-wait, no ioctls) when given a NULL native window. This is Mali's spin-wait waiting for a fence/condition that never arrives without a real window and display pipeline.
  - This is a test harness issue, not a libhybris bug: `test_egl.cpp` was written for simpler platforms (null/wayland) that create an offscreen surface for `NULL`. The hwcomposer platform requires a real window object.

  **Next step (5.9a):** Run `test_egl_configs` first (no window surface creation) to confirm EGL config enumeration works. Then proceed directly to `test_hwcomposer` (5.10) which properly creates a `HWComposerNativeWindow` before calling `eglCreateWindowSurface`.

### 5.9a Run `test_egl_configs` (EGL Config Enumeration)
- [x] **Action:** Run `test_egl_configs` inside the container.
- **Status & Notes:** **Skipped / Blocked — same Mali spin-wait as test_egl.**
  - `eglInitialize` hangs in a userspace busy-spin (single thread, `wchan=0`, no syscall). 12 Mali worker threads spawn, confirming EGL init starts, but the main thread enters a `clock_gettime(CLOCK_MONOTONIC)` polling loop waiting for a vsync/display-sync signal that never arrives.
  - Root cause identical to 5.9: the hwcomposer EGL platform requires `HWComposerNativeWindow` (and thus a live HWC2 display client) BEFORE `eglInitialize` returns. Without one, Mali spins indefinitely.
  - `test_egl_configs` is not a valid smoke test for the hwcomposer EGL platform. **Proceed directly to `test_hwcomposer` (5.10)**, which creates the window before EGL init.

### 5.10 Deploy and Run `test_hwcomposer` (Display Validation)
- [x] **Action:** Build `test_hwcomposer` — multiple fixes required (see below).
- [x] **Action:** Deploy to device and run inside the OHOS LXC container.
- **Status & Notes:** **COMPLETE — full EGL + OpenGL ES 3.2 render loop confirmed working on physical display (720×1560, MTK HWC2). Re-verified after full hybris_generic rebuild (2026-03-19).**

  #### Build fixes applied
  The following source and build file changes were necessary to compile `test_hwcomposer`:

  1. **`hybris/hwc2/BUILD.gn`** — two defines added:
     - `HAS_HWCOMPOSER2_HEADERS=1`: `hwc2.c` is entirely `#if HAS_HWCOMPOSER2_HEADERS`-gated; without this define the library compiled to empty (only `_init`/`_fini`) and exported no symbols.
     - `RTLD_LAZY=1`: `libhybris_config` adds `hybris/include/hybris/common` to include paths, causing `#include <dlfcn.h>` inside `hwc2.c` to resolve to hybris's custom `dlfcn.h` (which doesn't define `RTLD_LAZY`) instead of musl's. Defining it avoids linker errors without changing source.

  2. **`hybris/tests/BUILD.gn`** — `test_hwcomposer` `ohos_executable` target added with sources `test_hwcomposer.cpp` + `test_common.cpp`, all required deps (`libhwc2`, `libhybris-hwcomposerwindow`, `libsync`, `libGLESv2`, `libEGL`), and `HAS_HWCOMPOSER2_HEADERS=1` / `USE_HWCOMPOSER=1` defines.

  3. **`third_party/android-headers/android-version.h`** — bumped `ANDROID_VERSION_MAJOR` from `11` to `12`. The `halium-11.0` headers defined version 11 but the physical device runs Android 12 (SDK 32). This was needed to activate Android 12 code paths in libhybris.

  4. **`hybris/libsync/sync.c`** — relaxed condition from `(ANDROID_VERSION_MAJOR >= 10) && (ANDROID_VERSION_MAJOR < 12)` to `(ANDROID_VERSION_MAJOR >= 10)`. The `< 12` upper bound excluded `sync_get_fence_info` and `sync_file_info_free` declarations on Android 12, causing compile errors.

  5. **`hybris/tests/test_common.cpp`** — added Android 12 bypass in `create_hwcomposer_window()`:
     ```cpp
     #if ANDROID_VERSION_MAJOR >= 12
       return create_hwcomposer2_window();
     #else
       /* legacy path: hw_get_module("hwcomposer", ...) version detection */
     #endif
     ```
     On Android 12, `hw_get_module_by_class` internally calls `android_load_sphal_library` which uses Android linker namespace APIs (`android_create_namespace`) incompatible with the hybris linker context, causing an immediate SIGSEGV. Skipping this and going directly to `create_hwcomposer2_window()` avoids the crash.

  #### Runtime findings — hwc2_compat_device_new spin (resolved)

  `create_hwcomposer2_window()` calls `hwc2_compat_device_new()` which communicates with the HWC2 composer via HIDL. This was initially hanging because the composer HIDL service was not registered with hwservicemanager.

  - **Two LXC containers share IPC namespace**: The `android` LXC container (running Android HALs) and the `openharmony` LXC container share the same IPC namespace (`ipc:[4026534369]`). The host Ubuntu Touch `hwservicemanager` is the binder context manager for `/dev/hwbinder`.
  - **Composer service binary exists**: `/android/vendor/bin/hw/android.hardware.graphics.composer@2.1-service` is present and runnable.
  - **Fix**: Start `android.hardware.graphics.composer@2.1-service` from within the Android LXC container before running `test_hwcomposer`. The service registers `android.hardware.graphics.composer@2.1::IComposer/default` with hwservicemanager, and `hwc2_compat_device_new` then returns immediately (no spin).
  - **Display confirmed**: Power mode ON, display resolution 720×1560 confirmed via `hwc2_compat_device_new`.

  #### gralloc-mapper fix (resolved)

  After `hwc2_compat_device_new` succeeded, `test_hwcomposer` crashed with:
  > `FATAL EXCEPTION: gralloc-mapper is missing`

  **Root cause**: `GraphicBufferMapper::getInstance()` (C++11 magic static, initialized on first GPU buffer operation from a Mali worker thread) calls `IMapper::getService("default", true)` → `android_load_sphal_library("/vendor/lib64/hw/android.hardware.graphics.mapper@4.0-impl-mediatek.so")`. The path `/vendor/lib64/hw/` does not exist inside the OHOS LXC container, so the load fails silently and the mapper aborts.

  **Two compounding issues, two fixes:**

  1. **`access()` check before dlopen**: `hw_get_module_by_class` checks file existence via `access("/vendor/lib64/hw/...")` before calling `dlopen`. The `access()` syscall cannot be hooked by libhybris. The path must physically exist.

  2. **SPHAL namespace path rejection**: `android_load_sphal_library` calls `android_dlopen_ext` with the SPHAL namespace, which has `permitted_paths = /vendor/lib64/...`. After libhybris path-remapping (`/vendor/` → `/android/vendor/`), the resulting path no longer matches the namespace `permitted_paths`, causing silent failure even when the file exists.

  **Fix 1 — LXC bind mount** (`device/board/oniro/hybris_generic/utils/lxc/config`):
  ```
  # Android gralloc/HIDL mapper HAL implementations at /vendor/lib64/hw/
  # Required for GraphicBufferMapper (libui) to load the gralloc passthrough mapper via
  # android_load_sphal_library("/vendor/lib64/hw/android.hardware.graphics.mapper@4.0-impl*.so").
  # Both access() path-existence checks (in hw_get_module) and dlopen calls need the path to exist.
  lxc.mount.entry = /vendor/lib64/hw  vendor/lib64/hw  none bind,ro,create=dir,optional 0 0
  ```
  This makes `/vendor/lib64/hw/` accessible inside the container so `access()` succeeds and dlopen can find the file via the Android linker.

  **Fix 2 — bypass SPHAL namespace** (`hybris/common/hooks.c`, `_hybris_hook_android_dlopen_ext`):
  ```c
  /* Use _android_dlopen instead of _android_dlopen_ext to bypass namespace
   * permitted_path checks. The SPHAL namespace has /vendor/lib64/... in its
   * permitted_paths, but in our OHOS container Android libs are at /android/vendor/.
   * After path remapping the path no longer matches the namespace, causing silent
   * failure. Bypassing the namespace is safe in our single-container context. */
  void *h = _android_dlopen(filename, flag);
  if (!h && extinfo) {
      /* Fallback: try with namespace extinfo in case it's needed for linking */
      h = _android_dlopen_ext(filename, flag, extinfo);
  }
  ```

  **Fix 3 — `android_load_sphal_library` hook** (`hybris/common/hooks.c`):
  Added `_hybris_hook_android_load_sphal_library` to intercept `libvndksupport.so`'s PLT entry:
  ```c
  void* _hybris_hook_android_load_sphal_library(const char* filename, int rtld_flags)
  {
      void *handle = _android_dlopen(filename, rtld_flags);
      return handle;
  }
  ```
  Filename here already has the `/vendor/` prefix (not yet remapped); the bind mount makes it resolvable directly without remapping.

  #### Pre-loading the gralloc mapper (test_hwcomposer.cpp)

  To ensure the mapper library is already in the hybris linker's table when `libhidlbase`'s `android_load_sphal_library` tries to load it, `test_hwcomposer.cpp` pre-loads it via `android_dlopen` at startup:
  ```cpp
  static const char *gralloc_mapper_paths[] = {
      "/android/vendor/lib64/hw/android.hardware.graphics.mapper@4.0-impl-mediatek.so",
      "/android/vendor/lib64/hw/android.hardware.graphics.mapper@4.0-impl.so",
      NULL
  };
  for (int i = 0; gralloc_mapper_paths[i]; i++) {
      void *h = android_dlopen(gralloc_mapper_paths[i], RTLD_LAZY | RTLD_GLOBAL);
      if (h) break;
  }
  ```

  #### GL function access (test_hwcomposer.cpp)

  `glGetString(GL_VERSION)` via the `libGLESv2.z.so` dispatch layer caused a crash due to bionic TLS mismatch. Fix: use `eglGetProcAddress` to resolve GL functions directly from Mali EGL, bypassing the dispatch layer:
  ```cpp
  typedef const GLubyte* (*PFNGLGETSTRINGPROC_t)(GLenum);
  PFNGLGETSTRINGPROC_t my_glGetString =
      (PFNGLGETSTRINGPROC_t)eglGetProcAddress("glGetString");
  version = (const char *)my_glGetString(GL_VERSION);
  ```

  #### Confirmed working configuration

  **Environment variables** (set before running `test_hwcomposer`):
  ```bash
  LIBEGL=/android/vendor/lib64/egl/libGLES_mali.so
  LIBGLESV2=/android/vendor/lib64/egl/libGLES_mali.so
  HYBRIS_LD_LIBRARY_PATH=/android/vendor/lib64:/android/system/lib64
  LD_LIBRARY_PATH=/system/lib64/libhybris:/system/lib64
  ```

  **Result**: Full EGL + OpenGL ES 3.2 render loop completes (shader compile, 61200 frames, `eglSwapBuffers` — display shows animated pattern on physical screen), prints `stop`. GL_VERSION: `OpenGL ES 3.2 v1.r32p1-01eac0.394145956bc7cd8e697b330aba11e3d3` (Mali GPU).

- **Deliverable:** ✓ Visual confirmation that the Hybris bridge drives the physical display via the MTK HWC2 Android HAL.

  #### LXC config additions (already applied)
  The following are in `device/board/oniro/hybris_generic/utils/lxc/config` and active on device:
  ```
  lxc.mount.entry = /dev/__properties__ dev/__properties__ none bind,create=dir,optional 0 0
  lxc.mount.entry = /vendor/lib64/hw    vendor/lib64/hw    none bind,ro,create=dir,optional 0 0
  ```

  #### NDEBUG and eglMakeCurrent fix (discovered 2026-03-19)

  After a full product rebuild, `test_hwcomposer` regressed to `"vertex shader not compiled"`. Root cause: the OHOS release build defines `-DNDEBUG`, which causes `assert(expr)` to expand to `((void)0)` without evaluating `expr`. The original code was:
  ```cpp
  assert(eglMakeCurrent((EGLDisplay) display, surface, surface, context) == EGL_TRUE);
  ```
  With NDEBUG, `eglMakeCurrent` was never called. Since the GL context was never made current, `glCreateShader` returned 0 (no current context error).

  **Fix** (`hybris/tests/test_hwcomposer.cpp`): call `eglMakeCurrent` as a separate statement:
  ```cpp
  EGLBoolean mc = eglMakeCurrent((EGLDisplay) display, surface, surface, context);
  assert(mc == EGL_TRUE);
  ```
  **Note on ninja caching:** When rebuilding a targeted binary (`--build-target third_party/libhybris/hybris/tests:test_hwcomposer`), the newly compiled binary lands in `out/hybris_generic/thirdparty/libhybris/test_hwcomposer`. The `packages/phone/system/bin/test_hwcomposer` copy is only updated during a full packaging run. `deploy-lxc-container.sh` now overlays test binaries from `thirdparty/libhybris/` to ensure the latest version is always deployed.

---
