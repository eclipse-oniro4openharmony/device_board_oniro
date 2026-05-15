# Phase 13: Audio Support

> **Legacy (LXC-era) document.** Describes the original OHOS-as-LXC-container
> path, which is **no longer maintained** — the project now boots OHOS
> natively (no Ubuntu Touch host, no LXC). Kept as a reference for the HAL /
> driver bring-up detail (libhybris, graphics, audio, WiFi, …) that still
> applies under native boot. For current status start at [README.md](README.md).

## Status: ✅ Phase 13B WORKING on BOTH Volla Tablet (mimir, 2026-04-16) AND Volla X23 (2026-04-17) — OHOS speaker playback audible end-to-end

**X23 fix (2026-04-17):** the tablet-only hardcoded numids in `common.h` were wrong on X23 (kernel inserts extra codec controls earlier, shifting subsequent numids by +2), and the X23 uses a completely different speaker path than the tablet. Fixed with:

1. **Name-based mixer lookup** — all `SND_NUMID_*` defines in `common.h` set to 0 so `SetElementInfo` falls back to name lookup via `snd_ctl_elem_id_set_name`. Names are identical across both kernels; numids drift.
2. **X23 speaker path** (`DL1 → I2S3 → AW883xx smart PA`) added to `vendor_render.c::RenderInitImpl`: `I2S3_Out_Mux=Normal`, `I2S3_CH1 DL1_CH1=on`, `I2S3_CH2 DL1_CH2=on`, `aw_dev_0_prof=Music`, `aw_dev_0_switch=Enable` (with Disable→Enable DAPM toggle in Init + Start, Disable in Stop). Tablet speaker goes `ADDA → Lineout → Ext_Speaker_Amp` instead; on tablet the I2S3 path has no physical output (harmless) and the `aw_dev_0_*` kcontrols don't exist (silent no-op via `(void)SndElementWrite`). On X23 the `Ext_Speaker_Amp` DAPM widget actually feeds the headphone jack (LINEOUT L + Headphone L/R Ext Spk Amp), not the speaker — debugfs `asoc/mt6789-mt6366/dapm/Ext_Speaker_Amp` confirms. The X23 speaker path was discovered via `/sys/kernel/debug/asoc/mt6789-mt6366/aw883xx_smartpa.6-0034/dapm/Speaker_Playback_6_34`: `in "static" "I2S3"`.
3. **test_audio portability** — diagnostic tool updated to also use name-based lookup (same fix class).

**Tablet fix (one-line summary, 2026-04-16):** `snd_pcm_close` + `snd_pcm_open` on every `Start` in `device/board/oniro/hybris_generic/audio_alsa/vendor_render.c::RenderStartImpl`. All other Phase 13B infrastructure (flag flip, alsa_paths.json, DAPM route bring-up, blocking PCM, period-time-before-buffer-time, start_threshold=period_size) remains in place; the close+reopen is the final ingredient that makes OHOS playback audible.

**Root cause:** when `audio_host` keeps the PCM handle open across multiple render sessions (the upstream design — `RenderOpenImpl` happens once and lives for the lifetime of `audio_host`), the MT6789 AFE / ASoC stream-session state enters a configuration where `hw_ptr` advances, `snd_pcm_state` reports `RUNNING`, DAPM widgets are On, the codec backend reports `start`, and real PCM data (verified peak=19601 reaching `snd_pcm_writei`) is written to the kernel ring buffer — yet no audio reaches the speaker. A fresh `snd_pcm_open` (as `test_audio` does) under the exact same mixer/hw_params/sw_params configuration is audible. The stream-session state is not observable through any userspace API we exercised (`/proc/asound/card0/pcm0p/sub0/*`, `/sys/kernel/debug/asoc/.../dapm/*`, `Playback_1/state`, `amixer contents`, `strace ioctl` — all byte-identical between the silent and audible runs). Forcing a fresh PCM session on every Start sidesteps it.

13A libhybris bridge remains stashed as fallback.

