# Hybris Generic: OpenHarmony on Volla X23 (LXC)

Detailed roadmap for running OpenHarmony 6.1 rootfs as an LXC container on the Volla X23 (MT6789, aarch64) using libhybris for Android HAL integration.

---

## Phase Overview

| Phase | Title | Status | Details |
|-------|-------|--------|---------|
| 1 | [Infrastructure & Target Setup](phase1_infrastructure_target_setup.md) | ✅ Complete | Build system setup, `init` container-mode patches, SELinux disabled |
| 2 | [Kernel Adaptation (Volla X23)](phase2_kernel_adaptation.md) | ✅ Complete | OHOS kernel config, HDF patches, binder/accesstokenid drivers, build automation |
| 3 | [Core Service Stability](phase3_core_service_stability.md) | ✅ Complete | `hilogd` containerization, sandbox bypass, socket creation |
| 4 | [Deployment & Automation](phase4_deployment_automation.md) | ✅ Complete | Deploy scripts, LXC config with binder/ashmem bind mounts |
| 5 | [Libhybris Integration & HAL Bridge](phase5_libhybris_integration.md) | ✅ Complete | Android headers, HWC2/gralloc build, EGL smoke tests, `test_hwcomposer` confirmed working |
| 6 | [Graphics Stack & RenderService Adaptation](phase6_graphics_stack.md) | 🔄 In Progress | Display/buffer VDIs, EGL impl symlinks, RenderService bring-up, boot animation, launcher/lockscreen visible |
| 7 | [Input System Integration](phase7_input_system.md) | ✅ Complete | `/dev/input` rbind, `CAP_DAC_OVERRIDE` for libinput `O_RDWR` opens, touch confirmed on physical display |
| 8 | [System Stability](phase8_system_stability.md) | 🔄 In Progress | Unlock, tap, and display composition bugs fixed (8.1–8.9, 8.12–8.17); power button shutdown fixed (systemd-logind `HandlePowerKey=ignore` + `DoSuspend()` container bypass); Bug 8.16: all layers start as CLIENT composition; Bug 8.17: eglSwapBuffers/eglDestroySurface race fixed (rwlock in libhybris EGL); Bug 8.11 (`SetLayerAlpha` UAF) not yet fixed |
| 9 | [Volla Tablet (mimir) Bring-Up](phase9_volla_tablet_mimir.md) | ✅ Complete | Kernel adaptation, Android 13 compatibility (all via libhybris hooks — no binary patching), display + touch working on 1600×2560 tablet |
| 10 | [WiFi Support](phase10_wifi_support.md) | ✅ Complete | Native OHOS WiFi stack via HDI WPA path (not legacy wifi_hal_service); host/Android daemon conflict resolution; `chip_interface_service` + `wpa_host` HDF config; MediaTek `GetChipCaps` SIGSEGV fix; WiFi scan + connect + DHCP + DNS working |

---

## Phase Summaries

### Phase 1 — Infrastructure & Target Setup
Establishes the `hybris_generic` product target in the OHOS build system. Patches `init` to skip privileged operations (filesystem mounting, device node creation, SELinux) that fail inside an LXC container. Adds `InContainerMode()` detection via the `container=lxc` environment variable.

### Phase 2 — Kernel Adaptation (Volla X23)
Ports OpenHarmony 6.1 kernel requirements to the Volla X23 kernel (`linux-5.10`, MT6789). Adds OHOS staging drivers (`hilog`, `hievent`, `accesstokenid`, `blackbox`), enhances the binder driver with OHOS AccessTokenID and transaction tracking, applies HDF patches, and automates the full kernel build and deployment via scripts in `device/board/oniro/hybris_generic/kernel/x23/`.

### Phase 3 — Core Service Stability
Fixes `hilogd` for container operation (no cgroups, run as root), globally disables mksandbox for services that cannot use Linux namespaces inside LXC, and verifies UNIX socket creation paths.

### Phase 4 — Deployment & Automation
Creates `deploy-lxc-container.sh` for one-command rootfs packaging and deployment. Establishes the baseline LXC configuration with bind mounts for binder, ashmem, kmsg, and required environment variables (`container=lxc`, `OHOS_RUNTIME_CONFIG=1`).

