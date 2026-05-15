# Phase N8 — Graphics & Display (Native)

**Status:** ✅ **Display working — OHOS lockscreen renders on the physical Volla X23 panel under native boot (2026-05-15).**  The full libhybris + Halium graphics stack is up: Halium HAL services stable, Mali GPU loaded, `render_service` composites via GPU, `allocator_host` allocates buffers, `composer_host` drives `hybris-hwc2-display` 720×1560@59 Hz.  Five distinct blockers were cleared this session — see § N8.9.2-fix through § N8.13 below.  **Touch:** ✅ working — `chipone-tddi` (ICNL9911C) SPI touch driver bundled + loaded at pre-init; § N8.13, verified on device (driver bound, sensor scanning, `event2` open in OHOS multimodalinput).

## Session 2026-05-15 — five blockers to first pixels

The path from "composer_host alive but no pixels" to "lockscreen on panel" went through five independent root causes, each documented in its own section:

1. **§ N8.9.2-fix — SELinux absent → Halium HAL restart loop.**  Kernel cmdline pinned `security=apparmor`; SELinux never initialised, `/sys/fs/selinux` absent, Halium's `vndservicemanager` SIGABRTed on `selinux_status_open`, cascading into a `class hal` restart loop (composer\@2.3 cycling every 4–6 s).  Fixed by `lsm=selinux` on the chainload cmdline + mounting `selinuxfs` in both namespaces.
2. **§ N8.10b — `/mnt` + `/storage` on RO root → launcher crash-loop.**  Native boot skips `FirstStageMain`, so `MountBasicFs()` never ran; `/mnt` stayed read-only, `mkdir /mnt/sandbox` failed, every app-spawn sandbox bind hit ENOENT.  Fixed by tmpfs mounts in the chainload.
3. **§ N8.11 — Mali GPU driver not loaded.**  `/dev/mali0` absent — `mali_kbase` + 20 dependency modules were not in `vendor_boot`.  Fixed by bundling the 52-module GPU+touch closure and loading it at OHOS `pre-init`.
4. **§ N8.12 — `/vendor/lib64/{hw,egl}` absent → `allocator_host` SIGABRT.**  Android `libui` `GraphicBufferMapper` `access()`-checks a hardcoded `/vendor/lib64/hw/...` path; it didn't exist, the gralloc mapper failed to load, `allocator_host` aborted, `render_service` got no buffers.  Fixed by binding the Halium HAL dirs over OHOS-side `/vendor/lib64/{hw,egl}`.
5. **§ N8.13 — touch input.**  The X23's `chipone-tddi` SPI touch driver (`chipone-tddi.ko`) was not bundled/loaded under native boot — `/dev/input/` had no touch panel.  Fixed: bundle it in `vendor_boot` and `insmod` at pre-init.

---

## Historical (pre-2026-05-15): N8.9 — composer_host published display_composer_service

**N8.9 substantively unblocked 2026-05-14: composer_host now publishes display_composer_service** (full detail in § N8.9.1 below).  `render_service`, `com.ohos.systemui`, and `com.ohos.launcher` all see `hybris-hwc2-display` at 720×1560@59Hz and backlight is on.

> **Goal.** `render_service` lights pixels on the panel under native OHOS, inheriting Phases 5–8 (libhybris, display VDIs, stability fixes) without modification beyond the gating cfg added below.

## Dependencies

This phase is **wiring**, not new source. The hard work happens in:

- **N5** — Halium `system_a` + `vendor_a` mounted at `/android/system` + `/android/vendor` by the chainload.
- **N4** — `androidd` brings up hwservicemanager + servicemanager + vndservicemanager + composer@2.x + gralloc@4.0 in a child namespace, sharing OHOS's `/dev/hwbinder`. Parent sets `param android.composer.ready=1` when composer registers.
- **N6** — OHOS owns default `/dev/binder`; Android binds `android-binder` as its `/dev/binder` in its namespace; `hwbinder` + `vndbinder` shared.
- **Phases 5, 6, 7, 8, 11** of the LXC plan — all carry over unchanged.

If N4/N5 aren't done, **everything in N8 is a no-op**.

---

## N8.1 — Library path resolution: nothing to do

libhybris's path-redirect map already remaps `/vendor/lib64 → /android/vendor/lib64` for Android-vendor library loads (`third_party/libhybris/hybris/common/hooks.c:2368`, `hybris/common/q/linker.cpp:119`). OHOS-side processes (composer_host, render_service) using libhybris call into Android HALs via `/android/vendor/lib64/{egl,hw}/...` automatically — no source edits, no special bind mounts.

The 2026-03-20 SPHAL revert documented in `device/board/oniro/hybris_generic/utils/device/lxc/config:71-79` was caused by Android libs leaking into OHOS's *own* `/vendor/lib64/`. Native boot's `/vendor` is OHOS vendor only; Halium content lives at `/android/vendor`. **No collision possible.** The LXC bind-mounts that wired Android libs into OHOS-vendor in the old build are simply not present.

---

## N8.2 — EGL/GLES symlinks: inherit from Phase 6

The OHOS-side EGL impl symlinks (`/system/lib64/libEGL_impl.so → libhybris EGL.so`, etc.) are shipped by `device/soc/oniro/hybris_generic/hardware/display/` (Phase 6). These live in OHOS's `system` partition and ride along into native boot without modification.

The Android-side EGL impl (`/vendor/lib64/egl/libGLES_mali.so`) ships in Halium's `vendor_a` and is reachable at `/android/vendor/lib64/egl/libGLES_mali.so` after N5's chainload mount. Libhybris's linker remap finds it.

---

## N8.3 — Environment variables: already in tree

`device/board/oniro/hybris_generic/cfg/hybris_graphic_env.cfg` and `cfg/z_hybris_hdf_env.cfg` set:

```
HYBRIS_LD_LIBRARY_PATH = /android/vendor/lib64:/android/system/lib64
LD_LIBRARY_PATH        = /system/lib64/libhybris:/system/lib64
HYBRIS_EGLPLATFORM     = ohos
LIBEGL                 = /android/vendor/lib64/egl/libGLES_mali.so
LIBGLESV2              = /android/vendor/lib64/egl/libGLES_mali.so
```

on `composer_host`, `allocator_host`, `render_service`, `bootanimation`. Native boot inherits these unchanged.

> Important: these env vars *only* take effect when `/android/{system,vendor}` actually exist (N5). Without that, libhybris's `dlopen` lookups will fail with ENOENT and composer_host will SIGSEGV (or, with the gate cfg below, never start at all).

