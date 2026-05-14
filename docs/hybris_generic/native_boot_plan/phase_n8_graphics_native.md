# Phase N8 — Graphics & Display (Native)

**Status:** 🔄 In progress.  Composer-ready gate cfg shipped, Bug 8.18 chmod fix moved into the chainload, **N8.7 samgr binder access + native-boot CanRequest bypass landed 2026-05-14** (full detail below).  Awaiting on-device verification that `composer_host` reaches the Halium HIDL composer end-to-end.

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
| `/dev/binderfs/binder` chmod 0666 + marker file | `vendor/oniro/hybris_generic/etc/init/init.x23.cfg` | ✅ Landed 2026-05-14 (N8.7) — pre-init `chmod 0666` on the three OHOS binder devices + `write /dev/.ohos_native_boot 1`. |
| samgr `CanRequest()` native-boot bypass | `foundation/systemabilitymgr/samgr/services/samgr/native/source/system_ability_manager_stub.cpp` | ✅ Landed 2026-05-14 (N8.7) — `access("/dev/.ohos_native_boot", F_OK) == 0` short-circuit after the existing `OHOS_RUNTIME_CONFIG` check. |

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