### Phase 5 — Libhybris Integration & HAL Bridge
Builds the full libhybris stack (EGL, GLESv1/v2, gralloc, hwc2, hardware) against Android 12 headers. Configures the HWC2 EGL platform, resolves Android library path issues via targeted LXC bind mounts and SPHAL namespace hooks, handles DMA-BUF heaps and Mali GPU device nodes. Culminates in `test_hwcomposer` producing a confirmed OpenGL ES 3.2 render loop on the physical 720×1560 display via the MTK HWC2 Android HAL.

Key fixes:
- `android_load_sphal_library` hook to bypass SPHAL namespace path checks
- `rbind /apex` for correct bionic symlink resolution
- `/vendor/lib64/hw` bind mount + gralloc mapper pre-load
- NDEBUG-safe `eglMakeCurrent` call pattern

### Phase 6 — Graphics Stack & RenderService Adaptation
Implements the two OHOS display VDI libraries that bridge the OHOS graphics stack to Android HALs via libhybris:

- **`libdisplay_composer_vdi_impl.z.so`** — wraps `libhybris-hwc2` (HWC2 HIDL) to implement `IDisplayComposerVdi`
- **`libdisplay_buffer_vdi_impl.z.so`** — wraps `libhybris-gralloc` to implement `IDisplayBufferVdi`
- **EGL impl symlinks** — `libEGL_impl.so` etc. point to libhybris EGL, which loads `libGLES_mali.so`
- **`HYBRIS_EGLPLATFORM=ohos`** env for `render_service`; `ohos_window.cpp` fixed for correct `ANativeWindowBuffer` layout

Critical bugs found and fixed:
- **6.8.1** — Android/OHOS binder context collision: dedicated `ohos-binder` binderfs devices created
- **6.8.2** — samgr `CanRequest()` UID fallback + `build_selinux=false` to remove all container-hostile SELinux checks
- **6.8.3 / 6.9C** — `composer_host` SIGABRT (recursive static init) + mutex deadlock in `RegHotPlugCallback`
- **6.9A** — Android EGL bind mounts poisoning the OHOS library namespace (removed)
- **6.10D** — IPC `GetTokenType()` UID fallback for Android→OHOS binder calls
- **6.10E** — Cross-process gralloc `Mmap`: `hybris_gralloc_import_buffer` for IPC-marshalled handles
- **6.11** — `GetDefaultScreenId()` race fixed; hardware vsync pointer-comparison bug fixed; CLIENT composition deadlock fixed; **all 150 bootanimation frames play at 30 fps on physical display**
- **6.12** — `EGL_BAD_SURFACE` (benign, no-draw frame); `EGL_PLATFORM_OHOS_KHR` added to libhybris; `ohosws_DestroyWindow` vtable-offset SIGSEGV fixed
- **6.13** — HWC2 `native_handle_t` size mismatch + double-close FD bug; **first frame visible on display**
- **6.14** — BMS preinstall-config; `InitHapToken` token fix (`/dev/access_token_id` bind mount + `InContainerMode()` guard removed); seccomp workaround; `/android` app-sandbox bind mount; hybris env vars in `appspawn.cfg`; all 47 HAPs install, **launcher renders lockscreen on physical display**, boot animation exits cleanly

**Current status (as of 2026-03-27):** All primary display and launcher bring-up milestones complete. Boot animation plays 150 frames at 30 fps with hardware vsync. Launcher reaches `onPageShow`; lockscreen is visible on physical Volla X23 display and votes `persist.window.boot.inited`. Open items: seccomp workaround (`persist.init.debug.seccomp.enable=0`) is not permanent.

### Phase 7 — Input System Integration
Bind-mounts `/dev/input` (`rbind`) into the container. Grants `CAP_DAC_OVERRIDE` to `multimodalinput` via a late-loaded init cfg overlay (`z_multimodalinput_caps.cfg`) so libinput can open `root:android_input 0660` nodes with `O_RDWR`. Key fixes: `CAP_DAC_READ_SEARCH` (originally planned) is insufficient for `O_RDWR` — `DAC_OVERRIDE` is required; cfg override filename must sort after `multimodalinput.cfg` in inode order to win the MUSL init merge. All five event devices (`event0`–`event4`) enumerated; touch and hardware keys confirmed working on physical 720×1560 display.