---

## N8.4 — Composer-readiness gate (the only new artifact in N8)

This is the one cfg N8 contributes. Without it, `composer_host` and `allocator_host` start at OHOS init's normal boot trigger and crash because hwservicemanager isn't up yet (the launcher is still running Halium init).

### `device/board/oniro/hybris_generic/cfg/z_composer_host_gate.cfg`

```json
{
    "services" : [{
            "name" : "composer_host",
            "start-mode" : "condition",
            "condition"  : "param:android.composer.ready=1"
        },
        {
            "name" : "allocator_host",
            "start-mode" : "condition",
            "condition"  : "param:android.composer.ready=1"
        }
    ]
}
```

The `z_` prefix sorts after the upstream `composer_host.cfg` in `/system/etc/init/`, so OHOS init's cfg-merge applies our `start-mode`/`condition` overrides last.

Wire into `device/board/oniro/hybris_generic/cfg/BUILD.gn` (sibling to `z_hybris_hdf_env.cfg` etc.). Add to `hybris_generic_cfg_group`.

Parameter source: the parent of `androidd` (Phase N4.4) polls hwservicemanager for `android.hardware.graphics.composer@2.1::IComposer/default` and calls `SystemSetParameter("android.composer.ready", "1")` on success.

### Why not just `wait_other` or `bootevent`?

- `bootevent` watchers are for OHOS-side init events; we need a Halium-side signal crossing the namespace boundary.
- `wait_other` (waiting on another OHOS service) doesn't apply — `androidd` *is* an OHOS service, but its readiness signal is about a child in another namespace.
- `param:` condition is the natural fit and already supported by OHOS init's condition parser (used by many existing cfgs).

---

## N8.5 — Device-node access: already covered

ueventd handles `/dev/mali0`, `/dev/dri/*`, `/dev/dma_heap/*` perms via:

- `vendor/oniro/hybris_generic/etc/ueventd/ueventd.config` (existing)
- `vendor/oniro/hybris_generic/etc/init/init.x23.cfg` (creates the `/android` mount points)

The chainload also pre-creates these on the host `/dev` and the binds inherit. `composer_host` runs uid 3036 (composer_host) + `gid graphics` + `caps SYS_NICE DAC_OVERRIDE` (Phase 11 — for the backlight sysfs writer) — these are unchanged in native.

---

## N8.7 — samgr + binder bring-up (2026-05-14)

After N4 (all Halium HAL services up + IComposer registered + `android.composer.ready=1`), starting `composer_host` manually with `begetctl start_service composer_host` resulted in `SERVICE_STOPPED` (status 5).  Two cascading root causes uncovered:

### 1. `/dev/binderfs/binder` is mode 0600 root:root (kernel binderfs default)

Symptom: `dmesg | grep SAMGR` shows samgr crash-looping every ~1 s:
```
SAMGR: main called, enter System Ability Manager
SAMGR: System Ability Manager enter init
SAMGR: set context fail!
SAMGR: set samgr ready ret : succeed
SAMGR: JoinWorkThread error, samgr main exit!
```
samgr runs as `uid samgr (5555)` and could not open `/dev/binder` → `BINDER_SET_CONTEXT_MGR` failed → `JoinWorkThread` exit, restart.  hwbinder/vndbinder/android-binder were already 0666 because `androidd` (Phase N4) chmods its bind-mount targets; `/dev/binderfs/binder` was not touched because OHOS uses it directly, not through the androidd bind.

**Fix:** add `chmod 0666 /dev/binderfs/{binder,hwbinder,vndbinder}` to `init.x23.cfg` pre-init job (the hwbinder/vndbinder chmods are belt-and-suspenders — androidd does them too, but it's cheap and survives if androidd's bind ordering ever changes).

### 2. `samgr CanRequest()` rejects every native-uid caller

With samgr alive, manual `begetctl start_service composer_host` produces a running pid, but `render_service` keeps logging `failed to get sa hdf service manager` / `display_composer_proxy: Get:get IServiceManager failed!` indefinitely.

Root cause traced via `dmesg | grep SAMGR`:
```
SAMGR: CanRequest callingTkid:3044, tokenType:0
SAMGR: AddSystemAbilityInner PERMISSION DENIED!
```

`hdf_devmgr` (uid 3044) is denied when registering SA 5100 (HDF service manager).  `system_ability_manager_stub.cpp::CanRequest()` checks tokenType for `TOKEN_NATIVE`; on this Halium 5.10 kernel `/dev/access_token_id` doesn't exist (the OHOS staging driver isn't in the chainload's kernel — the chainload uses `boot_a.bak`'s Halium kernel, not our patched `kernel/linux/volla-vidofnir/out/boot.img`).  Every caller has `tokenType=TOKEN_INVALID` and `tid==uid`.  The existing uid-fallback only allows `0` and `1000`, so service-uid callers (`hdf_devmgr=3044`, `composer_host=3036`, etc.) hit `return false` and registration fails.