**Phase 13B progress (2026-04-15 → 2026-04-16):**
- All planned 13B sub-steps (13B.1–13B.8) implemented + deployed. Build green.
- `audio_host` is stable, loads `libaudio_render_adapter.z.so` (community + alsa_lib path), opens `/dev/snd/pcmC0D0p`, sets hw_params successfully.
- DAPM bring-up enables `ADDA_DL_CH1 DL1_CH1`, `ADDA_DL_CH2 DL1_CH2`, `HPL/HPR Mux = "Audio Playback"`, `Ext_Speaker_Amp Switch = on` from `vendor_render.c::RenderInitImpl` so the MT6789 ASoC accepts hw_params (it would otherwise reject every Playback_X format combination with `no backend DAIs enabled`). Confirmed via `dmesg` and reproduced by failing `aplay` from the host without these writes.
- Several additional fixes landed in upstream `alsa_snd_render.c`: open the PCM in **blocking** mode (was `SND_PCM_NONBLOCK`, causing silent frame drops on EAGAIN per the `tryNum == 0 → return HDF_SUCCESS` path); set `period_time` *before* `buffer_time` (so the kernel honours `period_time` and returns 4×125 ms periods instead of 2×250 ms — matches `aplay`'s ordering); lower `start_threshold` from `buffer_size` to `period_size` (was forcing the app to fully refill the entire 500 ms buffer after every xrun before play could resume).
- DAPM toggle Off→On (rather than just-write-On) added to both `RenderInitImpl` and `RenderStartImpl` so the kernel's DAPM power sequencer fires whenever the `Ext_Speaker_Amp Switch` kcontrol is already in the requested state from a prior session.
- **Close + reopen PCM on every Start (2026-04-16, this is the fix that flipped silence → audible):** see `device/board/oniro/hybris_generic/audio_alsa/vendor_render.c::RenderStartImpl`. `audio_host` is designed to hold the PCM open across sessions, but on MT6789 that causes the speaker to stay silent even when every observable stream/mixer/DAPM state looks correct. Replacing the persistent handle with a fresh `snd_pcm_close` + `snd_pcm_open` on every Start makes every session behave like `test_audio` (which always worked). Also landed: fd-leak fix in `alsa_soundcard.c::SndElementWrite` (`snd_ctl_close` on all return paths) — without this the reopen would also leak ctl handles.
- `SndElementWrite` control-handle leak fixed in `drivers/peripheral/audio/supportlibs/alsa_adapter/src/alsa_soundcard.c:1225` — the function opened a new `snd_ctl_t` per write and never closed it, accumulating 25+ `/dev/snd/controlC0` fds in `audio_host` after a few minutes of playback. Added `snd_ctl_close` on every return path.
- Music playback driven by `ohos.samples.distributedmusicplayer` on the Volla Tablet (mimir, 1600×2560, MT6789 + MT6366) plays `dynamic.wav` audibly through the built-in speaker with stereo 44.1 kHz S16_LE at `MUSIC` volume = 12/15.

### Bisection log that led to the fix

1. **Built `test_audio` (`device/board/oniro/hybris_generic/audio_alsa/test_audio/test_audio.c`)** — a standalone ARM binary that opens `/vendor/lib64/libasound.so` directly, programs the MT6789 DAPM route via `snd_ctl_elem_write` (numids 211, 226, 311, 312, 305 + Lineout/Headset volumes), opens `hw:0,0` with the same hw/sw params as `alsa_snd_render.c`, and writes a 440 Hz sine for 3 s. Audible → OHOS `libasound.so` + kernel path fully functional. **Bug is above the PCM write boundary.**
2. **Overwrite-sine hack in `RenderWritei`** — replaced the framework data with a known-good sine in the adapter just before `snd_pcm_writei`. Still silent. **Bug is not in the data the framework gives us** — the adapter+PCM-session itself is silent with loud valid samples.
3. **Full state diff** — captured `/proc/asound/card0/pcm0p/sub0/{status,hw_params,sw_params}`, `/sys/kernel/debug/asoc/mt6789-mt6366/{Playback_1/state,dapm/*,mt6358-sound/dapm/*}`, and `amixer -c 0` contents for the silent OHOS run and the audible `test_audio` run. Every captured field **byte-identical** except expected run-time variance (pid, trigger_time, hw/appl_ptr snapshots). Mixer, DAPM, DAI state all On/start.
4. **`strace -e trace=ioctl`** on both runs — `SNDRV_PCM_IOCTL_*` sequence virtually identical. No suspicious pause/drain cycles during playback (1 DRAIN, 1 PREPARE across 421 WRITEI_FRAMES).
5. **Reduced test_audio write granularity to 1024-frame chunks** to mimic OHOS's `AudioRenderSink::RenderFrame` 4 KiB cadence. Still audible — so the write size isn't the issue.
6. **Close+reopen PCM on every Start in `RenderStartImpl`** — immediately audible. Confirmed root cause: per-session hidden state on the MT6789 ASoC side that only clears on a fresh `snd_pcm_open`.

The `test_audio` binary remains deployed at `/system/bin/test_audio` on the rootfs as a standing diagnostic tool for future audio regressions (BUILD.gn + bundle.json entry are in-tree — see 13B.10 below).

## Status (older, superseded): ⚠️ Pivoting — Phase 13A (libhybris → MTK HAL bridge) blocked by Bug 13.A (aurisys SIGSEGV); superseded by **Phase 13B (native ALSA via alsa-lib)** — see bottom of document (2026-04-15)

**Summary of work done (2026-04-15):**
- Build-flag flip + VDI skeleton (13.1) ✅
- LXC mount entries, host PulseAudio mask/unmask, `audio_host` caps in HCS (13.2) ✅
- `IAudioRender` playback vtable (13.3) ✅
- `IAudioCapture` capture vtable (13.4) ✅
- `IAudioAdapter` routing/volume/mute/extra-params (13.5) ✅
- Headset jack poll thread wired to `RegExtraParamObserver` (13.6) ✅
- Full `./build.sh --product-name hybris_generic` passes; `libaudio_primary_impl.z.so` (31 KiB) + `libaudio_primary_impl_vendor.z.so` (VDI dispatcher rebuilt in dispatcher mode) both ship in the rootfs.
- **Deployed + booted on X23 (2026-04-15):** VDI dispatcher dlopens our plugin successfully, `audio_host` enters its HDF idle loop, `GetAllAdapters` returns `primary`, `/dev/snd`/`aurisys_config*.xml`/`audio_param/*` all bound correctly. **First `RenderFrame` call crashes inside `audio.primary.mt6789.so!new_aurisys_lib_manager+148` (SIGSEGV NULL deref) — tracked as Bug 13.A below; three fix attempts (XML overlay, aurisys config binds, `set_parameters` search) all confirmed ineffective. Next iteration: try `AUDIO_OUTPUT_FLAG_DIRECT|MMAP_NOIRQ` to bypass the mixer handler entirely, or stand up `vendor.mediatek.hardware.audio@7.1-service` inside the android LXC.**

**Deviations from the original plan:** four changes were needed to get the skeleton green before any on-device work. They are all recorded in the "Deviations" section below — bridge-layer choice (runtime `dlopen(libhardware.z.so)` instead of link-time `//third_party/libhybris/hybris/hardware:libhardware`), upstream typo + C-in-.c bugs patched in `vdi_src/`, install path + deps_guard plumbing (passthrough dir + `passthrough_info.json` entry + rootfs symlink).

---

## Overview

Brings audio playback, capture, volume, and headset routing to the OpenHarmony LXC container on Volla X23 (Halium 12) and Volla Tablet mimir (Halium 13). Like Phase 6 (display) but unlike Phase 10 (wifi), there is **no native OHOS path** that works on the Halium 5.10 kernel:

- The default OHOS audio HDI impl (`drivers_peripheral_audio_feature_community = true`, see `drivers/peripheral/audio/audio.gni:18`) is a kernel-HDF-ADM consumer — it talks to an in-kernel `/dev/audio_*_control` HDF driver via `hdf_io_service_if.h` (see `drivers/peripheral/audio/supportlibs/adm_adapter/src/audio_interface_lib_render.c:17`). The Halium 5.10 kernel has no such driver.
- Raw tinyalsa-direct would ignore the MTK vendor mixer paths, DSP tuning, headset-detect extn, volume curves, and call-audio routing that are baked into `/android/vendor/lib64/hw/audio.primary.mt6789.so`.

So we use the same bridge pattern as Phase 5/6: expose the Android legacy audio HAL (`<hardware/audio.h>` module plugin) to OHOS by implementing the OHOS VDI plugin that audio_host dlopens.

---

## Architecture

```
┌────────────────────────────────────────────────────────────────────────┐
│  OHOS apps (Music, Settings, Camera, Phone)                            │
└─────────┬──────────────────────────────────────────────────────────────┘
          │ AudioRenderer/AudioCapturer (NAPI / taihe)
          ▼
┌────────────────────────────────────────────────────────────────────────┐
│  audio_server  (SA 3001, uid audio)                                    │
│   ├─ audio_policy_service      (loads audio_policy_config.xml)         │
│   └─ audio_service             (stream mgmt, volume, focus)            │
│       └─ HDI sink / HDI source plugins:                                │
│          libmodule-hdi-sink.z.so, libmodule-hdi-source.z.so            │
└─────────┬──────────────────────────────────────────────────────────────┘
          │ HDI IPC via /dev/hwbinder (samgr + hdi-gen stubs v6.0)
          ▼
┌────────────────────────────────────────────────────────────────────────┐
│  audio_host (-i 7, uid audio_host)                                     │
│   libaudio_primary_driver.z.so                                         │
│      (hdi_service/primary/audio_manager_driver.c +                     │
│       audio_manager_service.c:35 dlopen(libaudio_primary_impl_vendor)) │
│          │                                                             │
│          ▼                                                             │
│   libaudio_primary_impl_vendor.z.so   ← REBUILT IN VDI MODE            │
│      (audio_manager_vdi.c:505 dlopen(libaudio_primary_impl))           │
└─────────┬──────────────────────────────────────────────────────────────┘
          │ VDI C-struct vtables (IAudioManagerVdi, IAudioAdapterVdi,
          │                       IAudioRenderVdi, IAudioCaptureVdi)
          │ (declared in drivers/peripheral/audio/interfaces/sound/v1_0/)
          ▼
┌────────────────────────────────────────────────────────────────────────┐
│  libaudio_primary_impl.z.so   ← NEW (device/soc/oniro/hybris_generic   │
│                                      /hardware/audio/)                 │
│                                                                        │
│  HybrisAudioManager  ─── HybrisAudioAdapter "primary" ──┬─ Render      │
│                                                          └─ Capture    │
│                                                                        │
│  uses hybris hardware.c → hw_get_module("audio.primary",…)             │
└─────────┬──────────────────────────────────────────────────────────────┘
          │ libhybris Q linker loads Android 12 bionic
          ▼
┌────────────────────────────────────────────────────────────────────────┐
│  /android/vendor/lib64/hw/audio.primary.mt6789.so                      │
│  (audio_hw_device + audio_stream_out + audio_stream_in vtables)        │
│  NEEDED: libtinyalsa, libhidlbase, android.hardware.audio@7.0,         │
│          vendor.mediatek.hardware.mtkpower@1.0,                        │
│          vendor.mediatek.hardware.audio@7.1,                           │
│          libalsautils, libladder, libaudioutils                        │
└─────────┬──────────────────────────────────────────────────────────────┘
          │ libtinyalsa ioctls
          ▼
   /dev/snd/pcmC0D0p …   (bound into container)
   /proc/asound/*        (already visible — proc is mounted)
          │
          ▼
   Linux kernel: sound/soc/mediatek/mt6789/… mt6366 codec
```

**Note the analogy to Phase 6:** `libdisplay_composer_vdi_impl.z.so` wraps `libhybris-hwc2` to implement `IDisplayComposerVdi`. Here, `libaudio_primary_impl.z.so` wraps `libhybris-hardware` (`hw_get_module`) to implement `IAudioManagerVdi`. Same linker namespace trickery applies (Phase 5.7a `/vendor/lib64/hw` bind, `_hybris_hook_android_load_sphal_library`, NDEBUG guards).

---

## Key decision points

### 13.D1 — Bridge layer: legacy HAL plugin vs HIDL 7.0 vs tinyalsa-direct

| Option | Pros | Cons | Verdict |
|--------|------|------|---------|
| (a) **Legacy `hw_get_module("audio.primary",…)` via libhybris** (recommended) | Same pattern as Phase 5/6, already prototyped in `third_party/libhybris/hybris/tests/test_audio.c`, minimal new libhybris code (wrap `<hardware/audio.h>`), the plugin `audio.primary.mt6789.so` is present, no extra Android HIDL service to start, MTK vendor mixer/DSP/routing paths preserved | Legacy plugin is formally deprecated in Android 12 (still fully functional, but MTK layers HIDL-only features on top via `vendor.mediatek.hardware.audio@7.1`), need to bind-mount MTK mixer configs | **Chosen** |
| (b) HIDL 7.0 via `android.hardware.audio@7.0-impl-mediatek.so` | Official Android 12 path; gets the MTK HIDL extensions for free | Requires starting `android.hardware.audio@7.0-service` inside the android LXC (like we did for `android.hardware.graphics.composer@2.1-service` in Phase 5.10), plus a full new libhybris HIDL-audio wrapper mirroring `hybris/hwc2/`. Much more code, two services to babysit | Reserve as fallback if (a) hits blockers (e.g., MTK vendor HIDL dep refuses to load without its service) |
| (c) tinyalsa-direct | No Android HAL, no libhybris, no namespace problems | Loses MTK mixer path config (jack detect, gain curves, echo cancellation), no routing between speaker/headset/earpiece, no call audio, silent `audioserver` policy-level RT handling; essentially re-implements a whole primary HAL | Rejected |

**Rationale for (a):** `audio.primary.mt6789.so` is a well-defined `HMI("audio_hw_hal", AUDIO_DEVICE_API_VERSION_CURRENT)` module. `hw_get_module` returns a `struct audio_module`, and `open_output_stream`/`open_input_stream` give us `audio_stream_out*` and `audio_stream_in*` whose `write()`/`read()` methods map 1:1 to `IAudioRenderVdi::RenderFrame` / `IAudioCaptureVdi::CaptureFrame`. Mirrors how `hwc2_compat_*` wraps HWC2 in Phase 6.

### 13.D2 — PulseAudio conflict on Ubuntu Touch host

UT ships PulseAudio + `pulseaudio-modules-droid-30` + `pulseaudio-modules-droid-hidl` (confirmed via `dpkg -l` during research). When the Lomiri session is up, PA auto-spawns and its droid module does `hw_get_module("audio.primary")` itself, opening the same MTK ALSA controls we want. Symptoms would mirror Phase 10.1: our container starts, `open_output_stream` succeeds but writes produce no sound, or `pcm_open()` returns `-EBUSY`.

**Mitigation** (same playbook as Phase 10.1, `start-ohos.sh`):

```bash
# Mask host PulseAudio before OHOS takes the audio HAL
systemctl --global mask pulseaudio.service pulseaudio.socket 2>&1 | tee -a $LOG_FILE
pkill -u phablet pulseaudio 2>/dev/null || true
```

And `lxc.hook.post-stop` / `ohos-post-stop.sh` should unmask + allow PA autospawn again so UT recovers cleanly when OHOS is stopped:

```bash
systemctl --global unmask pulseaudio.service pulseaudio.socket
```

Same pattern as `wpa_supplicant` masking in Phase 10.1 and logind drop-in in Phase 8.15.

### 13.D3 — Capabilities + SELinux

- SELinux is globally disabled via `build_selinux=false` (Phase 6.8.2). No policy work needed.
- `audio_host` in `vendor/oniro/hybris_generic/hdf_config/uhdf/device_info.hcs:210–213` lacks a `caps = [...]` line. `/dev/snd/pcmC0D*` is `system:audio 0660` on the UT host; audio_host gid list includes `audio` already, so 0660 access might be fine — **but** Android's `audio.primary.mt6789.so` opens mixer control nodes via `/dev/snd/controlC0` (`system:audio 0660`) and also `open()`s vendor bwc/ladder nodes via paths like `/dev/mtk_ladder` owned by `system:system`. Add `CAP_DAC_OVERRIDE` + `CAP_SYS_NICE` to `audio_host` in `device_info.hcs` — matches Android's `capabilities BLOCK_SUSPEND SYS_NICE` for `vendor.audio-hal` (see `/android/vendor/etc/init/android.hardware.audio.service.mediatek.rc:6`). `BLOCK_SUSPEND` is not available on our kernel and we handle suspend elsewhere (Phase 11).

---

## Sub-phase plan

### 13.1 — Build-flag flip + VDI skeleton

- [x] In `vendor/oniro/hybris_generic/hals/audio/product.gni`, add:
  ```gni
  drivers_peripheral_audio_feature_community = false
  ```
  This switches `drivers/peripheral/audio/hdi_service/primary_impl/BUILD.gn:21` to the `vdi_src/` branch, so `libaudio_primary_impl_vendor.z.so` becomes the VDI dispatcher that calls `dlopen(HDF_LIBRARY_FULL_PATH("libaudio_primary_impl"))` (`drivers/peripheral/audio/hdi_service/primary_impl/vdi_src/audio_manager_vdi.c:505`).
- [ ] Also disable community-only sibling targets that pull kernel-ADM deps. Check `drivers/peripheral/audio/hdi_service/BUILD.gn:24–31`, `drivers/peripheral/audio/hdi_service/event/BUILD.gn:41`, `drivers/peripheral/audio/audio_dfx/BUILD.gn:32`. We may need to keep `audio_capture_adapter` / `audio_render_adapter` out of the rootfs tarball to stop the old kernel path loading; confirm via `system_module_info.json` diff after rebuild. **(deferred — the kernel-ADM libs currently still build alongside the VDI dispatcher but are never loaded because the dispatcher dlopens our plugin directly; revisit if we see ADM init noise on-device.)**
- [x] Create `device/soc/oniro/hybris_generic/hardware/audio/` with the same layout as `hardware/display/`:
  ```
  hardware/audio/
  ├── BUILD.gn
  ├── include/
  │   └── hybris_audio_common.h
  └── src/
      ├── hybris_audio_manager.cpp   // IAudioManagerVdi vtable + AudioManagerCreateIfInstance
      ├── hybris_audio_adapter.cpp   // IAudioAdapterVdi vtable (primary adapter only)
      ├── hybris_audio_render.cpp    // IAudioRenderVdi vtable
      └── hybris_audio_capture.cpp   // IAudioCaptureVdi vtable
  ```
  Mirror `device/soc/oniro/hybris_generic/hardware/display/BUILD.gn`:
  - `subsystem_name = "oniro_soc_products"`, `part_name = "hybris_generic_soc"` (see the Phase 10.10.A install-metadata pitfall — must match the enclosing `bundle.json` component)
  - `output_name = "libaudio_primary_impl"` (no `.z.so` suffix in GN)
  - `install_images = [ chipset_base_dir ]` → deployed to `/vendor/lib64/`
  - deps on `//third_party/libhybris/hybris/hardware:libhardware`, `//third_party/libhybris/hybris/common:libhybris-common`
  - include `//third_party/android-headers` (already set up in Phase 5.1) — especially `<hardware/audio.h>` and `<system/audio.h>`
  - `defines = [ "ANDROID_VERSION_MAJOR=12", "RTLD_LAZY=1" ]` (same as display VDI, per Phase 5.6 findings)
- [x] `AudioManagerCreateIfInstance` returns a `struct IAudioManagerVdi*` whose `LoadAdapter("primary", &adapter)` calls `hw_get_module(AUDIO_HARDWARE_MODULE_ID, &mod)` then `audio_hw_device_open(mod, &hwdev)`. Use the exact code in `third_party/libhybris/hybris/tests/test_audio.c:30–90` as the starting point. **(implemented in `hybris_audio_manager.cpp`; uses the runtime-dlopen variant `HybrisHwGetModuleByClass` — see Deviation 1 below.)**
- **Deliverable:** `audio_host` boots without error, hilog shows `audio vdiManager load path/vendor/lib64/libaudio_primary_impl.z.so success`, `IAudioManager::GetAllAdapters` returns exactly one adapter named `primary`. **(Builds clean and exports `AudioManagerCreateIfInstance`; on-device verification pending — covered by 13.7.)**

### 13.2 — Device node + config bind mounts, host PA mask

- [x] Add to `device/board/oniro/hybris_generic/utils/lxc/config`:
  ```
  # ALSA device nodes — required by libtinyalsa inside audio.primary.mt6789.so
  lxc.mount.entry = /dev/snd  dev/snd  none rbind,create=dir,optional 0 0

  # MTK vendor audio configs — audio.primary.mt6789.so reads /vendor/etc/audio_param/*
  # at absolute paths (same trick as Phase 5.7a /vendor/lib64/hw)
  lxc.mount.entry = /android/vendor/etc/audio_param  vendor/etc/audio_param  none bind,ro,create=dir,optional 0 0
  lxc.mount.entry = /android/vendor/etc/audio_policy_configuration.xml  vendor/etc/audio_policy_configuration.xml  none bind,ro,create=file,optional 0 0
  lxc.mount.entry = /android/vendor/etc/audio_policy_volumes.xml        vendor/etc/audio_policy_volumes.xml        none bind,ro,create=file,optional 0 0
  lxc.mount.entry = /android/vendor/etc/audio_effects.xml               vendor/etc/audio_effects.xml               none bind,ro,create=file,optional 0 0
  lxc.mount.entry = /android/vendor/etc/audio_device.xml                vendor/etc/audio_device.xml                none bind,ro,create=file,optional 0 0
  ```
  (Corresponding placeholder dirs/files must be created in rootfs by `deploy-lxc-container.sh`, like Phase 5.7a `touch`ed `libEGL.so`.)
- [x] In `device/board/oniro/hybris_generic/utils/start-ohos.sh`, before `lxc-start`, add:
  ```bash
  # Phase 13: prevent UT PulseAudio from holding the MTK audio HAL
  if command -v systemctl >/dev/null; then
      systemctl --global mask pulseaudio.service pulseaudio.socket 2>&1 | tee -a $LOG_FILE || true
  fi
  pkill -u phablet -x pulseaudio 2>/dev/null || true
  ```
  And in `ohos-post-stop.sh` (Phase 11 Fix 2) add the corresponding `unmask`.
- [x] Add `CAP_DAC_OVERRIDE` + `CAP_SYS_NICE` to `audio_host` in `vendor/oniro/hybris_generic/hdf_config/uhdf/device_info.hcs:210` (sibling of `gid = [...]`), matching Phase 11's pattern for `composer_host`:
  ```hcs
  audio :: host {
      hostName = "audio_host";
      priority = 50;
      caps = ["DAC_OVERRIDE", "SYS_NICE"];
      gid = ["audio_host", "uhdf_driver", "root", "audio"];
      …
  ```
  Then rebuild HCS via `hc-gen -b` + `hc-gen -s` (Phase 10.4 recipe).
- **Deliverable:** inside the container, `ls -la /dev/snd/pcmC0D0p` succeeds, `cat /proc/asound/cards` shows `mt6789-mt6366`, `/vendor/etc/audio_policy_configuration.xml` resolves to the MTK config.

### 13.3 — Playback (IAudioRender)

- [x] Implement `IAudioAdapterVdi::CreateRender` (`hybris_audio_adapter.cpp`).
- [x] Implement `IAudioRenderVdi::RenderFrame` + lifecycle (`hybris_audio_render.cpp`). `Start` / `Resume` issue `set_parameters("standby=0")`; `Stop` / `Pause` fall back to `stream->common.standby()` when the HAL lacks a `pause` op.
- [x] `GetLatency` → `stream_out->get_latency`; `GetRenderPosition` → `stream_out->get_presentation_position`.
- [x] `SetVolume` / `SetVolumeWithRamp` → `stream_out->set_volume(left, right)`. Ramp falls back to plain `set_volume`.
- [ ] `ReqMmapBuffer` / `GetMmapPosition` — **deferred to 13.8** (stubs return `HDF_ERR_NOT_SUPPORT`).
- **Deliverable:** `hdc shell "LD_LIBRARY_PATH=/system/lib64/libhybris:/system/lib64 /system/bin/idl_render /data/test.wav"` plays a 44.1kHz stereo PCM file to the speaker.

### 13.4 — Capture (IAudioCapture)

- [x] Symmetric to 13.3: `CreateCapture` → `open_input_stream` (Android 12 eight-arg form); `CaptureFrame` → `stream_in->read(...)`.
- [x] `CaptureFrameEc` — stub forwarding the primary-frame read and zeroing the EC frame; full voip_tx=on integration deferred to on-device tuning.
- **Deliverable:** `idl_capture` records 5s from built-in mic into `/data/test_rec.pcm`.

### 13.5 — Routing, volume, mute (IAudioAdapter)

- [x] `IAudioAdapterVdi::UpdateAudioRoute`: walks `AudioRouteVdi::sinks`, builds a combined `audio_devices_t` bitmask and calls `hwdev->set_parameters("routing=<n>")` on the hwdev (not per-stream — matches the PulseAudio droid module pattern more closely than the original plan).
- [x] `SetMicMute` → `hwdev->set_mic_mute(hwdev, mute)`.
- [x] `SetVoiceVolume` → `hwdev->set_voice_volume(hwdev, volume)` (effective only under call mode; stubbed pass-through for v1).
- [x] `SetExtraParams` / `GetExtraParams` → passed through as `"<condition>=<value>"` to `hwdev->set_parameters` / `get_parameters`.
- **Deliverable:** plugging a wired headset reroutes active playback from speaker to headset (validated by ear).

### 13.6 — Headset detection (IAudioCallback)

- [x] **Adopted the polling alternative.** `AdapterRegExtraParamObserver` stores the caller's `IAudioCallbackVdi` and spawns a worker thread that polls `/sys/class/switch/h2w/state` (fallback `/sys/class/extcon/extcon0/state`) at 500ms, emitting the `"headset=<0|1>"` event via `ParamCallback(AUDIO_VDI_EXT_PARAM_KEY_STATUS, "headset", …)`.
- [ ] Feed into OHOS `libhdi_audio_pnp_server.z.so` (already configured in `device_info.hcs:238–245`) — **confirm on-device whether the pnp server consumes the VDI's param callback path or whether we need a separate `AUDIO_VDI_DEVICE_ADD`/`REMOVE` dispatcher.** Still open until 13.7.
- **Deliverable:** inserting a headphone triggers an OHOS UI toast + routes audio to headphones within ~500ms.

### 13.7 — End-to-end validation

**Status (2026-04-15): partial — HAL load confirmed, first playback blocked by MTK aurisys SIGSEGV (Bug 13.A below). All other infrastructure is functional.**

- [x] `libaudio_primary_impl.z.so` symlink visible at `/vendor/lib64/` after deploy + container restart.
- [x] `/dev/snd` tree visible inside container: `controlC0`, `timer`, and ~48 `pcmC0D*[cp]` nodes (mt6789-mt6366 card).
- [x] `/proc/asound/cards` shows `mt6789-mt6366` from inside the container.
- [x] `audio_host` starts successfully, VDI dispatcher loads our plugin (no `dlopen` errors in hilog).
- [x] Bind-mounted MTK audio configs (`/vendor/etc/audio_param/*`, `audio_policy_configuration.xml`, etc.) resolve correctly from inside the container.
- [ ] Play/record from OHOS Music app / via `idl_render` / `idl_capture` — **blocked by Bug 13.A**.
- [ ] Volume slider in Settings moves in response to hardware volume keys (Phase 7 input already surfaces KEY_VOLUMEUP/DOWN).
- [ ] Record-then-play loop: capture 3s from mic, play back — no glitches, no EAGAIN bursts.
- [ ] Stability: 30-minute music loop, monitor `audio_host` RSS (watch for leaks in the VDI-dispatcher path — `audio_manager_vdi.c:500` `AudioManagerReleaseDescs` has visible `mallopt(M_FLUSH_THREAD_CACHE)` calls, suggesting known leak risk).

#### Bug 13.A — MTK aurisys SIGSEGV on first playback write

**Symptom:** `audio_host` crashes on the first `RenderFrame` call with `SIGSEGV(SEGV_MAPERR)@0x0` (NULL pointer deref) at `audio.primary.mt6789.so!new_aurisys_lib_manager+148`. Three back-to-back crashes during `audio_server`'s init-time capability probe (pids 230, 1036, 1445) before HDF's respawn logic gives up; the fourth `audio_host` stays alive as long as nothing actually writes PCM frames.

**Root cause (confirmed via disassembly of `audio.primary.mt6789.so`):**

`create_aurisys_lib_manager` at `+0x150` calls `new_aurisys_lib_manager(x20, x19)` where `x20` is loaded from `controller + 16` — i.e. `controller->hifi3_config`. `new_aurisys_lib_manager` immediately does `ldr x8, [x22]` where `x22 = x0 = x20`, and since `controller->hifi3_config == NULL`, the load segfaults at PC offset +148.

The dispatch between `normal_config` (+8), `hifi3_config` (+16), and `rv_config` (+24) is driven by `[x19, #92]` — the MTK `AudioALSAPlaybackHandler` stores an `aurisys_type` byte at offset 92 of its own object, which is `1` (hifi3) for the Mixer handler. The hifi3 branch additionally checks the `.bss` capability byte at `[adrp(0x28b000) + 248]` bit 1 — if that bit is clear, the function errors out with a log line instead of dereferencing. In our environment the bit IS set, which means something in `init_aurisys_controller` (probably `.init_array` SoC-detection code) enabled hifi3 capability even though no matching `aurisys_config_hifi3.xml` was parsed to populate `controller->hifi3_config`.

Supporting evidence: `/android/vendor/etc/aurisys_config_hifi3.xml` doesn't exist on this device (only `aurisys_config.xml` and `aurisys_config_rv.xml` do). So the SoC has hifi3 capability reported in code but lacks the hifi3 DSP config data — inconsistent. On UT, PulseAudio's droid-card-30 presumably works around this because `module-droid-sink-30.c` opens a DIRECT/FAST stream whose `aurisys_type` is 0 (normal), not 1 (hifi3), and our OHOS → HAL flag translation routes through the Mixer handler which has type=1.

**Stack excerpt (most recent frame-by-frame):**
```
#00 audio.primary.mt6789.so  new_aurisys_lib_manager+148
#01 audio.primary.mt6789.so  create_aurisys_lib_manager+344
#02 audio.primary.mt6789.so  AudioALSAPlaybackHandlerBase::CreateAurisysLibManager+1072
#03 audio.primary.mt6789.so  AudioALSAPlaybackHandlerMixer::open+448
#04 audio.primary.mt6789.so  AudioALSAStreamOut::open+484
#05 audio.primary.mt6789.so  AudioALSAStreamOut::write+1904
#06 libaudio_primary_impl.z.so  OHOS::...::RenderRenderFrame+88        ← our plugin entry
#07 libaudio_primary_impl_vendor.z.so  AudioRenderFrameVdi+512         ← VDI dispatcher
#08 libaudio_stub_6.0.z.so  SerStubRenderFrame+408                     ← HDI IDL stub
```

**Reference — how Ubuntu Touch avoids this:** UT's `pulseaudio-modules-droid-30` loads `audio.primary.mt6789.so` via libhybris `hw_get_module("audio.primary")` — **exactly the same legacy HAL path we use** (confirmed by `ldd libdroid-util-30.so → libhardware.so.2 → libhybris-common.so.1`). Their `module-droid-hidl.so` is a ~32 KB dbus-only helper unrelated to the audio path. The one structural difference: UT runs PulseAudio in the **host** mount namespace where `/system/vendor → /vendor` is a stock Android symlink and MTK's hardcoded absolute paths resolve against the real vendor partition. We matched that layout (symlink + bind mounts) but the crash persists, so the divergence is NOT in filesystem visibility.

**Fix options (all direct workarounds tried; all ineffective):**

| # | Workaround | Status | Notes |
|---|------------|--------|-------|
| 1 | `MTK_AURISYS_FRAMEWORK_SUPPORT=yes → no` via single-file overlay bind of `AudioParamOptions_mgvi.xml` on top of the `audio_param` dir bind | ❌ Tried, no effect | Override file reaches container (`grep MTK_AURISYS` reads `"no"`) but HAL doesn't honor. Overlay left in place (cheap; keeps the code exercised). |
| 2 | Bind `/android/vendor/etc/aurisys_config{,_rv,_hifi3}.xml` over `/vendor/etc/…` so the aurisys config parser finds its XMLs at the absolute paths the binary hardcodes | ❌ Tried, no effect | Stack frames confirm crash is *before* any XML parse. Bind left in place as belt-and-braces. |
| 3 | `set_parameters("is_bypass_aurisys_lib=1")` on hwdev pre-open + stream post-open | ❌ Not applied | `strings` grep of the HAL binary shows `is_bypass_aurisys_lib` only as a *read-side* string (no `=1` / `=%d` companion) — set side silently ignores. Code reverted. |
| 4 | `/system/vendor → /vendor` symlink inside the rootfs, matching UT's native Android layout so MTK's hardcoded `/system/vendor/etc/...` paths resolve | ❌ Tried, no effect | Symlink verified (`ls /system/vendor/etc/aurisys_config.xml` resolves inside the container). Same crash offset — so the NULL isn't path-lookup either. Symlink left in place (zero cost; fixes a class of bugs even if not this one). |
| 5 | Force `AUDIO_OUTPUT_FLAG_DIRECT \| AUDIO_OUTPUT_FLAG_PRIMARY` in `CreateRender` to bypass `AudioALSAPlaybackHandlerMixer` (the specific handler that calls `CreateAurisysLibManager`) | ❌ Tried, no effect | Stack still shows `AudioALSAPlaybackHandlerMixer::open` — the MTK HAL's handler dispatch routes PRIMARY streams through the Mixer path regardless of the DIRECT flag. Code reverted. |
| 6 | Start `android.hardware.audio.service.mediatek` inside the `android` LXC (`/vendor/bin/hw/android.hardware.audio.service.mediatek &` via `lxc-attach`) so `IDevicesFactory/default` is registered on `hwservicemanager` — the HAL's `IMtkPower::tryGetService()` path resolves to mtkpower (PID 192, already running) but the hypothesis was that aurisys needs IDevicesFactory too | ❌ Tried, no effect | `lshal` confirmed `android.hardware.audio@7.0::IDevicesFactory/default` registered on PID 1724. OHOS container restarted; first `RenderFrame` still SIGSEGV'd at `new_aurisys_lib_manager+148`. So the NULL is NOT a `getService` result — the android audio service was stopped again to avoid double-HAL-load risk. |
| 7 | **Set `MTK_HIFIAUDIO_SUPPORT=""`** (disabled) in the AudioParamOptions_mgvi.xml overlay. Hypothesis: the hifi3 capability bit at `[0x28b0f8] bit 1` is derived from this XML flag during `init_aurisys_controller`. | ❌ Tried, no effect | Override verified present in container (`grep MTK_HIFIAUDIO` reads `""`). Same +148 crash. Bit is set by something else (most likely a `.init_array` SoC-detection constructor). |
| 8 | **Bind `aurisys_config.xml` (valid, 22 KB) over `aurisys_config_hifi3.xml`** so that the hifi3 XML parser — even if it runs — gets a valid input and populates `controller->hifi3_config` with a non-NULL object (even if schema-semantically wrong, better than NULL deref). | ❌ Tried, no effect | Same +148 crash. This confirms the hifi3 XML parser either isn't being called at all, or rejects the file as schema-incompatible and leaves the slot NULL. **Implication: populating the slot via config alone is impossible — the only routes forward are (a) clear the hifi3 capability bit so the error-log branch fires instead, or (b) make the playback handler report `aurisys_type != 1`.** |

**Remaining options for next iteration (now narrowed to two concrete paths):**

1. **Clear the hifi3 capability bit** at `.bss[0x28b0f8] bit 1` before the first `RenderFrame`. Disassembling `init_aurisys_controller` and the `.init_array` constructors of `audio.primary.mt6789.so` to find the writer is the fastest route. If the bit is set from reading a sysfs/proc node or a system property, we can change the input; if it's hardcoded from a compile-time SoC constant, we'd need to either skip aurisys entirely or patch the bss byte from our plugin post-`hw_get_module` via `dlsym`-d access (easy, since the HAL's module handle is in our hands).
2. **Redirect the mixer handler's `aurisys_type`** (byte at playback-handler+92) from `1` to `0` before first write. Either by setting a MTK param that steers the mixer handler through the normal aurisys lib, or — again — by post-patching the handler struct after `open_output_stream` returns. This requires knowing the handler struct layout (we have its `aurisys_type` offset, +92).

Both (1) and (2) are one-line runtime pokes from inside the plugin once we've mapped the right addresses. The NULL at `+148` is fully explained by the disassembly — remaining work is mechanical. A concrete path to a fix exists; it just requires another investigation session with the ELF disassembler, not more guessing.

**Reference — what does not work (ruled out):**
- HIDL vendor services (rows 6 — `IDevicesFactory`, `IMtkPower` both registered — same crash)
- Config XML bypass flags (rows 1, 7 — HAL doesn't honor either `MTK_AURISYS_FRAMEWORK_SUPPORT=no` or `MTK_HIFIAUDIO_SUPPORT=""`)
- Populating the hifi3 config slot via XML (rows 2, 8 — hifi3 XML parser either not called or rejects the input)
- Output flag tricks (row 5 — PRIMARY streams forced through Mixer regardless)
- Filesystem path alignment with UT (row 4 — `/system/vendor → /vendor` symlink in place)
- `set_parameters` with the `is_bypass_aurisys_lib` key (row 3 — no setter codepath)

---

## Deviations from the original plan (2026-04-15)

Four issues surfaced while bringing the skeleton to a green build.

### D1 — Runtime-`dlopen` of libhardware instead of link-time dep

**Plan said:** `deps += //third_party/libhybris/hybris/hardware:libhardware`, same as `test_audio.c`.

**Reality:** as soon as the plugin is tagged `innerapi_tags = [ "passthrough" ]` (required so its artifact actually installs into `vendor/lib64/passthrough/` and gets past `BaseInnerApi` rule checking), the stricter `Passthrough` rule kicks in and refuses any dep on a `system/lib64/` module that isn't itself tagged passthrough — libhardware isn't. The display VDI avoids this by depending on `libhwc2` instead of libhardware directly; there's no analogous `libhybris-audio` wrapper we can piggy-back on.

**Fix:** dropped the link-time dep and dlopen `libhardware.z.so` at runtime from inside `hybris_audio_manager.cpp` (`LoadHybrisHardware()` / `HybrisHwGetModuleByClass()`). Tries the lib in several paths (bare name, `/system/lib64`, `/system/lib`, bare-no-.z-suffix) so the plugin works regardless of whether the dispatcher is loaded from a namespaced linker or the default OHOS dynamic-linker. Added identical deps-hygiene for `libhilog` by replacing all `HDF_LOG*` macros with `fprintf(stderr, …)` — audio_host's stderr gets captured by hilogd so logs still show up in `hilog` output, just under `audio_host`'s PID rather than the `HybrisAudio` tag.

**Resulting NEEDED list** (`readelf -d libaudio_primary_impl.z.so`): only `libc.so` + `libc++.so`. Everything else is dlopen'd.

### D2 — Upstream `audio_render_vdi.c` typo (`rendrId` vs `renderId`)

`drivers/peripheral/audio/hdi_service/primary_impl/vdi_src/audio_render_vdi.c` declares local variables as `rendrId` but references `renderId` 15× times in the same functions. Compiles silently in the community (kernel-ADM) path because that path doesn't build this file; fails on first compile the moment `drivers_peripheral_audio_feature_community = false`. Fixed in place with a search-and-replace (`rendrId` → `renderId`) in the .c file only — the .h keeps its parameter name `rendrId` since function-param names don't have to match across decl/def.

### D3 — Upstream `audio_manager_vdi.c` C++ code in a `.c` file

The same `vdi_src/` branch has `SetMaxWorkThreadNum()` calling `OHOS::IPCSkeleton::GetInstance().SetMaxWorkThreadNum(count)` — C++ scope-resolution syntax in a file compiled as C. Stubbed out to `(void)count;` — the default IPC thread-pool (16 threads) is more than adequate for our single-adapter VDI bridge.

### D4 — `libaudio_primary_impl.z.so` install path

`ohos_shared_library` with `innerapi_tags = [ "passthrough" ]` installs to `vendor/lib64/passthrough/`. The VDI dispatcher, however, dlopens `HDF_LIBRARY_FULL_PATH("libaudio_primary_impl")` which expands to `/vendor/lib64/libaudio_primary_impl.z.so` (no `passthrough/` subdir). We can't change that macro without touching `hdf_core` for every other product. `deploy-lxc-container.sh` now adds a relative symlink (`/vendor/lib64/libaudio_primary_impl.z.so → passthrough/libaudio_primary_impl.z.so`) after rootfs assembly.

Also required: an entry for our plugin in `developtools/integration_verification/tools/deps_guard/rules/Passthrough/passthrough_info.json` (the display VDI has one, we didn't), otherwise `check_depends_on_passthrough` fails on our so with "should be add in file passthrough_info.json".

---

### 13.8 — (stretch) MMAP low-latency path

Only if 13.3–13.7 are stable. `ReqMmapBuffer` → `stream_out->create_mmap_buffer` → `AudioMmapBufferDescriptorVdi::memoryFd`. OHOS shares the fd over HDI binder; the consumer (`libmodule-hdi-sink.z.so`) mmaps and writes without the RenderFrame binder round-trip. Useful for AAudio-style apps.

---

## Anticipated bugs and lessons from prior phases

Be specific: grep for these when things break.

1. **`audio.primary.mt6789.so` HIDL dependency chain fails to load** — readelf shows NEEDED on `android.hardware.audio@7.0.so`, `vendor.mediatek.hardware.audio@7.1.so`, `vendor.mediatek.hardware.mtkpower@1.0.so`. These are pure HIDL marshalling libs (not services), so they should load as plain shared libs via libhybris. **BUT** their constructors may call `defaultServiceManager()` and try to talk to `hwservicemanager`, which mirrors the Phase 6.8.1 binder context collision. **Mitigation in advance:** boot audio_host with `HYBRIS_TRACE=3` and watch for `hwservicemanager` binder calls; if the HAL needs a vendor service (e.g., `vendor.mediatek.hardware.audio@7.1::IAudio/default`), start the corresponding binary inside the android LXC container (Phase 5.10 pattern).

2. **SPHAL namespace rejection of the audio HAL** — `audio.primary.mt6789.so` will load via `hw_get_module_by_class` which funnels through `android_load_sphal_library` (`third_party/libhybris/hybris/common/hooks.c`). Phase 5.7a already added a hook to bypass SPHAL `permitted_paths`; verify that hook path still fires for audio (same call site, different sub-namespace). No fix expected, but double-check with `HYBRIS_LD_DEBUG=4`.

3. **Android `/vendor/etc` path drift** — the MTK HAL does `fopen("/vendor/etc/audio_param/AudioParam.xml")` and similar (MTK tuning tool). Phase 5.7a's pattern (bind-mount absolute paths, `cat /android/vendor/build.prop /android/system/build.prop > /rootfs/system/build.prop`) must extend to `/vendor/etc/audio_*`. The plan in 13.2 already lists these binds.

4. **Binder context collision** — Phase 6.8.1 showed that OHOS and Android binder transactions can collide when they share `/dev/binder`. Audio doesn't use binder directly (the VDI plugin talks to Android via dlopen, not binder), BUT the MTK HAL's HIDL deps *do* use `/dev/hwbinder`. We already run `ohos-binder` binderfs dev nodes (Phase 6.8.1) — audio inherits that fix. Watch for `bad binder transaction` in hilog.

5. **`HDF_LIBRARY_FULL_PATH` resolution** — that macro expands to `HDF_CHIPSET_DIR/lib/<arch>/<name>.z.so` (or similar). Confirm the `install_images = [chipset_base_dir]` places `libaudio_primary_impl.z.so` in the path the macro resolves to at runtime. Same pitfall as Phase 10.10.A (wrong install_images → lib not deployed despite clean build).

6. **OHOS `audio_server` cached adapter list** — `audio_policy_service` reads `audio_policy_config.xml` once at startup and expects `primary` + `a2dp` + `remote` adapters. Our VDI returns only `primary`. `a2dp` entries will log `LoadAdapter(a2dp) failed` and the A2DP sink module won't init; that's fine and matches the current state on device (out of scope for v1). If audio_server refuses to start without all three, add a "stub" adapter mode.

7. **Sample-rate/format mismatch** — OHOS policy asks for 44100Hz (see `audio_policy_config.xml`), MTK primary HAL prefers 48000Hz. The HAL's `open_output_stream` mutates the passed `audio_config` to the supported value when called with a mismatch. Our VDI must honour the mutation and propagate it back via `GetSampleAttributes` so the framework resamples, OR accept only 48000Hz and let OHOS resample.

8. **`jemalloc M_FLUSH_THREAD_CACHE` crashes** — `audio_manager_vdi.c:97–101` calls `mallopt(M_FLUSH_THREAD_CACHE, 0)` gated on `CONFIG_USE_JEMALLOC_DFX_INTF`. If our rootfs uses a different musl/jemalloc combo, this may SIGABRT in `AudioManagerReleaseVdiDesc`. If seen, add a `defines += [ "CONFIG_USE_JEMALLOC_DFX_INTF=0" ]` override.

9. **Host PulseAudio autorespawn** — just like `wpa_supplicant` in Phase 10.1 and Android `wificond` — `systemctl stop` is not enough; `systemctl --global mask` is required, plus the user-level `pulseaudio.socket` auto-activates on client connect (`/etc/xdg/autostart/pulseaudio.desktop`). Mask both.

10. **Capability order** — Phase 7 taught us `CAP_DAC_READ_SEARCH` is insufficient for `O_RDWR` opens; use `CAP_DAC_OVERRIDE`. Same applies here (the HAL opens `/dev/snd/controlC0` RDWR).

---

## Testing strategy

Build:
```bash
# Fast rebuild after GN changes
sudo docker exec -u root -w /home/openharmony/workdir 8f7084d45c89 \
  ./build.sh --product-name hybris_generic --ccache
# Targeted rebuild of the new VDI plugin
sudo docker exec -u root -w /home/openharmony/workdir 8f7084d45c89 \
  ./build.sh --product-name hybris_generic --ccache \
    --build-target "device/soc/oniro/hybris_generic/hardware/audio:libaudio_primary_impl"
```

Deploy:
```bash
device/board/oniro/hybris_generic/utils/deploy-lxc-container.sh
```

On-device live log capture (keep short, hilog runs forever — CLAUDE.md tip):
```bash
hdc shell "timeout 20 hilog 2>&1 | grep -E 'HDF_AUDIO|audio_host|AudioServer|AudioHdi|audio_vdi|AudioAdapter'"
```

Smoke tests:
```bash
# 1. Adapter enumeration
hdc shell "LD_LIBRARY_PATH=/system/lib64/libhybris:/system/lib64 \
    /system/bin/idl_render --enum-adapters"
# 2. Playback (sample idl_render)
hdc shell "LD_LIBRARY_PATH=/system/lib64/libhybris:/system/lib64 \
    /system/bin/idl_render /data/test_441k.wav"
# 3. Capture
hdc shell "LD_LIBRARY_PATH=/system/lib64/libhybris:/system/lib64 \
    /system/bin/idl_capture /data/rec.pcm 5"
```

Crash triage:
```bash
hdc shell "ls -la /data/log/faultlog/faultlogger/ | grep audio_host"
hdc shell "cat /data/log/faultlog/faultlogger/cppcrash-audio_host-*" | head -100
```

Dependency probes:
```bash
hdc shell "cat /proc/asound/cards"         # should list mt6789-mt6366
hdc shell "ls /dev/snd/"                   # should list pcmC0D*, controlC0, timer
hdc shell "ls /vendor/lib64/libaudio_primary_impl.z.so"  # our new plugin
hdc shell "ls /android/vendor/lib64/hw/audio.primary.mt6789.so"
```

Static analysis pre-flight (`test_audio` from libhybris):
```bash
hdc shell "LD_LIBRARY_PATH=/system/lib64/libhybris:/system/lib64 \
    /system/bin/test_audio"
```
(Add `test_audio` to `third_party/libhybris/hybris/tests/BUILD.gn` — same trick as Phase 5.10 for `test_hwcomposer`.)

---

## Open questions

1. **Bridge-layer choice** — confirm (a) legacy `hw_get_module` vs (b) HIDL 7.0. Plan assumes (a).
2. **PulseAudio masking** — acceptable to hard-mask host PA while OHOS container runs? Consequence: any host-side Lomiri audio is muted for the session.
3. **Feature-flag spillover** — flipping `drivers_peripheral_audio_feature_community=false` also drops `pathselect`/`capture_adapter`/`render_adapter`. OK for non-hybris targets sharing the OHOS tree?
4. **Voice-call audio** — earpiece, SCO, voice volume in-scope for Phase 13, or a later telephony phase? Default: defer.
5. **A2DP audio** — `libhdi_audio_a2dp_server.z.so` is deployed and configured. Out of scope for Phase 13 (tied to a future Bluetooth phase)?

---

## File / path references with line numbers

| Item | Path | Notes |
|------|------|-------|
| Audio HDI IDL v6.0 (consumer side) | `drivers/interface/audio/v6_0/IAudioManager.idl` | The binder IDL that `audio_server` calls |
| HDI IDL-server stub | `drivers/peripheral/audio/hdi_service/primary/audio_manager_service.c:35` | `dlopen("libaudio_primary_impl_vendor")` |
| IDL-server HDF driver | `drivers/peripheral/audio/hdi_service/primary/audio_manager_driver.c` | Registers `audio_manager_service` with HDI |
| Community default impl | `drivers/peripheral/audio/hdi_service/primary_impl/src/` | Uses kernel HDF ADM — unusable on Halium 5.10 |
| **VDI dispatcher** | `drivers/peripheral/audio/hdi_service/primary_impl/vdi_src/audio_manager_vdi.c:502–527` | `AudioManagerLoadVendorLib` — dlopens `libaudio_primary_impl.z.so` (our new plugin) |
| VDI interfaces (targets for our plugin) | `drivers/peripheral/audio/interfaces/sound/v1_0/` | `iaudio_manager_vdi.h`, `iaudio_adapter_vdi.h`, `iaudio_render_vdi.h`, `iaudio_capture_vdi.h`, `iaudio_callback_vdi.h`, `audio_types_vdi.h` |
| VDI types enum reference | `drivers/peripheral/audio/interfaces/sound/v1_0/audio_types_vdi.h:33–61` | `AudioPortPinVdi` — used for routing translations |
| Build-flag flip point | `drivers/peripheral/audio/audio.gni:18`, `drivers/peripheral/audio/hdi_service/primary_impl/BUILD.gn:21` | `drivers_peripheral_audio_feature_community` |
| Product GNI for audio | `vendor/oniro/hybris_generic/hals/audio/product.gni` | Target of `feature_community = false` override |
| Product features JSON | `vendor/oniro/hybris_generic/config.json:106–111` | Existing audio component entry |
| UHDF device config | `vendor/oniro/hybris_generic/hdf_config/uhdf/device_info.hcs:210–253` | `audio :: host`, add `caps = ["DAC_OVERRIDE","SYS_NICE"]` |
| Display VDI precedent (arch analog) | `device/soc/oniro/hybris_generic/hardware/display/BUILD.gn` | Copy BUILD.gn structure, adjust for audio |
| libhybris hardware bridge | `third_party/libhybris/hybris/hardware/hardware.c` | `hw_get_module` implementation |
| libhybris audio test precedent | `third_party/libhybris/hybris/tests/test_audio.c:30–150` | Working `hw_get_module("audio.primary")` call sequence on Halium |
| Android audio HAL (device-side) | `/android/vendor/lib64/hw/audio.primary.mt6789.so` | NEEDED: libtinyalsa, libhidlbase, android.hardware.audio@7.0.so, vendor.mediatek.hardware.mtkpower@1.0.so |
| Android audio HIDL impl (alt path) | `/android/vendor/lib64/hw/android.hardware.audio@7.0-impl-mediatek.so` | Reserve for decision 13.D1 fallback (b) |
| Android HIDL service rc | `/android/vendor/etc/init/android.hardware.audio.service.mediatek.rc:6` | Reference capabilities: `BLOCK_SUSPEND SYS_NICE` |
| Android audio policy XMLs | `/android/vendor/etc/audio_*.xml` | Bind-mount into OHOS container per 13.2 |
| Android MTK mixer tuning | `/android/vendor/etc/audio_param/` | Bind-mount into `/vendor/etc/audio_param/` |
| OHOS audio policy config | `/vendor/etc/audio/audio_policy_config.xml` | Default `primary`+`a2dp`+`remote` adapters — ours returns only `primary` |
| OHOS rootfs sound card | `/proc/asound/cards` | Already visible in container: shows `mt6789-mt6366` |
| LXC config | `device/board/oniro/hybris_generic/utils/lxc/config` | Add `/dev/snd` rbind + vendor audio config binds (13.2) |
| start script | `device/board/oniro/hybris_generic/utils/start-ohos.sh` | Add PulseAudio mask (13.2) |
| Post-stop hook | `device/board/oniro/hybris_generic/utils/ohos-post-stop.sh` | Add PulseAudio unmask (13.2) |
| Host PulseAudio droid module | `pulseaudio-modules-droid-30` (UT deb pkg) | The conflicting consumer of `hw_get_module("audio.primary")` |
| Phase-10 conflict-resolution template | `legacy_wifi_support.md` section 10.1 | Same host-daemon-masking pattern |
| Phase-11 DAC_OVERRIDE template | `legacy_power_off_and_backlight.md` Fix 1 | Same cap-add-to-HDF-host pattern |
| Phase-6 binder-collision fixes | `legacy_graphics_stack.md` 6.8.1 | Same HIDL binder risk class |

---

## Key Paths Reference

| Item | Path |
|------|------|
| Phase 13 plugin source (to create) | `device/soc/oniro/hybris_generic/hardware/audio/` |
| VDI dispatcher (upstream, no change) | `drivers/peripheral/audio/hdi_service/primary_impl/vdi_src/audio_manager_vdi.c` |
| VDI interface headers | `drivers/peripheral/audio/interfaces/sound/v1_0/` |
| Product GNI override | `vendor/oniro/hybris_generic/hals/audio/product.gni` |
| UHDF device_info.hcs (caps add) | `vendor/oniro/hybris_generic/hdf_config/uhdf/device_info.hcs` |
| LXC config (dev/config binds) | `device/board/oniro/hybris_generic/utils/lxc/config` |
| start-ohos.sh (PA mask) | `device/board/oniro/hybris_generic/utils/start-ohos.sh` |
| Android audio HAL (bridge target) | `/android/vendor/lib64/hw/audio.primary.mt6789.so` |
| OHOS audio policy (on-device) | `/vendor/etc/audio/audio_policy_config.xml` |

---

## Phase 13B — Replacement plan: native ALSA audio (proposed, not yet started)

### Why replace the libhybris bridge

The libhybris-MTK bridge (Phase 13.1–13.6 above) is Halium-era scaffolding, not an architectural endpoint. It only works while OHOS runs as an LXC guest on a UT host that provides `audio.primary.mt6789.so` via the Android vendor partition. The MTK HAL carries a large proprietary DSP stack (aurisys / hifi3 / rv) which is the direct source of Bug 13.A and which we cannot reasonably debug without vendor source access. Three structural problems make this path a dead end for the longer term:

1. **Native-boot OHOS won't have the Android vendor partition at `/android/vendor/`** and the `hw_get_module("audio.primary")` codepath becomes meaningless — you're running the OS the HAL was meant to be bridged *into*.
2. **The MTK DSP state machine is tuned for Android audioserver + audiopolicy + HIDL vendor services.** Any bug encountered (13.A is only the first; 13.B, 13.C etc. are likely latent in call audio, MMAP paths, BT routing, volume curves) requires vendor assistance to triage.
3. **`libhybris` + `libc++` + Android HIDL transitive deps** inflate the audio trust boundary dramatically — attestation / ASLR / seccomp postures diverge from the rest of OHOS.

The native ALSA path eliminates all three: OHOS drives `/dev/snd/pcmC0D*p` directly via `libasound`, with no Android HAL, no DSP middleware, no HIDL. The trade-off is losing MTK's vendor tuning — speaker-protection curves, voice-call audio, noise suppression, echo cancellation — all of which are already unused/broken in our container setup anyway. Plain stereo music playback, mic capture, and volume control via mixer controls are entirely sufficient for the MVP and are what the existing `drivers_peripheral_audio_feature_alsa_lib` path in OHOS delivers.

### What already exists in OHOS (no new code needed)

- **`drivers/peripheral/audio/supportlibs/alsa_adapter/`** — upstream OHOS ALSA-lib backend: `alsa_soundcard.c`, `alsa_snd_render.c`, `alsa_snd_capture.c`, `alsa_lib_render.c`, `alsa_lib_capture.c`. This is the full render + capture path wired against `libasound`.
- **`third_party/alsa-lib/`** — the alsa-lib source tree + `BUILD.gn`, builds `libasound.so`.
- **Build-system flip point**: `drivers/peripheral/audio/hdi_service/supportlibs/BUILD.gn` — when `drivers_peripheral_audio_feature_alsa_lib = true` and `drivers_peripheral_audio_feature_community = true`, `audio_capture_adapter.z.so` + `audio_render_adapter.z.so` are rebuilt against `$hdf_audio_path/supportlibs/alsa_adapter/src/*.c` + `alsa-lib:libasound` + `cJSON:cjson` instead of the kernel-HDF-ADM sources.
- **Two working reference products**: `device/board/hihope/rk3568/audio_alsa/{common.h,vendor_render.c,vendor_capture.c}` (RK3568 reference, ~266 lines vendor_render.c) and `device/board/oniro/x86_general/audio_alsa/...` (oniro-sibling product, near-identical to rk3568 — only ~3 lines of diff). Both define mixer-control names and ALSA element IDs for their SoC.
- **Product-level config stubs** already present: `vendor/oniro/hybris_generic/hals/audio/alsa_adapter.json` + `alsa_paths.json`. Today they carry RK3568 placeholder content (cardName `rockchiprk809co` etc.) — literally copied unchanged from the reference product. They need to be re-authored for `mt6789-mt6366`.

### Delta we need to produce (the actual plan)

#### 13B.1 — Product flag flip

In `vendor/oniro/hybris_generic/hals/audio/product.gni`:
```gni
-drivers_peripheral_audio_feature_community = false   # Phase 13 libhybris bridge
+drivers_peripheral_audio_feature_community = true    # 13B: native kernel/ALSA path
+drivers_peripheral_audio_feature_alsa_lib  = true    # 13B: libasound backend (not ADM)
```
Mirror both in `vendor/oniro/hybris_generic/config.json` under the `drivers_peripheral_audio` component `features` array. When `feature_community=true` + `feature_alsa_lib=true`, `libaudio_primary_impl_vendor.z.so` goes back to the legacy-community dispatcher (direct, no VDI plugin dlopen) and its adapters call `alsa_adapter` instead of kernel HDF ADM.

#### 13B.2 — Retire the VDI plugin

- Drop `hardware/audio:audio_primary_model` from `device/soc/oniro/hybris_generic/BUILD.gn` — the VDI plugin is no longer built, and the dispatcher no longer looks for it.
- Keep the source tree around initially (`device/soc/oniro/hybris_generic/hardware/audio/`) for one release cycle as a fallback; delete once 13B is stable.
- Remove the plugin entry from `developtools/integration_verification/tools/deps_guard/rules/Passthrough/passthrough_info.json` (otherwise deps_guard flags the missing binary).
- Remove the `/vendor/lib64/libaudio_primary_impl.z.so` symlink creation from `deploy-lxc-container.sh`.

#### 13B.3 — MTK-accurate `alsa_adapter.json`

Replace the RK3568 placeholder content:
```json
{
    "adapters": [
        {
            "name": "primary",
            "cardId": 0,
            "cardName": "mt6789mt6366"
        }
    ]
}
```
Card name comes from `/proc/asound/cards` on-device (we confirmed `0 [mt6789mt6366   ]: mt6789-mt6366 - mt6789-mt6366`). `cardId` 0 matches the single card. No HDMI on this device — drop the second adapter.

#### 13B.4 — Author a new `alsa_paths.json`

The existing file contains RK3568 mixer-path descriptions (speaker/headphone/mic gating via specific mixer element names that don't exist on MT6789). The new version must describe MTK's mixer element names for the `mt6789-mt6366` card. Capture the ground truth with:
```bash
hdc shell "tinymix -D 0 contents"    # lists all controls + current values
hdc shell "amixer -c 0 controls"     # alternative listing
```
Then author one path per scene (`deep-buffer-playback`, `low-latency-playback`, `primary-capture`, etc.) matching the structure in `device/board/hihope/rk3568/audio_alsa/...` / existing rk3568 `alsa_paths.json`. Scope of effort: ~100–200 lines of JSON, mechanical once mixer control names are known.

#### 13B.5 — Create `device/board/oniro/hybris_generic/audio_alsa/`

Three files, modelled after `device/board/oniro/x86_general/audio_alsa/`:
- `common.h` — define `HDF_AUDIO_HAL_{RENDER,CAPTURE}` log tags, pull in the upstream alsa_snd_render/capture headers. Near-verbatim copy.
- `vendor_render.c` (~270 lines) — MTK-specific render hooks: `RenderInitImpl`, volume setter/getter, mute, device select. The hooks mostly call `AlsaMixerCtlElement` setters with MTK-named controls (e.g. `"Speaker Volume"`, `"Headphone Switch"`) — the rk3568 template's `"PCM Playback Volume"`, `"Playback Path"` strings get swapped for MTK's equivalents identified in 13B.4.
- `vendor_capture.c` (~100–150 lines) — symmetric for capture (mic gain, mic source selection).

The upstream `alsa_snd_render.c` / `alsa_snd_capture.c` reference these vendor hooks via `RenderOpsInit()` / `CaptureOpsInit()`, so the plumbing is already present — we only need the vendor-specific leaves.

#### 13B.6 — Wire the new `audio_alsa` dir into the BUILD

The upstream `drivers/peripheral/audio/hdi_service/supportlibs/BUILD.gn` already looks for `//device/board/${product_company}/${device_name}/audio_alsa/vendor_{render,capture}.c` — so just having the files at the expected path is enough, no new GN target required. But we need to verify the existing path substitution works for `product_company="oniro"` + `device_name="hybris_generic"`. If not, add a small BUILD.gn in the new dir.

#### 13B.7 — LXC config simplification

With the libhybris bridge gone:
- Remove `/android/vendor/etc/audio_*` binds (no longer needed — MTK HAL is out of the picture).
- Remove `/android/vendor/etc/aurisys_config*.xml` binds.
- Remove the `AudioParamOptions_mgvi.xml` override + push step in `deploy-lxc-container.sh`.
- Remove the `/system/vendor → /vendor` symlink creation (was only for MTK HAL hardcoded-path resolution).
- Remove `audio_param_mgvi_override.xml` push step.
- **Keep** `/dev/snd` rbind (still needed — alsa-lib opens `/dev/snd/pcmC0D*` and `/dev/snd/controlC0`).
- **Keep** host PulseAudio mask in `start-ohos.sh` — UT's PA still wants to open the same PCM nodes and would contend.
- **Keep** `CAP_DAC_OVERRIDE` + `CAP_SYS_NICE` on `audio_host` in `device_info.hcs` (still needed — `/dev/snd/controlC0` is `system:audio 0660`).

#### 13B.8 — Build + deploy + verify

```bash
# 1. Build
sudo docker exec -u root -w /home/openharmony/workdir 8f7084d45c89 \
    ./build.sh --product-name hybris_generic --ccache

# 2. Readelf sanity — audio_{render,capture}_adapter.z.so should now link libasound
sudo docker exec -u root -w /home/openharmony/workdir 8f7084d45c89 \
    readelf -d out/hybris_generic/packages/phone/images/ohos-rootfs/vendor/lib64/libaudio_render_adapter.z.so | grep NEEDED
# expected: libasound.so

# 3. Deploy + restart
device/board/oniro/hybris_generic/utils/deploy-lxc-container.sh
adb shell "echo 1234 | sudo -S lxc-stop -n openharmony -k; sleep 2; echo 1234 | sudo -S lxc-start -n openharmony"

# 4. Smoke
hdc shell "timeout 20 hilog 2>&1 | grep -iE 'alsa|HDF_AUDIO|audio_host'"
hdc shell "LD_LIBRARY_PATH=/system/lib64 /system/bin/idl_render /data/test_441k.wav"
hdc shell "tinyplay /data/test_441k.wav -D 0 -d 0"   # sanity: direct tinyalsa test bypasses OHOS entirely
```

#### 13B.9 — Feature gaps vs. MTK HAL (document as known limitations)

| Capability | MTK HAL (Phase 13A, broken) | Native ALSA (13B) | Impact |
|---|---|---|---|
| Speaker protection / amp calibration | ✓ (aurisys) | ✗ | Risk of over-drive at high volume — document max safe level |
| Echo cancellation (voice) | ✓ | ✗ (OHOS audioeffect ring can partially compensate) | Voice-call audio quality |
| Noise suppression | ✓ | ✗ | Capture quality in noisy environments |
| Call-audio routing (earpiece/SCO) | ✓ | ✗ | Phone calls won't work — telephony is already out of scope for Phase 13A anyway |
| MMAP low-latency path | Phase 13.8 (pending) | ✓ (alsa-lib has MMAP directly) | Parity or better |
| Hardware volume curve | ✓ (DSP-tuned) | ✗ (linear mixer ctrl) | Perceptually slightly different but functional |
| HiFi offload | n/a on this device anyway | n/a | No change |

Everything that actually works in 13A (basic stereo playback + mic capture + hardware-key volume change) is preserved. Everything that doesn't work yet in 13A (call audio, BT audio) continues not to work in 13B.

#### 13B.10 — Transition path for the native-boot future

When OHOS becomes the primary OS with Android-as-guest:
- 13B stays valid unchanged — it already doesn't depend on the Android container, libhybris, or any hwbinder plumbing.
- A future "13C" iteration could add a proper OHOS HDF audio driver (backed by the MTK kernel audio subsystem via a `device/soc/oniro/.../hal/audio/` HDF driver, then flipping back to `feature_alsa_lib=false` + `feature_community=true` to use the ADM path). But this is optional — alsa-lib works fine for the long term.
- `libhybris` stays in the tree only for graphics (HWC2/gralloc) until Phase 6's VDI bridge is similarly retired by a native HDF graphics driver.

### 13A stash — restoration recipe

All 13A code changes were stashed (2026-04-15) under the label `phase13a-libhybris-mtk-audio-bridge` across five separate git repos (the source tree is `repo`-managed, each component is its own git). The docs (this file + `README.md`) were intentionally left out of the stash to guide 13B execution.

To restore 13A (e.g. if 13B stalls and libhybris bridge becomes a viable fallback again):

```bash
cd /home/mrfrank/openharmony-6.1/drivers/peripheral/audio       && git stash pop  # upstream patches (rendrId typo + C-in-.c stub)
cd /home/mrfrank/openharmony-6.1/vendor/oniro/hybris_generic    && git stash pop  # product.gni, config.json, device_info.hcs
cd /home/mrfrank/openharmony-6.1/device/soc/oniro/hybris_generic && git stash pop  # BUILD.gn + hardware/audio/ VDI plugin tree
cd /home/mrfrank/openharmony-6.1/device/board/oniro             && git stash pop  # utils/{lxc/config,*.sh} + utils/audio_overlays/
cd /home/mrfrank/openharmony-6.1/developtools/integration_verification && git stash pop  # passthrough_info.json entry
```

Each repo has exactly one stash with the label (verify with `git stash list`). If `git stash pop` conflicts against 13B changes in those same files, `git stash show stash@{0} -p` is the patch; `git stash drop` after manual merge.

---

### Order of operations recommendation

1. First execute 13B.3 + 13B.4 on a live device (capture `tinymix` output — 5 minutes of work).
2. Do 13B.5 vendor_render/capture (template-copy + swap control names — ~30 minutes).
3. Flip flags (13B.1) + kill VDI plugin deps (13B.2) + simplify LXC config (13B.7).
4. Build, deploy, smoke-test. Keep the 13A source tree frozen as fallback for one cycle.
5. Once 13B is confirmed stable, delete `device/soc/oniro/hybris_generic/hardware/audio/` and all 13A docs except the Bug 13.A postmortem.

Estimated total effort: half a day to write code + JSON, plus one on-device iteration cycle.

---

## Phase 13B — Execution log (2026-04-15)

| Sub-step | Status | Notes |
|---|---|---|
| 13B.1 — product.gni/config.json flag flip (`feature_alsa_lib = true`) | ✅ | `feature_community` defaults to `true` in `audio.gni:18`; only `feature_alsa_lib` needed flipping in `vendor/oniro/hybris_generic/hals/audio/product.gni` + `vendor/oniro/hybris_generic/config.json:109`. |
| 13B.2 — retire VDI plugin references | ✅ | No `hardware/audio:audio_primary_model` dep existed in `device/soc/oniro/hybris_generic/BUILD.gn` after the stash — nothing to remove. The 13A source tree is still stashed, not committed. |
| 13B.3 — `alsa_adapter.json` | ✅ | Single `primary` adapter on `cardId: 0`, `cardName: mt6789mt6366`. Second (hdmi) entry dropped. |
| 13B.4 — `alsa_paths.json` | ✅ | MTK-specific scenes (`deep-buffer-playback`, `low-latency-communication`, `ringtone-playback`, `voice-call`, `low-latency-noirq-playback`) referencing `Ext_Speaker_Amp Switch`, `HPL Mux` / `HPR Mux`, `Mic Type Mux`, `PGA L/R Mux`. Values captured from `amixer -c 0` on-device. |
| 13B.5 — `device/board/oniro/hybris_generic/audio_alsa/{common.h,vendor_render.c,vendor_capture.c}` | ✅ | Modelled after `device/board/oniro/x86_general/audio_alsa/`. Volume master = `Lineout Volume` (mirrored onto `Headset Volume`). Capture volume = `PGA1 Volume` (+ `PGA2 Volume` mirror). Scene-select stubbed to `descPins` assignment (path JSON is applied elsewhere). |
| 13B.6 — BUILD wire-up | ✅ | Upstream `drivers/peripheral/audio/hdi_service/supportlibs/BUILD.gn:85,159,104,179` already picks up `//device/board/${product_company}/${device_name}/audio_alsa/` sources + include dir — no new GN target needed. |
| 13B.7 — LXC config simplification | ✅ | Added `/dev/snd rbind` to `utils/lxc/config`. Added PulseAudio mask in `start-ohos.sh` (pre-`lxc-start`) + unmask in `ohos-post-stop.sh`. Added `caps = ["DAC_OVERRIDE","SYS_NICE"]` to `audio_host` in `vendor/oniro/hybris_generic/hdf_config/uhdf/device_info.hcs:213`. No 13A audio binds existed in the non-stashed config, so there was nothing to remove there. |
| 13B.8 — build + deploy + smoke | ✅ | Full build green (`build.sh --product-name hybris_generic --ccache`). `readelf -d libaudio_render_adapter.z.so` confirms `NEEDED libasound.so`. Deployed via `deploy-lxc-container.sh`, container restarted, music player (`ohos.samples.distributedmusicplayer`) plays `dynamic.wav`; HDF_AUDIO reports `AudioRenderGetLatency: 21 ms` continuously. |
| 13B.9 — DAPM route bring-up (new, not in original plan) | ✅ | See below. |

### DAPM route bring-up (additional fix)

**Symptom observed on first deploy:** `audio_host` hit `HDF_AUDIO_HAL_RENDER / SetHWParams line 201: Unable to set hw params for playback` on the first `RenderCreate` call from `audio_server`. `aplay -D hw:0,0 /path/to/any.wav` from the UT host produced the identical failure (`set_params:1435: Unable to install hw params`), confirming the fault was outside our code.

**Root cause:** `dmesg` showed `Playback_X: ASoC: no backend DAIs enabled for Playback_X`. MT6789 ASoC ships with all DAPM widgets disconnected by default. Android's MTK audio HAL sets up the FE → BE route at stream open via a sequence of mixer-kcontrol writes. Until those run, the kernel rejects every `snd_pcm_hw_params` ioctl for *any* `Playback_X` device with *any* param combination.

**Minimum DAPM route for primary playback on MT6789+MT6366, captured via `amixer -c 0 cget`:**

```
numid=211 ADDA_DL_CH1 DL1_CH1  = on     # connect FE DL1 ch1 -> ADDA DAC ch1
numid=226 ADDA_DL_CH2 DL1_CH2  = on     # connect FE DL1 ch2 -> ADDA DAC ch2
numid=311 HPL Mux              = "Audio Playback"
numid=312 HPR Mux              = "Audio Playback"
numid=305 Ext_Speaker_Amp Switch = on
```

**Where to apply:** `RenderInitImpl()` in `device/board/oniro/hybris_generic/audio_alsa/vendor_render.c`. This runs via `AlsaRender::Init` from `AudioOutputRenderOpen()` in the upstream alsa_adapter, which the community HDI impl invokes *before* `AUDIO_DRV_PCM_IOCTL_HW_PARAMS`. So the routes are powered up just in time for the following `snd_pcm_hw_params` call. The macros + implementation live in `common.h` (`SND_NUMID_ADDA_DL_CH{1,2}_DL{1,2}_CH{1,2}`, `SND_NUMID_HPL_MUX`, `SND_NUMID_HPR_MUX`, `SND_NUMID_EXT_SPK_AMP_SWITCH`) and `vendor_render.c::RenderInitImpl()`.

**Why the original 13B plan missed this:** the rk3568/x86_general reference templates run on SoCs whose codecs come up with sensible DAPM defaults (ALC code paths are live at card probe) — they only need volume/playback-path trims, not end-to-end DAPM power-up. MTK's ASoC driver is markedly more conservative and expects the HAL to drive every connection. Any future MTK-family SoC targeting this 13B native-ALSA path will need to publish the equivalent route list.

### Changes made (summary, post-execution)

Vendor / product tree:
- `vendor/oniro/hybris_generic/hals/audio/product.gni` — `drivers_peripheral_audio_feature_alsa_lib = true`
- `vendor/oniro/hybris_generic/config.json:109` — mirror of the flag
- `vendor/oniro/hybris_generic/hals/audio/alsa_adapter.json` — single `primary`/`mt6789mt6366` adapter
- `vendor/oniro/hybris_generic/hals/audio/alsa_paths.json` — MT6789-accurate scene paths
- `vendor/oniro/hybris_generic/hdf_config/uhdf/device_info.hcs:213` — `caps = ["DAC_OVERRIDE","SYS_NICE"]` on `audio_host`

Board / device tree:
- `device/board/oniro/hybris_generic/audio_alsa/common.h` (new)
- `device/board/oniro/hybris_generic/audio_alsa/vendor_render.c` (new)
- `device/board/oniro/hybris_generic/audio_alsa/vendor_capture.c` (new)
- `device/board/oniro/hybris_generic/utils/lxc/config` — `/dev/snd rbind` entry
- `device/board/oniro/hybris_generic/utils/start-ohos.sh` — `systemctl --global mask pulseaudio.{service,socket}` + `pkill -u phablet pulseaudio` pre-`lxc-start`
- `device/board/oniro/hybris_generic/utils/ohos-post-stop.sh` — matching unmask

Upstream tree: **no patches required**. `drivers/peripheral/audio/hdi_service/supportlibs/BUILD.gn` already consumes `//device/board/${product_company}/${device_name}/audio_alsa/vendor_{render,capture}.c` via the existing `product_company/device_name` substitution.

### 13B.10 — Bisection tool: `test_audio` against OHOS `libasound` (2026-04-16)

Added a standalone executable (`device/board/oniro/hybris_generic/audio_alsa/test_audio/test_audio.c`) that exercises `/vendor/lib64/libasound.so` **directly**, bypassing `audio_host` / `audio_server` / HDF. It programs the MT6789 DAPM route via `snd_ctl_elem_write` (same numids as `vendor_render.c::RenderInitImpl` — 211/226/311/312/305 + Lineout/Headset vol), opens `hw:0,0` blocking with identical hw/sw params (44100 S16_LE stereo, period_time=125 ms, buffer_time=500 ms, start_threshold=period_size), and writes a 440 Hz sine tone for ~3 s.

Build + deploy:
```bash
sudo docker exec -u root -w /home/openharmony/workdir 8f7084d45c89 \
    ./build.sh --product-name hybris_generic --ccache \
    --build-target "device/board/oniro/hybris_generic/audio_alsa/test_audio:test_audio"
hdc file send out/hybris_generic/device_hybris_generic/device_hybris_generic/test_audio /data/local/tmp/test_audio
hdc shell "chmod +x /data/local/tmp/test_audio"
```

Run (audio_host must be killed because it holds `pcmC0D0p` exclusive):
```bash
hdc shell "kill -9 \$(pidof audio_host); \
    LD_LIBRARY_PATH=/vendor/lib64:/system/lib64 /data/local/tmp/test_audio"
```

**Outcome (2026-04-16): AUDIBLE.** The 440 Hz sine tone played clearly from the speaker. `snd_pcm_open` returns 0, `hw_params` applies (rate=44100 ch=2 period_us=125011 buffer_us=500045 → period_size=5513 frames, buffer_size=22052 frames), PCM reports `RUNNING` with `avail=67`, 23 periods written without a single EAGAIN / recover. This result kicked off the bisection chain (overwrite-sine → state diff → strace → close+reopen) that led to the final fix in `RenderStartImpl`.

The binary is retained as a standing diagnostic — see its comment header for how to re-use it for future audio regressions.

Wire-up details (new files + one-line hooks):
- `device/board/oniro/hybris_generic/audio_alsa/test_audio/test_audio.c` (new, ~240 lines)
- `device/board/oniro/hybris_generic/audio_alsa/test_audio/BUILD.gn` (new — `ohos_executable("test_audio")` → `/system/bin/test_audio`; `part_name = subsystem_name = "device_hybris_generic"`, deps on `//third_party/alsa-lib:libasound`, include dir on alsa-lib's public headers)
- `device/board/oniro/hybris_generic/BUILD.gn` — `audio_alsa/test_audio:test_audio_group` added to `hybris_generic_group`
- `device/board/oniro/hybris_generic/bundle.json` — `"alsa-lib"` added to `component.deps.third_party` (without this, `check_deps_handler.py` fails the build with `need set part deps alsa-lib info`)

### Open items (13B)

- ✅ **Silence root cause — RESOLVED (2026-04-16).** Close+reopen PCM on every Start in `RenderStartImpl`. See status banner + bisection log.
- Capture-side verification: `vendor_capture.c` is written but not smoke-tested yet. Mic routing mixers (`Mic Type Mux`, `PGA L/R Mux`) may also require DAPM wake-up and/or the same close+reopen-on-Start pattern. Expect the same fix class.
- Headset jack detect: the previous 13A poll thread on `/sys/class/switch/h2w/state` was tied to the VDI plugin; a native-ALSA equivalent (probably via `AudioSocketThread` in the adapter) is not yet wired.
- Scene routing: `alsa_paths.json` is authored but the pathselect parser runs in the `alsa_lib_render` path via `AudioInterfaceLibCtlRender(SCENESELECT_WRITE)`; the upstream alsa_adapter currently stubs `SelectScene` to `HDF_SUCCESS`, so the scene JSON writes don't reach the mixer yet. Wire this up when per-scene audio (ringtone / voice) is exercised.
- Volume UI: volume keys reach the `Lineout Volume` kcontrol via the vendor hook; on-device confirmation that the slider in Settings also drives this path is pending.