### Phase 8 — System Stability
Investigates and fixes post-unlock regressions.

**8.1–8.3:** Three EGL bugs fixed: `com.ohos.systemui` SIGSEGV on `OhosNativeWindow` teardown (Mali permanent incRef / double-free — fixed with map-ownership `incRef` + `decRef`-based `freeBuffers`); render_service `EGL_BAD_SURFACE` retry loop (zero-dimension surface — geometry resolution moved into virtual `width()`/`height()` methods); `eglCreateImageKHR` `EGL_BAD_PARAMETER` (`EGL_NATIVE_BUFFER_OHOS` → `EGL_NATIVE_BUFFER_ANDROID` translation + buffer pointer lookup via `g_bufferLookup` map). Swipe-to-unlock now works end-to-end.

**8.5:** Two tap-after-unlock crashes fixed: `allocator_host` `SEGV_ACCERR` in `_hybris_hook_readdir` (memcpy of 256 bytes crosses page boundary — fixed with `strnlen`); `composer_host` SIGABRT in `writeNativeHandleNoDup` (use-after-free: `HybrisLayer::SetLayerBuffer` freed `HybrisNativeBuffer` before `Composer::execute()` read its `native_handle_t*` — fixed by storing the buffer in `currentLayerBuffer_` until the next `SetLayerBuffer` call).

**8.7:** Display interface type corrected from `DISP_INTF_HDMI` to `DISP_INTF_MIPI` so the Volla X23 panel is classified as `BUILT_IN_TYPE_SCREEN`.

**8.8–8.10:** Composition oscillation (status bar / nav bar flicker + RSRenderThread SIGSEGV). Root cause: the OHOS VDI calls `SetLayerCompositionType` between `validateDisplay` and `acceptDisplayChanges`, violating the HWC2 spec and triggering EGL surface destruction races in RSRenderThread. Five approaches tried, all failed. Current deployed code calls `acceptDisplayChanges` immediately after `validateDisplay` in `PrepareDisplayLayers` (before returning to render_service), eliminating the spec-violation window. Only correct general fix requires rebuilding `libhwc2_compat_layer.so` with the `hwc2_compat_display_get_changed_composition_types` C wrapper (requires Android bionic toolchain, not currently available).

**8.11:** `composer_host` SIGSEGV in `SetLayerAlpha` after ~46 minutes — race between `DestroyLayer` freeing `hwc2_compat_layer_t*` and a concurrent `SetLayerAlpha` IPC thread. Fix (per-display reader-writer lock) not yet applied.

**8.12–8.13:** Persistent 1Hz flicker (status bar / nav bar alternating visible/invisible with clock updates). Root cause: render_service resets all layers to DEVICE at the start of every frame; on frames where the HAL accepts DEVICE (`numTypes=0`), `acceptDisplayChanges` commits DEVICE and `presentDisplay` ignores the client target. Fixed with two mechanisms: (1) `stickyClientLayers_` set — once the HAL requests CLIENT for a layer it stays CLIENT until destroyed, preventing DEVICE↔CLIENT surface churn; (2) pre-validate CLIENT override — before `validateDisplay`, sticky layers are forced to CLIENT on the HAL so the HAL always commits CLIENT and the client target is always presented. 1Hz flicker eliminated.

**8.14:** Status bar/nav bar invisible after app transitions — originally planned to pre-register layers as sticky CLIENT conditionally. Superseded by Bug 8.16.

**8.15:** Short power button press caused host shutdown. `systemd-logind` on the Ubuntu Touch host had `HandlePowerKey=poweroff` — fixed by a persistent systemd oneshot service (`ohos-logind-powerkey.service`) that writes a logind drop-in with `HandlePowerKey=ignore` before logind starts. Power button now handled correctly by OHOS power manager.

**8.16:** Status bar/nav bar invisible after app transitions (revisited). Root cause: the MTK HAL accepts DEVICE for overlay layers indefinitely (`numTypes=0` on all frames), never requesting CLIENT. Bug 8.14's conditional pre-registration never triggered because `stickyClientLayers_` stayed empty. Fix: all new layers start as `COMPOSITION_CLIENT` and are immediately added to `stickyClientLayers_`. GPU composition is now guaranteed from the first frame for all layers. Trade-off: all composition is GPU-based (no hardware overlay optimization), acceptable on this hardware.

