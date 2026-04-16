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
| 8 | [System Stability](phase8_system_stability.md) | 🔄 In Progress | Unlock, tap, and display composition bugs fixed (8.1–8.9, 8.12–8.18); power button shutdown fixed (systemd-logind `HandlePowerKey=ignore` + `DoSuspend()` container bypass); Bug 8.16: all layers start as CLIENT composition; Bug 8.17: eglSwapBuffers/eglDestroySurface race fixed (rwlock in libhybris EGL); Bug 8.18: webview/nweb render fixed via `chmod 0644` on sandbox configs (nwebspawn uid 3081 couldn't read root-only files); Bug 8.11 (`SetLayerAlpha` UAF) not yet fixed |
| 9 | [Volla Tablet (mimir) Bring-Up](phase9_volla_tablet_mimir.md) | ✅ Complete | Kernel adaptation, Android 13 compatibility (all via libhybris hooks — no binary patching), display + touch working on 1600×2560 tablet |
| 10 | [WiFi Support](phase10_wifi_support.md) | ✅ Complete | Native OHOS WiFi stack via HDI WPA path (not legacy wifi_hal_service); host/Android daemon conflict resolution; `chip_interface_service` + `wpa_host` HDF config; MediaTek `GetChipCaps` SIGSEGV fix; WiFi scan + connect + DHCP + DNS working |
| 11 | [Power Off & Backlight](phase11_power_off_and_backlight_plan.md) | 🔄 In Progress | Fix 1 (backlight sysfs write via `composer_host` + `DAC_OVERRIDE`) ✅ deployed and verified on X23; Fix 2 (container shutdown/reboot propagation via `/ohos-host-action` flag + `lxc.hook.post-stop`) implemented and built, on-device verification pending |
| 12 | [User-File Access via `sharefs`](phase12_sharefs_user_files.md) | ⚠️ Workaround deployed / proper fix pending | Normal apps (VLC etc.) see `/storage/Users` via an LXC bind `nosharefs/docs` → `sharefs/docs` because the Halium 5.10 kernel has no `sharefs` driver; `storage_user_path.json` patched to mode `0755` on docs dirs so `ExternalFileManager` can enumerate. 12.1 (2026-04-16): normal-app file picker via MediaLibrary (VLC "Open File") — `accountmgr` patched to mark user 100 verified in container mode (works around `hmdfs` mount ENODEV → `UnlockUser` failure from storage_daemon), and `storage_user_path.json` now also creates `/storage/cloud/<userId>/files` so MediaLibrary's `ROOT_MEDIA_DIR` check passes. **TODO:** port `fs/sharefs/` and `fs/hmdfs/` from OHOS linux-6.6 onto the X23/mimir 5.10 kernel (Phase-2-style work) and revert both workarounds |
| 13 | [Audio Support](phase13_audio_support.md) | ✅ 13B Complete (2026-04-16) | Native ALSA via alsa-lib: `audio_host` + `libaudio_render_adapter.z.so` + `libasound` + `/dev/snd/pcmC0D0p`, MT6789 DAPM route bring-up in `vendor_render.c::RenderInitImpl`, blocking PCM, period-time-before-buffer-time, start_threshold=period_size, `snd_ctl_close` leak-fix — and the final ingredient: **`snd_pcm_close`+`snd_pcm_open` on every Start in `RenderStartImpl`**. Root cause of silence: MT6789 AFE/ASoC hidden per-session state that only clears on a fresh PCM open — `test_audio` (single-shot open) was always audible, persistent `audio_host` handle was silent with byte-identical state (mixer, hw/sw params, DAI state, DAPM). Confirmed audible end-to-end with the OHOS sample music player. 13A (libhybris→MTK HAL) stays stashed as fallback. |

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

**8.18:** Webview (ArkWeb / nweb render) failed to load in any webview app (e.g. `io.ionic.starter`). Symptom: render child spawn loop with `appspawn_common.c:350 open dev_null error: 2`. Root cause: upstream `appdata_sandbox_fixer.py` installs `/system/etc/sandbox/appdata-sandbox.json` with mode 0640 root:root. Production OHOS images' fs_config rewrites this; our LXC rootfs copy preserves it literally. `nwebspawn` runs as uid 3081 and hits EACCES in `GetJsonObjFromFile()`, so `appSandboxCJsonConfig_` stays empty, `SetRenderSandboxPropertyNweb` no-ops, `pivot_root` lands in an empty sandbox dir, and `SetFileDescriptors` fails at `open("/dev/null")`. Fix: `chmod 0644` on both `appdata-sandbox.json` and `appdata-sandbox-isolated.json` in `deploy-lxc-container.sh` after rootfs extraction. No source patch needed.

### Phase 9 — Volla Tablet (mimir) Bring-Up
Adapts the `hybris_generic` target to support the Volla Tablet (codename mimir, MT8781 ≈ MT6789, aarch64). The tablet shares the Helio G99 SoC with the X23, so all HAL bridges and libhybris code carry over. Kernel adapted from X23 patches with minor fixes (`-Wundef` removal, 2 context mismatches). Three Android 13-specific issues resolved entirely via libhybris hooks (no binary patching or stub libraries): (1) `SIGBUS` in `libunwindstack.so` — CallStack C++ symbols hooked as no-ops; (2) `SIGABRT` in bionic `emutls_init` — `__ctype_get_mb_cur_max` hook bypasses emutls; (3) IPC namespace mismatch — `start-ohos.sh` dynamically switches to `lxc.namespace.share.ipc = android`. Additional fixes: `ro.hardware.egl=meow→mali`, `/dev/access_token_id` + `/dev/input/*` permissions via boot job, `hdf_devhost.cfg` with HYBRIS env vars. All changes backward-compatible with X23 (Halium 12). Boot animation plays 150 frames at 30 fps, lockscreen visible, touch + hardware keys working on physical 1600×2560 display.

### Phase 10 — WiFi Support
Brings WiFi connectivity using the native OHOS WiFi stack via the HDI WPA path (`wifi_feature_with_hdi_wpa_supported = true`), not the legacy `wifi_hal_service` cRPC bridge. The OHOS container shares the host network namespace, so `wlan0` (MediaTek gen4m-6789) is directly accessible. Key fixes: (1) conflicting host `wpa_supplicant` masked via systemd, Android `wificond`/`wlan_assistant` stopped via `setprop ctl.stop`; (2) UHDF `device_info.hcs` updated with `chip_interface_service` (Chip HDI v2.0), `wpa_host` (WPA HDI), and `CAP_NET_ADMIN`/`CAP_NET_RAW`/`CAP_DAC_OVERRIDE`/`CAP_DAC_READ_SEARCH` capabilities on `wifi_host`; (3) `/dev/rfkill` bind mount added to LXC config; (4) `GetChipCaps`/`WifiGetSupportedFeatureSet` in `wifi_ioctl.cpp` stubbed to avoid SIGSEGV — MediaTek's vendor ioctl (`SIOCDEVPRIVATE+1`) returns `"UNSUPPORTED"` string causing memcpy from garbage pointer. DHCP and DNS work automatically in the shared network namespace. WiFi scan, connect, and internet access confirmed from OHOS Settings UI (ping google.com: 0% loss, ~16ms RTT).

### Phase 11 — Power Off & Backlight
Two tightly related bugs left over from the Phase 8.15 power-button fix: (1) a short power-button press blanked the composition but left the LCD backlight lit, and (2) "Power off" from the OHOS menu only stopped the LXC container, leaving the Ubuntu Touch host running.

**Fix 1 — Backlight (✅ deployed + verified on Volla X23, 2026-04-10):** the display composer VDI's `SetDisplayBacklight` / `GetDisplayBacklight` were no-op stubs because HWC2 does not expose panel backlight on MediaTek Halium. Replaced with a sysfs writer that probes `/sys/class/leds/lcd-backlight/brightness` → `/sys/class/backlight/panel0-backlight/brightness` → `…/panel1-backlight/brightness`, reads `max_brightness` once, and scales OHOS's 0–255 level into the kernel range. A belt-and-braces `WriteBacklight(0)` in `HybrisDisplay::SetDisplayPowerStatus(POWER_STATUS_OFF)` turned out to be load-bearing (the OHOS suspend path doesn't call `SetDisplayBacklight(0)` separately). `composer_host` runs as uid 3036 and the sysfs node is `system:system 0664`, so `DAC_OVERRIDE` was added to its caps in `device_info.hcs` alongside the existing `SYS_NICE`. Brightness slider, auto-dim, and power-button blank all verified via `power-shell display -s … / power-shell suspend / power-shell wakeup`.

**Fix 2 — Container → host shutdown propagation (implemented + built, on-device verification pending):** inside an LXC PID namespace, `reboot()` from container-init just terminates the namespace — the host never powers off or reboots. Added a static helper `WriteHostShutdownRequest()` in `base/startup/init/services/modules/reboot/reboot.c` (guarded by `InContainerMode()`) that writes `"poweroff"` or `"reboot"` to `/ohos-host-action` before the existing reboot syscall. `/ohos-host-action` is a bind mount of `/run/ohos-host-action` on the host, (re)created fresh by `start-ohos.sh` on every container start. A new `lxc.hook.post-stop = /home/phablet/openharmony/ohos-post-stop.sh` reads the flag after the container exits and execs `systemctl poweroff` / `systemctl reboot`; empty flag → exit 0 (handles crashes, manual `lxc-stop`, and host-initiated teardown so those paths stay safe). `deploy-lxc-container.sh` now pushes the new hook script. Deviation from the original plan: no tmpfiles.d drop-in — inline `: > /run/ohos-host-action` in `start-ohos.sh` (same pattern already used for the logind `HandlePowerKey=ignore` drop-in) is simpler and races-free against container start.

### Phase 12 — User-File Access via `sharefs`
Normal apps couldn't see picker-granted files in `/storage/Users`: their sandbox mount is sourced from `sharefs/docs`, which on stock OHOS is produced by the in-kernel `sharefs` driver stacking onto `nosharefs/docs` with per-URI filtering. Our Halium 5.10 kernel has no `sharefs` driver, so `sharefs/docs` is empty and picker URIs resolve to `ENOENT`.

**Workaround deployed (2026-04-15):** LXC-time bind `nosharefs/docs` → `sharefs/docs` (`storage_daemon::MountSharefs` early-exits on `IsPathMounted(dst)`, so the bind survives); `storage_user_path.json` patched to `mode: "0755"` on the four `{no,}sharefs/docs{,/currentUser}` entries so the sandboxed `ExternalFileManager` can enumerate them. Trade-off: `UriPermMgr` filtering is bypassed — every app with `FILE_ACCESS_COMMON_DIR` sees all of `/storage/Users`.

**Proper fix (TODO):** port `fs/sharefs/` from OHOS linux-6.6 onto the X23/mimir 5.10 kernel (same Phase-2-style work as `hilog`/`accesstokenid`/binder token-id), then revert the LXC bind. See phase12 doc.

### Phase 13 — Audio Support (13B native ALSA COMPLETE on Volla Tablet mimir, 2026-04-16)

**Final state (Phase 13B, 2026-04-16):** ✅ OHOS speaker playback is audible end-to-end. `audio_host` + `libaudio_render_adapter.z.so` + `libasound` drive `/dev/snd/pcmC0D0p` directly (MT6789 `Playback_1`); MT6789 DAPM route bring-up in `vendor_render.c::RenderInitImpl` (ADDA_DL_CH{1,2} on, HPL/HPR Mux="Audio Playback", Ext_Speaker_Amp Switch off→on toggle); PCM opened blocking with period_time-before-buffer-time and start_threshold=period_size; and the final ingredient: **`snd_pcm_close`+`snd_pcm_open` on every Start in `RenderStartImpl`**. The sample music player (`ohos.samples.distributedmusicplayer` playing `dynamic.wav`) is audible through the built-in speaker at stereo 44.1 kHz S16_LE.

**Root cause of the silence that took the bisection to untangle:** when `audio_host` holds the PCM open across render sessions (its upstream design), the MT6789 AFE/ASoC enters a per-session state where `hw_ptr` advances at 44.1 kHz, `snd_pcm_state` reports `RUNNING`, DAPM widgets are On, the codec backend reports `start`, and verified peak=19601 PCM samples land in the ring buffer — yet no audio reaches the speaker. The stream-session state was not observable through any userspace interface (`/proc/asound`, `/sys/kernel/debug/asoc`, `strace ioctl`, `amixer contents` — all byte-identical to the audible `test_audio` run). Closing and reopening the PCM on every Start sidesteps the state entirely. See `phase13_audio_support.md` for the full bisection log (test_audio → overwrite-sine → state diff → strace → close+reopen).

Work delivered (all committed in-tree, not stashed):
- `vendor/oniro/hybris_generic/hals/audio/product.gni` — `drivers_peripheral_audio_feature_alsa_lib = true` (mirrored in `config.json`)
- `vendor/oniro/hybris_generic/hals/audio/alsa_adapter.json` + `alsa_paths.json` — MT6789-accurate adapter + scene paths
- `vendor/oniro/hybris_generic/hdf_config/uhdf/device_info.hcs` — `caps = ["DAC_OVERRIDE","SYS_NICE"]` on `audio_host`
- `device/board/oniro/hybris_generic/audio_alsa/{common.h,vendor_render.c,vendor_capture.c}` — MTK render/capture vendor hooks. `RenderInitImpl` drives the DAPM route bring-up + Ext_Speaker_Amp Off→On. `RenderStartImpl` close+reopens the PCM (the silence fix).
- `device/board/oniro/hybris_generic/audio_alsa/test_audio/{BUILD.gn,test_audio.c}` — standalone libasound smoke-test binary deployed at `/system/bin/test_audio`; kept as a standing diagnostic tool.
- `device/board/oniro/hybris_generic/bundle.json` — added `alsa-lib` to `component.deps.third_party` (required for the test_audio build target to pass `check_deps_handler`).
- `device/board/oniro/hybris_generic/utils/lxc/config` — `/dev/snd rbind` entry
- `device/board/oniro/hybris_generic/utils/start-ohos.sh` — PulseAudio mask
- `device/board/oniro/hybris_generic/utils/ohos-post-stop.sh` — PulseAudio unmask on teardown
- `drivers/peripheral/audio/supportlibs/alsa_adapter/src/alsa_snd_render.c` — open PCM **blocking** (was `SND_PCM_NONBLOCK`, silent frame drops on EAGAIN), set `period_time` *before* `buffer_time` (matches `aplay`, gives 4×125ms periods instead of 2×250ms), `start_threshold = period_size` (was `buffer_size`).
- `drivers/peripheral/audio/supportlibs/alsa_adapter/src/alsa_soundcard.c::SndElementWrite` — `snd_ctl_close` on all return paths (was leaking a ctl handle on every mixer write, accumulating 25+ `/dev/snd/controlC0` fds in `audio_host`). Without this leak-fix the close+reopen would also leak ctl handles.

Open items (Phase 13B):
- Capture-side smoke test (mic recording). `vendor_capture.c` exists but is untested. Expect the same fix class: may also need close+reopen on `CaptureStart` and DAPM wake-up for `Mic Type Mux`/`PGA L/R Mux`.
- Headset-jack detect: the 13A poll thread on `/sys/class/switch/h2w/state` was tied to the VDI plugin; native-ALSA equivalent not yet wired.
- Per-scene routing: `alsa_paths.json` is authored, but upstream `alsa_snd_render.c::RenderSelectSceneImpl` stubs it; wire it when ringtone/voice paths are exercised.
- Volume slider from Settings — confirm it reaches the `Lineout Volume` kcontrol.

**13A (libhybris → MTK HAL) history (kept as fallback):**


Brings primary-output playback, primary-input capture, volume, routing, and wired-headset detection to the OHOS container. No kernel-side HDF audio driver exists on Halium 5.10, so the default community path (`drivers_peripheral_audio_feature_community = true`) is unusable. The plan flips the flag, rebuilds `libaudio_primary_impl_vendor.z.so` as a VDI dispatcher that `dlopen`s a plugin, and ships the plugin from `device/soc/oniro/hybris_generic/hardware/audio/` as a libhybris bridge onto `/android/vendor/lib64/hw/audio.primary.mt6789.so`.

**Status (2026-04-15):** all code + LXC/deploy changes landed, full build green, deployed + booted on Volla X23 — HAL loads, adapter enumerates, `audio_host` stable while idle. **First-playback write still blocked by an MTK aurisys NULL-deref SIGSEGV (Bug 13.A).**

Work completed:
- VDI plugin `libaudio_primary_impl.z.so`: `IAudioManagerVdi` (single `primary` adapter), `IAudioAdapterVdi` (route/mute/voice-volume/extra-params + `CreateRender`/`CreateCapture`), `IAudioRenderVdi` (Start/Stop/Pause/Resume/Flush/Drain, Get{Latency,RenderPosition}, volume + mute, extra-params pass-through), `IAudioCaptureVdi` (symmetric read path + EC-frame stub).
- Headset detection (13.6) via a worker thread polling `/sys/class/switch/h2w/state` (extcon fallback) and dispatching `AUDIO_VDI_EXT_PARAM_KEY_STATUS` param callbacks.
- LXC config: `/dev/snd` rbind, `/android/vendor/etc/audio_{param,policy_*,effects,device}*` absolute-path binds; deploy script creates rootfs placeholders.
- `start-ohos.sh` masks host PulseAudio before container start; `ohos-post-stop.sh` unmasks on container teardown.
- `audio_host` in `device_info.hcs` gains `caps = ["DAC_OVERRIDE","SYS_NICE"]` (matches Phase 11 `composer_host` pattern).
- Deploy script symlinks `/vendor/lib64/libaudio_primary_impl.z.so → passthrough/libaudio_primary_impl.z.so` because `innerapi_tags = [ "passthrough" ]` forces the subdir but the VDI dispatcher's `HDF_LIBRARY_FULL_PATH` resolves to the parent dir.

Deviations from the original plan (all documented in `phase13_audio_support.md`):
1. No link-time dep on libhardware/libhilog — `deps_guard` Passthrough rule refused the deps; instead the plugin dlopens `libhardware.z.so` at runtime and logs via `fprintf(stderr, …)`. Resulting `NEEDED` list is just `libc.so` + `libc++.so`.
2. Patched upstream `vdi_src/audio_render_vdi.c` typo (`rendrId` vs `renderId`, 15 occurrences in a single file).
3. Patched upstream `vdi_src/audio_manager_vdi.c` — `SetMaxWorkThreadNum` had C++ scope-resolution syntax in a `.c` file; stubbed to `(void)count`.
4. Registered our plugin in `developtools/integration_verification/tools/deps_guard/rules/Passthrough/passthrough_info.json` so `check_depends_on_passthrough` accepts it.

Open on-device items (13.7): `audio_host` boots clean, `IAudioManager::GetAllAdapters` returns `primary`, `idl_render`/`idl_capture` smoke tests, volume key integration, 30-minute music-loop RSS monitoring, verification that `libhdi_audio_pnp_server` consumes our jack-state callbacks or needs additional plumbing.

**Pivot (2026-04-15):** 13A is now frozen in place as a fallback while Phase 13B — native ALSA via OHOS's existing `drivers_peripheral_audio_feature_alsa_lib` path — replaces it. 13B drives `/dev/snd/pcmC0D*` directly via `libasound`, skipping the MTK HAL and its DSP middleware entirely. The MTK kernel ALSA card (`mt6789-mt6366`) is already present and visible from the container; OHOS's upstream `drivers/peripheral/audio/supportlibs/alsa_adapter/` is the render/capture path; two reference products (rk3568, x86_general) exist as template. The delta to author is small: an MTK-accurate `alsa_adapter.json` + `alsa_paths.json`, a ~250-line `vendor_render.c` + ~150-line `vendor_capture.c` under `device/board/oniro/hybris_generic/audio_alsa/`, and product-gni flag flips. Full step-by-step plan in `phase13_audio_support.md` section "Phase 13B — Replacement plan". Trade-off: loses MTK's proprietary speaker protection, echo cancellation, noise suppression, and call-audio routing — none of which were working in 13A anyway. Preserves everything functional in 13A (stereo playback, mic capture, mixer volume). Stays valid unchanged when OHOS eventually boots natively with Android-as-guest.

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