The LXC build solves this by exporting `OHOS_RUNTIME_CONFIG=1`; LXC env-var injection makes it visible to samgr.  Native init does NOT propagate env (`/proc/1/environ` shows only `bootopt=` from the kernel cmdline — our `env OHOS_NATIVE_BOOT=1 chroot` in `init-chainload.sh` does not carry to OHOS init's children).

**Fix:** marker-file mechanism rather than env, since it doesn't depend on env propagation:
- `init.x23.cfg` pre-init writes `/dev/.ohos_native_boot` (1 byte, mode 0600).
- Patched `CanRequest()` in `foundation/systemabilitymgr/samgr/services/samgr/native/source/system_ability_manager_stub.cpp` adds an `access("/dev/.ohos_native_boot", F_OK) == 0` short-circuit immediately after the existing `OHOS_RUNTIME_CONFIG` check.
- Remove when the OHOS-patched kernel (`kernel/linux/volla-vidofnir/out/boot.img`) replaces `boot_a.bak`'s kernel under the chainload and `/dev/access_token_id` appears — then tokenType will be `TOKEN_NATIVE` and the bypass is unneeded.

### Why not use the patched kernel today? — superseded 2026-05-14 (see below)

Originally `build_boot_img_chainload.sh` unpacked `out/hybris_generic/backups/boot_a.bak` and reused its kernel because reusing the Halium kernel guaranteed module-set compatibility.  As of 2026-05-14, this is being lifted via Phase N8.10 below — Halium kernel modules are now rebuilt against our patched tree (same vermagic both sides), so we can ship the OHOS-patched kernel under the chainload and let `/dev/access_token_id` come up naturally.

## N8.10 — Replacing the chainload kernel with the OHOS-patched build (2026-05-14)

The marker-file `CanRequest()` bypass landed in N8.7 is a security-degraded workaround for the missing `/dev/access_token_id`.  The proper fix is to flip the chainload kernel to our OHOS-patched build, which carries the access_tokenid staging driver (plus hilog, hievent, blackbox, binder token-id).

### Mechanics

Halium 12 splits the boot image:
- `boot_a` (the `boot.img` partition) = generic ramdisk + kernel.
- `vendor_boot_a` (separate partition) = vendor ramdisk **containing `/lib/modules/<vermagic>/`** + DTB blob.

At boot, the kernel decompresses both ramdisks into `/`.  Halium's initramfs `init` runs `modprobe -a` against the modules in `/lib/modules/`, bringing up UFS / display / WiFi / camera / etc.  Our chainload's Stage 1 does the same (`modprobe -a` over `modules.load`), so module loading still works post-swap as long as kernel and modules share vermagic.

### The same-vermagic guarantee

`build_kernel.sh` (in our tree) checks out `kernel-volla-mt6789` and applies `ohos_adaptation.patch` + `openharmony.config`.  Modules built from that tree carry `vermagic=5.10.209 SMP preempt mod_unload modversions aarch64` — note: no `-ga4ec076d798b` scmversion suffix, because the build tree has no `.git` (the Halium build pipeline copies sources into a tmp workspace before compiling).

The live Halium kernel reports `5.10.209-ga4ec076d798b` (suffix from `scripts/setlocalversion`), but its module set was *also* built without `.git` against the same upstream — Halium modules' `/sys/module/<name>/scmversion` shows `ga4ec076d798b` only because the kernel's `init/version.c` stamps it.  Module vermagic is matched against the kernel's `MODULE_VERMAGIC_STR` at insert, which is the base `5.10.209` + flags, **not** the scmversion.  So:

- OHOS-patched kernel: `vermagic=5.10.209 SMP preempt mod_unload modversions aarch64`
- OHOS-built modules: `vermagic=5.10.209 SMP preempt mod_unload modversions aarch64`

Match.  Existing Halium modules (with their own different scmversion) would have failed only if the OHOS patch had altered `CONFIG_MODULE_SCMVERSION` or `CONFIG_LOCALVERSION` — it doesn't.  We rebuild both sides to belt-and-braces this.

### Deliverables

| Item | Status |
|---|---|
| `build_boot_img_chainload.sh` — substitute OHOS-patched kernel (env override `OHOS_KERNEL_BOOT_IMG`, defaults to `$KERNEL_TREE/out/boot.img`) | ✅ Landed |
| `flash-native.sh` — also flash `vendor_boot_a` from `kernel/linux/volla-vidofnir/out/vendor_boot.img` when present | ✅ Landed |
| `ohos_adaptation.patch` — drop `-Wundef` / `-Werror=strict-prototypes` from `KBUILD_CFLAGS` (HDF USB headers don't include `<stdbool.h>`) | ✅ Landed |
| Build OHOS-patched kernel + matched vendor_boot.img + modules.tar.gz | ✅ Done 2026-05-14 |
| Verify `/dev/access_token_id` appears on first boot | ✅ Confirmed 2026-05-14: `crw-rw-rw- access_token:access_token 10:126 /dev/access_token_id` |
| 161 Halium kernel modules load against OHOS-patched kernel (same `vermagic=5.10.209`) | ✅ Confirmed (matches Halium baseline module count) |
| Revert N8.7 marker-file bypass | ❌ Cannot revert yet — the access_tokenid kernel driver is present but the `SetSelfTokenID` userspace path is not fully wired (dmesg shows `access_tokenid_ioctl: access tokenid magic fail, TYPE=84` from unknown callers; OHOS native services still get `tokenType=TOKEN_INVALID` so samgr `CanRequest` still rejects them without the marker).  The marker bypass stays as a workaround.  Tracked as a separate userspace TokenID-population issue, out of scope for graphics. |

### What N8.10 unlocks vs what's still pending

✅ Functional after N8.10:
- Halium kernel modules load against our patched kernel — no `vermagic` mismatch (proved with `lsmod | wc -l` = 161, same as the Halium baseline).
- `/dev/access_token_id` exists; kernel driver accepts the OHOS `ACCESS_TOKEN_ID_IOCTL_BASE='A'` ioctls.
- All OHOS staging drivers from `ohos_adaptation.patch` are available: hilog, hievent, accesstokenid, blackbox, binder token-id, binder transaction tracking.

⏳ Still pending (not blocked by kernel):
- OHOS userspace `init` doesn't actually populate `service->tokenId` correctly on this build — `SetSelfTokenID` is a no-op-equivalent.  Investigate in a separate phase.
- N8.9 (display_composer_service not published) is unchanged — it's a libhybris HDF Bind issue, not a token issue.

## N8.8 — chainload mount layout for libhybris (2026-05-14 evening, continued from N8.7)

With samgr alive (N8.7) the manual `begetctl start_service composer_host` brought composer_host up, but it immediately SIGSEGV'd.  Two more cascading mount-path issues uncovered:

### A. `/android/system/lib64` did not contain the Android libs

The chainload was mounting `halium_system_a` directly at `/android/system`.  But `halium_system_a` is a dynamic-partition image with a Halium-style outer FHS (`acct/`, `apex/`, `bin/`, `system/`, …) — the actual Android `/system` content (lib64/, bin/, etc.) lives in the *inner* `system/` subdir.  So `/android/system/lib64/libhardware.so` did not exist; it was at `/android/system/system/lib64/libhardware.so`.

libhybris hardcodes `/android/system/lib64` as a search path in its bionic linker (`hybris/common/q/linker.cpp`, `hybris/common/mm/linker.cpp`, …) and in its path-redirect map (`hybris/common/hooks.c` maps `/system/` → `/android/system/`).  Without the inner content at `/android/system`, every Android-namespace dlopen failed.

**Fix:** chainload now mounts `halium_system_a` at `/halium-system` AND bind-mounts `/halium-system/system` over `/android/system`.  This gives libhybris the LXC-style view (`/android/system/lib64/...` works) while keeping the outer halium root mounted separately for `androidd`'s pivot-root needs.  `androidd.c` `ANDROID_ROOT` macro changed from `"/android/system"` to `"/halium-system"` so its mount-setup + `pivot_root` go to the outer root (where `/system/bin/init` resolves correctly post-pivot).

### B. `/apex/com.android.runtime/lib64/bionic/libc.so` not found

After fix A, manual launch of `composer_host` produced `library "libc.so" not found` in `/module_update/composer_run.log` and the SIGSEGV moved one layer in (early in the bionic linker's libc lookup).  Halium 12 ships `libc.so` from APEX (`/apex/com.android.runtime/lib64/bionic/libc.so`), not from `/system/lib64`.  LXC binds host `/apex` into the container; native boot had no `/apex` at all.

**Fix:** chainload mkdirs `/root/apex` in the rw window and bind-mounts `/halium-system/system/apex` over `/apex` after mounting `halium_system_a`.

### Current status after A + B (2026-05-14 evening)

- `composer_host` (pid alive, no SIGSEGV) — main thread idle, two IPC threads in `binder_wait_for_work`.
- `allocator_host` — same.
- `/proc/$(pidof composer_host)/maps` shows the full Android lib stack loaded: `libEGL.so`, `libGLESv2.so`, `libhwc2_compat_layer.so`, `libgralloctypes.so`, `libbinder.so`, `libbinder_ndk.so`, `libfmq.so`, `libcutils.so`, `android.hardware.graphics.{allocator,common,mapper,bufferqueue}@*.so`, and our `/vendor/lib64/passthrough/libdisplay_composer_vdi_impl.z.so` and `/vendor/lib64/libdisplay_composer_driver_1.0.z.so`.
- **But:** `hdf_devmgr` still logs `StubGetService service display_composer_service not found` at 100 Hz.  composer_host has loaded the driver but hasn't *registered* `display_composer_service` with hdf_devmgr.  This is the next debugging layer (N8.9).

## N8.9 — Open: composer_host loaded but service not registered

composer_host's HDF driver (`libdisplay_composer_driver_1.0.z.so`) is loaded but its `Bind()` either hasn't run, hasn't completed, or completed without publishing the service to hdf_servmgr.  Candidates to investigate next session:

1. **`hdf_servmgr_client` cannot reach hdf_devmgr from composer_host.**  composer_host uid is 3036; binder is 0666; samgr accepts the `CanRequest` bypass — but the SA registration may need a different ATM check.  Check `dmesg | grep PERMISSION` for fresh denials.
2. **The driver's `Bind()` is hanging on libhybris EGL/HWC init.**  Without the OHOS panel/display init, `IDisplayComposerVdi::CreateHandler` may block.  Add `hilog` traces to `device/soc/oniro/hybris_generic/hardware/display/src/display_composer/hybris_composer_vdi_impl.cpp::Bind()` (or the dispatcher in `libdisplay_composer_driver`).
3. **HCS not loaded for composer_host's host instance.**  `device_info.hcs` lists `composer_device` under `display_composer :: host`; verify the compiled `device_info.hcb` on the device matches.  `cat /vendor/etc/hdfconfig/device_info.hcb | strings | grep composer` to confirm.
4. **`g_module` symbol resolution failure.**  HDF driver modules export a `g_module` (or `HdfDriverEntry`) struct via dlsym.  If the libdisplay_composer_driver_1.0.z.so on device differs in symbol naming from what hdf_devhost expects, the bind step silently no-ops.

Recommended first probe: add `HDF_LOGE` traces to `hybris_composer_vdi_impl.cpp::CreateHandler` / `Bind()` paths and check whether they're hit.

## N8.9.1 — composer_host stuck in `WaitForProperty` (2026-05-14 evening) — FIXED

The N8.9 hypothesis list above (Bind() not running, HCS mismatch, etc.) turned out to be all wrong.  hiperf revealed the real story: composer_host's HDF driver `Bind()` DID run, called our `HybrisComposerVdiImpl::InitHwc2Device()`, which called `hwc2_compat_device_new()`, which called `HWC2::Device::Device()`, which called `android::Hwc2::Composer::Composer()`, which called `getServiceInternal<BpHwComposer>`, which called `getRawServiceInternal()`, which called **`android::hardware::defaultServiceManager1_2()`** — and that's where the main thread was spinning at 99% CPU forever, inside `android::base::WaitForProperty("hwservicemanager.ready", "true", ...)`.

### Why WaitForProperty never returned

`/dev/__properties__/` is the Android property store directory.  The Halium guest's mount NS (created by `androidd` with `CLONE_NEWNS`) had a **private** tmpfs at `/dev/__properties__/` (mounted by `androidd::child_main`), and Halium init wrote its property-area files (`properties_serial`, `property_info`, per-context files) into that private tmpfs — including the `hwservicemanager.ready=true` flag once hwservicemanager registered.

The OHOS-side `composer_host` runs **outside** the Halium NS — it inherits OHOS's root mount NS, which had **no** `/dev/__properties__/` directory at all.  When `composer_host`'s libhybris loaded Android's `libc.so` (from `/apex/com.android.runtime/lib64/bionic/libc.so`), bionic's `__system_property_init` couldn't find any property files to mmap, so every `__system_property_find("hwservicemanager.ready")` returned `nullptr`.  `WaitForProperty()` then degraded to a hot polling loop on `std::chrono::steady_clock::now()` + `__system_property_wait(nullptr, …)` — burning CPU forever, never returning.

OHOS itself uses `/dev/__parameters__/` for its parameter store (separate name, separate format), so `/dev/__properties__/` is unused real estate on the OHOS side.

### Fix — bind-share `/dev/__properties__/` between the two namespaces

Two edits, no source changes outside the chainload/launcher area:

1. **`vendor/oniro/hybris_generic/etc/init/init.x23.cfg`** (pre-init job) — pre-mount an empty tmpfs at OHOS-side `/dev/__properties__/` so it exists before `androidd` clones:

   ```
   "mkdir /dev/__properties__ 0755 root root",
   "mount tmpfs tmpfs /dev/__properties__ noexec,nosuid,nodev mode=0755",
   ```

2. **`device/board/oniro/hybris_generic/launcher/androidd.c`** (child_main, after the per-NS `/dev` tmpfs is set up) — replace the previous private-tmpfs mount with a bind-mount from OHOS's pre-mounted tmpfs:

   ```c
   if (mkdir(ANDROID_ROOT "/dev/__properties__", 0755) < 0 && errno != EEXIST)
       die("mkdir __properties__: %s", strerror(errno));
   if (mount("/dev/__properties__", ANDROID_ROOT "/dev/__properties__",
             NULL, MS_BIND, NULL) < 0)
       die("bind /dev/__properties__: %s", strerror(errno));
   ```

The child inherits OHOS's mount table at `clone()` time (the OHOS pre-init tmpfs at `/dev/__properties__/` is visible to it via the inherited mount), then `mount(NULL, "/", MS_REC | MS_PRIVATE, NULL)` makes future mount events private — but the existing inode mappings stay shared.  The bind from `/dev/__properties__` (OHOS-side) into `ANDROID_ROOT/dev/__properties__` (Halium-side) gives Halium init's writes a destination that's visible from OHOS too.  Halium init's property store init writes `properties_serial` + `property_info` + the per-context files into the shared tmpfs; `composer_host`'s `__system_property_find()` then reads exactly those files and `hwservicemanager.ready=true` is found.

### Verified end-to-end after the fix (fresh boot, 2026-05-14 evening)

```sh
# Property files visible from OHOS side
$ hdc shell 'ls /dev/__properties__/ | wc -l'
483
$ hdc shell 'cat /dev/__properties__/properties_serial | head -c 4 | xxd'
00000000: 5050 5253                                PPRS

# composer_host no longer spins
$ hdc shell 'cat /proc/$(pidof composer_host)/status | grep -E "State|voluntary"'
State:  S (sleeping)
voluntary_ctxt_switches:    51       # was 31 (never returning to userspace)
nonvoluntary_ctxt_switches: 113      # was 286420 (constant preemption)

# Display registered with render_service + SystemUI + Launcher
$ hdc shell 'timeout 3 hilog | grep hybris-hwc2-display | head -1'
... "id":0,"name":"hybris-hwc2-display","width":720,"height":1560,"refreshRate":59 ...
$ hdc shell 'cat /sys/class/leds/lcd-backlight/brightness'
223
$ hdc shell 'pidof com.ohos.systemui com.ohos.launcher'
4222  4218
```

The OHOS graphics stack reaches the same point it does in the LXC build, on the same physical device, under native boot.

### Remaining blocker — `vendor.hwcomposer-2-3` cycles, breaking the stale `IComposer` ref

Despite N8.9.1, no pixels reach the panel yet.  Symptoms:

- `composer_host` has a single SIGABRT in its history (early in boot) at `libhwc2_compat_layer::Composer::getActiveConfig` → `libhidlbase::return_status::assertOk()` → `abort()`.  Crash signature: HIDL transport error (the remote service died between `getService()` and the first `getActiveConfig` call).
- Polling `pidof android.hardware.graphics.composer@2.3-service` over 20 s shows the service has a **new pid every 4–6 s** and is dead in the gaps — a tight restart loop driven by Halium init's `class hal` + `oneshot=false`.
- `dmesg` reports the corresponding `init: starting service 'vendor.hwcomposer-2-3'...` lines at 5 s cadence.  No explicit "received signal N" line for the composer (unlike `vndservicemanager` which loudly SIGABRTs every 5 s).  The composer simply exits with a non-zero status mid-init.
- Other Halium services also cycle: `vndservicemanager` (SIGABRT), `wfca` (status 1), `storaged` (SIGABRT).  `loghidlsysservice`, `osi`, `lbs_dbg`, `zygote`, `zygote_secondary` can't even start because their binaries are missing from this Halium 12 system image (it's the UBports image — a stripped Android tree for HAL-only use).  Most of these are not graphics-relevant, but the composer-service one IS.
- Inside the Halium NS, `/dev/socket/property_service` does exist (so it's not that), and `/dev/__properties__/` is correctly populated.  `/dev/cpuset/` is **missing** in the Halium NS, which makes the `writepid /dev/cpuset/system-background/tasks` in `android.hardware.graphics.composer@2.3-service.rc` no-op (init logs a warning but doesn't kill the service).
- The watchdog in `androidd.c` only checks `lshal | grep IComposer/default` **once** and flips the `android.composer.ready` param — so a freshly restarted (but still alive on next poll) composer service is enough to satisfy it.  The cycling continues invisibly after.

### N8.9.2 — Next-session investigation plan

1. **Get Halium-side logcat** (the failing service should log its abort reason before dying).  `nsenter -t <halium-init-pid> -m -F -- chroot /root /system/bin/logcat -d -t 200` runs but produces no output on its own — needs deeper investigation.  Probably need to `chroot` properly into the Halium NS view (Halium's mount NS shows `/dev/mapper/halium_system_a on /root type ext4`, not at `/` — Halium init does an additional pivot during its boot that puts halium content at `/root`).  See N8.9.3 below.

2. **Run the composer binary manually with strace inside the Halium NS.**  Once N8.9.3 (proper chroot into Halium) is solved, `strace -f -e trace=openat,connect,ioctl /vendor/bin/hw/android.hardware.graphics.composer@2.3-service` will reveal the failing syscall.

3. **Look for a missing dependency.**  `ldd` the composer binary against Halium's `/system/lib64` + `/vendor/lib64` to spot any libs whose `dlopen` fails (e.g. Mali GLES not linkable from the composer's namespace, missing vendor HAL passthrough lib).

4. **Compare LXC vs native.**  In the LXC build the same composer binary is presumably running fine (it served us under N4 watchdog).  Diff the env vars and namespace setup.  The most likely culprit: in LXC the Halium binaries see OHOS's `/dev` (one shared `/dev` per-container), but in native boot they see a per-NS `/dev` set up by `androidd` — that `/dev` may be missing something LXC has.

5. **Harden composer_host against transient `IComposer` death.**  Independent of fixing the cycle: wrap `hwc2_compat_*` calls in retry-with-getService logic so a single SIGABRT doesn't crash the whole `hdf_devhost`.  Pattern: catch the HIDL transport error before it reaches `assertOk()` (would require a libhybris patch to `libhwc2_compat_layer` since `assertOk` is called inside the compat layer, not user code).

6. **Have the watchdog require N consecutive successful probes.**  Easier mitigation: change `androidd::watchdog` to require, say, 6 × 5 s consecutive `IComposer/default` hits before flipping `android.composer.ready=1`.  Won't fix the cycling, but it'll prevent composer_host from starting until the Halium side stabilises (if it ever does).

### N8.9.3 — Why nsenter into the Halium NS is awkward

Looking at the Halium NS mount table from outside:

```
none on / type rootfs (rw)
/dev/mapper/halium_system_a on /root type ext4 (ro,nodev,relatime)
tmpfs on /root/dev type tmpfs (rw,relatime,size=8192k,mode=755)
...
```

`androidd::child_main` does `pivot_root("/halium-system", "/halium-system/data/old_root")` so the post-pivot view should have `/` = halium content.  But Halium init (`/system/bin/init second_stage`) appears to do an ADDITIONAL pivot/chroot itself, putting halium content at `/root` inside its own NS.  Result: `nsenter -t <halium-pid> -m -F -- /system/bin/lshal` looks up `/system/bin/lshal` in the Halium mount NS root, which is the leftover rootfs (just `/root/` and a few stubs), and fails with `No such file or directory`.

Workarounds tried:
- `nsenter ... -- chroot /root /system/bin/sh`: works for sh, but `logcat`'s buffer setup may need a different context.
- Read `/proc/<halium-init>/root/system/bin/logcat` directly: visible but exec needs proper cwd/env from Halium init.

Solution candidate: `nsenter -t <halium-init> -a -F -- /bin/bash -c "exec </dev/null >&/dev/null 2>&1; logcat -d"` — try entering all of halium's NSes and using the symlink view (`/bin -> /system/bin`).  Or: drop a small debug binary into `/module_update/halium-debug/` and use `androidd`'s existing debug-overlay path (`/data/halium-debug/overlay.txt`) to bind it into the Halium FS, then run via the probe hook.

## N8.9.2-fix — SELinux absent → Halium `class hal` restart loop (2026-05-15)

**Symptom.**  `vendor.hwcomposer-2-3` (and `allocator@4.0`, `vndservicemanager`, `muxreport`, `storaged`, …) cycled every 4–6 s.  Halium-side tombstones (`/data/tombstones/` in the Halium NS, reachable as `/proc/<halium-init>/root/data/tombstones/`) showed `vndservicemanager` aborting with:

```
Abort message: 'Check failed: selinux_status_open(true ) >= 0 '
```

**Root cause.**  `vndservicemanager` is the binder context manager for `vndbinder`.  Its `Access` ctor does `CHECK(selinux_status_open(true) >= 0)`.  libselinux needs `selinuxfs` mounted; `selinuxfs` is only registered if the SELinux LSM initialised.  The Volla X23 LK pins `security=apparmor` on the kernel cmdline — that selects AppArmor as the sole "major" LSM, SELinux never initialises, `selinuxfs` is never registered, `/sys/fs/selinux/` does not exist.  `vndservicemanager` aborts → `vndbinder` context manager dies → every HAL in `class hal` that talks vndbinder fails and init restart-loops the whole class.  This is why composer\@2.3 cycled (N8.9.2's "pre-existing instability" was *this*).

**Fix.**
- `build_boot_img_chainload.sh` — chainload boot.img cmdline gains `lsm=selinux`.  Linux 5.10's `lsm=` overrides the legacy `security=`; SELinux and AppArmor are mutually-exclusive exclusive-LSMs, and native boot has no Ubuntu Touch host so dropping AppArmor is free (OHOS was already built `build_selinux=false`).
- **LK cmdline truncation quirk:** the X23 LK strips the **first 20 bytes** of the boot.img cmdline and keeps only up to the first space after that.  Only one space-free token survives.  `build_boot_img_chainload.sh` pads with a 20-char dummy `PAD=xxxxxxxxxxxxxxx` so `lsm=selinux` arrives intact.  `androidboot.selinux=permissive` could **not** be threaded through (second token) — not needed: with `lsm=selinux` and no policy loaded, `/sys/fs/selinux/enforce` reads 0 (permissive) and all access checks pass.
- `androidd.c::child_main` — mounts `selinuxfs` on the Halium-NS `/sys/fs/selinux` (after the fresh per-NS `sysfs` mount).  Without this the Halium NS — which gets its own `sysfs` — has no `selinuxfs` even though the kernel registered it.
- `init-chainload.sh` Stage 4b — `mount -t selinuxfs none /sys/fs/selinux` on the chainload `/sys` (bind-inherited into OHOS's `/root/sys`).

After this: `vndservicemanager`, `hwservicemanager`, `composer@2.3-service`, `allocator@4.0-service-mediatek` all stable; no restart loop.

## N8.10b — `/mnt` + `/storage` left on RO root → launcher crash-loop (2026-05-15)

**Symptom.**  `composer@2.3` stable, render_service up, but launcher crash-looped: `appspawn` logged `DoAppSandboxMountOnce section app-base failed` / `errno:2` on every bind under `/mnt/sandbox/100/com.ohos.launcher/...`.  `/mnt/sandbox` did not exist.

**Root cause.**  `/mnt`, `/mnt/data`, `/storage` are set up as tmpfs by `MountBasicFs()` in `base/startup/init/services/init/standard/device.c`, which runs **only from `FirstStageMain` → `SystemPrepare`**.  The chainload `exec`s OHOS init directly with `--second-stage`, so `FirstStageMain` never runs.  `/mnt` stayed a directory on the RO `system_a` rootfs; `appspawn.cfg`'s boot-job `mkdir /mnt/sandbox` silently failed; every app-spawn sandbox bind then hit ENOENT.

**Fix.**  `init-chainload.sh` Stage 4b pre-mounts the tmpfs that `MountBasicFs()` would have: `tmpfs` on `/root/mnt` (+`--make-slave`), `tmpfs` on `/root/mnt/data` (+`--make-shared`), `tmpfs` on `/root/storage`.  `/storage` is mkdir'd in the Stage 3a rw-window (it doesn't exist on `system_a`).  After this `appspawn` builds `/mnt/sandbox/<uid>/<bundle>/` correctly and launcher + SystemUI start.

## N8.11 — Mali GPU stack: bundle + load 21 modules (2026-05-15)

**Symptom.**  Launcher/SystemUI ran and saw `hybris-hwc2-display`, but nothing painted.  `render_service` + apps had no `/dev/mali0`; `/dev/mali0` did not exist; `lsmod` had no `mali`.

**Root cause.**  `mali_kbase_mt6789.ko` and its dependency modules are **not** in Halium's `vendor_boot` `modules.load` (Halium 12 on this device never loaded Mali from the kernel ramdisk — stock Ubuntu Touch loads it later via udev coldplug from the Ubuntu rootfs's `/lib/modules`, which native boot doesn't have).

**Investigation dead-ends (recorded so they aren't repeated):**
- Adding `mali_*` to `modules.load` with the **stock** `modules.dep` → kernel panic / device drops to MT65xx Preloader (modprobe loads `mali_kbase` with no dep info).
- Regenerating the whole `modules.dep` → **also panics**: the curated 180-entry `vendor-ramdisk-overlay/lib/modules/modules.dep` is special; a structurally-different regenerated file breaks early-boot module loading (UFS etc.).  **Never regenerate that file wholesale.**
- Loading the Mali stack during the chainload (before OHOS userspace) is unrecoverable on panic.

**What works.**  The 21-module GPU closure loads cleanly **post-boot, in topological order, with all dependencies satisfied** — no panic, `mali_kbase`'s probe just `-EPROBE_DEFER`s until its DT-runtime supply chain is up (`mali → ged-supply → soc:ged → gpufreq-supply → 13fbf000.gpufreq → fhctl-supply → fhctl → mcupm`).  Required platform modules not in the link-time dep graph: `mtk_gpufreq_mt6789.ko` (the MT6789 gpufreq platform driver — registers the `gpufreq` regulator), `fhctl.ko`, `mcupm.ko`.

**Fix (deliverables):**
- 52 `.ko` files (Mali GPU closure + the `chipone-tddi` touch driver) copied into `kernel/linux/volla-vidofnir/vendor-ramdisk-overlay/lib/modules/`.  The overlay is copied wholesale into `vendor_boot`, so the files ship **without** being in `modules.load` (no early chainload load, no panic risk).
- `init-chainload.sh` — Stage 1 modprobe loop `case`-skips `mali_*`; Stage 4b stashes the whole `/lib/modules` into an OHOS-visible tmpfs at `/mnt/kmodules`.
- `init.x23.cfg` pre-init — 21 `insmod /mnt/kmodules/<mod>.ko` lines in topological order (`mali_mgm_` → … → `ged` → `mali_kbase_mt6789`).  pre-init runs before `render_service`/`composer_host`, so `/dev/mali0` exists when they start.

Verified: clean boot, `/dev/mali0` present, no GPU device in `/sys/kernel/debug/devices_deferred`, `render_service` + launcher + SystemUI open `/dev/mali0` and GPU-render.

## N8.12 — `/vendor/lib64/{hw,egl}` absent → `allocator_host` SIGABRT (2026-05-15)

**Symptom.**  With Mali up, `render_service` spammed `get hdi service allocator_service failed` and nothing presented.  `allocator_host` SIGABRTed (faultlog) in:

```
android::GraphicBufferMapper::GraphicBufferMapper()  →  __android_log_assert → abort
  ← hybris_gralloc_allocate ← HybrisBufferVdiImpl::AllocMem ← AllocatorService::AllocMem
```

**Root cause.**  Android `libui`'s `GraphicBufferMapper` ctor `LOG_ALWAYS_FATAL`s if no Gralloc4/3/2 mapper loads.  The mapper passthrough is loaded from a **hardcoded** `/vendor/lib64/hw/android.hardware.graphics.mapper@4.0-impl*.so`, and the loader does an `access()` existence check on that literal path before `dlopen`.  libhybris remaps the *dlopen* `/vendor → /android/vendor` but does **not** hook `access()`; OHOS-side `/vendor/lib64/hw` did not exist → check fails → no mapper → abort.  N8.1's claim that native boot needs no `/vendor` binds was wrong — the LXC build's `lxc.mount.entry` binds of `/vendor/lib64/{hw,egl}` exist for exactly this.

**Fix.**  `init-chainload.sh` Stage 3b — briefly remount `vendor_a` rw to `mkdir /vendor/lib64/{hw,egl}`, then bind `/android/vendor/lib64/hw` and `/android/vendor/lib64/egl` over them.  Only those two subdirs are bound (not all of `/vendor/lib64`) so Android libs don't poison OHOS's vendor linker namespace — same containment as the LXC config.  After this `allocator_host` stays alive, `render_service` gets buffers, **the OHOS lockscreen renders on the physical panel.**

## N8.13 — touchscreen bring-up (chipone-tddi, 2026-05-16)

**Symptom.**  Lockscreen visible, but `/dev/input/` had only `event0` (mtk-pmic-keys), `event1` (mtk-kpd) and a virtual keyboard — no touch panel.

**The X23 touch is a `chipone-tddi` controller on SPI.**  `/sys/bus/spi/devices/spi1.0` → `spi:chipone-tddi` (DT node `chipone-tddi@0`, `chipone,x-res=720 chipone,y-res=1560` — matching the 720×1560 panel; chip is an ICNL9911C).  Volla's `gx4.config` already selects it with `CONFIG_TOUCHSCREEN_MTK_TOUCH="chipone-tddi"`, and the LXC builds (Phases 7–8) used that and had working touch.

> The X23 dts (`cust_mt6789_touch_1080x2400.dtsi`, `#include`d by `k6789v1_64.dts`) declares several touch-panel variants — an i2c node and the `chipone-tddi@0` SPI node — all `status="ok"`.  The i2c/SPI subsystems create a client device for *every* such node regardless of whether that silicon is fitted, so an enumerated touch device does not prove the hardware.  `CONFIG_TOUCHSCREEN_MTK_TOUCH` picks the single driver to build — do not override `gx4.config`'s value.

**Bring-up under native boot.**  The fix is two-fold: (1) leave `CONFIG_TOUCHSCREEN_MTK_TOUCH` alone — `gx4.config` already selects `chipone-tddi`, so the driver builds as `chipone-tddi.ko`; (2) `chipone-tddi.ko` is not in Halium's `modules.load`, so — same pattern as the Mali stack (§ N8.11) — it is bundled into the `vendor_boot` overlay (`extra-modules.list`) and `insmod`'d at OHOS `pre-init` from `/mnt/kmodules` by `init.x23.cfg`.  Link-deps `hardware_info.ko` + `mtk_disp_notify.ko` are already loaded by then.

**On-device verification (2026-05-16).**
- `chipone-tddi.ko` loaded, bound to `/sys/bus/spi/devices/spi1.0` (`spi:chipone-tddi`).
- `misc/ic_type` → `IC Type : ICNL9911C` — the driver read the chip ID over SPI; SPI `statistics` show 0 errors; `misc/rawdata` returns a live capacitance grid (sensor actively scanning).
- `mtk-tpd` input device on `/dev/input/event2` (`PROP=2` direct-touch, multitouch ABS axes); IRQ 160 (`mtk-eint 9`) registered.
- OHOS `multimodalinput` has `event2` open — the touch device is wired into the OHOS input stack.

A physical finger-on-glass coordinate test still needs hands on the device, but the full driver→input→OHOS chain is confirmed up.

**Unrelated anomaly (not touch).**  Two PMIC-class i2c chips fail to bind under native boot — `rt5133` (`5-0018`, probe `-ENXIO`) and `mt6375` (`5-0034`).  The touch does not depend on them.  Tracked separately if a consumer ever needs those rails.

## N8.6 — Expect Phase 8 stability bugs to reproduce

Native boot doesn't change the EGL teardown sequence, the HWC2 spec violations, or the Mali driver's NULL+0x1d8 crash on dropdown close. Specifically:

- **Bug 8.11** — `composer_host` SIGSEGV in `SetLayerAlpha` after ~46 minutes. Reproduces.
- **Bug 8.17** — Mali NULL+0x1d8 on dropdown close (RSRenderThread). Reproduces. Mitigations from Phase 8.17 (rwlock in libhybris EGL) carry over.
- **Bug 8.18** — Webview / nweb sandbox fix (`chmod 0644` on appdata-sandbox.json). Need to verify the native rootfs has this fix baked in (it's currently in `deploy-lxc-container.sh` for the LXC build, not in the build pipeline).

Action: port the appdata-sandbox.json chmod into the OHOS image build (or into `init.x23.cfg` as a `chmod` cmd) so it survives the native flash. Track in Phase 8 doc updates.

---

## N8 deliverables

| Item | Path | Status |
|---|---|---|
| Composer-ready gate cfg | `device/board/oniro/hybris_generic/cfg/z_composer_host_gate.cfg` | ✅ Authored |
| BUILD.gn entry | `device/board/oniro/hybris_generic/cfg/BUILD.gn` (`hybris_composer_gate_group`) | ✅ Wired into `hybris_generic_group` |
| Port Bug 8.18 fix to native image | `device/board/oniro/hybris_generic/launcher/init-chainload.sh` Stage 3a | ✅ Remount-rw → chmod 0644 → remount-ro on `system_a` mount in the chainload (init.x23.cfg can't do it — `/system` is RO once OHOS init owns the namespace).  Targets `appdata-sandbox.json` + `appdata-sandbox-isolated.json`. |
| `/dev/binderfs/binder` chmod 0666 | `vendor/oniro/hybris_generic/etc/init/init.x23.cfg` | ✅ Landed 2026-05-14 (N8.7) — pre-init `chmod 0666` on the three OHOS binder devices. The native-boot marker file `/dev/.ohos_native_boot` was REMOVED after N3.5 because real TokenIDs now propagate; the chmod stays. |
| samgr `CanRequest()` native-boot bypass | `foundation/systemabilitymgr/samgr/services/samgr/native/source/system_ability_manager_stub.cpp` | ✅ Landed 2026-05-14 (N8.7), then REVERTED 2026-05-14 (N3.5) — the userdata mount fix unblocked the real TOKEN_NATIVE path, so the bypass is no longer needed and removing it restores normal SA permissioning. |
| Property-store share between OHOS and Halium NSes | `vendor/oniro/hybris_generic/etc/init/init.x23.cfg` (pre-init `mount tmpfs /dev/__properties__`) + `device/board/oniro/hybris_generic/launcher/androidd.c` (replaces per-NS private tmpfs with bind from OHOS-side `/dev/__properties__`) | ✅ Landed 2026-05-14 evening (N8.9.1) — composer_host's `WaitForProperty(hwservicemanager.ready)` now resolves; display_composer_service publishes; SystemUI/Launcher detect `hybris-hwc2-display`. |

## Bring-up checklist

After N4 + N5 + N8 deploy, from `hdc shell`:

```sh
# Halium content present
ls /android/system/bin/hwservicemanager       # exists
ls /android/vendor/lib64/hw/                  # populated
ls /android/vendor/lib64/egl/libGLES_mali.so  # exists

# Halium HAL stack alive
pidof androidd                                # one PID
nsenter -t $(pidof androidd) -m -p -- /system/bin/lshal | grep IComposer
# android.hardware.graphics.composer@2.1::IComposer/default ...

# Ready param flipped
param get android.composer.ready              # 1

# OHOS-side composers up
pidof composer_host allocator_host render_service
# three PIDs

# Boot animation visible on the panel
hilog -x | grep -E "render_service|bootanim|composer_host" | head
```

If the screen stays black despite all checks passing:

- `hilog -x | grep -i 'EGL_BAD\|GL_INVALID\|HwcLayer'` — Phase 6/8 stability issues.
- `/data/log/faultlog/faultlogger/` — service crashes.

## Inheritance map (Phase X → native boot)

| Phase | Inherits? | Notes |
|---|---|---|
| Phase 5 (libhybris) | ✅ | No source change. Library binaries identical; path redirection already in libhybris itself. |
| Phase 6 (display VDIs) | ✅ | `device/soc/oniro/hybris_generic/hardware/display/` unchanged. composer_host cfg gains the gate (N8.4). |
| Phase 7 (input) | ✅ | `CAP_DAC_OVERRIDE` + `z_multimodalinput_caps.cfg` carry over. `/dev/input/*` perms via ueventd. |
| Phase 8.1–8.18 | ⏳ | All reproduce. Bug 8.18 chmod needs porting (see N8.6). |
| Phase 11 (backlight) | ✅ | sysfs writer in `composer_host` works untouched. |
| Phase 12 (sharefs) | ⏳ | LXC bind disappears; replace with kernel port (Phase N9.10) or an equivalent bind in `init.x23.cfg`. |

## Plan adjustments vs prior draft

1. **Removed the "Source-side complete" claim** — N8 is fundamentally a wiring + gating phase, not source delivery. Its prerequisites (N4 + N5) had not been delivered.
2. **N8.1 simplified** — no source relocation, no LXC-bind workaround. libhybris's internal path map handles `/vendor → /android/vendor` for us; native boot inherits.
3. **N8.4 is the only new artifact** — a small cfg overlay gating `composer_host`/`allocator_host` on `android.composer.ready=1`.
4. **Bug 8.18 port called out explicitly** — was in `deploy-lxc-container.sh`; needs to move into the build / init path for native.
5. **Removed mention of `lshal` polling from inside `androidd`'s child** — that's an implementation detail of N4.4, not an N8 deliverable.