**8.17:** `com.ohos.systemui` SIGSEGV in Mali `eglSwapBuffers` when dismissing the dropdown panel. Root cause: race between `eglDestroySurface` (main thread, freeing Mali internal state) and `eglSwapBuffers` (RSRenderThread, using the freed state) — `NULL+0x1d8` dereference. Fix: added `pthread_rwlock_t` in `egl.c` — `eglSwapBuffers` takes a read lock, `eglDestroySurface` takes a write lock, ensuring destroy waits for in-flight swaps; also added `std::mutex` to `helper.cpp`'s `_surface_window_map` for thread safety.

### Phase 9 — Volla Tablet (mimir) Bring-Up
Adapts the `hybris_generic` target to support the Volla Tablet (codename mimir, MT8781 ≈ MT6789, aarch64). The tablet shares the Helio G99 SoC with the X23, so all HAL bridges and libhybris code carry over. Kernel adapted from X23 patches with minor fixes (`-Wundef` removal, 2 context mismatches). Three Android 13-specific issues resolved entirely via libhybris hooks (no binary patching or stub libraries): (1) `SIGBUS` in `libunwindstack.so` — CallStack C++ symbols hooked as no-ops; (2) `SIGABRT` in bionic `emutls_init` — `__ctype_get_mb_cur_max` hook bypasses emutls; (3) IPC namespace mismatch — `start-ohos.sh` dynamically switches to `lxc.namespace.share.ipc = android`. Additional fixes: `ro.hardware.egl=meow→mali`, `/dev/access_token_id` + `/dev/input/*` permissions via boot job, `hdf_devhost.cfg` with HYBRIS env vars. All changes backward-compatible with X23 (Halium 12). Boot animation plays 150 frames at 30 fps, lockscreen visible, touch + hardware keys working on physical 1600×2560 display.

### Phase 10 — WiFi Support
Brings WiFi connectivity using the native OHOS WiFi stack via the HDI WPA path (`wifi_feature_with_hdi_wpa_supported = true`), not the legacy `wifi_hal_service` cRPC bridge. The OHOS container shares the host network namespace, so `wlan0` (MediaTek gen4m-6789) is directly accessible. Key fixes: (1) conflicting host `wpa_supplicant` masked via systemd, Android `wificond`/`wlan_assistant` stopped via `setprop ctl.stop`; (2) UHDF `device_info.hcs` updated with `chip_interface_service` (Chip HDI v2.0), `wpa_host` (WPA HDI), and `CAP_NET_ADMIN`/`CAP_NET_RAW`/`CAP_DAC_OVERRIDE`/`CAP_DAC_READ_SEARCH` capabilities on `wifi_host`; (3) `/dev/rfkill` bind mount added to LXC config; (4) `GetChipCaps`/`WifiGetSupportedFeatureSet` in `wifi_ioctl.cpp` stubbed to avoid SIGSEGV — MediaTek's vendor ioctl (`SIOCDEVPRIVATE+1`) returns `"UNSUPPORTED"` string causing memcpy from garbage pointer. DHCP and DNS work automatically in the shared network namespace. WiFi scan, connect, and internet access confirmed from OHOS Settings UI (ping google.com: 0% loss, ~16ms RTT).

---

## Key Paths Reference

| Item | Path |
|------|------|
| OHOS rootfs (on device) | `/home/phablet/openharmony/rootfs/` |
| LXC config (on device) | `/var/lib/lxc/openharmony/config` |
| LXC config (source) | `device/board/oniro/hybris_generic/utils/lxc/config` |
| Display VDI source | `device/soc/oniro/hybris_generic/hardware/display/` |
| Libhybris source | `third_party/libhybris/` |
| Build artifacts | `out/hybris_generic/` |
| Deploy scripts | `device/board/oniro/hybris_generic/utils/` |
| Kernel (X23) | `device/board/oniro/hybris_generic/kernel/x23/` |
| Kernel (mimir) | `device/board/oniro/hybris_generic/kernel/mimir/` |
| Rootfs overlays | `device/board/oniro/hybris_generic/utils/rootfs_overlay/` |
